// SPDX-License-Identifier: MIT
#ifndef SAVESCAN_H
#define SAVESCAN_H

#include <nds/ndstypes.h>

// Full paths get built by appending suffixes to base_path (".sav", but also
// ".sav.conflict-XXXXXXXX"), so the buffer for a full path has to leave room
// for the longest of those on top of a base path.
#define SAVE_BASE_MAX   200
#define SAVE_PATH_MAX   256
// Room for a full path plus a staging/backup suffix (".recv", ".bak",
// ".conflict-XXXXXXXX").
#define SAVE_PATH_EXT_MAX (SAVE_PATH_MAX + 32)
#define SAVE_NAME_MAX   48
#define MAX_SAVE_ENTRIES 64

// A single .sav (DS game) has kind SAVE_SINGLE.
// A DSiWare game has a .pub AND a .prv that must travel together.
typedef enum {
    SAVE_KIND_SINGLE  = 0, // <base>.sav
    SAVE_KIND_DSIWARE = 1, // <base>.pub + <base>.prv
} SaveKind;

typedef struct {
    char base_path[SAVE_BASE_MAX]; // full path without extension
    char display_name[SAVE_NAME_MAX];
    SaveKind kind;
} SaveEntry;

// Sub-file indices used throughout the sync protocol.
#define SUBFILE_SAV 0
#define SUBFILE_PUB 1
#define SUBFILE_PRV 2
#define SUBFILE_COUNT 3

// Metadata persisted next to each save (sidecar file "<base>.syncmeta") and
// exchanged with the peer console during a sync session.
typedef struct {
    u32 magic;        // SYNCMETA_MAGIC, sanity check for the sidecar file
    u32 console_id;   // id of the console that produced this version
    u32 version;      // monotonically increasing per console, bumped whenever
                       // a live file's CRC no longer matches what's on record
    u32 crc32[SUBFILE_COUNT];
    u32 len[SUBFILE_COUNT];
    u8  present[SUBFILE_COUNT]; // 1 if that sub-file exists on this console
} SyncMeta;

#define SYNCMETA_MAGIC 0x53535931u // "SSY1"

// Recursively scans `root` for .sav/.pub/.prv files (depth-limited) and
// fills `out` with up to `max_entries` SaveEntry records.
// Returns the number of entries found.
int scan_saves(const char *root, SaveEntry *out, int max_entries);

// Returns this console's persistent random id, creating and storing one on
// first run at "/_nds/savesync/console_id.dat".
u32 get_console_id(void);

// Loads the sidecar for `entry` into `meta`. If the sidecar is missing or
// invalid, builds a fresh one from the live files on disk (version = 1) and
// writes it out. Always leaves `meta` reflecting the CURRENT on-disk files.
// Returns 0 on success, negative on unrecoverable error.
int refresh_local_meta(const SaveEntry *entry, SyncMeta *meta);

// Persists `meta` as the sidecar for `entry`.
int save_sync_meta(const SaveEntry *entry, const SyncMeta *meta);

// Builds the full path of a given sub-file (".sav" / ".pub" / ".prv") for an
// entry, e.g. base_path + ".sav" into `out` (size SAVE_PATH_MAX).
void save_subfile_path(const SaveEntry *entry, int subfile, char *out);

#endif // SAVESCAN_H
