// SPDX-License-Identifier: MIT
//
// NiFi (DSWiFi local multiplayer) transport for save syncing.
//
// Packet size is the hard constraint here. The NDS wifi hardware has a
// 320-byte buffer for host CMD frames and a 256-byte buffer for client reply
// frames, and the IEEE + multiplayer headers eat into both (Wifi_TxHeader 12
// + IEEE_DataFrameHeader 24 + a few MP bytes + checksum). That leaves roughly
// 215 usable bytes towards the host, so SyncPkt has to stay well under that
// or Wifi_MultiplayerHostMode() simply refuses the mode change.
//
// Hence the union: a packet carries EITHER control data (which save we're
// syncing, and the sender's metadata) OR one chunk of file data, never both.
//
// Both sides transmit one packet per frame over their native channel (host ->
// CMD, client -> REPLY), and each packet also carries an ack, so a
// stop-and-wait transfer can run independently in each direction at the same
// time. That is what a real conflict needs: each side hands the other a copy
// of its own save.

#include "netsync.h"
#include "crc32.h"

#include <dswifi9.h>
#include <nds.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// 160 keeps SyncPkt at 180 bytes. The client reply buffer is 256 bytes on the
// wire, and the headers below it (Wifi_TxHeader 12 + IEEE_DataFrameHeader 24 +
// AID 1 + checksum 4) leave roughly 215 usable, so this keeps ~35 bytes of
// margin. Raising it further trades that margin for throughput; if a future
// value is refused, Wifi_MultiplayerHostMode() now reports it on screen
// instead of hanging, but recovering means reflashing both cards.
#define SYNC_CHUNK_SIZE   160
#define SYNC_GAME_ID      0x53415653u // 'SAVS', used to recognise our beacons
#define SYNC_PROTO_VER    3
#define SYNC_TIMEOUT_VBL  (60 * 20)   // ~20s with no progress -> give up
#define SYNC_MODE_VBL     (60 * 5)    // ~5s for a wifi mode change
#define SYNC_DRIFT_VBL    (60 * 15)   // ~15s of packets for another save -> give up

#define PKT_CTRL 0
#define PKT_DATA 1

typedef struct {
    u8  proto_version;
    u8  kind;              // PKT_CTRL or PKT_DATA
    u8  have_peer_meta;    // "I already have your metadata"
    u8  have_ack;
    u8  ack_file_idx;
    // Which save of the batch this packet belongs to, 1-based; 0 means the
    // sender is idle between saves. Without it, a console that has moved on
    // to the next save and one still finishing the previous one accept each
    // other's packets, each resetting the other's timeout -- they hang
    // forever instead of failing.
    u8  save_index;
    u16 ack_seq;

    union {
        struct {
            u8  selection_valid;
            u8  selection_kind;
            u8  meta_valid;
            // Position of this save in the batch, 1-based (0 = the host hasn't
            // chosen yet). The client uses it to tell "still announcing the
            // save we just finished" from "moving on to the next one", which
            // it couldn't do from the name alone.
            u8  selection_index;
            u8  batch_total;
            u8  batch_done;   // the host is through the whole batch
            u8  pad[2];
            char name[SAVE_NAME_MAX];
            SyncMeta meta;
        } ctrl;

        struct {
            u8  active;        // carries a real chunk
            u8  done;          // sender has nothing left to push, ever
            u8  file_idx;
            u8  pad;
            u16 seq;
            u16 total_chunks;
            u16 payload_len;
            u16 pad2;
            u8  data[SYNC_CHUNK_SIZE];
        } data;
    } u;
} SyncPkt;

// The client reply buffer is 256 bytes on real hardware, headers included
// (Wifi_TxHeader 12 + IEEE_DataFrameHeader 24 + AID + checksum), leaving
// roughly 215 usable bytes. Going over makes Wifi_MultiplayerHostMode()
// return -1 and the mode change never completes, so guard it at build time.
_Static_assert(sizeof(SyncPkt) <= 192, "SyncPkt too large for a NiFi reply frame");

// Deliberately wrong on purpose? No: this makes the exact size visible in the
// build log the one time we need it. Keep commented out.
// extern int sync_pkt_size_probe[sizeof(SyncPkt)]; int sync_pkt_size_probe[1];

// --- shared receive buffer, filled from the packet handler ----------------

static volatile SyncPkt s_rx_pkt;
static volatile bool s_rx_pending = false;

static void on_packet(Wifi_MPPacketType type, int base, int len)
{
    if (type != WIFI_MPTYPE_CMD && type != WIFI_MPTYPE_REPLY)
        return;
    if ((size_t)len < sizeof(SyncPkt))
        return;

    Wifi_RxRawReadPacket(base, sizeof(SyncPkt), (void *)&s_rx_pkt);
    s_rx_pending = true;
}

// libnds gives host and client different handler signatures (the host also
// gets the sender's AID, which we don't need since we only talk to one peer).
static void host_rx_handler(Wifi_MPPacketType type, int aid, int base, int len)
{
    (void)aid;
    on_packet(type, base, len);
}

static void client_rx_handler(Wifi_MPPacketType type, int base, int len)
{
    on_packet(type, base, len);
}

static void tx_packet(bool is_host, const SyncPkt *pkt)
{
    if (is_host)
        Wifi_MultiplayerHostCmdTxFrame(pkt, sizeof(*pkt));
    else
        Wifi_MultiplayerClientReplyTxFrame(pkt, sizeof(*pkt));
}

// Le handler d'interruption ecrit s_rx_pkt sans se soucier de nous : sans
// section critique, il peut le remplacer au milieu de notre copie et nous
// livrer un paquet decousu (l'en-tete d'un paquet avec les donnees d'un
// autre). Sur 180 octets recopies a chaque image, ca finit par arriver.
static bool take_rx_packet(SyncPkt *dst)
{
    int old_ime = enterCriticalSection();
    bool had = s_rx_pending;
    if (had)
    {
        memcpy(dst, (const void *)&s_rx_pkt, sizeof(*dst));
        s_rx_pending = false;
    }
    leaveCriticalSection(old_ime);
    return had;
}

static void pkt_init(SyncPkt *out, u8 kind, int save_index)
{
    memset(out, 0, sizeof(*out));
    out->proto_version = SYNC_PROTO_VER;
    out->kind = kind;
    out->save_index = (u8)save_index;
}

// Waits for a wifi mode change without ever locking the console up: the user
// can always back out with B, and we give up rather than spin forever.
static bool wait_mode_ready(char *msg, int msg_size)
{
    int timeout = SYNC_MODE_VBL;

    while (!Wifi_LibraryModeReady())
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
        {
            snprintf(msg, msg_size, "Annule");
            return false;
        }

        if (--timeout <= 0)
        {
            snprintf(msg, msg_size, "Le Wi-Fi ne demarre pas.\nRelance l'appli.");
            return false;
        }
    }
    return true;
}

// --- helpers ---------------------------------------------------------------

static const int SUBFILES_SINGLE[]  = { SUBFILE_SAV };
static const int SUBFILES_DSIWARE[] = { SUBFILE_PUB, SUBFILE_PRV };

static const int *subfile_order(const SaveEntry *e, int *count)
{
    if (e->kind == SAVE_KIND_SINGLE)
    {
        *count = 1;
        return SUBFILES_SINGLE;
    }
    *count = 2;
    return SUBFILES_DSIWARE;
}

static u32 chunks_for_len(u32 len)
{
    if (len == 0)
        return 1;
    return (len + SYNC_CHUNK_SIZE - 1) / SYNC_CHUNK_SIZE;
}

// Does `a` need to push its data towards `b`? See file header comment.
//
// The first rule is the one that matters: a console can only send what it
// actually holds. Comparing presence in both directions -- "a has it and b
// doesn't" OR "b has it and a doesn't" -- made the console WITHOUT the save
// believe it had to send. Its sender then found no file to open and declared
// itself done immediately, which the other side read as "my peer gave up
// while it still owed me data" and aborted the save on the spot, leaving the
// receiver waiting for chunks that would never come.
static bool decide_send(const SaveEntry *entry, const SyncMeta *a, const SyncMeta *b)
{
    int count;
    const int *order = subfile_order(entry, &count);

    bool a_has_something = false;
    for (int i = 0; i < count; i++)
    {
        if (a->present[order[i]])
            a_has_something = true;
    }

    // Nothing to offer. Note this also means a save deleted on one console is
    // never propagated as a deletion: the other console keeps its copy.
    if (!a_has_something)
        return false;

    if (a->version < b->version)
        return false;
    if (a->version > b->version)
        return true;

    // Same version: send if something we hold differs from what they hold.
    for (int i = 0; i < count; i++)
    {
        int sf = order[i];
        if (!a->present[sf])
            continue;
        if (!b->present[sf])
            return true;
        if (a->crc32[sf] != b->crc32[sf])
            return true;
    }
    return false;
}

// True only when there is real, irreconcilable disagreement: both consoles
// already have actual content for the same sub-file, and it differs. This is
// what decide_send() can't tell apart on its own -- from decide_send()'s
// point of view, "peer has it and I don't yet" and "we both have it but it
// diverged" look the same (presence or CRC differ at equal version), but
// only the second one is a real conflict. The first is just a plain copy.
static bool subfiles_really_disagree(const SaveEntry *entry, const SyncMeta *a,
                                      const SyncMeta *b)
{
    int count;
    const int *order = subfile_order(entry, &count);
    for (int i = 0; i < count; i++)
    {
        int sf = order[i];
        if (a->present[sf] && b->present[sf] && a->crc32[sf] != b->crc32[sf])
            return true;
    }
    return false;
}

static void hex8(u32 v, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++)
        out[i] = digits[(v >> ((7 - i) * 4)) & 0xF];
    out[8] = '\0';
}

// --- sender side state -----------------------------------------------------

typedef struct {
    bool active;
    bool done;
    int  order_count;
    const int *order;
    int  order_pos;
    int  cur_file_idx;
    u32  cur_total_chunks;
    u32  cur_seq;
    FILE *cur_fp;
} SenderState;

static void sender_start(SenderState *s, const SaveEntry *entry)
{
    memset(s, 0, sizeof(*s));
    s->order = subfile_order(entry, &s->order_count);
    s->order_pos = -1;
    s->done = (s->order_count == 0);
}

static bool sender_open_next_file(SenderState *s, const SaveEntry *entry, const SyncMeta *local)
{
    if (s->cur_fp)
    {
        fclose(s->cur_fp);
        s->cur_fp = NULL;
    }

    s->order_pos++;
    if (s->order_pos >= s->order_count)
    {
        s->done = true;
        return false;
    }

    s->cur_file_idx = s->order[s->order_pos];
    s->cur_seq = 0;
    s->cur_total_chunks = chunks_for_len(local->len[s->cur_file_idx]);

    char path[SAVE_PATH_MAX];
    save_subfile_path(entry, s->cur_file_idx, path);
    s->cur_fp = fopen(path, "rb");
    if (s->cur_fp == NULL)
    {
        // Shouldn't happen (we just CRC'd this file), but don't get stuck.
        return sender_open_next_file(s, entry, local);
    }

    return true;
}

static bool sender_fill(SenderState *s, const SaveEntry *entry, const SyncMeta *local,
                         SyncPkt *out)
{
    if (!s->active || s->done)
        return false;

    if (s->order_pos < 0)
    {
        if (!sender_open_next_file(s, entry, local))
            return false;
    }

    if (s->cur_seq >= s->cur_total_chunks)
    {
        if (!sender_open_next_file(s, entry, local))
            return false;
    }

    u8 buf[SYNC_CHUNK_SIZE];
    fseek(s->cur_fp, (long)(s->cur_seq * SYNC_CHUNK_SIZE), SEEK_SET);
    size_t n = fread(buf, 1, SYNC_CHUNK_SIZE, s->cur_fp);

    out->u.data.active = 1;
    out->u.data.file_idx = (u8)s->cur_file_idx;
    out->u.data.seq = (u16)s->cur_seq;
    out->u.data.total_chunks = (u16)s->cur_total_chunks;
    out->u.data.payload_len = (u16)n;
    memcpy(out->u.data.data, buf, n);
    return true;
}

static void sender_on_ack(SenderState *s, int file_idx, int seq)
{
    if (!s->active || s->done)
        return;
    if (s->order_pos < 0)
        return;
    if (file_idx == s->cur_file_idx && (u32)seq == s->cur_seq)
        s->cur_seq++;
}

static void sender_close(SenderState *s)
{
    if (s->cur_fp)
    {
        fclose(s->cur_fp);
        s->cur_fp = NULL;
    }
}

// --- receiver side state ---------------------------------------------------

typedef enum { RMODE_NONE, RMODE_OVERWRITE, RMODE_CONFLICT } ReceiveMode;

typedef struct {
    bool expect;
    ReceiveMode mode;
    int  cur_file_idx;
    u32  expected_seq;
    FILE *cur_fp;
    char cur_tmp_path[SAVE_PATH_EXT_MAX];
    bool error;
    bool have_ack;
    u8   ack_file_idx;
    u16  ack_seq;
    int  files_done;
    int  files_expected;
} ReceiverState;

static void receiver_start(ReceiverState *r, const SaveEntry *entry, ReceiveMode mode, bool expect)
{
    memset(r, 0, sizeof(*r));
    r->expect = expect;
    r->mode = mode;
    r->cur_file_idx = -1;
    int count;
    subfile_order(entry, &count);
    r->files_expected = expect ? count : 0;
}

static void receiver_finish_file(ReceiverState *r, const SyncMeta *peer)
{
    if (r->cur_fp)
    {
        fclose(r->cur_fp);
        r->cur_fp = NULL;
    }

    int ok = 0;
    u32 len = 0;
    u32 crc = crc32_file(r->cur_tmp_path, &ok, &len);

    if (!ok || crc != peer->crc32[r->cur_file_idx] || len != peer->len[r->cur_file_idx])
    {
        r->error = true;
        remove(r->cur_tmp_path);
        return;
    }

    r->files_done++;
}

static void receiver_on_chunk(ReceiverState *r, const SaveEntry *entry, const SyncMeta *peer,
                               int file_idx, int seq, int total_chunks,
                               const u8 *data, int payload_len)
{
    if (!r->expect || r->error)
        return;

    if (file_idx != r->cur_file_idx)
    {
        if (r->cur_fp != NULL)
            return;
        if (seq != 0)
            return; // wait for the start of this file

        r->cur_file_idx = file_idx;
        r->expected_seq = 0;

        char real_path[SAVE_PATH_MAX];
        save_subfile_path(entry, file_idx, real_path);
        snprintf(r->cur_tmp_path, sizeof(r->cur_tmp_path), "%s.recv", real_path);

        r->cur_fp = fopen(r->cur_tmp_path, "wb");
        if (r->cur_fp == NULL)
        {
            r->error = true;
            return;
        }
    }

    if ((u32)seq != r->expected_seq)
    {
        // Duplicate or out of order: re-ack the last good chunk.
        r->have_ack = true;
        r->ack_file_idx = (u8)file_idx;
        r->ack_seq = (r->expected_seq > 0) ? (u16)(r->expected_seq - 1) : 0;
        return;
    }

    fwrite(data, 1, payload_len, r->cur_fp);
    r->have_ack = true;
    r->ack_file_idx = (u8)file_idx;
    r->ack_seq = (u16)seq;
    r->expected_seq++;

    if ((int)r->expected_seq >= total_chunks)
    {
        receiver_finish_file(r, peer);
        r->cur_file_idx = -1;
    }
}

static bool receiver_all_done(const ReceiverState *r)
{
    return !r->expect || r->error || (r->files_done >= r->files_expected);
}

static void receiver_close(ReceiverState *r)
{
    if (r->cur_fp)
    {
        fclose(r->cur_fp);
        r->cur_fp = NULL;
    }
}

static bool receiver_commit(const SaveEntry *entry)
{
    int count;
    const int *order = subfile_order(entry, &count);

    for (int i = 0; i < count; i++)
    {
        int sf = order[i];
        char real_path[SAVE_PATH_MAX];
        char tmp_path[SAVE_PATH_EXT_MAX];
        char bak_path[SAVE_PATH_EXT_MAX];
        save_subfile_path(entry, sf, real_path);
        snprintf(tmp_path, sizeof(tmp_path), "%s.recv", real_path);
        snprintf(bak_path, sizeof(bak_path), "%s.bak", real_path);

        remove(bak_path);
        rename(real_path, bak_path); // fails harmlessly if there was no file

        if (rename(tmp_path, real_path) != 0)
            return false;
    }
    return true;
}

static bool receiver_commit_conflict(const SaveEntry *entry, const SyncMeta *peer)
{
    char suffix[9];
    hex8(peer->console_id, suffix);

    int count;
    const int *order = subfile_order(entry, &count);

    for (int i = 0; i < count; i++)
    {
        int sf = order[i];
        char real_path[SAVE_PATH_MAX];
        char tmp_path[SAVE_PATH_EXT_MAX];
        char conflict_path[SAVE_PATH_EXT_MAX];
        save_subfile_path(entry, sf, real_path);
        snprintf(tmp_path, sizeof(tmp_path), "%s.recv", real_path);
        snprintf(conflict_path, sizeof(conflict_path), "%s.conflict-%s", real_path, suffix);

        remove(conflict_path);
        if (rename(tmp_path, conflict_path) != 0)
            return false;
    }
    return true;
}

// --- step 1: connecting ----------------------------------------------------

bool netsync_host_connect(char *msg, int msg_size)
{
    s_rx_pending = false;

    if (Wifi_MultiplayerHostMode(1, sizeof(SyncPkt), sizeof(SyncPkt)) != 0)
    {
        snprintf(msg, msg_size, "Paquet trop gros pour le\nNiFi (%d octets)", (int)sizeof(SyncPkt));
        return false;
    }

    Wifi_MultiplayerFromClientSetPacketHandler(host_rx_handler);

    if (!wait_mode_ready(msg, msg_size))
        return false;

    Wifi_SetChannel(6);
    Wifi_MultiplayerAllowNewClients(true);
    Wifi_BeaconStart("DSISAVESYNC", SYNC_GAME_ID);

    while (Wifi_MultiplayerGetNumClients() == 0)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
        {
            snprintf(msg, msg_size, "Annule");
            return false;
        }
    }

    Wifi_MultiplayerAllowNewClients(false);
    return true;
}

bool netsync_client_connect(char *msg, int msg_size)
{
    s_rx_pending = false;

    if (Wifi_MultiplayerClientMode(sizeof(SyncPkt)) != 0)
    {
        snprintf(msg, msg_size, "Paquet trop gros pour le\nNiFi (%d octets)", (int)sizeof(SyncPkt));
        return false;
    }

    Wifi_MultiplayerFromHostSetPacketHandler(client_rx_handler);

    if (!wait_mode_ready(msg, msg_size))
        return false;

    Wifi_ScanMode();

    Wifi_AccessPoint chosen;
    bool found = false;
    int timeout = SYNC_TIMEOUT_VBL;

    while (!found)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
        {
            snprintf(msg, msg_size, "Annule");
            return false;
        }

        int count = Wifi_GetNumAP();
        for (int i = 0; i < count; i++)
        {
            Wifi_AccessPoint ap;
            Wifi_GetAPData(i, &ap);
            if (ap.nintendo.allows_connections && ap.nintendo.game_id == SYNC_GAME_ID)
            {
                chosen = ap;
                found = true;
                break;
            }
        }

        if (!found && --timeout <= 0)
        {
            snprintf(msg, msg_size, "Aucune console trouvee.\nL'autre doit etre en mode\nHEBERGER.");
            return false;
        }
    }

    Wifi_ConnectOpenAP(&chosen);

    int timeout_assoc = SYNC_TIMEOUT_VBL;
    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
        {
            snprintf(msg, msg_size, "Annule");
            return false;
        }

        int status = Wifi_AssocStatus();
        if (status == ASSOCSTATUS_CANNOTCONNECT)
        {
            snprintf(msg, msg_size, "Connexion impossible");
            return false;
        }
        if (status == ASSOCSTATUS_ASSOCIATED)
            return true;

        if (--timeout_assoc <= 0)
        {
            snprintf(msg, msg_size, "Connexion trop longue,\nabandon");
            return false;
        }
    }
}

void netsync_end(void)
{
    Wifi_IdleMode();
}

int netsync_packet_size(void)
{
    return (int)sizeof(SyncPkt);
}

// --- step 2: announcing / awaiting the host's selection --------------------

void netsync_host_keepalive(const SaveEntry *entry)
{
    SyncPkt out;
    pkt_init(&out, PKT_CTRL, 0);

    if (entry != NULL)
    {
        out.u.ctrl.selection_valid = 1;
        out.u.ctrl.selection_kind = (u8)entry->kind;
        snprintf(out.u.ctrl.name, SAVE_NAME_MAX, "%s", entry->display_name);
    }

    tx_packet(true, &out);
    s_rx_pending = false;
}

void netsync_host_batch_finished(void)
{
    // Repeated because a single frame can be missed; the client only needs
    // to see one of them to stop waiting.
    for (int i = 0; i < 30; i++)
    {
        SyncPkt out;
        pkt_init(&out, PKT_CTRL, 0);
        out.u.ctrl.batch_done = 1;
        tx_packet(true, &out);
        swiWaitForVBlank();
    }
    s_rx_pending = false;
}

bool netsync_host_wait_peer_idle(char *msg, int msg_size)
{
    int timeout = SYNC_TIMEOUT_VBL;

    while (timeout-- > 0)
    {
        SyncPkt out;
        pkt_init(&out, PKT_CTRL, 0);
        tx_packet(true, &out);

        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
        {
            snprintf(msg, msg_size, "Annule");
            return false;
        }

        SyncPkt in;
        if (take_rx_packet(&in))
        {

            // save_index 0 means the client is between saves, i.e. back in
            // its "waiting for the next selection" loop.
            if (in.proto_version == SYNC_PROTO_VER && in.save_index == 0)
                return true;
        }
    }

    snprintf(msg, msg_size, "L'autre console n'a pas\nfini la sauvegarde\nprecedente.");
    return false;
}

bool netsync_client_wait_selection(char *name_out, int name_size,
                                    SaveKind *kind_out, int *index_out,
                                    int *total_out, bool *batch_done_out,
                                    char *msg, int msg_size)
{
    int timeout = SYNC_TIMEOUT_VBL;

    *batch_done_out = false;
    *index_out = 0;
    *total_out = 0;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
        {
            snprintf(msg, msg_size, "Annule");
            return false;
        }

        // A client can only transmit in response to a host frame, so keep a
        // packet queued at all times.
        // save_index 0 : "je suis disponible, entre deux sauvegardes".
        SyncPkt out;
        pkt_init(&out, PKT_CTRL, 0);
        tx_packet(false, &out);

        SyncPkt in;
        if (take_rx_packet(&in))
        {
            timeout = SYNC_TIMEOUT_VBL;

            if (in.proto_version != SYNC_PROTO_VER)
            {
                snprintf(msg, msg_size, "Versions de l'appli\ndifferentes entre consoles");
                return false;
            }

            if (in.kind == PKT_CTRL && in.u.ctrl.batch_done)
            {
                *batch_done_out = true;
                return true;
            }

            if (in.kind == PKT_CTRL && in.u.ctrl.selection_valid)
            {
                in.u.ctrl.name[SAVE_NAME_MAX - 1] = '\0';
                snprintf(name_out, name_size, "%s", in.u.ctrl.name);
                *kind_out = (SaveKind)in.u.ctrl.selection_kind;
                *index_out = in.u.ctrl.selection_index;
                *total_out = in.u.ctrl.batch_total;
                return true;
            }
        }
        else if (--timeout <= 0)
        {
            snprintf(msg, msg_size, "L'autre console ne repond plus");
            return false;
        }
    }
}

// --- step 3: compare and transfer ------------------------------------------

// Builds the single packet we transmit this tick. Before both sides know
// each other's metadata we send control packets (and the host keeps
// announcing its selection); afterwards we send data packets, carrying a
// chunk when we have one to push. Either way the packet also reports whether
// we already hold the peer's metadata, and acks the last chunk we received --
// those two fields live outside the union precisely so they ride along in
// both phases.
static void build_outgoing(SyncPkt *out, bool is_host, bool decided,
                            const SaveEntry *entry, const SyncMeta *local_meta,
                            SenderState *sender, const ReceiverState *receiver,
                            bool have_peer_meta, int batch_index, int batch_total)
{
    if (!decided)
    {
        // Metadata exchange. We keep sending ours until they confirm they
        // have it, so a single lost packet can't stall the session.
        pkt_init(out, PKT_CTRL, batch_index);
        out->u.ctrl.meta_valid = 1;
        out->u.ctrl.meta = *local_meta;
        if (is_host)
        {
            out->u.ctrl.selection_valid = 1;
            out->u.ctrl.selection_kind = (u8)entry->kind;
            out->u.ctrl.selection_index = (u8)batch_index;
            out->u.ctrl.batch_total = (u8)batch_total;
            snprintf(out->u.ctrl.name, SAVE_NAME_MAX, "%s", entry->display_name);
        }
    }
    else
    {
        pkt_init(out, PKT_DATA, batch_index);
        if (!sender_fill(sender, entry, local_meta, out))
            out->u.data.done = sender->done;
    }

    out->have_peer_meta = have_peer_meta ? 1 : 0;

    if (receiver->have_ack)
    {
        out->have_ack = 1;
        out->ack_file_idx = receiver->ack_file_idx;
        out->ack_seq = receiver->ack_seq;
    }
}

// Bytes already transferred for the sub-file in flight. Stop-and-wait means
// the sequence number doubles as a byte counter, capped so the last (short)
// chunk doesn't report more than the file actually holds.
static u32 bytes_from_seq(u32 seq, u32 total_len)
{
    u32 done = seq * SYNC_CHUNK_SIZE;
    return (done > total_len) ? total_len : done;
}

static void report_progress(SyncProgressFn on_progress, bool decided,
                             const SenderState *sender, const ReceiverState *receiver,
                             const SyncMeta *local_meta, const SyncMeta *peer_meta,
                             bool have_peer_meta, int peer_index, int idle_frames)
{
    if (on_progress == NULL)
        return;

    SyncProgress p;
    memset(&p, 0, sizeof(p));
    p.phase = decided ? SYNC_PHASE_TRANSFER : SYNC_PHASE_HANDSHAKE;
    p.peer_index = peer_index;
    p.idle_frames = idle_frames;

    if (decided && sender->active && !sender->done && sender->order_pos >= 0)
    {
        p.sending = true;
        p.sent_total = local_meta->len[sender->cur_file_idx];
        p.sent_done = bytes_from_seq(sender->cur_seq, p.sent_total);
    }

    if (decided && receiver->expect && have_peer_meta && receiver->cur_file_idx >= 0)
    {
        p.receiving = true;
        p.recv_total = peer_meta->len[receiver->cur_file_idx];
        p.recv_done = bytes_from_seq(receiver->expected_seq, p.recv_total);
    }

    on_progress(&p);
}

SyncResult netsync_sync(bool is_host, const SaveEntry *entry, SyncMeta *local_meta,
                         int batch_index, int batch_total,
                         SyncProgressFn on_progress, char *msg, int msg_size)
{
    s_rx_pending = false;

    SyncMeta peer_meta;
    bool have_peer_meta = false;   // we received theirs
    bool peer_has_my_meta = false; // they told us they received ours
    bool decided = false;
    bool peer_send_done = false;

    SenderState sender;
    sender_start(&sender, entry);

    ReceiverState receiver;
    receiver_start(&receiver, entry, RMODE_NONE, false);

    int timeout = SYNC_TIMEOUT_VBL;
    int finishing = 0; // frames spent transmitting our final ack before leaving
    int drift = 0;     // consecutive packets belonging to another save
    int peer_index = -1;  // save index seen on the last packet, for diagnostics
    int idle_frames = 0;  // frames since a packet meant for this save
    bool version_mismatch = false;

    while (1)
    {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_B)
        {
            sender_close(&sender);
            receiver_close(&receiver);
            snprintf(msg, msg_size, "Annule, rien n'a ete modifie");
            return SYNC_RESULT_CANCELLED;
        }

        // --- build and send our packet for this tick ---
        SyncPkt out;
        build_outgoing(&out, is_host, decided, entry, local_meta,
                        &sender, &receiver, have_peer_meta,
                        batch_index, batch_total);
        tx_packet(is_host, &out);

        // --- process what the peer sent this tick ---
        SyncPkt in;
        if (take_rx_packet(&in))
        {

            if (in.proto_version != SYNC_PROTO_VER)
            {
                version_mismatch = true;
                break;
            }

            bool for_us = (in.save_index == batch_index);

            // Only a packet about OUR save proves the peer is still working
            // with us. Resetting the timeout on any packet at all is what
            // froze both consoles: the peer had moved on and was sending
            // packets about something else -- or nothing at all -- and each
            // one bought us another 20 seconds, forever.
            peer_index = in.save_index;

            if (for_us)
            {
                timeout = SYNC_TIMEOUT_VBL;
                drift = 0;
                idle_frames = 0;
            }
            else if (++drift > SYNC_DRIFT_VBL)
            {
                sender_close(&sender);
                receiver_close(&receiver);
                snprintf(msg, msg_size, in.save_index == 0
                         ? "L'autre console a abandonne\ncette sauvegarde."
                         : "Les deux consoles ne sont\npas sur la meme sauvegarde.\nRelance la synchro.");
                return SYNC_RESULT_ERROR;
            }

            if (for_us && in.have_peer_meta)
                peer_has_my_meta = true;

            // Only before the decision. Afterwards peer_meta is what we check
            // the received CRCs against, and what we copy into our own sidecar
            // -- and in a batch the host may already be announcing the NEXT
            // save while we finish this one, so accepting its metadata here
            // would attribute the wrong version and CRC to this save.
            if (for_us && in.kind == PKT_CTRL && in.u.ctrl.meta_valid && !decided)
            {
                peer_meta = in.u.ctrl.meta;
                have_peer_meta = true;
            }
            else if (for_us && in.kind == PKT_DATA)
            {
                if (in.u.data.done)
                    peer_send_done = true;

                if (decided && in.u.data.active)
                {
                    receiver_on_chunk(&receiver, entry, &peer_meta, in.u.data.file_idx,
                                       in.u.data.seq, in.u.data.total_chunks,
                                       in.u.data.data, in.u.data.payload_len);
                }
            }

            if (for_us && in.have_ack)
                sender_on_ack(&sender, in.ack_file_idx, in.ack_seq);
        }
        else if (--timeout <= 0)
        {
            sender_close(&sender);
            receiver_close(&receiver);
            snprintf(msg, msg_size, "L'autre console ne repond plus");
            return SYNC_RESULT_NO_PEER;
        }

        idle_frames++;

        // Both sides have each other's metadata: decide who sends what.
        if (!decided && have_peer_meta && peer_has_my_meta)
        {
            bool i_send = decide_send(entry, local_meta, &peer_meta);
            bool peer_sends = decide_send(entry, &peer_meta, local_meta);

            sender.active = i_send;
            if (!i_send)
                sender.done = true;

            ReceiveMode mode = RMODE_NONE;
            if (peer_sends)
            {
                if (peer_meta.version > local_meta->version)
                    mode = RMODE_OVERWRITE;
                else if (subfiles_really_disagree(entry, local_meta, &peer_meta))
                    mode = RMODE_CONFLICT;
                else
                    mode = RMODE_OVERWRITE; // e.g. equal version, I just don't have it yet
            }
            receiver_start(&receiver, entry, mode, peer_sends);
            decided = true;
        }

        report_progress(on_progress, decided, &sender, &receiver,
                         local_meta, &peer_meta, have_peer_meta,
                         peer_index, idle_frames);

        // The peer says it has nothing left to push, but we're still short of
        // what its metadata promised: waiting longer would just burn the
        // timeout, so fail now with something the user can act on.
        if (decided && peer_send_done && !receiver_all_done(&receiver))
        {
            sender_close(&sender);
            receiver_close(&receiver);
            snprintf(msg, msg_size, "Transfert incomplet.\nRien n'a ete modifie.");
            return SYNC_RESULT_ERROR;
        }

        // Exit on our own state only. Waiting for the peer's "done" flag as
        // well used to deadlock: whichever side finished first left the data
        // phase, and from then on it only sent control packets, so the other
        // side waited for a flag that could never arrive.
        if (decided && sender.done && receiver_all_done(&receiver))
        {
            // Keep transmitting for a few frames before leaving. Our packet
            // is built before the peer's is processed, so the ack for the
            // last chunk we received is still sitting in our outgoing packet
            // -- leaving immediately would strand the peer's sender one chunk
            // short of finishing.
            if (++finishing >= 6)
                break;
        }
    }

    sender_close(&sender);
    receiver_close(&receiver);

    if (version_mismatch)
    {
        snprintf(msg, msg_size, "Versions de l'appli\ndifferentes entre consoles");
        return SYNC_RESULT_ERROR;
    }

    if (receiver.error)
    {
        snprintf(msg, msg_size, "Erreur: donnees corrompues.\nRien n'a ete modifie.");
        return SYNC_RESULT_ERROR;
    }

    if (!sender.active && !receiver.expect)
    {
        snprintf(msg, msg_size, "Les deux consoles ont deja\nla meme sauvegarde");
        return SYNC_RESULT_OK_ALREADY_SYNCED;
    }

    if (receiver.expect)
    {
        if (receiver.mode == RMODE_CONFLICT)
        {
            if (!receiver_commit_conflict(entry, &peer_meta))
            {
                snprintf(msg, msg_size, "Erreur en enregistrant\nla copie de conflit");
                return SYNC_RESULT_ERROR;
            }
            snprintf(msg, msg_size, "Les deux ont change.\nCopie de l'autre console\ngardee en .conflict-*");
            return SYNC_RESULT_CONFLICT;
        }

        if (!receiver_commit(entry))
        {
            snprintf(msg, msg_size, "Erreur en remplacant\nle fichier");
            return SYNC_RESULT_ERROR;
        }

        *local_meta = peer_meta;
        save_sync_meta(entry, local_meta);
        snprintf(msg, msg_size, "Sauvegarde mise a jour.\nAncienne gardee en .bak");
        return SYNC_RESULT_OK_RECEIVED;
    }

    snprintf(msg, msg_size, "Sauvegarde envoyee a\nl'autre console");
    return SYNC_RESULT_OK_SENT;
}
