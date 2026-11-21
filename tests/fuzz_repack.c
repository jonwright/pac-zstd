/*
 * libFuzzer target for pac_repack_chunk: feeds arbitrary/mutated bitshuffle
 * chunks across codecs, element widths, block sizes, ISAs and capacities.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#include "fuzz_common.h"

static pac_encoder *g_enc[2];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int src_codec, dst_ci, dst_codec, isa, cap_mode, raw_percent;
    size_t elem, block, cap_hint, src_len, dst_cap, out_len;
    unsigned char *src, *dst;
    pac_encoder *enc;

    if (size < 6)
        return 0;

    if (!g_enc[0]) {
        g_enc[0] = pac_create(PAC_LZ4);
        g_enc[1] = pac_create(PAC_ZSTD);
        if (!g_enc[0] || !g_enc[1])
            fuzz_fail("pac_create failed");
    }

    src_codec = (data[0] & 1) ? PAC_ZSTD : PAC_LZ4;
    if ((data[0] & 0x80) && data[5] == 0xff)
        src_codec = 99; /* invalid src_codec rejection check */

    dst_ci = (data[0] >> 1) & 1;
    dst_codec = dst_ci ? PAC_ZSTD : PAC_LZ4;
    elem = (size_t) 1 << ((data[0] >> 2) & 3);
    isa = (int) ((data[0] >> 4) & 7);
    cap_mode = (data[0] >> 7) & 1;
    raw_percent = (data[1] & 3) == 0 ? 0 : ((data[1] & 3) == 1 ? 100 : (int) (data[1] >> 2));
    block = fuzz_pick_block(data[2], data[3], elem, 1);
    cap_hint = (size_t) data[4] | ((size_t) data[5] << 8);

    enc = g_enc[dst_ci];
    if (!pac_force_isa(enc, isa))
        pac_force_isa(enc, 0);
    pac_set_raw_literals(enc, raw_percent);

    src_len = size - 6;
    src = (unsigned char *) malloc(src_len ? src_len : 1);
    if (!src)
        fuzz_fail("oom allocating src");
    if (src_len)
        memcpy(src, data + 6, src_len);

    if (cap_mode == 1) {
        dst_cap = cap_hint % 4096;
    } else {
        dst_cap = 262144;
        if (src_len >= 12) {
            uint64_t hdr_total = fuzz_be(src, 8);
            if (hdr_total <= ((size_t) 1 << 20) && block > 0) {
                size_t b = pac_bound((size_t) hdr_total, elem, block);
                if (b > 0 && b <= ((size_t) 2 << 20))
                    dst_cap = b;
            }
        }
    }

    dst = (unsigned char *) malloc(dst_cap ? dst_cap : 1);
    if (!dst) {
        free(src);
        fuzz_fail("oom allocating dst");
    }

    out_len = pac_repack_chunk(enc, dst, dst_cap, src, src_len, src_codec, elem, block);

    if (out_len > 0) {
        uint64_t out_total;
        size_t out_block;

        if (out_len > dst_cap || out_len < 12 || src_len < 12 ||
            (src_codec != PAC_LZ4 && src_codec != PAC_ZSTD))
            fuzz_fail("repack invalid output length or accepted bad input");

        out_total = fuzz_be(dst, 8);
        out_block = (size_t) fuzz_be(dst + 8, 4);
        if (out_total != fuzz_be(src, 8) || out_block != block)
            fuzz_fail("repack header mismatch");

        if (out_total <= ((size_t) 2 << 20)) {
            unsigned char *dec_dst = (unsigned char *) malloc(out_total ? (size_t) out_total : 1);
            unsigned char *dec_src = (unsigned char *) malloc(out_total ? (size_t) out_total : 1);
            long got, got_src;
            if (!dec_dst || !dec_src)
                fuzz_fail("oom allocating decode buffers");

            got = fuzz_ref_decode(dst_codec, dst, out_len, elem, dec_dst, (size_t) out_total);
            if (got != (long) out_total)
                fuzz_fail("repack produced undecodable chunk");

            got_src = fuzz_ref_decode(src_codec, src, src_len, elem, dec_src, (size_t) out_total);
            if (got_src == (long) out_total && out_total > 0 &&
                memcmp(dec_src, dec_dst, (size_t) out_total) != 0)
                fuzz_fail("repack pixel mismatch vs decoded input");

            free(dec_dst);
            free(dec_src);
        }
    }

    free(src);
    free(dst);
    return 0;
}
