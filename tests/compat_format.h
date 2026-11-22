/*
 * Shared binary corpus format for pac-zstd compatibility and RFC checks.
 *
 * Layout (all integers little-endian):
 *   8 bytes: magic "PACC0001"
 *   4 bytes: record count
 *   Followed by `count` records:
 *     4 bytes: name_len
 *     4 bytes: codec (PAC_LZ4 or PAC_ZSTD)
 *     4 bytes: elem_size (1, 2, 4, or 8)
 *     4 bytes: block_size (bytes)
 *     4 bytes: raw_size (bytes)
 *     4 bytes: chunk_size (bytes)
 *     name_len bytes: ASCII case name (not NUL-terminated)
 *     raw_size bytes: original uncompressed pixels
 *     chunk_size bytes: bitshuffle HDF5 chunk
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#ifndef PAC_COMPAT_FORMAT_H
#define PAC_COMPAT_FORMAT_H

#include <stdint.h>
#include <stdio.h>

#define COMPAT_MAGIC "PACC0001"
#define COMPAT_MAGIC_LEN 8

static int compat_write_u32(FILE *f, uint32_t v)
{
    unsigned char b[4];
    b[0] = (unsigned char) (v & 0xffU);
    b[1] = (unsigned char) ((v >> 8) & 0xffU);
    b[2] = (unsigned char) ((v >> 16) & 0xffU);
    b[3] = (unsigned char) ((v >> 24) & 0xffU);
    return fwrite(b, 1, 4, f) == 4;
}

static int compat_read_u32(FILE *f, uint32_t *out)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *out = (uint32_t) b[0] | ((uint32_t) b[1] << 8) | ((uint32_t) b[2] << 16) |
           ((uint32_t) b[3] << 24);
    return 1;
}

#endif /* PAC_COMPAT_FORMAT_H */
