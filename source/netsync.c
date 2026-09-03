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

#define SYNC_CHUNK_SIZE   128         // keeps SyncPkt at 148 bytes, see above
#define SYNC_GAME_ID      0x53415653u // 'SAVS', used to recognise our beacons
#define SYNC_PROTO_VER    2
#define SYNC_TIMEOUT_VBL  (60 * 20)   // ~20s with no progress -> give up
#define SYNC_MODE_VBL     (60 * 5)    // ~5s for a wifi mode change

#define PKT_CTRL 0
#define PKT_DATA 1

typedef struct {
    u8  proto_version;
    u8  kind;              // PKT_CTRL or PKT_DATA
    u8  have_peer_meta;    // "I already have your metadata"
    u8  have_ack;
    u8  ack_file_idx;
    u8  pad;
    u16 ack_seq;

    union {
        struct {
            u8  selection_valid;
            u8  selection_kind;
            u8  meta_valid;
            u8  pad;
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

static void pkt_init(SyncPkt *out, u8 kind)
{
    memset(out, 0, sizeof(*out));
    out->proto_version = SYNC_PROTO_VER;
    out->kind = kind;
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
static bool decide_send(const SaveEntry *entry, const SyncMeta *a, const SyncMeta *b)
{
    if (a->version < b->version)
        return false;
    if (a->version > b->version)
        return true;

    int count;
    const int *order = subfile_order(entry, &count);
    for (int i = 0; i < count; i++)
    {
        int sf = order[i];
        if (a->present[sf] != b->present[sf])
            return true;
        if (a->present[sf] && a->crc32[sf] != b->crc32[sf])
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
    pkt_init(&out, PKT_CTRL);

    if (entry != NULL)
    {
        out.u.ctrl.selection_valid = 1;
        out.u.ctrl.selection_kind = (u8)entry->kind;
        snprintf(out.u.ctrl.name, SAVE_NAME_MAX, "%s", entry->display_name);
    }

    tx_packet(true, &out);
    s_rx_pending = false;
}

bool netsync_client_wait_selection(char *name_out, int name_size,
                                    SaveKind *kind_out, char *msg, int msg_size)
{
    int timeout = SYNC_TIMEOUT_VBL;

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
        SyncPkt out;
        pkt_init(&out, PKT_CTRL);
        tx_packet(false, &out);

        if (s_rx_pending)
        {
            s_rx_pending = false;
            timeout = SYNC_TIMEOUT_VBL;

            SyncPkt in;
            memcpy(&in, (const void *)&s_rx_pkt, sizeof(in));

            if (in.proto_version != SYNC_PROTO_VER)
            {
                snprintf(msg, msg_size, "Versions de l'appli\ndifferentes entre consoles");
                return false;
            }

            if (in.kind == PKT_CTRL && in.u.ctrl.selection_valid)
            {
                in.u.ctrl.name[SAVE_NAME_MAX - 1] = '\0';
                snprintf(name_out, name_size, "%s", in.u.ctrl.name);
                *kind_out = (SaveKind)in.u.ctrl.selection_kind;
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

SyncResult netsync_sync(bool is_host, const SaveEntry *entry, SyncMeta *local_meta,
                         char *msg, int msg_size)
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

        if (!decided)
        {
            // Metadata exchange. We keep sending ours until they confirm they
            // have it, so a single lost packet can't stall the session.
            pkt_init(&out, PKT_CTRL);
            out.u.ctrl.meta_valid = 1;
            out.u.ctrl.meta = *local_meta;
            if (is_host)
            {
                out.u.ctrl.selection_valid = 1;
                out.u.ctrl.selection_kind = (u8)entry->kind;
                snprintf(out.u.ctrl.name, SAVE_NAME_MAX, "%s", entry->display_name);
            }
        }
        else
        {
            pkt_init(&out, PKT_DATA);
            if (!sender_fill(&sender, entry, local_meta, &out))
                out.u.data.done = sender.done;
        }

        out.have_peer_meta = have_peer_meta ? 1 : 0;

        if (receiver.have_ack)
        {
            out.have_ack = 1;
            out.ack_file_idx = receiver.ack_file_idx;
            out.ack_seq = receiver.ack_seq;
        }

        tx_packet(is_host, &out);

        // --- process what the peer sent this tick ---
        if (s_rx_pending)
        {
            s_rx_pending = false;
            timeout = SYNC_TIMEOUT_VBL;

            SyncPkt in;
            memcpy(&in, (const void *)&s_rx_pkt, sizeof(in));

            if (in.proto_version != SYNC_PROTO_VER)
            {
                version_mismatch = true;
                break;
            }

            if (in.have_peer_meta)
                peer_has_my_meta = true;

            if (in.kind == PKT_CTRL && in.u.ctrl.meta_valid)
            {
                peer_meta = in.u.ctrl.meta;
                have_peer_meta = true;
            }
            else if (in.kind == PKT_DATA)
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

            if (in.have_ack)
                sender_on_ack(&sender, in.ack_file_idx, in.ack_seq);
        }
        else if (--timeout <= 0)
        {
            sender_close(&sender);
            receiver_close(&receiver);
            snprintf(msg, msg_size, "L'autre console ne repond plus");
            return SYNC_RESULT_NO_PEER;
        }

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

        if (decided && sender.done && peer_send_done && receiver_all_done(&receiver))
            break;
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
