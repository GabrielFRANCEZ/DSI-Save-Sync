// SPDX-License-Identifier: MIT
#include "crc32.h"

#include <stdbool.h>
#include <stdio.h>

static u32 crc_table[256];
static bool crc_table_ready = false;

static void crc32_build_table(void)
{
    for (u32 i = 0; i < 256; i++)
    {
        u32 c = i;
        for (int k = 0; k < 8; k++)
        {
            if (c & 1)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[i] = c;
    }
    crc_table_ready = true;
}

static u32 crc32_update(u32 crc, const void *buf, size_t len)
{
    if (!crc_table_ready)
        crc32_build_table();

    const u8 *p = (const u8 *)buf;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

    return crc;
}

u32 crc32_buf(const void *data, size_t len)
{
    u32 crc = crc32_update(0xFFFFFFFFu, data, len);
    return crc ^ 0xFFFFFFFFu;
}

u32 crc32_file(const char *path, int *ok, u32 *out_len)
{
    if (ok)
        *ok = 0;

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0xFFFFFFFFu;

    u32 crc = 0xFFFFFFFFu;
    u32 total = 0;
    u8 buf[512];

    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        crc = crc32_update(crc, buf, n);
        total += (u32)n;
    }

    fclose(f);

    if (ok)
        *ok = 1;
    if (out_len)
        *out_len = total;

    return crc ^ 0xFFFFFFFFu;
}
