// SPDX-License-Identifier: MIT
#include "savescan.h"
#include "crc32.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include <nds.h>

#define SCAN_MAX_DEPTH 6

static bool has_suffix(const char *name, const char *suffix)
{
    size_t nlen = strlen(name);
    size_t slen = strlen(suffix);
    if (nlen < slen)
        return false;
    return strcasecmp(name + (nlen - slen), suffix) == 0;
}

static void strip_suffix(char *path, size_t suffix_len)
{
    size_t len = strlen(path);
    if (len >= suffix_len)
        path[len - suffix_len] = '\0';
}

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    fclose(f);
    return true;
}

static void basename_into(const char *path, char *out, size_t out_size)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    snprintf(out, out_size, "%.*s", (int)(out_size - 1), name);
}

// Forward declaration for recursion.
static void scan_dir(const char *dir, SaveEntry *out, int max_entries,
                      int *count, int depth);

static bool already_have_base(SaveEntry *out, int count, const char *base_path)
{
    for (int i = 0; i < count; i++)
    {
        if (strcmp(out[i].base_path, base_path) == 0)
            return true;
    }
    return false;
}

static void scan_dir(const char *dir, SaveEntry *out, int max_entries,
                      int *count, int depth)
{
    if (depth > SCAN_MAX_DEPTH || *count >= max_entries)
        return;

    DIR *dirp = opendir(dir);
    if (dirp == NULL)
        return;

    struct dirent *cur;
    while ((cur = readdir(dirp)) != NULL)
    {
        if (*count >= max_entries)
            break;

        if (strcmp(cur->d_name, ".") == 0 || strcmp(cur->d_name, "..") == 0)
            continue;

        char full_path[SAVE_PATH_MAX];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir, cur->d_name);
        if (n < 0 || (size_t)n >= sizeof(full_path))
            continue; // path too long, skip rather than overflow

        if (cur->d_type == DT_DIR)
        {
            scan_dir(full_path, out, max_entries, count, depth + 1);
            continue;
        }

        if (has_suffix(full_path, ".sav"))
        {
            char base_path[SAVE_PATH_MAX];
            strncpy(base_path, full_path, sizeof(base_path) - 1);
            base_path[sizeof(base_path) - 1] = '\0';
            strip_suffix(base_path, 4); // strip ".sav"

            if (already_have_base(out, *count, base_path))
                continue;

            // Truncating a base path would make us read and overwrite the
            // wrong file, so skip anything that doesn't fit instead.
            if (strlen(base_path) >= SAVE_BASE_MAX)
                continue;

            SaveEntry *e = &out[*count];
            snprintf(e->base_path, SAVE_BASE_MAX, "%s", base_path);
            basename_into(base_path, e->display_name, SAVE_NAME_MAX);
            e->kind = SAVE_KIND_SINGLE;
            (*count)++;
        }
        else if (has_suffix(full_path, ".pub"))
        {
            char base_path[SAVE_PATH_MAX];
            strncpy(base_path, full_path, sizeof(base_path) - 1);
            base_path[sizeof(base_path) - 1] = '\0';
            strip_suffix(base_path, 4); // strip ".pub"

            if (already_have_base(out, *count, base_path))
                continue;

            char prv_path[SAVE_PATH_MAX];
            snprintf(prv_path, sizeof(prv_path), "%s.prv", base_path);
            if (!file_exists(prv_path))
                continue; // incomplete DSiWare save pair, skip it

            if (strlen(base_path) >= SAVE_BASE_MAX)
                continue;

            SaveEntry *e = &out[*count];
            snprintf(e->base_path, SAVE_BASE_MAX, "%s", base_path);
            basename_into(base_path, e->display_name, SAVE_NAME_MAX);
            e->kind = SAVE_KIND_DSIWARE;
            (*count)++;
        }
    }

    closedir(dirp);
}

int scan_saves(const char *root, SaveEntry *out, int max_entries)
{
    int count = 0;
    scan_dir(root, out, max_entries, &count, 0);
    return count;
}

void save_subfile_path(const SaveEntry *entry, int subfile, char *out)
{
    const char *ext = ".sav";
    if (subfile == SUBFILE_PUB)
        ext = ".pub";
    else if (subfile == SUBFILE_PRV)
        ext = ".prv";

    snprintf(out, SAVE_PATH_MAX, "%s%s", entry->base_path, ext);
}

u32 get_console_id(void)
{
    static u32 cached_id = 0;
    static bool have_cached = false;
    if (have_cached)
        return cached_id;

    const char *dir = "/_nds/savesync";
    const char *path = "/_nds/savesync/console_id.dat";

    FILE *f = fopen(path, "rb");
    if (f != NULL)
    {
        u32 id = 0;
        size_t n = fread(&id, sizeof(id), 1, f);
        fclose(f);
        if (n == 1 && id != 0)
        {
            cached_id = id;
            have_cached = true;
            return id;
        }
    }

    // First run on this console (or corrupt file): generate a new id.
    // This doesn't need to be cryptographically random, just different
    // between the two consoles being synced.
    mkdir("/_nds", 0777);
    mkdir(dir, 0777);

    srand((unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)&cached_id);
    u32 id = ((u32)rand() << 16) ^ (u32)rand() ^ (u32)time(NULL);
    if (id == 0)
        id = 1;

    f = fopen(path, "wb");
    if (f != NULL)
    {
        fwrite(&id, sizeof(id), 1, f);
        fclose(f);
    }

    cached_id = id;
    have_cached = true;
    return id;
}

static void sidecar_path(const SaveEntry *entry, char *out, size_t out_size)
{
    snprintf(out, out_size, "%s.syncmeta", entry->base_path);
}

static void build_fresh_meta(const SaveEntry *entry, SyncMeta *meta)
{
    memset(meta, 0, sizeof(*meta));
    meta->magic = SYNCMETA_MAGIC;
    meta->console_id = get_console_id();
    meta->version = 1;

    int subfiles[SUBFILE_COUNT] = { SUBFILE_SAV, SUBFILE_PUB, SUBFILE_PRV };
    for (int i = 0; i < SUBFILE_COUNT; i++)
    {
        int subfile = subfiles[i];
        if (entry->kind == SAVE_KIND_SINGLE && subfile != SUBFILE_SAV)
            continue;
        if (entry->kind == SAVE_KIND_DSIWARE && subfile == SUBFILE_SAV)
            continue;

        char path[SAVE_PATH_MAX];
        save_subfile_path(entry, subfile, path);

        int ok = 0;
        u32 len = 0;
        u32 crc = crc32_file(path, &ok, &len);
        if (ok)
        {
            meta->crc32[subfile] = crc;
            meta->len[subfile] = len;
            meta->present[subfile] = 1;
        }
    }
}

int save_sync_meta(const SaveEntry *entry, const SyncMeta *meta)
{
    char path[SAVE_PATH_MAX];
    sidecar_path(entry, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return -1;

    size_t n = fwrite(meta, sizeof(*meta), 1, f);
    fclose(f);
    return (n == 1) ? 0 : -1;
}

int refresh_local_meta(const SaveEntry *entry, SyncMeta *meta)
{
    char path[SAVE_PATH_MAX];
    sidecar_path(entry, path, sizeof(path));

    SyncMeta stored;
    bool have_stored = false;

    FILE *f = fopen(path, "rb");
    if (f != NULL)
    {
        size_t n = fread(&stored, sizeof(stored), 1, f);
        fclose(f);
        if (n == 1 && stored.magic == SYNCMETA_MAGIC)
            have_stored = true;
    }

    if (!have_stored)
    {
        build_fresh_meta(entry, meta);
        return save_sync_meta(entry, meta);
    }

    // Compare the stored CRCs against what's actually on disk right now.
    // Any mismatch means the game (or the user) changed the save since the
    // last time we recorded its state, so we bump the version counter.
    *meta = stored;
    bool changed = false;

    int subfiles[SUBFILE_COUNT] = { SUBFILE_SAV, SUBFILE_PUB, SUBFILE_PRV };
    for (int i = 0; i < SUBFILE_COUNT; i++)
    {
        int subfile = subfiles[i];
        if (entry->kind == SAVE_KIND_SINGLE && subfile != SUBFILE_SAV)
            continue;
        if (entry->kind == SAVE_KIND_DSIWARE && subfile == SUBFILE_SAV)
            continue;

        char subpath[SAVE_PATH_MAX];
        save_subfile_path(entry, subfile, subpath);

        int ok = 0;
        u32 len = 0;
        u32 crc = crc32_file(subpath, &ok, &len);

        if (!ok)
        {
            if (meta->present[subfile])
                changed = true; // file got deleted since last time
            meta->present[subfile] = 0;
            meta->crc32[subfile] = 0;
            meta->len[subfile] = 0;
            continue;
        }

        if (!meta->present[subfile] || meta->crc32[subfile] != crc ||
            meta->len[subfile] != len)
        {
            changed = true;
        }

        meta->present[subfile] = 1;
        meta->crc32[subfile] = crc;
        meta->len[subfile] = len;
    }

    if (changed)
    {
        meta->version++;
        meta->console_id = get_console_id();
        save_sync_meta(entry, meta);
    }

    return 0;
}
