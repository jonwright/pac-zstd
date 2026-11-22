/*
 * libFuzzer target for pac_compress_chunk, pac_bound, pac_set_narrow,
 * and SIMD-vs-generic / narrow differential equivalence.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.
 */
#include "fuzz_common.h"

static pac_encoder *g_enc[2];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    int ci, codec, narrow, isa, tight_cap, used_isa;
    size_t elem, block, cap_hint, raw_len, src_len, bound, n;
    unsigned char *src, *dst, *dec;
    pac_encoder *enc;

    if (size < 6)
        return 0;

    if (!g_enc[0]) {
        g_enc[0] = pac_create(PAC_LZ4);
        g_enc[1] = pac_create(PAC_ZSTD);
        if (!g_enc[0] || !g_enc[1])
            fuzz_fail("pac_create failed");
    }

    ci = data[0] & 1;
    codec = ci ? PAC_ZSTD : PAC_LZ4;
    elem = (size_t) 1 << ((data[0] >> 1) & 3);
    narrow = (data[0] >> 3) & 1;
    isa = (int) ((data[0] >> 4) & 7);
    tight_cap = (data[0] >> 7) & 1;
    block = fuzz_pick_block(data[2], data[3], elem, tight_cap);
    cap_hint = (size_t) data[4] | ((size_t) data[5] << 8);

    raw_len = size - 6;
    if (raw_len > 524288)
        raw_len = 524288;
    if (tight_cap && data[5] == 0xff && raw_len > 0 && elem > 1 && (raw_len % elem != 0))
        src_len = raw_len;
    else
        src_len = raw_len - (raw_len % elem);

    src = (unsigned char *) malloc(src_len ? src_len : 1);
    if (!src)
        fuzz_fail("oom allocating src");
    if (src_len)
        memcpy(src, data + 6, src_len);

    enc = g_enc[ci];
    bound = pac_bound(src_len, elem, block);
    if (bound == 0) {
        unsigned char tiny_buf[64];
        memset(tiny_buf, 0, sizeof(tiny_buf));
        if (pac_compress_chunk(enc, tiny_buf, sizeof(tiny_buf), src, src_len, elem, block) != 0)
            fuzz_fail("compress accepted args rejected by pac_bound");
        free(src);
        return 0;
    }

    if (pac_force_isa(enc, isa))
        used_isa = isa;
    else {
        pac_force_isa(enc, 0);
        used_isa = 0;
    }
    pac_set_narrow(enc, narrow);

    dst = (unsigned char *) malloc(bound);
    if (!dst) {
        free(src);
        fuzz_fail("oom allocating dst");
    }

    n = pac_compress_chunk(enc, dst, bound, src, src_len, elem, block);
    if (n == 0 || n > bound)
        fuzz_fail("compress failed or exceeded pac_bound");

    dec = (unsigned char *) malloc(src_len ? src_len : 1);
    if (!dec)
        fuzz_fail("oom allocating dec");
    if (fuzz_ref_decode(codec, dst, n, elem, dec, src_len) != (long) src_len ||
        (src_len > 0 && memcmp(dec, src, src_len) != 0))
        fuzz_fail("compress round-trip mismatch");

    /* Differential check: SIMD ISA must produce byte-for-byte identical output to generic. */
    if (used_isa != 0) {
        unsigned char *dst2 = (unsigned char *) malloc(bound);
        size_t n2;
        if (!dst2)
            fuzz_fail("oom allocating dst2 (isa)");
        pac_force_isa(enc, 0);
        n2 = pac_compress_chunk(enc, dst2, bound, src, src_len, elem, block);
        if (n2 != n || memcmp(dst2, dst, n) != 0)
            fuzz_fail("compress ISA mismatch vs generic");
        free(dst2);
        pac_force_isa(enc, used_isa);
    }

    /* Differential check: narrow optimization must preserve compressed chunk bytes. */
    if (elem > 1) {
        unsigned char *dst2 = (unsigned char *) malloc(bound);
        size_t n2;
        if (!dst2)
            fuzz_fail("oom allocating dst2 (narrow)");
        pac_set_narrow(enc, !narrow);
        n2 = pac_compress_chunk(enc, dst2, bound, src, src_len, elem, block);
        if (n2 != n || memcmp(dst2, dst, n) != 0)
            fuzz_fail("compress narrow mismatch");
        free(dst2);
        pac_set_narrow(enc, narrow);
    }

    /* Tight capacity check under ASan: must never write out of bounds. */
    if (tight_cap && n > 0) {
        size_t tcap = cap_hint % n;
        unsigned char *tdst = (unsigned char *) malloc(tcap ? tcap : 1);
        size_t tn;
        if (!tdst)
            fuzz_fail("oom allocating tdst");
        tn = pac_compress_chunk(enc, tdst, tcap, src, src_len, elem, block);
        if (tn != 0) {
            if (tn > tcap ||
                fuzz_ref_decode(codec, tdst, tn, elem, dec, src_len) != (long) src_len ||
                (src_len > 0 && memcmp(dec, src, src_len) != 0))
                fuzz_fail("tight-cap compress invalid output");
        }
        free(tdst);
    }

    free(dec);
    free(dst);
    free(src);
    return 0;
}
