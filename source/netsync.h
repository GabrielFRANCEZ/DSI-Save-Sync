// SPDX-License-Identifier: MIT
#ifndef NETSYNC_H
#define NETSYNC_H

#include "savescan.h"

// Result of a sync session, meant to be shown to the user.
typedef enum {
    SYNC_RESULT_OK_ALREADY_SYNCED,
    SYNC_RESULT_OK_SENT,       // we were the source, peer now has our data
    SYNC_RESULT_OK_RECEIVED,   // we were the target, we replaced our files
    SYNC_RESULT_CONFLICT,      // both sides changed independently, see .conflict-*
    SYNC_RESULT_CANCELLED,     // user pressed B
    SYNC_RESULT_NO_PEER,       // couldn't find/connect to the other console
    SYNC_RESULT_ERROR,         // CRC mismatch or I/O error, nothing was replaced
} SyncResult;

// A session runs in three steps, in this order:
//
//   1. connect  (host beacons and waits / client scans and joins)
//   2. the host picks a save and announces it; the client waits for that name
//   3. both sides compare metadata for that save and transfer what's needed
//
// Announcing the name in step 2 is what guarantees both consoles are talking
// about the same game: the client never picks anything itself.

// Step 1. Both return false if the user cancelled with B or nothing was found.
bool netsync_host_connect(char *msg, int msg_size);
bool netsync_client_connect(char *msg, int msg_size);

// Step 2, host side: keeps the link alive while the user browses the menu.
// Call once per frame. Pass NULL while nothing is selected yet.
void netsync_host_keepalive(const SaveEntry *entry);

// Step 2, client side: blocks until the host announces its next save, or
// tells us the batch is over. Returns false only on cancel/timeout; on
// success check *batch_done_out before using the name.
//
// *index_out is the save's 1-based position in the batch. The client must
// ignore an index it has already handled: the host keeps re-announcing the
// current save every frame, so the name alone can't say whether it has moved
// on -- especially if the same save were queued twice.
bool netsync_client_wait_selection(char *name_out, int name_size,
                                    SaveKind *kind_out, int *index_out,
                                    int *total_out, bool *batch_done_out,
                                    char *msg, int msg_size);

// Step 2 bis, host side: tells the client the batch is over, so it stops
// waiting for another save. Sends for a few frames, then returns.
void netsync_host_batch_finished(void);

// Barrier between two saves of a batch. The host must not announce the next
// save while the client is still finishing the previous one: the client would
// keep talking about the old save while the host talks about the new one, and
// neither would make progress. Returns false on timeout or cancel.
bool netsync_host_wait_peer_idle(char *msg, int msg_size);

// Where a session currently is, for display only. A transfer can take a
// minute, and without feedback there is no way to tell it apart from a hang.
typedef enum {
    SYNC_PHASE_HANDSHAKE = 0, // comparing metadata, nothing transferred yet
    SYNC_PHASE_TRANSFER,
} SyncPhase;

typedef struct {
    SyncPhase phase;
    bool sending;     // we are pushing data to the peer
    bool receiving;   // the peer is pushing data to us
    u32  sent_done;   // bytes, current sub-file
    u32  sent_total;
    u32  recv_done;
    u32  recv_total;

    // Diagnostics, shown once a session stops making progress. Debugging a
    // stall from a photo of the screen is the only tool available here.
    int  peer_index;    // save index of the last packet received, -1 if none
    int  idle_frames;   // frames since the last packet meant for this save
} SyncProgress;

// Called once per frame during netsync_sync(). May be NULL.
typedef void (*SyncProgressFn)(const SyncProgress *progress);

// Step 3. `local_meta` must be up to date (see refresh_local_meta()); it is
// updated in place when we end up being the receiving side.
//
// `batch_index` / `batch_total` are only used by the host, which stamps them
// on the control packets so the client can follow along; pass 1/1 for a
// single save. They have no effect on what gets transferred.
SyncResult netsync_sync(bool is_host, const SaveEntry *entry, SyncMeta *local_meta,
                         int batch_index, int batch_total,
                         SyncProgressFn on_progress, char *msg, int msg_size);

// Tears the link down. Safe to call at any point after a connect attempt.
void netsync_end(void);

// Size of one NiFi packet, in bytes. The NDS hardware caps client replies at
// a 256-byte buffer including headers, so this has to stay well below that;
// it's shown on the boot screen to make a regression obvious.
int netsync_packet_size(void);

#endif // NETSYNC_H
