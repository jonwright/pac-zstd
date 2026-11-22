/*
 * Deterministic corpus seed generator for fuzz_repack and fuzz_compress.
 *
 * Usage: fuzz_seed <repack_dir> <compress_dir>
 * Writes seed_00, seed_01, ... only if they do not already exist.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#include "fuzz_common.h"

static uint64_t seed_rng_state = 20261121ULL;
static uint64_t seed_rng(void)
{
    seed_rng_state ^= seed_rng_state << 13;
    seed_rng_state ^= seed_rng_state >> 7;
    seed_rng_state ^= seed_rng_state << 17;
    return seed_rng_state;
}

static void fill_pattern(unsigned char *buf, size_t size, size_t elem, int pattern)
{
    size_t n = size / elem, i;
    const uint64_t maxv = elem == 8 ? ~(uint64_t) 0 : (((uint64_t) 1 << (8 * elem)) - 1);
    for (i = 0; i < n; i++) {
        uint64_t v = 0, r = seed_rng();
        switch (pattern) {
        case 0: v = 0; break;
        case 1: v = maxv; break;
        case 2: v = (r & 63) == 0 ? (1 + ((r >> 8) & 1)) : 0; break; /* sparse photons */
        case 3: v = ((r & 63) + ((r >> 6) & 63) + ((r >> 12) & 63)) & maxv; break; /* dense */
        case 4: v = (i / 3) & maxv; break; /* ramp */
        case 5: v = r & 0xff & maxv; break; /* low byte only */
        case 6: v = r & 0xffff & maxv; break; /* low 2 bytes only */
        default: v = r & maxv; break; /* random noise */
        }
        memcpy(buf + i * elem, &v, elem);
    }
}

static void write_seed_if_missing(const char *dir, int idx, const unsigned char *hdr,
                                  const unsigned char *body, size_t body_len)
{
    char path[4096];
    FILE *f;

    if ((size_t) snprintf(path, sizeof(path), "%s/seed_%02d", dir, idx) >= sizeof(path)) {
        fprintf(stderr, "seed path too long\n");
        exit(1);
    }
    f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return;
    }
    f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    if (fwrite(hdr, 1, 6, f) != 6 || (body_len && fwrite(body, 1, body_len, f) != body_len)) {
        perror(path);
        fclose(f);
        exit(1);
    }
    fclose(f);
}

static int elem_to_idx(size_t elem)
{
    return elem == 1 ? 0 : (elem == 2 ? 1 : (elem == 4 ? 2 : 3));
}

struct repack_spec {
    int src_codec;
    int dst_codec;
    size_t elem;
    int isa;
    int cap_mode;
    unsigned char b2;
    unsigned char b3;
    int zero_hdr_block;
    size_t sblock;
    size_t pixels_bytes;
    int pattern;
};

struct compress_spec {
    int codec;
    size_t elem;
    int narrow;
    int isa;
    int tight_cap;
    unsigned char b2;
    unsigned char b3;
    unsigned char cap_lo;
    unsigned char cap_hi;
    size_t pixels_bytes;
    int pattern;
};

static size_t spec_sblock(size_t elem, size_t mult_units)
{
    return mult_units * 8 * elem;
}

static void generate_repack_seeds(const char *dir)
{
    static const struct repack_spec specs[] = {
        /* 0..3: elem 1,2,4,8 basic roundtrips */
        {PAC_LZ4,  PAC_ZSTD, 1, 0, 0,  0, 0, 0,   8,    32, 0},
        {PAC_ZSTD, PAC_LZ4,  2, 1, 0,  1, 0, 0,  16,    64, 2},
        {PAC_LZ4,  PAC_ZSTD, 4, 2, 0,  2, 0, 0,  64,   256, 3},
        {PAC_ZSTD, PAC_ZSTD, 8, 2, 0,  2, 0, 0, 128,   512, 4},
        /* 4..6: empty, tail-only (< 8*elem), and block + tail */
        {PAC_LZ4,  PAC_ZSTD, 2, 2, 0,  0, 0, 0,  16,     0, 0},
        {PAC_ZSTD, PAC_LZ4,  4, 0, 0,  0, 0, 0,  32,    12, 1},
        {PAC_LZ4,  PAC_ZSTD, 2, 2, 0,  1, 0, 0,  16,    38, 4},
        /* 7..10: multi-block regrouping sblock -> larger block with tail */
        {PAC_LZ4,  PAC_ZSTD, 2, 2, 0,  6, 0, 0,  64,  1040, 2},
        {PAC_LZ4,  PAC_ZSTD, 2, 2, 0,  6, 0, 1, 8192, 16400, 3}, /* detector sblock=0 default! */
        {PAC_ZSTD, PAC_ZSTD, 4, 1, 0,  4, 0, 0, 256,  2100, 5},
        {PAC_ZSTD, PAC_LZ4,  1, 2, 0,  5, 0, 0, 512,  8200, 4},
        /* 11..15: large dense plane (>= 1024 bytes) & zstd block splitting */
        {PAC_LZ4,  PAC_ZSTD, 1, 2, 0,  6, 0, 1, 8192, 16384, 3},
        {PAC_LZ4,  PAC_ZSTD, 1, 2, 0,  6, 0, 1, 8192, 16384, 7},
        {PAC_LZ4,  PAC_ZSTD, 1, 2, 0,  6, 0, 1, 8192, 16384, 3},
        {PAC_ZSTD, PAC_LZ4,  2, 0, 0,  9, 0, 0, 8192, 57344, 6},
        {PAC_LZ4,  PAC_ZSTD, 4, 2, 0,  9, 0, 1, 8192, 57356, 2},
        /* 16..20: cap_mode=1 (tight capacity) and arbitrary b2|0x80 block encodings */
        {PAC_LZ4,  PAC_ZSTD, 2, 2, 1,  0, 0, 0,  16,    64, 3},
        {PAC_LZ4,  PAC_LZ4,  1, 1, 0, 0x83, 0, 0, 32,   135, 7},
        {PAC_ZSTD, PAC_ZSTD, 8, 2, 0,  3, 0, 0, 128,  1032, 6},
        {PAC_LZ4,  PAC_ZSTD, 2, 0, 0,  4, 0, 0, 128,  2048, 1},
        {PAC_ZSTD, PAC_LZ4,  4, 1, 0,  5, 0, 0, 512,  8192, 5},
        /* 21..25: extra patterns & non-power-of-two multiples */
        {PAC_LZ4,  PAC_ZSTD, 2, 2, 0, 0x82, 0, 0, 16,   500, 2},
        {PAC_LZ4,  PAC_LZ4,  2, 0, 0,  0, 0, 0,  16,    16, 1},
        {PAC_ZSTD, PAC_ZSTD, 1, 2, 0,  4, 0, 0,  64,  2051, 3},
        {PAC_LZ4,  PAC_ZSTD, 8, 2, 1,  2, 0, 0,  64,   520, 4},
        {PAC_LZ4,  PAC_ZSTD, 4, 2, 0,  6, 0, 1, 8192, 8192, 6}
    };
    size_t i, nspecs = sizeof(specs) / sizeof(specs[0]);

    for (i = 0; i < nspecs; i++) {
        const struct repack_spec *sp = &specs[i];
        size_t sblock = sp->sblock ? sp->sblock : spec_sblock(sp->elem, 1);
        size_t px_sz = sp->pixels_bytes - (sp->pixels_bytes % sp->elem);
        size_t stock_cap = 12 + px_sz +
                           (px_sz / sblock + 2) *
                               (ZSTD_compressBound(sblock) + (size_t) LZ4_compressBound((int) sblock) + 16);
        unsigned char *px = (unsigned char *) malloc(px_sz ? px_sz : 1);
        unsigned char *stock = (unsigned char *) malloc(stock_cap);
        unsigned char hdr[6];
        size_t ss;

        if (!px || !stock) {
            fprintf(stderr, "oom generating repack seed\n");
            exit(1);
        }
        if (px_sz)
            fill_pattern(px, px_sz, sp->elem, sp->pattern);
        ss = fuzz_stock_encode(sp->src_codec, stock, stock_cap, px, px_sz, sp->elem, sblock);
        if (!ss) {
            fprintf(stderr, "fuzz_stock_encode failed on repack seed %lu\n", (unsigned long) i);
            exit(1);
        }
        if (sp->zero_hdr_block && sblock == 8192)
            fuzz_put_be(stock + 8, 0, 4);

        hdr[0] = (unsigned char) (((sp->src_codec == PAC_ZSTD ? 1 : 0)) |
                                  ((sp->dst_codec == PAC_ZSTD ? 1 : 0) << 1) |
                                  ((elem_to_idx(sp->elem) & 3) << 2) |
                                  ((sp->isa & 7) << 4) |
                                  ((sp->cap_mode & 1) << 7));
        hdr[1] = 0;
        hdr[2] = sp->b2;
        hdr[3] = sp->b3;
        hdr[4] = sp->cap_mode ? 24 : 0;
        hdr[5] = 0;

        write_seed_if_missing(dir, (int) i, hdr, stock, ss);
        free(px);
        free(stock);
    }
}

static void generate_compress_seeds(const char *dir)
{
    static const struct compress_spec specs[] = {
        /* 0..3: elem 1,2,4,8 basic coverage */
        {PAC_ZSTD, 1, 0, 0, 0,  0, 0, 0, 0,    32, 0},
        {PAC_LZ4,  2, 1, 1, 0,  1, 0, 0, 0,    64, 2},
        {PAC_ZSTD, 4, 1, 2, 0,  2, 0, 0, 0,   256, 3},
        {PAC_ZSTD, 8, 1, 2, 0,  2, 0, 0, 0,   512, 4},
        /* 4..6: empty, tail-only, and block + tail */
        {PAC_ZSTD, 2, 1, 2, 0,  0, 0, 0, 0,     0, 0},
        {PAC_LZ4,  4, 0, 0, 0,  0, 0, 0, 0,    12, 1},
        {PAC_ZSTD, 2, 1, 2, 0,  1, 0, 0, 0,    38, 4},
        /* 7..11: narrow SIMD pac_pack paths (elem=2 keep=1, elem=4 keep=1, elem=4 keep=2, elem=8) */
        {PAC_ZSTD, 2, 1, 2, 0,  4, 0, 0, 0,  1024, 5},
        {PAC_ZSTD, 4, 1, 2, 0,  4, 0, 0, 0,  1024, 5},
        {PAC_ZSTD, 4, 1, 2, 0,  5, 0, 0, 0,  4096, 6},
        {PAC_ZSTD, 8, 1, 1, 0,  5, 0, 0, 0,  4096, 6},
        {PAC_ZSTD, 8, 1, 2, 0,  5, 0, 0, 0,  4096, 5},
        /* 12..15: dense >= 1024-byte plane & tight_cap check */
        {PAC_ZSTD, 1, 0, 2, 0,  6, 0, 0, 0, 16384, 3},
        {PAC_ZSTD, 1, 0, 2, 1,  6, 0, 7, 0, 16384, 7},
        {PAC_ZSTD, 1, 0, 2, 0,  6, 0, 0, 0, 16384, 3},
        {PAC_LZ4,  2, 1, 2, 1,  6, 0, 5, 0,  8202, 2},
        /* 16..18: invalid block / unaligned tail rejection tests (tight_cap=1) */
        {PAC_ZSTD, 2, 1, 0, 1, 14, 0, 0, 0,    64, 0},       /* block = 0 */
        {PAC_LZ4,  4, 0, 2, 1, 15, 0, 0, 0,    64, 4},       /* block = 8*elem+1 */
        {PAC_ZSTD, 4, 1, 2, 1,  0, 0, 0, 0xff, 63, 7},      /* unaligned size */
        /* 19..23: large planes (idx 12/13) incl. LZ4 plane > 65535 & arbitrary b2|0x80 */
        {PAC_LZ4,  1, 0, 2, 0, 13, 0, 0, 0, 524288, 2},      /* 512k LZ4, plane=65536 > 65535 */
        {PAC_ZSTD, 2, 1, 2, 0,  9, 0, 0, 0,  57350, 3},
        {PAC_ZSTD, 4, 0, 1, 0, 0x85, 0, 0, 0, 1200, 7},
        {PAC_LZ4,  8, 1, 0, 0,  4, 0, 0, 0,  1024, 1},
        {PAC_ZSTD, 2, 1, 2, 0,  3, 0, 0, 0,   522, 4}
    };
    size_t i, nspecs = sizeof(specs) / sizeof(specs[0]);

    for (i = 0; i < nspecs; i++) {
        const struct compress_spec *sp = &specs[i];
        size_t px_sz = sp->pixels_bytes;
        unsigned char *px = (unsigned char *) malloc(px_sz ? px_sz : 1);
        unsigned char hdr[6];

        if (!px) {
            fprintf(stderr, "oom generating compress seed\n");
            exit(1);
        }
        if (px_sz)
            fill_pattern(px, px_sz, sp->elem, sp->pattern);

        hdr[0] = (unsigned char) (((sp->codec == PAC_ZSTD ? 1 : 0)) |
                                  ((elem_to_idx(sp->elem) & 3) << 1) |
                                  ((sp->narrow & 1) << 3) |
                                  ((sp->isa & 7) << 4) |
                                  ((sp->tight_cap & 1) << 7));
        hdr[1] = 0;
        hdr[2] = sp->b2;
        hdr[3] = sp->b3;
        hdr[4] = sp->cap_lo;
        hdr[5] = sp->cap_hi;

        write_seed_if_missing(dir, (int) i, hdr, px, px_sz);
        free(px);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <repack_dir> <compress_dir>\n", argv[0]);
        return 1;
    }
    generate_repack_seeds(argv[1]);
    generate_compress_seeds(argv[2]);
    return 0;
}
