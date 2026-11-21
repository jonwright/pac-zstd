/*
 * Shared helpers for the pac-zstd fuzz targets and standalone replay/seed tools.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#ifndef PAC_FUZZ_COMMON_H
#define PAC_FUZZ_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lz4.h"
#include "pac.h"
#include "zstd.h"

extern int bitshuf_encode_block(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);
extern int bitshuf_decode_block(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);

static uint64_t fuzz_be(const unsigned char *p, int n)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static void fuzz_put_be(unsigned char *p, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char) (v >> (8 * (n - 1 - i)));
}

static void fuzz_fail(const char *msg)
{
    fprintf(stderr, "FUZZ ORACLE FAILURE: %s\n", msg);
    abort();
}

/* Reference decoder of a bitshuffle HDF5 chunk; returns decoded bytes or -1. */
static long fuzz_ref_decode(int codec, const unsigned char *c, size_t csize, size_t elem,
                            unsigned char *out, size_t cap)
{
    uint64_t total, done = 0;
    size_t block, pos = 12;
    char *tmp, *scratch;

    if (csize < 12)
        return -1;
    if (elem != 1 && elem != 2 && elem != 4 && elem != 8)
        return -1;
    if (codec != PAC_LZ4 && codec != PAC_ZSTD)
        return -1;

    total = fuzz_be(c, 8);
    block = (size_t) fuzz_be(c + 8, 4);
    if (!block) /* filter default: 8192 bytes rounded down to multiple of 8 elements */
        block = (8192 / elem / 8) * 8 * elem;
    if (!block || block > ((size_t) 1 << 27) || block % (8 * elem) || total % elem || total > cap)
        return -1;
    if (total == 0)
        return csize == 12 ? 0 : -1;

    tmp = (char *) malloc(block + 64);
    scratch = (char *) malloc(block + 64);
    if (!tmp || !scratch) {
        free(tmp);
        free(scratch);
        return -1;
    }

    while (done < total) {
        size_t b = total - done < block ? (size_t) (total - done) : block;
        size_t left = b % (8 * elem), nb = b - left;
        if (nb) {
            size_t cs, got;
            if (csize - pos < 4)
                goto bad;
            cs = (size_t) fuzz_be(c + pos, 4);
            pos += 4;
            if (csize - pos < cs)
                goto bad;
            if (codec == PAC_LZ4) {
                int r;
                if (cs > 0x7fffffffUL || nb > 0x7fffffffUL)
                    goto bad;
                r = LZ4_decompress_safe((const char *) c + pos, tmp, (int) cs, (int) nb);
                got = r < 0 ? 0 : (size_t) r;
            } else {
                got = ZSTD_decompress(tmp, nb, c + pos, cs);
            }
            if (got != nb)
                goto bad;
            pos += cs;
            bitshuf_decode_block((char *) out + done, tmp, scratch, nb / elem, elem);
        }
        if (left) {
            if (csize - pos < left)
                goto bad;
            memcpy(out + done + nb, c + pos, left);
            pos += left;
        }
        done += b;
    }

    free(tmp);
    free(scratch);
    return pos == csize ? (long) total : -1;

bad:
    free(tmp);
    free(scratch);
    return -1;
}

/* Reference stock bitshuffle + LZ4/Zstd chunk encoder for generating fuzz seeds. */
static size_t fuzz_stock_encode(int codec, unsigned char *dst, size_t dst_cap,
                                const unsigned char *src, size_t size, size_t elem, size_t block)
{
    size_t n = 12, done = 0;
    char *tmp, *scratch;

    if (dst_cap < 12 || (elem != 1 && elem != 2 && elem != 4 && elem != 8))
        return 0;
    if (!block || block % (8 * elem) || size % elem)
        return 0;
    if (codec != PAC_LZ4 && codec != PAC_ZSTD)
        return 0;

    fuzz_put_be(dst, (uint64_t) size, 8);
    fuzz_put_be(dst + 8, (uint64_t) block, 4);
    if (size == 0)
        return 12;

    tmp = (char *) malloc(block + 64);
    scratch = (char *) malloc(block + 64);
    if (!tmp || !scratch) {
        free(tmp);
        free(scratch);
        return 0;
    }

    while (done < size) {
        size_t b = size - done < block ? size - done : block;
        size_t left = b % (8 * elem), nb = b - left, c = 0;
        if (nb) {
            size_t cbound;
            if (codec == PAC_LZ4) {
                if (nb > 0x7fffffffUL)
                    goto bad;
                cbound = (size_t) LZ4_compressBound((int) nb);
            } else {
                cbound = ZSTD_compressBound(nb);
            }
            if (dst_cap - n < 4 || dst_cap - n - 4 < cbound)
                goto bad;
            bitshuf_encode_block(tmp, (const char *) src + done, scratch, nb / elem, elem);
            if (codec == PAC_LZ4) {
                int r = LZ4_compress_default(tmp, (char *) dst + n + 4, (int) nb,
                                             (int) (dst_cap - n - 4));
                c = r > 0 ? (size_t) r : 0;
            } else {
                size_t r = ZSTD_compress(dst + n + 4, dst_cap - n - 4, tmp, nb, 1);
                c = ZSTD_isError(r) ? 0 : r;
            }
            if (!c)
                goto bad;
            fuzz_put_be(dst + n, (uint64_t) c, 4);
            n += 4 + c;
        }
        if (left) {
            if (dst_cap - n < left)
                goto bad;
            memcpy(dst + n, src + done + nb, left);
            n += left;
        }
        done += b;
    }

    free(tmp);
    free(scratch);
    return n;

bad:
    free(tmp);
    free(scratch);
    return 0;
}

/* Shared block-size selector encoded in control bytes b2..b3. */
static size_t fuzz_pick_block(unsigned char b2, unsigned char b3, size_t elem, int allow_invalid)
{
    if ((b2 & 0x80) == 0) {
        size_t mult = 8 * elem;
        size_t r512 = ((512 + mult - 1) / mult) * mult;
        switch (b2 & 0x0f) {
        case 0:  return 8 * elem;
        case 1:  return 16 * elem;
        case 2:  return 64 * elem;
        case 3:  return r512;
        case 4:  return 1024;
        case 5:  return 4096;
        case 6:  return 8192;
        case 7:  return 16384;
        case 8:  return 32768;
        case 9:  return PAC_LZ4_BLOCK;  /* 57344 */
        case 10: return 65536;
        case 11: return 131072;
        case 12: return PAC_ZSTD_BLOCK; /* 253952 */
        case 13: return 524288;         /* hits LZ4 with plane > 65535 when elem == 1 */
        case 14: return allow_invalid ? 0 : 8192;
        default: return allow_invalid ? (8 * elem + 1) : (24 * elem);
        }
    } else {
        size_t mult = 8 * elem;
        size_t max_units = 262144 / mult;
        size_t raw = (size_t) (b2 & 0x7f) | ((size_t) b3 << 7);
        return (raw % max_units + 1) * mult;
    }
}

#ifdef PAC_FUZZ_STANDALONE
#include <dirent.h>
#include <sys/stat.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int fuzz_run_file(const char *path, size_t *count)
{
    FILE *f = fopen(path, "rb");
    long lsz;
    size_t sz;
    uint8_t *buf;

    if (!f) {
        perror(path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (lsz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path);
        fclose(f);
        return -1;
    }
    sz = (size_t) lsz;
    buf = (uint8_t *) malloc(sz ? sz : 1);
    if (!buf) {
        fprintf(stderr, "out of memory reading %s\n", path);
        fclose(f);
        return -1;
    }
    if (sz && fread(buf, 1, sz, f) != sz) {
        fprintf(stderr, "short read on %s\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    LLVMFuzzerTestOneInput(buf, sz);
    free(buf);
    (*count)++;
    return 0;
}

static int fuzz_run_path(const char *path, size_t *count)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        perror(path);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        int rc = 0;
        if (!d) {
            perror(path);
            return -1;
        }
        while ((de = readdir(d)) != NULL) {
            char child[4096];
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if ((size_t) snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >= sizeof(child)) {
                fprintf(stderr, "path too long: %s/%s\n", path, de->d_name);
                rc = -1;
                break;
            }
            if (fuzz_run_path(child, count) != 0) {
                rc = -1;
                break;
            }
        }
        closedir(d);
        return rc;
    }
    if (S_ISREG(st.st_mode))
        return fuzz_run_file(path, count);
    return 0;
}

int main(int argc, char **argv)
{
    size_t count = 0;
    int i;

    if (argc < 2) {
        size_t cap = 4096, len = 0;
        uint8_t *tmp = (uint8_t *) malloc(cap), *exact;
        size_t nread;
        if (!tmp)
            return 1;
        while ((nread = fread(tmp + len, 1, cap - len, stdin)) > 0) {
            len += nread;
            if (len == cap) {
                uint8_t *nb;
                cap *= 2;
                nb = (uint8_t *) realloc(tmp, cap);
                if (!nb) {
                    free(tmp);
                    return 1;
                }
                tmp = nb;
            }
        }
        exact = (uint8_t *) malloc(len ? len : 1);
        if (!exact) {
            free(tmp);
            return 1;
        }
        if (len)
            memcpy(exact, tmp, len);
        free(tmp);
        LLVMFuzzerTestOneInput(exact, len);
        free(exact);
        count = 1;
        printf("replayed %lu inputs\n", (unsigned long) count);
        return 0;
    }

    for (i = 1; i < argc; i++) {
        if (fuzz_run_path(argv[i], &count) != 0)
            return 1;
    }
    printf("replayed %lu inputs\n", (unsigned long) count);
    return 0;
}
#endif /* PAC_FUZZ_STANDALONE */

#endif /* PAC_FUZZ_COMMON_H */
