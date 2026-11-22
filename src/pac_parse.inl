/*
 * The parse of one stretch of a bit-transposed block, for one instruction
 * set.  Included by pac.c with PAC_ISA (0 generic, 1 sse2, 2 avx2,
 * 3 avx512bw, 4 neon, 5 vsx), PAC_NAME(x) and PAC_TARGET defined.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.  Code written by Claude Fable 5.2.
 */

#if PAC_ISA == 4
/* bit k set: byte k of a equals byte k of b (AArch64 NEON) */
static PAC_INLINE unsigned pac_eq16_neon(const unsigned char *a, const unsigned char *b)
{
    static const unsigned char weights[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    uint8x16_t t = vandq_u8(vceqq_u8(vld1q_u8(a), vld1q_u8(b)), vld1q_u8(weights));
    return (unsigned) vaddv_u8(vget_low_u8(t)) | ((unsigned) vaddv_u8(vget_high_u8(t)) << 8);
}
#elif PAC_ISA == 5
/* bit k set: byte k of a equals byte k of b (POWER8 VSX, little endian): the bit permute
 * gathers the top bit of each byte of the comparison */
static PAC_INLINE unsigned pac_eq16_vsx(const unsigned char *a, const unsigned char *b)
{
    static const unsigned char pick[16] = {120, 112, 104, 96, 88, 80, 72, 64, 56, 48, 40, 32, 24, 16, 8, 0};
    const __vector unsigned char c =
        (__vector unsigned char) vec_cmpeq(vec_vsx_ld(0, a), vec_vsx_ld(0, b));
    const __vector unsigned long r =
        (__vector unsigned long) vec_vbpermq(c, vec_vsx_ld(0, pick));
    return (unsigned) vec_extract(r, 1);
}
#endif

/* bit k set: s[k] == s[k - d], for k in 0..63 */
PAC_TARGET static PAC_INLINE pac_u64 PAC_NAME(eq64)(const unsigned char *s, size_t d)
{
#if PAC_ISA == 4 || PAC_ISA == 5
    pac_u64 m = 0;
    int k;
    for (k = 0; k < 4; k++)
#if PAC_ISA == 4
        m |= (pac_u64) pac_eq16_neon(s + 16 * k, s + 16 * k - d) << (16 * k);
#else
        m |= (pac_u64) pac_eq16_vsx(s + 16 * k, s + 16 * k - d) << (16 * k);
#endif
    return m;
#elif PAC_ISA == 3
    return (pac_u64) _mm512_cmpeq_epi8_mask(_mm512_loadu_si512((const void *) s),
                                            _mm512_loadu_si512((const void *) (s - d)));
#elif PAC_ISA == 2
    __m256i a0 = _mm256_loadu_si256((const __m256i *) s);
    __m256i b0 = _mm256_loadu_si256((const __m256i *) (s - d));
    __m256i a1 = _mm256_loadu_si256((const __m256i *) (s + 32));
    __m256i b1 = _mm256_loadu_si256((const __m256i *) (s + 32 - d));
    pac_u64 lo = (pac_u32) _mm256_movemask_epi8(_mm256_cmpeq_epi8(a0, b0));
    pac_u64 hi = (pac_u32) _mm256_movemask_epi8(_mm256_cmpeq_epi8(a1, b1));
    return lo | (hi << 32);
#elif PAC_ISA == 1
    pac_u64 m = 0;
    int k;
    for (k = 0; k < 4; k++) {
        __m128i a = _mm_loadu_si128((const __m128i *) (s + 16 * k));
        __m128i b = _mm_loadu_si128((const __m128i *) (s + 16 * k - d));
        m |= (pac_u64) (unsigned) _mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) << (16 * k);
    }
    return m;
#else
    pac_u64 m = 0;
    int k;
#if PAC_LITTLE_ENDIAN
    const pac_u64 lo7 = ((pac_u64) 0x7f7f7f7fUL << 32) | 0x7f7f7f7fUL;
    const pac_u64 gather = ((pac_u64) 0x01020408UL << 32) | 0x10204080UL;
    for (k = 0; k < 8; k++) {
        pac_u64 x = pac_load64(s + 8 * k) ^ pac_load64(s + 8 * k - d);
        pac_u64 t = ~(((x & lo7) + lo7) | x | lo7); /* 0x80 in every byte of x that is zero */
        m |= (((t >> 7) * gather) >> 56) << (8 * k);
    }
#else
    for (k = 0; k < 64; k++)
        m |= (pac_u64) (s[k] == s[k - (ptrdiff_t) d]) << k;
#endif
    return m;
#endif
}

/* the number of k with a[k] == b[k] from k = 0, for a + k < end */
PAC_TARGET static PAC_INLINE size_t PAC_NAME(match_len)(const unsigned char *a,
                                                        const unsigned char *b,
                                                        const unsigned char *end)
{
    const unsigned char *a0 = a;
#if PAC_ISA == 3
    while ((size_t) (end - a) >= 64) {
        pac_u64 m = (pac_u64) _mm512_cmpneq_epi8_mask(_mm512_loadu_si512((const void *) a),
                                                      _mm512_loadu_si512((const void *) b));
        if (m)
            return (size_t) (a - a0) + (size_t) pac_ctz64(m);
        a += 64;
        b += 64;
    }
#endif
#if PAC_ISA == 2 || PAC_ISA == 3
    while ((size_t) (end - a) >= 32) {
        pac_u32 m = (pac_u32) _mm256_movemask_epi8(_mm256_cmpeq_epi8(
            _mm256_loadu_si256((const __m256i *) a), _mm256_loadu_si256((const __m256i *) b)));
        if (m != 0xffffffffUL)
            return (size_t) (a - a0) + (size_t) pac_ctz64((pac_u64) (~m & 0xffffffffUL));
        a += 32;
        b += 32;
    }
#endif
#if PAC_ISA == 4 || PAC_ISA == 5
    while ((size_t) (end - a) >= 16) {
#if PAC_ISA == 4
        unsigned m = pac_eq16_neon(a, b);
#else
        unsigned m = pac_eq16_vsx(a, b);
#endif
        if (m != 0xffffU)
            return (size_t) (a - a0) + (size_t) pac_ctz64((pac_u64) (~m & 0xffffU));
        a += 16;
        b += 16;
    }
#elif PAC_ISA >= 1
    while ((size_t) (end - a) >= 16) {
        unsigned m = (unsigned) _mm_movemask_epi8(_mm_cmpeq_epi8(
            _mm_loadu_si128((const __m128i *) a), _mm_loadu_si128((const __m128i *) b)));
        if (m != 0xffffU)
            return (size_t) (a - a0) + (size_t) pac_ctz64((pac_u64) (~m & 0xffffU));
        a += 16;
        b += 16;
    }
#elif PAC_LITTLE_ENDIAN
    while ((size_t) (end - a) >= 8) {
        pac_u64 x = pac_load64(a) ^ pac_load64(b);
        if (x)
            return (size_t) (a - a0) + (size_t) (pac_ctz64(x) >> 3);
        a += 8;
        b += 8;
    }
#endif
    while (a < end && *a == *b) {
        a++;
        b++;
    }
    return (size_t) (a - a0);
}

/*
 * Parse base[from..to): matches of at least 4 bytes at offset 1 (a run of
 * equal bytes) and, if plane != 0, at offset `plane` (the same bytes as the
 * bit plane before); where both start at the same byte the longer is taken.
 * Nothing else is searched.  Sequences are appended at seq; *lit is the
 * number of literal bytes pending before the next match, in and out;
 * *matched_out receives the sum of match lengths emitted in [from..to).
 * Returns the number of sequences added.
 *
 * base[to .. to + 63] must be readable (the content does not matter), and
 * from >= plane.
 */
PAC_TARGET PAC_NOINLINE static size_t PAC_NAME(parse)(const unsigned char *PAC_RESTRICT base,
                                                       size_t from, size_t to, size_t plane,
                                                       ZSTD_Sequence *PAC_RESTRICT seq,
                                                       size_t *lit, size_t *matched_out)
{
    const unsigned char *end = base + to;
    size_t i = from, lit_start = from - *lit, n = 0, matched = 0;
    if (i == 0)
        i = 1; /* nothing before the first byte */
    if (plane == 0) {
        while (i < to) {
            size_t left = to - i;
            pac_u64 valid = left >= 64 ? ~(pac_u64) 0 : (((pac_u64) 1 << left) - 1);
            pac_u64 e1 = PAC_NAME(eq64)(base + i, 1) & valid;
            pac_u64 c1 = e1 & (e1 >> 1);
            c1 &= c1 >> 2;
            if (!c1) {
                i += left >= 64 ? 61 : left; /* a match may start in the last 3 bytes of the window */
                continue;
            }
            for (;;) {
                size_t j = (size_t) pac_ctz64(c1);
                size_t pos = i + j;
                pac_u64 rem1 = ~(e1 >> j);
                size_t l1 = rem1 ? (size_t) pac_ctz64(rem1) : (64 - j);
                size_t next_k;
                if (j + l1 == 64 && left > 64)
                    l1 += PAC_NAME(match_len)(base + i + 64, base + i + 63, end);
                seq[n].litLength = (unsigned) (pos - lit_start);
                seq[n].rep = 0;
                seq[n].offset = 1;
                seq[n].matchLength = (unsigned) l1;
                n++;
                matched += l1;
                next_k = j + l1;
                lit_start = i + next_k;
                if (left < 64) {
                    if (next_k >= left) {
                        i = to;
                        break;
                    }
                    c1 &= (~(pac_u64) 0) << next_k;
                    if (!c1) {
                        i = to;
                        break;
                    }
                } else {
                    if (next_k >= 61) {
                        i += next_k;
                        break;
                    }
                    c1 &= (~(pac_u64) 0) << next_k;
                    if (!c1) {
                        i += 61;
                        break;
                    }
                }
            }
        }
    } else {
        while (i < to) {
            size_t left = to - i;
            pac_u64 valid = left >= 64 ? ~(pac_u64) 0 : (((pac_u64) 1 << left) - 1);
            pac_u64 e1 = PAC_NAME(eq64)(base + i, 1) & valid;
            pac_u64 ep = PAC_NAME(eq64)(base + i, plane) & valid;
            pac_u64 c1 = e1 & (e1 >> 1);
            pac_u64 cp = ep & (ep >> 1);
            pac_u64 c;
            c1 &= c1 >> 2;
            cp &= cp >> 2;
            c = c1 | cp;
            if (!c) {
                i += left >= 64 ? 61 : left;
                continue;
            }
            for (;;) {
                size_t j = (size_t) pac_ctz64(c);
                size_t pos = i + j;
                size_t l1 = 0, lp = 0, best_l, next_k;
                if ((c1 >> j) & 1) {
                    pac_u64 rem1 = ~(e1 >> j);
                    l1 = rem1 ? (size_t) pac_ctz64(rem1) : (64 - j);
                    if (j + l1 == 64 && left > 64)
                        l1 += PAC_NAME(match_len)(base + i + 64, base + i + 63, end);
                }
                if ((cp >> j) & 1) {
                    pac_u64 remp = ~(ep >> j);
                    lp = remp ? (size_t) pac_ctz64(remp) : (64 - j);
                    if (j + lp == 64 && left > 64)
                        lp += PAC_NAME(match_len)(base + i + 64, base + i + 64 - plane, end);
                }
                seq[n].litLength = (unsigned) (pos - lit_start);
                seq[n].rep = 0;
                if (l1 >= lp) {
                    seq[n].offset = 1;
                    seq[n].matchLength = (unsigned) l1;
                    best_l = l1;
                } else {
                    seq[n].offset = (unsigned) plane;
                    seq[n].matchLength = (unsigned) lp;
                    best_l = lp;
                }
                n++;
                matched += best_l;
                next_k = j + best_l;
                lit_start = i + next_k;
                if (left < 64) {
                    if (next_k >= left) {
                        i = to;
                        break;
                    }
                    c &= (~(pac_u64) 0) << next_k;
                    if (!c) {
                        i = to;
                        break;
                    }
                } else {
                    if (next_k >= 61) {
                        i += next_k;
                        break;
                    }
                    c &= (~(pac_u64) 0) << next_k;
                    if (!c) {
                        i += 61;
                        break;
                    }
                }
            }
        }
    }
    *lit = to - lit_start;
    *matched_out = matched;
    return n;
}
