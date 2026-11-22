/*
 * External-library compatibility verifier for pac-zstd corpus files.
 *
 * Compiled in two modes by tests/check_compat.sh:
 *   -DCOMPAT_USE_BSHUF    : bitshuffle 0.4.0 (bshuf_decompress_zstd / _lz4)
 *                           linked against zstd v1.5.2 and lz4 v1.9.3
 *   -DCOMPAT_USE_LZ4_ONLY : external LZ4_decompress_safe (e.g. lz4 v1.10.0)
 *                           + kcb bitshuf_decode_block
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat_format.h"

#define PAC_LZ4 1
#define PAC_ZSTD 2

#if defined(COMPAT_USE_BSHUF)
#include "bitshuffle.h"
#elif defined(COMPAT_USE_LZ4_ONLY)
#include "lz4.h"
extern int bitshuf_decode_block(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);
#else
#error "Define COMPAT_USE_BSHUF or COMPAT_USE_LZ4_ONLY"
#endif

static uint64_t read_be(const unsigned char *p, int n)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static int verify_case(const char *name, uint32_t codec, uint32_t elem, uint32_t hdr_block,
                       const unsigned char *raw, size_t raw_size, const unsigned char *chunk,
                       size_t chunk_size)
{
    uint64_t total;
    size_t block;
    unsigned char *out;
    int ok = 1;

#if defined(COMPAT_USE_LZ4_ONLY)
    if (codec != PAC_LZ4)
        return 1;
#endif
    (void) hdr_block;

    if (chunk_size < 12) {
        fprintf(stderr, "FAIL %s: chunk < 12 bytes\n", name);
        return 0;
    }
    total = read_be(chunk, 8);
    block = (size_t) read_be(chunk + 8, 4);
    if (!block)
        block = (8192U / elem / 8U) * 8U * elem;
    if (total != raw_size) {
        fprintf(stderr, "FAIL %s: header total %llu != raw %lu\n", name,
                (unsigned long long) total, (unsigned long) raw_size);
        return 0;
    }
    if (total == 0)
        return chunk_size == 12;

    out = (unsigned char *) malloc(raw_size);
    if (!out) {
        fprintf(stderr, "oom\n");
        return 0;
    }

#if defined(COMPAT_USE_BSHUF)
    {
        int64_t rc;
        size_t total_elems = raw_size / elem;
        size_t block_elems = block / elem;
        if (codec == PAC_ZSTD) {
            rc = bshuf_decompress_zstd(chunk + 12, out, total_elems, elem, block_elems);
            if (rc < 0) {
                fprintf(stderr, "FAIL %s: bitshuffle 0.4.0 zstd rc=%lld\n", name, (long long) rc);
                ok = 0;
            }
        } else {
            rc = bshuf_decompress_lz4(chunk + 12, out, total_elems, elem, block_elems);
            if (rc < 0 || (size_t) (rc + 12) != chunk_size) {
                fprintf(stderr, "FAIL %s: bitshuffle 0.4.0 lz4 rc=%lld (expected consumed=%lu)\n",
                        name, (long long) rc, (unsigned long) (chunk_size - 12));
                ok = 0;
            }
        }
    }
#elif defined(COMPAT_USE_LZ4_ONLY)
    {
        uint64_t done = 0;
        size_t pos = 12;
        char *tmp = (char *) malloc(block + 64);
        char *scratch = (char *) malloc(block + 64);
        if (!tmp || !scratch) {
            free(tmp);
            free(scratch);
            free(out);
            return 0;
        }
        while (done < total) {
            size_t b = (size_t) (total - done) < block ? (size_t) (total - done) : block;
            size_t left = b % (8U * elem), nb = b - left;
            if (nb) {
                size_t cs;
                int got;
                if (chunk_size - pos < 4) {
                    ok = 0;
                    break;
                }
                cs = (size_t) read_be(chunk + pos, 4);
                pos += 4;
                if (chunk_size - pos < cs) {
                    ok = 0;
                    break;
                }
                got = LZ4_decompress_safe((const char *) chunk + pos, tmp, (int) cs, (int) nb);
                if (got != (int) nb) {
                    fprintf(stderr, "FAIL %s: LZ4_decompress_safe got %d != %lu\n", name, got,
                            (unsigned long) nb);
                    ok = 0;
                    break;
                }
                pos += cs;
                bitshuf_decode_block((char *) out + done, tmp, scratch, nb / elem, elem);
            }
            if (left) {
                if (chunk_size - pos < left) {
                    ok = 0;
                    break;
                }
                memcpy(out + done + nb, chunk + pos, left);
                pos += left;
            }
            done += b;
        }
        if (ok && pos != chunk_size) {
            fprintf(stderr, "FAIL %s: pos %lu != chunk_size %lu\n", name, (unsigned long) pos,
                    (unsigned long) chunk_size);
            ok = 0;
        }
        free(tmp);
        free(scratch);
    }
#endif

    if (ok && memcmp(out, raw, raw_size) != 0) {
        fprintf(stderr, "FAIL %s: decoded bytes differ from raw\n", name);
        ok = 0;
    }
    free(out);
    return ok;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build-compat/compat_corpus.bin";
    const char *label = argc > 2 ? argv[2] : "compat";
    FILE *f;
    char magic[COMPAT_MAGIC_LEN];
    uint32_t count = 0, i, checked = 0, failures = 0;

    f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    if (fread(magic, 1, COMPAT_MAGIC_LEN, f) != COMPAT_MAGIC_LEN ||
        memcmp(magic, COMPAT_MAGIC, COMPAT_MAGIC_LEN) != 0 || !compat_read_u32(f, &count)) {
        fprintf(stderr, "invalid corpus header in %s\n", path);
        fclose(f);
        return 1;
    }

    for (i = 0; i < count; i++) {
        uint32_t name_len, codec, elem, block, raw_size, chunk_size;
        char name[256];
        unsigned char *raw, *chunk;
        if (!compat_read_u32(f, &name_len) || !compat_read_u32(f, &codec) ||
            !compat_read_u32(f, &elem) || !compat_read_u32(f, &block) ||
            !compat_read_u32(f, &raw_size) || !compat_read_u32(f, &chunk_size) ||
            name_len >= sizeof(name)) {
            fprintf(stderr, "corrupt record header at %u\n", i);
            fclose(f);
            return 1;
        }
        if (name_len && fread(name, 1, name_len, f) != name_len) {
            fclose(f);
            return 1;
        }
        name[name_len] = '\0';
        raw = (unsigned char *) malloc(raw_size ? raw_size : 1);
        chunk = (unsigned char *) malloc(chunk_size ? chunk_size : 1);
        if (!raw || !chunk) {
            free(raw);
            free(chunk);
            fclose(f);
            return 1;
        }
        if ((raw_size && fread(raw, 1, raw_size, f) != raw_size) ||
            (chunk_size && fread(chunk, 1, chunk_size, f) != chunk_size)) {
            free(raw);
            free(chunk);
            fclose(f);
            return 1;
        }
#if defined(COMPAT_USE_LZ4_ONLY)
        if (codec == PAC_LZ4)
            checked++;
#else
        checked++;
#endif
        if (!verify_case(name, codec, elem, block, raw, raw_size, chunk, chunk_size))
            failures++;
        free(raw);
        free(chunk);
    }

    fclose(f);
    printf("%s: %u cases checked, %u failures\n", label, checked, failures);
    return failures ? 1 : 0;
}
