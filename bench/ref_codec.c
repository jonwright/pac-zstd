/*
 * The reference codec of the benchmarks: the usual way to write and read a
 * bitshuffle HDF5 chunk, from the same vendored libraries as the encoder
 * under test.  "stock" encode = kcb bit transpose, then LZ4_compress_default
 * or ZSTD_compressCCtx on each block; decode = LZ4_decompress_safe or
 * ZSTD_decompress, then kcb bit untranspose.  So a stock chunk and a pac chunk
 * differ only in how the transposed block is compressed.
 *
 * One thread only (static buffers).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.  Code written by Claude Fable 5.2.
 */
#include <stdlib.h>
#include <string.h>

#include "lz4.h"
#include "zstd.h"

extern int bitshuf_encode_block(char *out, const char *in, char *scratch, size_t size, size_t elem_size);
extern int bitshuf_decode_block(char *out, const char *in, char *scratch, size_t size, size_t elem_size);

static char *tmp, *scratch;
static size_t cap_block;
static ZSTD_CCtx *cctx;
static ZSTD_DCtx *dctx;

static int reserve(size_t block)
{
    if (cap_block >= block)
        return 1;
    free(tmp);
    free(scratch);
    tmp = (char *) malloc(block + 64);
    scratch = (char *) malloc(block + 64);
    cap_block = tmp && scratch ? block : 0;
    return cap_block != 0;
}

static void put_be(unsigned char *p, unsigned long long v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char) (v >> (8 * (n - 1 - i)));
}

static unsigned long long get_be(const unsigned char *p, int n)
{
    unsigned long long v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

/* codec 1 = LZ4, 2 = zstd at `level`.  Returns the chunk size, 0 on error. */
size_t ref_encode(int codec, int level, unsigned char *dst, size_t cap, const unsigned char *src,
                  size_t size, size_t elem, size_t block)
{
    size_t n = 12, done = 0;
    if (!reserve(block) || cap < 12)
        return 0;
    if (!cctx)
        cctx = ZSTD_createCCtx();
    put_be(dst, size, 8);
    put_be(dst + 8, block, 4);
    while (done < size) {
        size_t b = size - done < block ? size - done : block;
        size_t left = b % (8 * elem), nb = b - left, c;
        if (nb) {
            if (cap - n < 4)
                return 0;
            bitshuf_encode_block(tmp, (const char *) src + done, scratch, nb / elem, elem);
            if (codec == 1) {
                int r = LZ4_compress_default(tmp, (char *) dst + n + 4, (int) nb, (int) (cap - n - 4));
                c = r > 0 ? (size_t) r : 0;
            } else {
                c = ZSTD_compressCCtx(cctx, dst + n + 4, cap - n - 4, tmp, nb, level);
                if (ZSTD_isError(c))
                    c = 0;
            }
            if (!c)
                return 0;
            put_be(dst + n, c, 4);
            n += 4 + c;
        }
        if (left) {
            if (cap - n < left)
                return 0;
            memcpy(dst + n, src + done + nb, left);
            n += left;
        }
        done += b;
    }
    return n;
}

/* Returns the decoded size, 0 on error (or for an empty chunk). */
size_t ref_decode(int codec, const unsigned char *c, size_t csize, size_t elem, unsigned char *out,
                  size_t cap)
{
    unsigned long long total, done = 0;
    size_t block, pos = 12;
    if (csize < 12)
        return 0;
    total = get_be(c, 8);
    block = (size_t) get_be(c + 8, 4);
    if (!block) /* the filter's default */
        block = (8192 / elem / 8) * 8 * elem;
    if (total > cap || block % (8 * elem) || !reserve(block))
        return 0;
    while (done < total) {
        size_t b = total - done < block ? (size_t) (total - done) : block;
        size_t left = b % (8 * elem), nb = b - left;
        if (nb) {
            size_t cs, got;
            if (csize - pos < 4)
                return 0;
            cs = (size_t) get_be(c + pos, 4);
            pos += 4;
            if (csize - pos < cs)
                return 0;
            if (codec == 1) {
                int r = LZ4_decompress_safe((const char *) c + pos, tmp, (int) cs, (int) nb);
                got = r < 0 ? 0 : (size_t) r;
            } else {
                if (!dctx)
                    dctx = ZSTD_createDCtx();
                got = ZSTD_decompressDCtx(dctx, tmp, nb, c + pos, cs);
                if (ZSTD_isError(got))
                    got = 0;
            }
            if (got != nb)
                return 0;
            pos += cs;
            bitshuf_decode_block((char *) out + done, tmp, scratch, nb / elem, elem);
        }
        if (left) {
            if (csize - pos < left)
                return 0;
            memcpy(out + done + nb, c + pos, left);
            pos += left;
        }
        done += b;
    }
    return pos == csize ? (size_t) total : 0;
}

/* The bit transpose alone, as an encoder does it: kcb's bitshuf_encode_block on
 * each block of `block` bytes of src, into dst (size bytes; the remainder of a
 * block that is not a multiple of 8 elements is copied).  The ceiling for the
 * speed of any bitshuffle encoder.  Returns size, 0 on error. */
size_t ref_transpose(unsigned char *dst, const unsigned char *src, size_t size, size_t elem, size_t block)
{
    size_t done = 0;
    if (!reserve(block))
        return 0;
    while (done < size) {
        size_t b = size - done < block ? size - done : block;
        size_t left = b % (8 * elem), nb = b - left;
        if (nb)
            bitshuf_encode_block((char *) dst + done, (const char *) src + done, scratch, nb / elem, elem);
        if (left)
            memcpy(dst + done + nb, src + done + nb, left);
        done += b;
    }
    return size;
}
