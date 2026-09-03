// SPDX-License-Identifier: MIT
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <nds/ndstypes.h>

// CRC32 (IEEE 802.3) of an in-memory buffer.
u32 crc32_buf(const void *data, size_t len);

// CRC32 of a whole file, read in small chunks so it works even on large
// saves without allocating the whole file in RAM. Returns 0xFFFFFFFF and
// sets *ok = false if the file can't be opened. On success *ok = true and
// *out_len (if not NULL) receives the file length in bytes.
u32 crc32_file(const char *path, int *ok, u32 *out_len);

#endif // CRC32_H
