/*
 * Tests of pac-zstd: every chunk it writes is decoded with the
 * reference pieces (LZ4_decompress_safe or ZSTD_decompress, then kcb's bit
 * untranspose) and compared with the pixels.
 *
 *     test_pac [seed [rounds [quick]]]     quick: small inputs only (for slow machines)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.  Code written by Claude Fable 5.2.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lz4.h"
#include "pac.h"
#include "zstd.h"

extern int bitshuf_encode_block(char *out, const char *in, char *scratch, size_t size, size_t elem_size);
extern int bitshuf_decode_block(char *out, const char *in, char *scratch, size_t size, size_t elem_size);

static uint64_t rng_state = 1;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static uint64_t be(const unsigned char *p, int n)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static void put_be(unsigned char *p, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char) (v >> (8 * (n - 1 - i)));
}

/* the reference decoder of a bitshuffle HDF5 chunk; returns the decoded size or -1 */
static long ref_decode(int codec, const unsigned char *c, size_t csize, size_t elem, unsigned char *out, size_t cap)
{
    uint64_t total, done = 0;
    size_t block, pos = 12;
    char *tmp, *scratch;
    if (csize < 12)
        return -1;
    total = be(c, 8);
    block = (size_t) be(c + 8, 4);
    if (!block) /* the filter's default */
        block = (8192 / elem / 8) * 8 * elem;
    if (total > cap || block % (8 * elem))
        return -1;
    tmp = malloc(block + 64);
    scratch = malloc(block + 64);
    while (done < total) {
        size_t b = total - done < block ? (size_t) (total - done) : block;
        size_t left = b % (8 * elem), nb = b - left;
        if (nb) {
            size_t cs, got;
            if (csize - pos < 4)
                goto bad;
            cs = (size_t) be(c + pos, 4);
            pos += 4;
            if (csize - pos < cs)
                goto bad;
            if (codec == PAC_LZ4) {
                int r = LZ4_decompress_safe((const char *) c + pos, tmp, (int) cs, (int) nb);
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

/* the stock encoders, to make the input of pac_repack_chunk */
static size_t stock_encode(int codec, unsigned char *dst, const unsigned char *src, size_t size, size_t elem, size_t block)
{
    size_t n = 12, done = 0;
    char *tmp = malloc(block + 64), *scratch = malloc(block + 64);
    put_be(dst, size, 8);
    put_be(dst + 8, block, 4);
    while (done < size) {
        size_t b = size - done < block ? size - done : block;
        size_t left = b % (8 * elem), nb = b - left, c;
        if (nb) {
            bitshuf_encode_block(tmp, (const char *) src + done, scratch, nb / elem, elem);
            if (codec == PAC_LZ4)
                c = (size_t) LZ4_compress_default(tmp, (char *) dst + n + 4, (int) nb, LZ4_compressBound((int) nb));
            else
                c = ZSTD_compress(dst + n + 4, ZSTD_compressBound(nb), tmp, nb, 1);
            put_be(dst + n, c, 4);
            n += 4 + c;
        }
        memcpy(dst + n, src + done + nb, left);
        n += left;
        done += b;
    }
    free(tmp);
    free(scratch);
    return n;
}

#define NPATTERNS 10
static const char *const pattern_names[NPATTERNS] = {
    "zeros", "all ones", "noise", "sparse photons", "dense counts", "sparse + saturated gaps",
    "dense + saturated gaps", "ramp", "low bytes only", "poisson int_max/2"};

static void put_elem(unsigned char *p, uint64_t v, size_t elem) { memcpy(p, &v, elem); } /* little endian */

static void fill(unsigned char *buf, size_t n, size_t elem, int pattern)
{
    const uint64_t maxv = elem == 8 ? ~(uint64_t) 0 : (((uint64_t) 1 << (8 * elem)) - 1);
    const double lam = (double) (maxv >> 2); /* ~(signed_max / 2), hard-to-compress counts */
    const double sig = sqrt(lam);
    double z_spare = 0.0;
    int has_spare = 0;
    size_t i, row = 97 + (size_t) (rng() % 300);
    for (i = 0; i < n; i++) {
        uint64_t v = 0, r = rng();
        int gap = (i % row) > row - 9 || (i / row) % 37 == 5;
        switch (pattern) {
        case 0: v = 0; break;
        case 1: v = maxv; break;
        case 2: v = r & maxv; break;
        case 3: v = (r & 127) == 0 ? 1 + ((r >> 8) & 1) : ((r & 0xffff00) == 0 ? (r >> 24) & 0xfff & maxv : 0); break;
        case 4: v = ((r & 63) + ((r >> 6) & 63) + ((r >> 12) & 63)) & maxv; break;
        case 5: v = gap ? maxv : ((r & 63) == 0 ? 1 : 0); break;
        case 6: v = gap ? maxv : (((r & 31) + ((r >> 5) & 31)) & maxv); break;
        case 7: v = (i / 3) & maxv; break;
        case 8: v = r & 0xff & maxv; break;
        default: {
            double z, dv;
            if (has_spare) {
                z = z_spare;
                has_spare = 0;
            } else {
                double u1 = ((double) (r & 0xffffffffUL) + 1.0) / 4294967297.0;
                double u2 = ((double) ((r >> 32) & 0xffffffffUL) + 1.0) / 4294967297.0;
                double mag = sqrt(-2.0 * log(u1));
                double ang = 6.28318530717958647692 * u2;
                z = mag * cos(ang);
                z_spare = mag * sin(ang);
                has_spare = 1;
            }
            dv = lam + sig * z + 0.5;
            if (dv <= 0.0)
                v = 0;
            else if (elem < 8 && dv >= (double) maxv)
                v = maxv;
            else
                v = (uint64_t) dv;
            break;
        }
        }
        put_elem(buf + i * elem, v, elem);
    }
}

static long checks = 0, failures = 0;

static void fail(const char *what, int codec, size_t elem, int pattern, size_t n, size_t block, const char *isa)
{
    failures++;
    if (failures < 20)
        printf("FAIL %s: codec %s elem %lu pattern '%s' elements %lu block %lu isa %s\n", what,
               codec == PAC_LZ4 ? "lz4" : "zstd", (unsigned long) elem, pattern_names[pattern],
               (unsigned long) n, (unsigned long) block, isa);
}

/* one buffer through pac_compress_chunk and pac_repack_chunk, all settings */
static void one_case(pac_encoder *enc[2], size_t elem, int pattern, size_t n, size_t block)
{
    const size_t size = n * elem;
    unsigned char *src = malloc(size + 1), *out = malloc(size + 1);
    size_t cap = pac_bound(size, elem, block), sblock;
    unsigned char *chunk = malloc(cap + 64), *stock, *re;
    int ci, narrow, raw;
    fill(src, n, elem, pattern);
    for (ci = 0; ci < 2; ci++) {
        const int codec = ci ? PAC_ZSTD : PAC_LZ4;
        for (narrow = 0; narrow < 2; narrow++) {
            for (raw = 0; raw < (ci ? 3 : 1); raw++) {
                size_t c;
                pac_set_narrow(enc[ci], narrow);
                pac_set_raw_literals(enc[ci], raw == 0 ? 10 : (raw == 1 ? 0 : 100));
                memset(chunk + cap, 0xA5, 64);
                c = pac_compress_chunk(enc[ci], chunk, cap, src, size, elem, block);
                checks++;
                if (!c || c > cap || ref_decode(codec, chunk, c, elem, out, size) != (long) size ||
                    memcmp(out, src, size) != 0)
                    fail("compress", codec, elem, pattern, n, block, pac_isa(enc[ci]));
                if (chunk[cap] != 0xA5 || chunk[cap + 63] != 0xA5)
                    fail("wrote past dst", codec, elem, pattern, n, block, pac_isa(enc[ci]));
                if (pac_compress_chunk(enc[ci], chunk, 11, src, size, elem, block) != 0)
                    fail("accepted a dst of 11 bytes", codec, elem, pattern, n, block, pac_isa(enc[ci]));
            }
        }
        pac_set_narrow(enc[ci], 1);
        pac_set_raw_literals(enc[ci], 0); /* the default */
    }
    /* repack: stock chunks with blocks of block / m for m in 1, 2, 31 (when that divides) */
    {
        static const size_t ms[3] = {1, 2, 31};
        int mi, sc;
        for (mi = 0; mi < 3; mi++) {
            if (block % ms[mi] || (block / ms[mi]) % (8 * elem))
                continue;
            sblock = block / ms[mi];
            stock = malloc(12 + size + (size / sblock + 2) * (ZSTD_compressBound(sblock) + 4 + (size_t) LZ4_compressBound((int) sblock)) + 64);
            re = malloc(cap + 64);
            for (sc = 0; sc < 2; sc++) {
                const int scodec = sc ? PAC_ZSTD : PAC_LZ4;
                size_t ss = stock_encode(scodec, stock, src, size, elem, sblock);
                if (sblock == 8192) /* as some detector software writes it: block size 0 = the default */
                    put_be(stock + 8, 0, 4);
                for (ci = 0; ci < 2; ci++) {
                    const int codec = ci ? PAC_ZSTD : PAC_LZ4;
                    size_t c = pac_repack_chunk(enc[ci], re, cap, stock, ss, scodec, elem, block);
                    checks++;
                    if (!c || c > cap || ref_decode(codec, re, c, elem, out, size) != (long) size ||
                        memcmp(out, src, size) != 0)
                        fail("repack", codec, elem, pattern, n, block, pac_isa(enc[ci]));
                    /* damaged input: any answer but a crash or an overrun (run under the sanitizers) */
                    if (ss > 12) {
                        size_t at = 8 + (size_t) (rng() % (ss - 8));
                        unsigned char keep = stock[at];
                        stock[at] ^= (unsigned char) (1 + (rng() & 0x7f));
                        (void) pac_repack_chunk(enc[ci], re, cap, stock, ss, scodec, elem, block);
                        stock[at] = keep;
                        (void) pac_repack_chunk(enc[ci], re, cap, stock, ss - 1 - (size_t) (rng() % (ss - 12)), scodec, elem, block);
                        checks += 2;
                    }
                }
            }
            free(stock);
            free(re);
        }
    }
    free(src);
    free(out);
    free(chunk);
}

int main(int argc, char **argv)
{
    static const size_t elems[4] = {1, 2, 4, 8};
    static const size_t counts[] = {0, 1, 7, 8, 9, 64, 65, 1000, 4099, 31744, 100003, 300007};
    static const size_t blocks[] = {8, 64, 248, 8192, 65536, PAC_LZ4_BLOCK, PAC_ZSTD_BLOCK, 1048576};
    const unsigned long seed = argc > 1 ? strtoul(argv[1], NULL, 0) : 20261116UL;
    const int rounds = argc > 2 ? atoi(argv[2]) : 300;
    const int quick = argc > 3;
    int isa, e, p, r, tested = 0;
    size_t ci, bi;
    rng_state = seed ? seed : 1;
    for (isa = 0; isa < 6; isa++) {
        pac_encoder *enc[2];
        long before = checks;
        enc[0] = pac_create(PAC_LZ4);
        enc[1] = pac_create(PAC_ZSTD);
        if (!enc[0] || !enc[1]) {
            printf("pac_create failed\n");
            return 1;
        }
        if (!pac_force_isa(enc[0], isa) || !pac_force_isa(enc[1], isa)) {
            static const char *const names[6] = {"generic", "sse2", "avx2", "avx512bw", "neon", "vsx"};
            printf("%-9s not available here: skipped\n", names[isa]);
            pac_free(enc[0]);
            pac_free(enc[1]);
            continue;
        }
        tested++;
        for (e = 0; e < 4; e++)
            for (p = 0; p < NPATTERNS; p++)
                for (ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++)
                    for (bi = 0; bi < sizeof(blocks) / sizeof(blocks[0]); bi++) {
                        size_t block = blocks[bi] * (blocks[bi] < 8192 ? elems[e] : 1);
                        if (counts[ci] > 100000 && (bi + (size_t) p + (size_t) e) % 3)
                            continue; /* the big ones: a third of the combinations */
                        if (quick && (counts[ci] > 5000 || blocks[bi] > 65536))
                            continue;
                        one_case(enc, elems[e], p, counts[ci], block);
                    }
        for (r = 0; r < rounds; r++) { /* random sizes and blocks */
            size_t elem = elems[rng() & 3];
            size_t n = (size_t) (rng() % (quick ? 20000 : 200000));
            size_t block = (1 + (size_t) (rng() % 3000)) * 8 * elem;
            if (rng() & 1)
                block *= 31;
            one_case(enc, elem, (int) (rng() % NPATTERNS), n, block);
        }
        printf("%-9s %ld checks\n", pac_isa(enc[0]), checks - before);
        pac_free(enc[0]);
        pac_free(enc[1]);
    }
    /* arguments that must be refused */
    {
        pac_encoder *enc = pac_create(PAC_ZSTD);
        unsigned char buf[256], src[64];
        memset(src, 0, sizeof(src));
        checks += 6;
        if (pac_create(7) != NULL) fail("pac_create(7)", PAC_ZSTD, 0, 0, 0, 0, "-");
        if (pac_compress_chunk(enc, buf, 256, src, 64, 3, 24)) fail("elem 3", PAC_ZSTD, 3, 0, 0, 0, "-");
        if (pac_compress_chunk(enc, buf, 256, src, 63, 2, 16)) fail("odd size", PAC_ZSTD, 2, 0, 0, 0, "-");
        if (pac_compress_chunk(enc, buf, 256, src, 64, 2, 24)) fail("block not 8 elements", PAC_ZSTD, 2, 0, 0, 0, "-");
        if (pac_compress_chunk(enc, buf, 256, src, 64, 2, 0)) fail("block 0", PAC_ZSTD, 2, 0, 0, 0, "-");
        if (pac_bound(64, 2, 24)) fail("pac_bound", PAC_ZSTD, 2, 0, 0, 0, "-");
        {
            unsigned char stock[1024];
            size_t ss = stock_encode(PAC_ZSTD, stock, src, 64, 2, 16), tc;
            for (tc = 12; tc <= 34; tc++) {
                unsigned char *tight = malloc(tc);
                checks += 2;
                if (pac_compress_chunk(enc, tight, tc, src, 64, 2, 16) != 0)
                    fail("tight dst compress", PAC_ZSTD, 2, 0, 32, 16, "-");
                if (pac_repack_chunk(enc, tight, tc, stock, ss, PAC_ZSTD, 2, 16) != 0)
                    fail("tight dst repack", PAC_ZSTD, 2, 0, 32, 16, "-");
                free(tight);
            }
        }
        pac_free(enc);
    }
    /* hard-to-compress poisson noise at ~int_max/2: must round-trip cleanly and not regress badly vs stock zstd */
    {
        pac_encoder *enc = pac_create(PAC_ZSTD);
        for (e = 0; e < 4; e++) {
            const size_t elem = elems[e];
            const size_t n = quick ? 32768 / elem : 1048576 / elem;
            const size_t size = n * elem;
            const size_t block = PAC_ZSTD_BLOCK;
            size_t cap = pac_bound(size, elem, block), pc, sc;
            unsigned char *src = malloc(size), *out = malloc(size), *pbuf = malloc(cap), *sbuf = malloc(cap);
            fill(src, n, elem, 9);
            pc = pac_compress_chunk(enc, pbuf, cap, src, size, elem, block);
            sc = stock_encode(PAC_ZSTD, sbuf, src, size, elem, block);
            checks += 2;
            if (!pc || pc > cap || ref_decode(PAC_ZSTD, pbuf, pc, elem, out, size) != (long) size ||
                memcmp(out, src, size) != 0)
                fail("poisson int_max/2 roundtrip", PAC_ZSTD, elem, 9, n, block, pac_isa(enc));
            if (!sc || pc > sc + sc / 100 + 64) {
                printf("FAIL poisson int_max/2 ratio: elem %lu pac %lu vs stock %lu\n",
                       (unsigned long) elem, (unsigned long) pc, (unsigned long) sc);
                fail("poisson int_max/2 ratio", PAC_ZSTD, elem, 9, n, block, pac_isa(enc));
            }
            free(src);
            free(out);
            free(pbuf);
            free(sbuf);
        }
        pac_free(enc);
    }
    printf("%ld checks, %ld failures, %d instruction sets, seed %lu\n", checks, failures, tested, seed);
    return failures ? 1 : 0;
}
