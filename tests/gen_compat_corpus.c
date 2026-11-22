/*
 * Deterministic corpus generator for 2022+ compatibility and RFC 8878 / LZ4
 * block-format verification.
 *
 *   gen_compat_corpus <output_path> [seed]
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat_format.h"
#include "lz4.h"
#include "pac.h"
#include "zstd.h"

extern int bitshuf_encode_block(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);

static uint64_t rng_state = 20261122ULL;
static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void put_be(unsigned char *p, uint64_t v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char) (v >> (8 * (n - 1 - i)));
}

static void put_elem(unsigned char *p, uint64_t v, size_t elem) { memcpy(p, &v, elem); }

#define NPATTERNS 10
static const char *const pattern_names[NPATTERNS] = {
    "zeros",   "ones",       "noise",      "sparse",   "dense",
    "sp_gaps", "dense_gaps", "ramp",       "lowbytes", "poisson"};

static void fill_pattern(unsigned char *buf, size_t n, size_t elem, int pattern)
{
    const uint64_t maxv = elem == 8 ? ~(uint64_t) 0 : (((uint64_t) 1 << (8 * elem)) - 1);
    const double lam = (double) (maxv >> 2);
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
        case 3:
            v = (r & 127) == 0 ? 1 + ((r >> 8) & 1)
                               : ((r & 0xffff00) == 0 ? (r >> 24) & 0xfff & maxv : 0);
            break;
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

static size_t stock_encode(int codec, unsigned char *dst, const unsigned char *src, size_t size,
                           size_t elem, size_t block)
{
    size_t n = 12, done = 0;
    char *tmp = (char *) malloc(block + 64);
    char *scratch = (char *) malloc(block + 64);
    if (!tmp || !scratch) {
        free(tmp);
        free(scratch);
        return 0;
    }
    put_be(dst, size, 8);
    put_be(dst + 8, block, 4);
    while (done < size) {
        size_t b = size - done < block ? size - done : block;
        size_t left = b % (8 * elem), nb = b - left, c = 0;
        if (nb) {
            bitshuf_encode_block(tmp, (const char *) src + done, scratch, nb / elem, elem);
            if (codec == PAC_LZ4)
                c = (size_t) LZ4_compress_default(tmp, (char *) dst + n + 4, (int) nb,
                                                  LZ4_compressBound((int) nb));
            else
                c = ZSTD_compress(dst + n + 4, ZSTD_compressBound(nb), tmp, nb, 1);
            if (!c || (codec == PAC_ZSTD && ZSTD_isError(c))) {
                free(tmp);
                free(scratch);
                return 0;
            }
            put_be(dst + n, c, 4);
            n += 4 + c;
        }
        if (left) {
            memcpy(dst + n, src + done + nb, left);
            n += left;
        }
        done += b;
    }
    free(tmp);
    free(scratch);
    return n;
}

typedef struct {
    FILE *f;
    uint32_t count;
    uint64_t total_bytes;
} corpus_writer;

static void emit_record(corpus_writer *w, const char *name, int codec, size_t elem, size_t block,
                        const unsigned char *raw, size_t raw_size, const unsigned char *chunk,
                        size_t chunk_size)
{
    uint32_t name_len = (uint32_t) strlen(name);
    if (!compat_write_u32(w->f, name_len) || !compat_write_u32(w->f, (uint32_t) codec) ||
        !compat_write_u32(w->f, (uint32_t) elem) || !compat_write_u32(w->f, (uint32_t) block) ||
        !compat_write_u32(w->f, (uint32_t) raw_size) ||
        !compat_write_u32(w->f, (uint32_t) chunk_size) ||
        (name_len && fwrite(name, 1, name_len, w->f) != name_len) ||
        (raw_size && fwrite(raw, 1, raw_size, w->f) != raw_size) ||
        (chunk_size && fwrite(chunk, 1, chunk_size, w->f) != chunk_size)) {
        fprintf(stderr, "write failed on case %s\n", name);
        exit(1);
    }
    w->count++;
    w->total_bytes += 24 + name_len + raw_size + chunk_size;
}

static void emit_compress(corpus_writer *w, pac_encoder *enc, const char *name, int codec,
                          size_t elem, size_t block, int narrow,
                          const unsigned char *raw, size_t raw_size)
{
    size_t cap = pac_bound(raw_size, elem, block);
    unsigned char *chunk = (unsigned char *) malloc(cap ? cap : 1);
    size_t c;
    if (!chunk || !cap) {
        fprintf(stderr, "pac_bound/malloc failed for %s\n", name);
        exit(1);
    }
    pac_set_narrow(enc, narrow);
    c = pac_compress_chunk(enc, chunk, cap, raw, raw_size, elem, block);
    if (!c || c > cap) {
        fprintf(stderr, "pac_compress_chunk failed for %s (c=%lu cap=%lu)\n", name,
                (unsigned long) c, (unsigned long) cap);
        exit(1);
    }
    emit_record(w, name, codec, elem, block, raw, raw_size, chunk, c);
    free(chunk);
}

static void emit_repack(corpus_writer *w, pac_encoder *enc, const char *name, int dst_codec,
                        int src_codec, size_t elem, size_t sblock, size_t dblock,
                        int use_default_hdr, const unsigned char *raw, size_t raw_size)
{
    size_t scap = 12 + raw_size +
                  (raw_size / sblock + 2) *
                      (ZSTD_compressBound(sblock) + 4 + (size_t) LZ4_compressBound((int) sblock));
    size_t dcap = pac_bound(raw_size, elem, dblock);
    unsigned char *stock = (unsigned char *) malloc(scap);
    unsigned char *chunk = (unsigned char *) malloc(dcap);
    size_t ss, c;
    if (!stock || !chunk) {
        fprintf(stderr, "malloc failed for repack %s\n", name);
        exit(1);
    }
    ss = stock_encode(src_codec, stock, raw, raw_size, elem, sblock);
    if (!ss) {
        fprintf(stderr, "stock_encode failed for %s\n", name);
        exit(1);
    }
    if (use_default_hdr)
        put_be(stock + 8, 0, 4);
    pac_set_narrow(enc, 1);
    c = pac_repack_chunk(enc, chunk, dcap, stock, ss, src_codec, elem, dblock);
    if (!c || c > dcap) {
        fprintf(stderr, "pac_repack_chunk failed for %s\n", name);
        exit(1);
    }
    emit_record(w, name, dst_codec, elem, dblock, raw, raw_size, chunk, c);
    free(stock);
    free(chunk);
}

int main(int argc, char **argv)
{
    static const size_t elems[4] = {1, 2, 4, 8};
    static const size_t block_table[] = {
        /* tiny (multiplied by elem when < 256) */
        8, 16, 40, 64, 248,
        /* FCS & bt_raw thresholds */
        256, 1024, 4096, 8192, 16384, 32768, 65536, 65792,
        /* odd / non-8kB & 128 KiB boundary */
        12288, 20000, 100000, 131072, 131136,
        /* defaults */
        PAC_LZ4_BLOCK, PAC_ZSTD_BLOCK};
    const char *out_path = argc > 1 ? argv[1] : "build-compat/compat_corpus.bin";
    const unsigned long seed = argc > 2 ? strtoul(argv[2], NULL, 0) : 20261122UL;
    corpus_writer w;
    pac_encoder *enc_lz4 = pac_create(PAC_LZ4);
    pac_encoder *enc_zstd = pac_create(PAC_ZSTD);
    size_t ei, bi;
    int p;
    char name[128];

    if (!enc_lz4 || !enc_zstd) {
        fprintf(stderr, "pac_create failed\n");
        return 1;
    }
    rng_state = seed ? seed : 1;

    w.f = fopen(out_path, "wb");
    if (!w.f) {
        perror(out_path);
        return 1;
    }
    w.count = 0;
    w.total_bytes = 12;
    if (fwrite(COMPAT_MAGIC, 1, COMPAT_MAGIC_LEN, w.f) != COMPAT_MAGIC_LEN ||
        !compat_write_u32(w.f, 0)) {
        fprintf(stderr, "header write failed\n");
        return 1;
    }

    /* 1. Chunk-size & framing edge cases (n = 0, 1, 7) */
    for (ei = 0; ei < 4; ei++) {
        size_t elem = elems[ei];
        size_t block = elem == 1 ? 8192 : PAC_ZSTD_BLOCK;
        unsigned char raw[64];
        fill_pattern(raw, 7, elem, 4);
        snprintf(name, sizeof(name), "edge_empty_lz4_e%lu", (unsigned long) elem);
        emit_compress(&w, enc_lz4, name, PAC_LZ4, elem, block, 1, raw, 0);
        snprintf(name, sizeof(name), "edge_empty_zstd_e%lu", (unsigned long) elem);
        emit_compress(&w, enc_zstd, name, PAC_ZSTD, elem, block, 1, raw, 0);
        snprintf(name, sizeof(name), "edge_tail1_e%lu", (unsigned long) elem);
        emit_compress(&w, enc_zstd, name, PAC_ZSTD, elem, block, 1, raw, 1 * elem);
        snprintf(name, sizeof(name), "edge_tail7_e%lu", (unsigned long) elem);
        emit_compress(&w, enc_lz4, name, PAC_LZ4, elem, block, 1, raw, 7 * elem);
    }

    /* 2. Non-default and default block sizes across elem, patterns, narrow */
    for (ei = 0; ei < 4; ei++) {
        size_t elem = elems[ei];
        for (bi = 0; bi < sizeof(block_table) / sizeof(block_table[0]); bi++) {
            size_t block = block_table[bi] < 256 ? block_table[bi] * elem : block_table[bi];
            size_t tail = 3 * elem;
            size_t raw_size;
            unsigned char *raw;
            int pat, ci;
            if (block % (8 * elem) != 0)
                continue;
            /* Mix exact single-block, sub-block, and multi-block + partial + tail */
            if (bi % 3 == 0)
                raw_size = block;
            else if (bi % 3 == 1 && block >= 16 * elem)
                raw_size = block - 8 * elem + tail;
            else
                raw_size = block + (block / 2) - ((block / 2) % (8 * elem)) + tail;
            if (raw_size > 512000)
                raw_size = block + tail;

            raw = (unsigned char *) malloc(raw_size ? raw_size : 1);
            if (!raw)
                return 1;
            /* Rotate through all 10 patterns across block sizes */
            for (p = 0; p < 3; p++) {
                pat = (int) ((ei * 3 + bi + (size_t) p * 3) % NPATTERNS);
                fill_pattern(raw, raw_size / elem, elem, pat);
                for (ci = 0; ci < 2; ci++) {
                    int codec = ci ? PAC_ZSTD : PAC_LZ4;
                    pac_encoder *enc = ci ? enc_zstd : enc_lz4;
                    int narrow = (int) ((bi + (size_t) p) & 1);
                    snprintf(name, sizeof(name), "blk%lu_e%lu_%s_%s_n%d",
                             (unsigned long) block, (unsigned long) elem,
                             ci ? "zstd" : "lz4", pattern_names[pat], narrow);
                    emit_compress(&w, enc, name, codec, elem, block, narrow, raw, raw_size);
                }
            }
            free(raw);
        }
    }

    /* 3. Decreasing block sizes on the same encoders (windowLog / state reuse) */
    {
        static const size_t seq_blocks[5] = {253952, 131072, 57344, 16384, 4096};
        size_t i;
        for (i = 0; i < 5; i++) {
            size_t block = seq_blocks[i];
            size_t raw_size = block + 64;
            unsigned char *raw = (unsigned char *) malloc(raw_size);
            fill_pattern(raw, raw_size / 2, 2, (int) (i % NPATTERNS));
            snprintf(name, sizeof(name), "dec_zstd_b%lu", (unsigned long) block);
            emit_compress(&w, enc_zstd, name, PAC_ZSTD, 2, block, 1, raw, raw_size);
            snprintf(name, sizeof(name), "dec_lz4_b%lu", (unsigned long) block);
            emit_compress(&w, enc_lz4, name, PAC_LZ4, 2, block, 1, raw, raw_size);
            free(raw);
        }
    }

    /* 4. Zstd bt_rle, constant non-zero, and bt_raw + cctx_tail hybrid splices */
    {
        size_t blk = 253952;
        unsigned char *raw = (unsigned char *) malloc(blk);
        size_t i;

        memset(raw, 0x5a, blk);
        emit_compress(&w, enc_zstd, "zstd_rle_0x5a_248k_n0", PAC_ZSTD, 1, blk, 0, raw, blk);
        emit_compress(&w, enc_zstd, "zstd_rle_0x5a_248k_n1", PAC_ZSTD, 1, blk, 1, raw, blk);

        memset(raw, 0xff, blk);
        emit_compress(&w, enc_zstd, "zstd_rle_0xff_248k", PAC_ZSTD, 2, blk, 1, raw, blk);

        fill_pattern(raw, blk, 1, 2); /* noise */
        emit_compress(&w, enc_zstd, "zstd_raw_noise_248k", PAC_ZSTD, 1, blk, 1, raw, blk);
        free(raw);

        /* bt_raw boundary match regression: plane 0 random ending 0x5a, plane 1 starting 0x5a*8 */
        raw = (unsigned char *) malloc(16384);
        for (i = 0; i < 16384; i++) {
            unsigned b0 = (unsigned) (rng() & 1);
            unsigned b1 = i < 64 ? ((0x5aU >> (i & 7)) & 1U) : 0U;
            if (i >= 16384 - 8)
                b0 = (0x5aU >> (i & 7)) & 1U;
            raw[i] = (unsigned char) (b0 | (b1 << 1));
        }
        emit_compress(&w, enc_zstd, "zstd_btraw_boundary_16k", PAC_ZSTD, 1, 16384, 1, raw, 16384);
        free(raw);
    }

    /* 5. LZ4 16-bit offset boundary (plane = 65535 vs 65536) */
    {
        static const size_t lz_blocks[3] = {524280, 524288, 1048576};
        static const size_t lz_elems[3] = {1, 1, 2};
        size_t i;
        for (i = 0; i < 3; i++) {
            size_t blk = lz_blocks[i], el = lz_elems[i];
            unsigned char *raw = (unsigned char *) malloc(blk);
            fill_pattern(raw, blk / el, el, 3); /* sparse photons */
            snprintf(name, sizeof(name), "lz4_plane64k_b%lu_e%lu", (unsigned long) blk,
                     (unsigned long) el);
            emit_compress(&w, enc_lz4, name, PAC_LZ4, el, blk, 1, raw, blk);
            free(raw);
        }
    }

    /* 6. Large blocks (plane > 128 KiB) */
    {
        /* 1310720 u8: plane 0 random (bt_raw), plane 1 patterned */
        size_t n1 = 1310720, i;
        unsigned char *raw1 = (unsigned char *) malloc(n1);
        for (i = 0; i < n1; i++) {
            unsigned b0 = (unsigned) (rng() & 1);
            unsigned b1 = (i & 7) == 0 ? 1U : 0U;
            raw1[i] = (unsigned char) (b0 | (b1 << 1));
        }
        emit_compress(&w, enc_zstd, "large_btraw_plane160k_u8", PAC_ZSTD, 1, n1, 1, raw1, n1);
        free(raw1);

        /* 2621440 u16: zero-prefix planes 0..1 + narrow vs non-narrow */
        size_t blk2 = 2621440;
        unsigned char *raw2 = (unsigned char *) malloc(blk2);
        for (i = 0; i < blk2 / 2; i++) {
            uint16_t v = (uint16_t) (((i & 127) << 2) | ((i & 31) == 0 ? 0xfffcU : 0U));
            memcpy(raw2 + 2 * i, &v, 2);
        }
        emit_compress(&w, enc_zstd, "large_plane160k_u16_n0", PAC_ZSTD, 2, blk2, 0, raw2, blk2);
        emit_compress(&w, enc_zstd, "large_plane160k_u16_n1", PAC_ZSTD, 2, blk2, 1, raw2, blk2);
        free(raw2);

        /* 4194368 u32: plane > 128 KiB for u32 */
        size_t blk4 = 4194368;
        unsigned char *raw4 = (unsigned char *) malloc(blk4);
        fill_pattern(raw4, blk4 / 4, 4, 4);
        emit_compress(&w, enc_zstd, "large_plane131k_u32", PAC_ZSTD, 4, blk4, 1, raw4, blk4);
        free(raw4);
    }

    /* 7. pac_repack_chunk across non-default and default block sizes + sblock=0 */
    {
        static const struct {
            size_t elem;
            size_t sblock;
            size_t dblock;
            int def_hdr;
        } rcases[] = {
            {2, 8192, 16384, 1},
            {2, 8192, PAC_LZ4_BLOCK, 1},
            {2, 8192, 131072, 0},
            {2, 8192, PAC_ZSTD_BLOCK, 1},
            {4, 4096, 12288, 0},
            {1, 8192, 139264, 0},
        };
        size_t ri;
        for (ri = 0; ri < sizeof(rcases) / sizeof(rcases[0]); ri++) {
            size_t elem = rcases[ri].elem;
            size_t sb = rcases[ri].sblock;
            size_t db = rcases[ri].dblock;
            size_t raw_size = db + sb + 3 * elem;
            unsigned char *raw = (unsigned char *) malloc(raw_size);
            int sc, dc;
            fill_pattern(raw, raw_size / elem, elem, (int) ((ri + 3) % NPATTERNS));
            for (sc = 0; sc < 2; sc++) {
                for (dc = 0; dc < 2; dc++) {
                    int scodec = sc ? PAC_ZSTD : PAC_LZ4;
                    int dcodec = dc ? PAC_ZSTD : PAC_LZ4;
                    snprintf(name, sizeof(name), "repack_%s2%s_e%lu_sb%lu_db%lu",
                             sc ? "zstd" : "lz4", dc ? "zstd" : "lz4", (unsigned long) elem,
                             (unsigned long) sb, (unsigned long) db);
                    emit_repack(&w, dc ? enc_zstd : enc_lz4, name, dcodec, scodec, elem, sb, db,
                                rcases[ri].def_hdr, raw, raw_size);
                }
            }
            free(raw);
        }
    }

    /* Seek back and write final record count */
    if (fseek(w.f, COMPAT_MAGIC_LEN, SEEK_SET) != 0 || !compat_write_u32(w.f, w.count)) {
        fprintf(stderr, "failed to update record count\n");
        return 1;
    }
    fclose(w.f);
    pac_free(enc_lz4);
    pac_free(enc_zstd);
    printf("generated %u corpus cases (%.2f MB) -> %s\n", w.count,
           (double) w.total_bytes / (1024.0 * 1024.0), out_path);
    return 0;
}
