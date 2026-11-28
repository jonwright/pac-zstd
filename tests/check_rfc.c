/*
 * Offline RFC 8878 (Zstandard) and LZ4 Block Format validator + clean-room
 * educational_decoder verifier for pac-zstd corpus files.
 *
 *   check_rfc <compat_corpus.bin>
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compat_format.h"
#include "zstd_decompress.h"

#define PAC_LZ4 1
#define PAC_ZSTD 2

extern int bitshuf_decode_block(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);

static char g_cur_case[256] = "(none)";
static size_t g_cur_block = 0;
static size_t g_max_cblock_bsize = 0; /* largest Compressed_Block (128 KiB erratum) seen */
static int g_clean_exit = 0;

static void on_exit_report(void)
{
    if (!g_clean_exit)
        fprintf(stderr, "check_rfc aborted during case '%s' block %lu\n", g_cur_case,
                (unsigned long) g_cur_block);
}

static uint64_t read_be(const unsigned char *p, int n)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t read_le24(const unsigned char *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16);
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static uint64_t read_le64(const unsigned char *p)
{
    return (uint64_t) read_le32(p) | ((uint64_t) read_le32(p + 4) << 32);
}

/*
 * Strict RFC 8878 structural validator (+ v1.5.2/v1.5.5 errata checks) on a
 * single Zstandard frame.  Returns 1 if valid, 0 with reason on failure.
 */
static int validate_zstd_rfc8878(const unsigned char *src, size_t len, size_t expected_nb,
                                 const char **err_out)
{
    size_t pos = 0, block_idx = 0;
    unsigned char fhd;
    int fcs_flag, single_seg, checksum_flag, dict_id_flag;
    size_t fcs_bytes, dict_bytes;
    uint64_t window_size = 0, fcs = 0;
    int has_fcs = 0;
    size_t block_max;
    int last_block = 0;
    int seen_huf_tree = 0;
    int seen_seq_tables = 0;

    if (len < 6) {
        *err_out = "frame shorter than 6 bytes";
        return 0;
    }
    if (read_le32(src) != 0xFD2FB528U) {
        *err_out = "bad ZSTD magic number";
        return 0;
    }
    pos = 4;
    fhd = src[pos++];

    /* RFC 8878 §3.1.1.1.1: bit 3 is reserved (must be 0); bit 4 is unused (must be 0) */
    if ((fhd & 0x18U) != 0) {
        *err_out = "FHD reserved/unused bits non-zero";
        return 0;
    }
    fcs_flag = (fhd >> 6) & 3;
    single_seg = (fhd >> 5) & 1;
    checksum_flag = (fhd >> 2) & 1;
    dict_id_flag = fhd & 3;

    if (dict_id_flag != 0) {
        *err_out = "unexpected non-zero Dictionary_ID_Flag";
        return 0;
    }
    dict_bytes = 0;

    if (!single_seg) {
        unsigned char wd;
        unsigned exp, mant;
        if (pos >= len) {
            *err_out = "truncated Window_Descriptor";
            return 0;
        }
        wd = src[pos++];
        exp = wd >> 3;
        mant = wd & 7;
        window_size = ((uint64_t) 1 << (10 + exp)) + (((uint64_t) 1 << (7 + exp)) * mant);
        if (window_size > ((uint64_t) 1 << 27)) {
            *err_out = "Window_Size exceeds 128 MiB limit";
            return 0;
        }
    }

    if (fcs_flag == 0)
        fcs_bytes = single_seg ? 1 : 0;
    else if (fcs_flag == 1)
        fcs_bytes = 2;
    else if (fcs_flag == 2)
        fcs_bytes = 4;
    else
        fcs_bytes = 8;

    if (pos + dict_bytes + fcs_bytes > len) {
        *err_out = "truncated Frame_Content_Size";
        return 0;
    }
    pos += dict_bytes;

    if (fcs_bytes > 0) {
        has_fcs = 1;
        if (fcs_bytes == 1)
            fcs = src[pos];
        else if (fcs_bytes == 2)
            fcs = (uint64_t) read_le16(src + pos) + 256ULL;
        else if (fcs_bytes == 4)
            fcs = read_le32(src + pos);
        else
            fcs = read_le64(src + pos);
        pos += fcs_bytes;
        if (fcs != (uint64_t) expected_nb) {
            *err_out = "Frame_Content_Size != expected block bytes";
            return 0;
        }
        if (single_seg)
            window_size = fcs;
    }

    if (window_size == 0)
        window_size = 1;
    block_max = window_size < 131072ULL ? (size_t) window_size : 131072U;

    while (!last_block) {
        uint32_t bh, btype, bsize;
        if (pos + 3 > len) {
            *err_out = "truncated Block_Header";
            return 0;
        }
        bh = read_le24(src + pos);
        pos += 3;
        last_block = (int) (bh & 1U);
        btype = (bh >> 1) & 3U;
        bsize = bh >> 3;
        block_idx++;

        if (btype == 3U) {
            *err_out = "Reserved Block_Type (3)";
            return 0;
        }
        if (btype == 0U) { /* Raw_Block */
            if (bsize > block_max || pos + bsize > len) {
                *err_out = "Raw_Block out of bounds";
                return 0;
            }
            pos += bsize;
        } else if (btype == 1U) { /* RLE_Block */
            if (bsize > block_max || pos + 1 > len) {
                *err_out = "RLE_Block out of bounds";
                return 0;
            }
            pos += 1;
        } else { /* Compressed_Block */
            const unsigned char *blk;
            size_t bpos = 0, lit_type, size_fmt, lit_hdr_bytes = 0, lit_payload = 0;
            size_t regen_size = 0, comp_size = 0, nb_seq = 0;
            unsigned char b0;

            if (bsize > g_max_cblock_bsize)
                g_max_cblock_bsize = bsize;
            /* v1.5.2 erratum: Compressed_Block size == 128 KiB was rejected */
            if (bsize > block_max || bsize >= 131072U || pos + bsize > len) {
                *err_out = "Compressed_Block size invalid or >= 128 KiB";
                return 0;
            }
            if (bsize < 2) {
                *err_out = "Compressed_Block too small for headers";
                return 0;
            }
            blk = src + pos;
            pos += bsize;

            /* Literals_Section_Header (RFC 8878 §3.1.1.3.1.1) */
            b0 = blk[0];
            lit_type = b0 & 3U;
            size_fmt = (b0 >> 2) & 3U;
            if (lit_type <= 1U) { /* Raw (0) or RLE (1) */
                if ((size_fmt & 1U) == 0U) {
                    lit_hdr_bytes = 1;
                    regen_size = b0 >> 3;
                } else if (size_fmt == 1U) {
                    lit_hdr_bytes = 2;
                    if (bsize < 2) {
                        *err_out = "truncated 2-byte literals header";
                        return 0;
                    }
                    regen_size = (size_t) ((b0 >> 4) | ((unsigned) blk[1] << 4));
                } else {
                    lit_hdr_bytes = 3;
                    if (bsize < 3) {
                        *err_out = "truncated 3-byte literals header";
                        return 0;
                    }
                    regen_size = (size_t) ((b0 >> 4) | ((unsigned) blk[1] << 4) |
                                           ((unsigned) blk[2] << 12));
                }
                lit_payload = (lit_type == 0U) ? regen_size : 1U;
            } else { /* Compressed (2) or Treeless (3) */
                if (lit_type == 3U && !seen_huf_tree) {
                    *err_out = "Treeless_Literals_Block without prior Huffman tree in frame";
                    return 0;
                }
                if (size_fmt <= 1U) {
                    uint32_t v;
                    lit_hdr_bytes = 3;
                    if (bsize < 3) {
                        *err_out = "truncated 3-byte compressed literals header";
                        return 0;
                    }
                    v = read_le24(blk);
                    regen_size = (v >> 4) & 0x3ffU;
                    comp_size = (v >> 14) & 0x3ffU;
                } else if (size_fmt == 2U) {
                    uint32_t v;
                    lit_hdr_bytes = 4;
                    if (bsize < 4) {
                        *err_out = "truncated 4-byte compressed literals header";
                        return 0;
                    }
                    v = read_le32(blk);
                    regen_size = (v >> 4) & 0x3fffU;
                    comp_size = (v >> 18) & 0x3fffU;
                } else {
                    uint64_t v;
                    lit_hdr_bytes = 5;
                    if (bsize < 5) {
                        *err_out = "truncated 5-byte compressed literals header";
                        return 0;
                    }
                    v = (uint64_t) read_le32(blk) | ((uint64_t) blk[4] << 32);
                    regen_size = (size_t) ((v >> 4) & 0x3ffffULL);
                    comp_size = (size_t) ((v >> 22) & 0x3ffffULL);
                }
                lit_payload = comp_size;
                if (lit_type == 2U)
                    seen_huf_tree = 1;
            }

            if (regen_size > block_max || lit_hdr_bytes + lit_payload > bsize) {
                *err_out = "literals section exceeds block bounds";
                return 0;
            }
            bpos = lit_hdr_bytes + lit_payload;

            /* Sequences_Section_Header (RFC 8878 §3.1.1.3.2) */
            if (bpos >= bsize) {
                *err_out = "missing Sequences_Section_Header";
                return 0;
            }
            {
                unsigned char sb0 = blk[bpos++];
                if (sb0 < 128U) {
                    nb_seq = sb0;
                } else if (sb0 < 255U) {
                    if (bpos >= bsize) {
                        *err_out = "truncated 2-byte Number_of_Sequences";
                        return 0;
                    }
                    nb_seq = ((size_t) (sb0 - 128U) << 8) + blk[bpos++];
                    /* v1.5.5 erratum: 2-byte encoding of 0 sequences */
                    if (nb_seq == 0) {
                        *err_out = "0 sequences encoded in 2-byte format";
                        return 0;
                    }
                } else {
                    if (bpos + 2 > bsize) {
                        *err_out = "truncated 3-byte Number_of_Sequences";
                        return 0;
                    }
                    nb_seq = (size_t) read_le16(blk + bpos) + 0x7f00U;
                    bpos += 2;
                }
            }

            /* v1.5.2 erratum: 0 literals and 0 sequences in Compressed_Block */
            if (regen_size == 0 && nb_seq == 0) {
                *err_out = "Compressed_Block with 0 literals and 0 sequences";
                return 0;
            }

            if (nb_seq == 0) {
                if (bpos != bsize) {
                    *err_out = "trailing bytes after 0-sequence section";
                    return 0;
                }
            } else {
                unsigned char modes;
                int ll_m, of_m, ml_m;
                if (bpos >= bsize) {
                    *err_out = "missing Symbol_Compression_Modes";
                    return 0;
                }
                modes = blk[bpos++];
                /* RFC 8878 §3.1.1.3.2.1: bits 0-1 reserved, must be 0 */
                if ((modes & 3U) != 0U) {
                    *err_out = "Symbol_Compression_Modes reserved bits non-zero";
                    return 0;
                }
                ll_m = (modes >> 6) & 3;
                of_m = (modes >> 4) & 3;
                ml_m = (modes >> 2) & 3;
                if ((ll_m == 3 || of_m == 3 || ml_m == 3) && !seen_seq_tables) {
                    *err_out = "Repeat_Mode FSE table without prior sequence tables in frame";
                    return 0;
                }
                seen_seq_tables = 1;
            }
        }
    }

    if (checksum_flag) {
        if (pos + 4 > len) {
            *err_out = "truncated Content_Checksum";
            return 0;
        }
        pos += 4;
    }
    if (pos != len) {
        *err_out = "trailing bytes after ZSTD frame";
        return 0;
    }
    if (!has_fcs && expected_nb > 0) {
        *err_out = "missing Frame_Content_Size (required by educational_decoder)";
        return 0;
    }
    (void) block_idx;
    return 1;
}

/*
 * Strict LZ4 Block Format validator and decoder per lz4_Block_format.md.
 * Enforces offset >= 1, offset <= op, match_len >= 4, and end-of-block rules
 * (final sequence literals-only; if any match occurred, >= 5 trailing literals
 * and last match starting at <= dst_len - 12).
 */
static int validate_and_decode_lz4_block(const unsigned char *src, size_t src_len,
                                         unsigned char *dst, size_t dst_len,
                                         const char **err_out)
{
    size_t ip = 0, op = 0;
    int had_match = 0;
    size_t last_match_start = 0;

    if (src_len == 0) {
        *err_out = "empty LZ4 block";
        return 0;
    }
    while (ip < src_len) {
        unsigned char tok = src[ip++];
        size_t lit_len = (tok >> 4) & 0x0fU;
        size_t match_len_low = tok & 0x0fU;
        size_t match_len, offset, k;

        if (lit_len == 15U) {
            unsigned char b;
            do {
                if (ip >= src_len) {
                    *err_out = "truncated LZ4 literal length";
                    return 0;
                }
                b = src[ip++];
                lit_len += b;
            } while (b == 255U);
        }
        if (ip + lit_len > src_len || op + lit_len > dst_len) {
            *err_out = "LZ4 literals out of bounds";
            return 0;
        }
        if (lit_len)
            memcpy(dst + op, src + ip, lit_len);
        ip += lit_len;
        op += lit_len;

        if (ip == src_len) {
            if (match_len_low != 0U) {
                *err_out = "LZ4 final sequence has non-zero match nibble";
                return 0;
            }
            if (had_match) {
                if (lit_len < 5U) {
                    *err_out = "LZ4 end-of-block rule violated: < 5 trailing literals";
                    return 0;
                }
                if (last_match_start + 12U > dst_len) {
                    *err_out = "LZ4 end-of-block rule violated: match in last 12 bytes";
                    return 0;
                }
            }
            break;
        }

        if (ip + 2 > src_len) {
            *err_out = "truncated LZ4 offset";
            return 0;
        }
        offset = read_le16(src + ip);
        ip += 2;
        if (offset == 0 || offset > op) {
            *err_out = "invalid LZ4 offset (0 or before start of block)";
            return 0;
        }

        match_len = match_len_low + 4U;
        if (match_len_low == 15U) {
            unsigned char b;
            do {
                if (ip >= src_len) {
                    *err_out = "truncated LZ4 match length";
                    return 0;
                }
                b = src[ip++];
                match_len += b;
            } while (b == 255U);
        }
        if (op + match_len > dst_len) {
            *err_out = "LZ4 match exceeds block size";
            return 0;
        }
        had_match = 1;
        last_match_start = op;
        for (k = 0; k < match_len; k++)
            dst[op + k] = dst[op + k - offset];
        op += match_len;

        if (ip >= src_len) {
            *err_out = "LZ4 block ends with a match instead of literals";
            return 0;
        }
    }
    if (op != dst_len || ip != src_len) {
        *err_out = "LZ4 decoded size mismatch";
        return 0;
    }
    return 1;
}

static int verify_case(const char *name, uint32_t codec, uint32_t elem, uint32_t hdr_block,
                       const unsigned char *raw, size_t raw_size, const unsigned char *chunk,
                       size_t chunk_size)
{
    uint64_t total, done = 0;
    size_t block, pos = 12, blk_idx = 0;
    unsigned char *out, *tmp, *scratch;

    snprintf(g_cur_case, sizeof(g_cur_case), "%s", name);
    g_cur_block = 0;

    if (chunk_size < 12) {
        fprintf(stderr, "FAIL %s: chunk < 12 bytes\n", name);
        return 0;
    }
    total = read_be(chunk, 8);
    block = (size_t) read_be(chunk + 8, 4);
    if (!block)
        block = (8192U / elem / 8U) * 8U * elem;
    if (total != raw_size || block != hdr_block || (block % (8U * elem)) != 0 ||
        block > (1U << 27)) {
        fprintf(stderr, "FAIL %s: bad chunk header (total=%llu raw=%lu block=%lu hdr=%u)\n", name,
                (unsigned long long) total, (unsigned long) raw_size, (unsigned long) block,
                hdr_block);
        return 0;
    }
    if (total == 0) {
        if (chunk_size != 12) {
            fprintf(stderr, "FAIL %s: empty chunk size %lu != 12\n", name,
                    (unsigned long) chunk_size);
            return 0;
        }
        return 1;
    }

    out = (unsigned char *) malloc(raw_size);
    tmp = (unsigned char *) malloc(block + 64);
    scratch = (unsigned char *) malloc(block + 64);
    if (!out || !tmp || !scratch) {
        fprintf(stderr, "oom\n");
        free(out);
        free(tmp);
        free(scratch);
        return 0;
    }

    while (done < total) {
        size_t b = (size_t) (total - done) < block ? (size_t) (total - done) : block;
        size_t left = b % (8U * elem), nb = b - left;
        g_cur_block = blk_idx++;
        if (nb) {
            size_t cs;
            const char *err = "unknown";
            if (chunk_size - pos < 4) {
                fprintf(stderr, "FAIL %s: truncated block length\n", name);
                goto bad;
            }
            cs = (size_t) read_be(chunk + pos, 4);
            pos += 4;
            if (chunk_size - pos < cs) {
                fprintf(stderr, "FAIL %s: truncated block payload\n", name);
                goto bad;
            }
            if (codec == PAC_ZSTD) {
                size_t got;
                if (!validate_zstd_rfc8878(chunk + pos, cs, nb, &err)) {
                    fprintf(stderr, "FAIL %s (block %lu): RFC 8878 violation: %s\n", name,
                            (unsigned long) g_cur_block, err);
                    goto bad;
                }
                got = ZSTD_decompress(tmp, nb, chunk + pos, cs);
                if (got != nb) {
                    fprintf(stderr, "FAIL %s (block %lu): educational_decoder got %lu != %lu\n",
                            name, (unsigned long) g_cur_block, (unsigned long) got,
                            (unsigned long) nb);
                    goto bad;
                }
            } else {
                if (!validate_and_decode_lz4_block(chunk + pos, cs, tmp, nb, &err)) {
                    fprintf(stderr, "FAIL %s (block %lu): LZ4 block violation: %s\n", name,
                            (unsigned long) g_cur_block, err);
                    goto bad;
                }
            }
            pos += cs;
            bitshuf_decode_block((char *) out + done, (const char *) tmp, (char *) scratch,
                                 nb / elem, elem);
        }
        if (left) {
            if (chunk_size - pos < left) {
                fprintf(stderr, "FAIL %s: truncated tail\n", name);
                goto bad;
            }
            memcpy(out + done + nb, chunk + pos, left);
            pos += left;
        }
        done += b;
    }

    if (pos != chunk_size) {
        fprintf(stderr, "FAIL %s: trailing chunk bytes (%lu != %lu)\n", name, (unsigned long) pos,
                (unsigned long) chunk_size);
        goto bad;
    }
    if (memcmp(out, raw, raw_size) != 0) {
        fprintf(stderr, "FAIL %s: decoded pixels differ from raw\n", name);
        goto bad;
    }

    free(out);
    free(tmp);
    free(scratch);
    return 1;

bad:
    free(out);
    free(tmp);
    free(scratch);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build-compat/compat_corpus.bin";
    FILE *f;
    char magic[COMPAT_MAGIC_LEN];
    uint32_t count = 0, i, failures = 0;

    atexit(on_exit_report);

    f = fopen(path, "rb");
    if (!f) {
        perror(path);
        g_clean_exit = 1;
        return 1;
    }
    if (fread(magic, 1, COMPAT_MAGIC_LEN, f) != COMPAT_MAGIC_LEN ||
        memcmp(magic, COMPAT_MAGIC, COMPAT_MAGIC_LEN) != 0 || !compat_read_u32(f, &count)) {
        fprintf(stderr, "invalid corpus header in %s\n", path);
        fclose(f);
        g_clean_exit = 1;
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
            g_clean_exit = 1;
            return 1;
        }
        if (name_len && fread(name, 1, name_len, f) != name_len) {
            fprintf(stderr, "truncated name at %u\n", i);
            fclose(f);
            g_clean_exit = 1;
            return 1;
        }
        name[name_len] = '\0';
        raw = (unsigned char *) malloc(raw_size ? raw_size : 1);
        chunk = (unsigned char *) malloc(chunk_size ? chunk_size : 1);
        if (!raw || !chunk) {
            fprintf(stderr, "oom at %s\n", name);
            free(raw);
            free(chunk);
            fclose(f);
            g_clean_exit = 1;
            return 1;
        }
        if ((raw_size && fread(raw, 1, raw_size, f) != raw_size) ||
            (chunk_size && fread(chunk, 1, chunk_size, f) != chunk_size)) {
            fprintf(stderr, "truncated payload at %s\n", name);
            free(raw);
            free(chunk);
            fclose(f);
            g_clean_exit = 1;
            return 1;
        }
        if (!verify_case(name, codec, elem, block, raw, raw_size, chunk, chunk_size))
            failures++;
        free(raw);
        free(chunk);
    }

    fclose(f);
    g_clean_exit = 1;
    printf("check_rfc: %u cases checked, %u failures\n", count, failures);
    printf("check_rfc: largest Compressed_Block size %lu bytes (zstd reject at >= 131072)\n",
           (unsigned long) g_max_cblock_bsize);
    if (g_max_cblock_bsize >= 131072U) {
        fprintf(stderr, "check_rfc: FATAL: a Compressed_Block of %lu bytes was emitted "
                        "(%s block %lu); <= v1.5.2 rejects it\n",
                (unsigned long) g_max_cblock_bsize, g_cur_case, (unsigned long) g_cur_block);
        return 1;
    }
    return failures ? 1 : 0;
}
