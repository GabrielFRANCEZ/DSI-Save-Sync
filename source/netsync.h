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

// Step 2, client side: blocks until the host announces its selection.
bool netsync_client_wait_selection(char *name_out, int name_size,
                                    SaveKind *kind_out, char *msg, int msg_size);

// Step 3. `local_meta` must be up to date (see refresh_local_meta()); it is
// updated in place when we end up being the receiving side.
SyncResult netsync_sync(bool is_host, const SaveEntry *entry, SyncMeta *local_meta,
                         char *msg, int msg_size);

// Tears the link down. Safe to call at any point after a connect attempt.
void netsync_end(void);

// Size of one NiFi packet, in bytes. The NDS hardware caps client replies at
// a 256-byte buffer including headers, so this has to stay well below that;
// it's shown on the boot screen to make a regression obvious.
int netsync_packet_size(void);

#endif // NETSYNC_H
