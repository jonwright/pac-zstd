/*
 * pac-zstd: see pac.h.
 *
 * Written in the C89 subset that old MSVC accepts (declarations first, no
 * inline / restrict keywords, no // comments), with 64-bit integers.
 * The instruction sets are compiled as in kcb's bitshuffle.c: one copy of the
 * kernel per set, with a target attribute where the compiler has them
 * (GCC, clang), else only what the compile flags allow (MSVC: /arch).  The
 * choice is made once per encoder with __builtin_cpu_supports, and kept in a
 * function pointer rather than an ifunc symbol, so tests can run every set.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.  Code written by Claude Fable 5.2.
 */
#define ZSTD_STATIC_LINKING_ONLY
#include "pac.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "lz4.h"
#include "zstd.h"

/* ------------------------------------------------------------------ */
/* compiler                                                            */

#if defined(_MSC_VER) && _MSC_VER < 1600
typedef unsigned __int64 pac_u64;
typedef unsigned int pac_u32;
typedef unsigned short pac_u16;
#else
#include <stdint.h>
typedef uint64_t pac_u64;
typedef uint32_t pac_u32;
typedef uint16_t pac_u16;
#endif

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#define PAC_X86 1
#if defined(_MSC_VER) && _MSC_VER < 1600
#include <emmintrin.h> /* SSE2 is all this compiler has */
#else
#include <immintrin.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#if (defined(_M_IX86) && _M_IX86_FP == 2 || defined(_M_X64)) && !defined(__SSE2__)
#define __SSE2__ 1
#endif
#endif
#else
#define PAC_X86 0
#endif

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>
#define PAC_HAVE_NEON 1
#else
#define PAC_HAVE_NEON 0
#endif

/* POWER8 and later, little endian */
#if defined(__powerpc64__) && defined(__LITTLE_ENDIAN__) && defined(__VSX__) && \
    defined(__POWER8_VECTOR__) && defined(__GNUC__)
#include <altivec.h>
#undef vector /* altivec.h's context keywords: not used here, and they break other code */
#undef pixel
#undef bool
#define PAC_HAVE_VSX 1
#else
#define PAC_HAVE_VSX 0
#endif

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if defined(_MSC_VER)
#define PAC_INLINE __forceinline
#define PAC_NOINLINE __declspec(noinline)
#define PAC_RESTRICT __restrict
#elif defined(__GNUC__)
#define PAC_INLINE __inline__ __attribute__((always_inline))
#define PAC_NOINLINE __attribute__((noinline))
#define PAC_RESTRICT __restrict__
#else
#define PAC_INLINE
#define PAC_NOINLINE
#define PAC_RESTRICT
#endif

/* runtime selection: target attributes and __builtin_cpu_supports (GCC >= 4.9, clang) */
#ifndef PAC_RUNTIME_DISPATCH
#if PAC_X86 && defined(__GNUC__) && !defined(__INTEL_COMPILER) && \
    (defined(__clang__) || __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 9))
#define PAC_RUNTIME_DISPATCH 1
#else
#define PAC_RUNTIME_DISPATCH 0
#endif
#endif

#if PAC_RUNTIME_DISPATCH
#define PAC_ATTRIBUTE_TARGET(x) __attribute__((target(x)))
#else
#define PAC_ATTRIBUTE_TARGET(x)
#endif

#if __has_attribute(no_sanitize_address)
#define PAC_NO_SANITIZE __attribute__((no_sanitize_address))
#else
#define PAC_NO_SANITIZE
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ || defined(__BIG_ENDIAN__)
#define PAC_LITTLE_ENDIAN 0
#else
#define PAC_LITTLE_ENDIAN 1
#endif

static PAC_INLINE pac_u64 pac_load64(const unsigned char *p)
{
    pac_u64 v;
    memcpy(&v, p, 8);
    return v;
}

/* the index of the lowest set bit; x != 0 */
static PAC_INLINE int pac_ctz64(pac_u64 x)
{
#if defined(__GNUC__)
    return sizeof(unsigned long) == 8 ? __builtin_ctzl((unsigned long) x)
                                      : ((pac_u32) x ? __builtin_ctzl((unsigned long) (pac_u32) x)
                                                     : 32 + __builtin_ctzl((unsigned long) (x >> 32)));
#elif defined(_MSC_VER) && defined(_M_X64)
    unsigned long i;
    _BitScanForward64(&i, x);
    return (int) i;
#else
    int n = 0;
    if (!(x & 0xffffffffUL)) { n += 32; x >>= 32; }
    if (!(x & 0xffffUL)) { n += 16; x >>= 16; }
    if (!(x & 0xffUL)) { n += 8; x >>= 8; }
    if (!(x & 0xfUL)) { n += 4; x >>= 4; }
    if (!(x & 0x3UL)) { n += 2; x >>= 2; }
    if (!(x & 0x1UL)) { n += 1; }
    return n;
#endif
}

static PAC_INLINE unsigned pac_popcount32(pac_u32 x)
{
#if defined(__GNUC__)
    return (unsigned) __builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555UL);
    x = (x & 0x33333333UL) + ((x >> 2) & 0x33333333UL);
    return (unsigned) ((((x + (x >> 4)) & 0x0f0f0f0fUL) * 0x01010101UL) >> 24);
#endif
}

/* ------------------------------------------------------------------ */
/* the parse, once per instruction set (pac_parse.inl)                 */

typedef size_t (*pac_parse_fn)(const unsigned char *PAC_RESTRICT base, size_t from, size_t to,
                               size_t plane, ZSTD_Sequence *PAC_RESTRICT seq, size_t *lit,
                               size_t *matched_out);

#define PAC_ISA 0
#define PAC_NAME(x) pac_##x##_generic
#define PAC_TARGET
#include "pac_parse.inl"
#undef PAC_ISA
#undef PAC_NAME
#undef PAC_TARGET

#if PAC_X86 && (defined(__SSE2__) || PAC_RUNTIME_DISPATCH)
#define PAC_HAVE_SSE2 1
#define PAC_ISA 1
#define PAC_NAME(x) pac_##x##_sse2
#define PAC_TARGET PAC_ATTRIBUTE_TARGET("sse2")
#include "pac_parse.inl"
#undef PAC_ISA
#undef PAC_NAME
#undef PAC_TARGET
#else
#define PAC_HAVE_SSE2 0
#endif

#if PAC_X86 && (defined(__AVX2__) || PAC_RUNTIME_DISPATCH)
#define PAC_HAVE_AVX2 1
#define PAC_ISA 2
#define PAC_NAME(x) pac_##x##_avx2
#define PAC_TARGET PAC_ATTRIBUTE_TARGET("avx2")
#include "pac_parse.inl"
#undef PAC_ISA
#undef PAC_NAME
#undef PAC_TARGET
#else
#define PAC_HAVE_AVX2 0
#endif

#if PAC_X86 && (defined(__AVX512BW__) || PAC_RUNTIME_DISPATCH)
#define PAC_HAVE_AVX512BW 1
#define PAC_ISA 3
#define PAC_NAME(x) pac_##x##_avx512bw
#define PAC_TARGET PAC_ATTRIBUTE_TARGET("avx512bw,avx2")
#include "pac_parse.inl"
#undef PAC_ISA
#undef PAC_NAME
#undef PAC_TARGET
#else
#define PAC_HAVE_AVX512BW 0
#endif

#if PAC_HAVE_NEON
#define PAC_ISA 4
#define PAC_NAME(x) pac_##x##_neon
#define PAC_TARGET
#include "pac_parse.inl"
#undef PAC_ISA
#undef PAC_NAME
#undef PAC_TARGET
#endif

#if PAC_HAVE_VSX
#define PAC_ISA 5
#define PAC_NAME(x) pac_##x##_vsx
#define PAC_TARGET
#include "pac_parse.inl"
#undef PAC_ISA
#undef PAC_NAME
#undef PAC_TARGET
#endif

#define PAC_NISA 6

/* variant `isa` if it was compiled and this CPU runs it, else NULL */
PAC_NO_SANITIZE static pac_parse_fn pac_parse_variant(int isa)
{
#if PAC_RUNTIME_DISPATCH
    __builtin_cpu_init();
#endif
    switch (isa) {
    case 0:
        return pac_parse_generic;
#if PAC_HAVE_SSE2
    case 1:
#if !defined(__SSE2__)
        if (!__builtin_cpu_supports("sse2"))
            return NULL;
#endif
        return pac_parse_sse2;
#endif
#if PAC_HAVE_AVX2
    case 2:
#if !defined(__AVX2__)
        if (!__builtin_cpu_supports("avx2"))
            return NULL;
#endif
        return pac_parse_avx2;
#endif
#if PAC_HAVE_AVX512BW
    case 3:
#if !defined(__AVX512BW__)
        if (!__builtin_cpu_supports("avx512bw"))
            return NULL;
#endif
        return pac_parse_avx512bw;
#endif
#if PAC_HAVE_NEON
    case 4: /* every AArch64 has it */
        return pac_parse_neon;
#endif
#if PAC_HAVE_VSX
    case 5: /* the compile flags already require it */
        return pac_parse_vsx;
#endif
    default:
        return NULL;
    }
}

/* The NEON kernel passes the tests under qemu-aarch64 (gcc 16); it has not been
 * run on ARM hardware yet.  -DPAC_NEON_DEFAULT=0 keeps the portable path. */
#ifndef PAC_NEON_DEFAULT
#define PAC_NEON_DEFAULT 1
#endif

static int pac_best_isa(void)
{
    int isa;
    for (isa = PAC_NISA - 1; isa > 0; isa--)
        if ((isa != 4 || PAC_NEON_DEFAULT) && pac_parse_variant(isa))
            return isa;
    return 0;
}

static const char *const pac_isa_names[PAC_NISA] = {"generic", "sse2", "avx2", "avx512bw", "neon", "vsx"};

/* ------------------------------------------------------------------ */
/* the encoder                                                         */

#define PAC_PAD 64 /* bytes the parse may read after the end of a block */
#define PAC_MULT 8 /* bitshuffle works on multiples of 8 elements */

struct pac_encoder {
    int codec;
    int narrow;
    int isa;
    pac_parse_fn parse;
    pac_transpose_fn transpose;
    ZSTD_CCtx *cctx;      /* literals: Huffman where it gains */
    ZSTD_CCtx *cctx_tail; /* magicless suffix for hybrid bt_raw frames */
    ZSTD_DCtx *dctx;      /* reused by pac_repack_chunk for PAC_ZSTD */
    int window_log;       /* monotonic windowLog set on cctx* */
    /* for blocks of up to cap_block bytes: */
    size_t cap_block;
    unsigned char *tmp;     /* the transposed block, PAC_PAD more bytes */
    unsigned char *scratch; /* for the transpose; a decoded source block in pac_repack_chunk */
    unsigned char *pack;    /* the lower bytes of the pixels of a block */
    ZSTD_Sequence *seq;
    /* per-plane invariance bitmasks from the narrow scan (bit p = plane p) */
    pac_u64 plane_zero;
    pac_u64 plane_one;
    pac_u64 plane_eq;
};

#if defined(PAC_WITH_KCB)
extern int bitshuf_encode_block(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);
#endif

static ZSTD_CCtx *pac_make_cctx(int magicless)
{
    ZSTD_CCtx *c = ZSTD_createCCtx();
    size_t e = 0;
    if (!c)
        return NULL;
    /* the level only selects the strength of the entropy stage: 1 and 3 give the same sizes */
    e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 1));
    e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_minMatch, 4));
    e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_blockDelimiters,
                                             ZSTD_sf_explicitBlockDelimiters));
    e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_repcodeResolution, ZSTD_ps_enable));
#if defined(PAC_VALIDATE_SEQUENCES)
    e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_validateSequences, 1));
#endif
    if (magicless) {
        e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_format, ZSTD_f_zstd1_magicless));
        e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 0));
    }
    if (e) {
        ZSTD_freeCCtx(c);
        return NULL;
    }
    return c;
}

pac_encoder *pac_create(int codec)
{
    pac_encoder *enc;
    if (codec != PAC_LZ4 && codec != PAC_ZSTD)
        return NULL;
    enc = (pac_encoder *) calloc(1, sizeof(pac_encoder));
    if (!enc)
        return NULL;
    enc->codec = codec;
    /* measured to help with kcb's SIMD transposes; with its portable transpose (POWER9) it is
     * much slower, so off x86 it is opt-in */
    enc->narrow = PAC_X86;
    enc->isa = pac_best_isa();
    enc->parse = pac_parse_variant(enc->isa);
#if defined(PAC_WITH_KCB)
    enc->transpose = bitshuf_encode_block;
#endif
    if (codec == PAC_ZSTD) {
        enc->cctx = pac_make_cctx(0);
        enc->cctx_tail = pac_make_cctx(1);
        if (!enc->cctx || !enc->cctx_tail) {
            pac_free(enc);
            return NULL;
        }
    }
    return enc;
}

void pac_free(pac_encoder *enc)
{
    if (!enc)
        return;
    ZSTD_freeCCtx(enc->cctx);
    ZSTD_freeCCtx(enc->cctx_tail);
    ZSTD_freeDCtx(enc->dctx);
    free(enc->tmp);
    free(enc->scratch);
    free(enc->pack);
    free(enc->seq);
    free(enc);
}

void pac_set_narrow(pac_encoder *enc, int on) { enc->narrow = on; }
void pac_set_transpose(pac_encoder *enc, pac_transpose_fn fn) { enc->transpose = fn; }
const char *pac_isa(const pac_encoder *enc) { return pac_isa_names[enc->isa]; }

int pac_force_isa(pac_encoder *enc, int isa)
{
    pac_parse_fn f = pac_parse_variant(isa);
    if (!f)
        return 0;
    enc->parse = f;
    enc->isa = isa;
    return 1;
}

static int pac_reserve(pac_encoder *enc, size_t block)
{
    if (enc->cap_block >= block)
        return 1;
    free(enc->tmp);
    free(enc->scratch);
    free(enc->pack);
    free(enc->seq);
    enc->cap_block = 0;
    enc->tmp = (unsigned char *) calloc(block + PAC_PAD, 1);
    enc->scratch = (unsigned char *) malloc(block + PAC_PAD);
    enc->pack = (unsigned char *) malloc(block);
    /* a match is at least 4 bytes; a delimiter per zstd block (at most one per plane piece) */
    enc->seq = (ZSTD_Sequence *) malloc((block / 4 + block / 512 + 1024) * sizeof(ZSTD_Sequence));
    if (!enc->tmp || !enc->scratch || !enc->pack || !enc->seq)
        return 0;
    enc->cap_block = block;
    return 1;
}

static void pac_put_be(unsigned char *p, pac_u64 v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[i] = (unsigned char) (v >> (8 * (n - 1 - i)));
}

static pac_u64 pac_get_be(const unsigned char *p, int n)
{
    pac_u64 v = 0;
    int i;
    for (i = 0; i < n; i++)
        v = (v << 8) | p[i];
    return v;
}

static void pac_put_le16(unsigned char *p, pac_u16 v)
{
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
}

static void pac_put_le24(unsigned char *p, pac_u32 v)
{
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
}

static void pac_put_le32(unsigned char *p, pac_u32 v)
{
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

/* ------------------------------------------------------------------ */
/* bit-plane invariance scan (elem > 1, little endian)                 */

#if PAC_HAVE_AVX2
PAC_ATTRIBUTE_TARGET("avx2")
static void pac_scan_invariance_avx2(const unsigned char *in, size_t nbytes, pac_u64 smask,
                                     pac_u64 *o_out, pac_u64 *a_out, pac_u64 *d_out, size_t *pos)
{
    size_t i = *pos, k;
    pac_u64 m4[4], o_lanes[4], a_lanes[4], d_lanes[4];
    __m256i v_or = _mm256_setzero_si256();
    __m256i v_and = _mm256_set1_epi8((char) 0xff);
    __m256i v_diff = _mm256_setzero_si256();
    __m256i v_smask;
    m4[0] = m4[1] = m4[2] = m4[3] = smask;
    v_smask = _mm256_loadu_si256((const __m256i *) m4);
    for (; i + 64 <= nbytes; i += 64) {
        __m256i x0 = _mm256_loadu_si256((const __m256i *) (in + i));
        __m256i x1 = _mm256_loadu_si256((const __m256i *) (in + i + 32));
        __m256i d0 = _mm256_xor_si256(x0, _mm256_and_si256(_mm256_srli_epi64(x0, 1), v_smask));
        __m256i d1 = _mm256_xor_si256(x1, _mm256_and_si256(_mm256_srli_epi64(x1, 1), v_smask));
        v_or = _mm256_or_si256(v_or, _mm256_or_si256(x0, x1));
        v_and = _mm256_and_si256(v_and, _mm256_and_si256(x0, x1));
        v_diff = _mm256_or_si256(v_diff, _mm256_or_si256(d0, d1));
    }
    _mm256_storeu_si256((__m256i *) o_lanes, v_or);
    _mm256_storeu_si256((__m256i *) a_lanes, v_and);
    _mm256_storeu_si256((__m256i *) d_lanes, v_diff);
    for (k = 0; k < 4; k++) {
        *o_out |= o_lanes[k];
        *a_out &= a_lanes[k];
        *d_out |= d_lanes[k];
    }
    *pos = i;
}
#endif

#if PAC_HAVE_SSE2
PAC_ATTRIBUTE_TARGET("sse2")
static void pac_scan_invariance_sse2(const unsigned char *in, size_t nbytes, pac_u64 smask,
                                     pac_u64 *o_out, pac_u64 *a_out, pac_u64 *d_out, size_t *pos)
{
    size_t i = *pos;
    pac_u64 m2[2], o_lanes[2], a_lanes[2], d_lanes[2];
    __m128i v_or = _mm_setzero_si128();
    __m128i v_and = _mm_set1_epi8((char) 0xff);
    __m128i v_diff = _mm_setzero_si128();
    __m128i v_smask;
    m2[0] = m2[1] = smask;
    v_smask = _mm_loadu_si128((const __m128i *) m2);
    for (; i + 32 <= nbytes; i += 32) {
        __m128i x0 = _mm_loadu_si128((const __m128i *) (in + i));
        __m128i x1 = _mm_loadu_si128((const __m128i *) (in + i + 16));
        __m128i d0 = _mm_xor_si128(x0, _mm_and_si128(_mm_srli_epi64(x0, 1), v_smask));
        __m128i d1 = _mm_xor_si128(x1, _mm_and_si128(_mm_srli_epi64(x1, 1), v_smask));
        v_or = _mm_or_si128(v_or, _mm_or_si128(x0, x1));
        v_and = _mm_and_si128(v_and, _mm_and_si128(x0, x1));
        v_diff = _mm_or_si128(v_diff, _mm_or_si128(d0, d1));
    }
    _mm_storeu_si128((__m128i *) o_lanes, v_or);
    _mm_storeu_si128((__m128i *) a_lanes, v_and);
    _mm_storeu_si128((__m128i *) d_lanes, v_diff);
    *o_out |= o_lanes[0] | o_lanes[1];
    *a_out &= a_lanes[0] & a_lanes[1];
    *d_out |= d_lanes[0] | d_lanes[1];
    *pos = i;
}
#endif

#if PAC_HAVE_NEON
static void pac_scan_invariance_neon(const unsigned char *in, size_t nbytes, pac_u64 smask,
                                     pac_u64 *o_out, pac_u64 *a_out, pac_u64 *d_out, size_t *pos)
{
    size_t i = *pos;
    pac_u64 m2[2], o_lanes[2], a_lanes[2], d_lanes[2];
    uint64x2_t v_or = vdupq_n_u64(0);
    uint64x2_t v_and = vdupq_n_u64(~(pac_u64) 0);
    uint64x2_t v_diff = vdupq_n_u64(0);
    uint64x2_t v_smask;
    m2[0] = m2[1] = smask;
    v_smask = vld1q_u64(m2);
    for (; i + 32 <= nbytes; i += 32) {
        uint64x2_t x0 = vreinterpretq_u64_u8(vld1q_u8(in + i));
        uint64x2_t x1 = vreinterpretq_u64_u8(vld1q_u8(in + i + 16));
        uint64x2_t d0 = veorq_u64(x0, vandq_u64(vshrq_n_u64(x0, 1), v_smask));
        uint64x2_t d1 = veorq_u64(x1, vandq_u64(vshrq_n_u64(x1, 1), v_smask));
        v_or = vorrq_u64(v_or, vorrq_u64(x0, x1));
        v_and = vandq_u64(v_and, vandq_u64(x0, x1));
        v_diff = vorrq_u64(v_diff, vorrq_u64(d0, d1));
    }
    vst1q_u64(o_lanes, v_or);
    vst1q_u64(a_lanes, v_and);
    vst1q_u64(d_lanes, v_diff);
    *o_out |= o_lanes[0] | o_lanes[1];
    *a_out &= a_lanes[0] & a_lanes[1];
    *d_out |= d_lanes[0] | d_lanes[1];
    *pos = i;
}
#endif

static void pac_scan_invariance(int isa, const unsigned char *in, size_t n, size_t elem,
                                pac_u64 *or_out, pac_u64 *and_out, pac_u64 *diff_out)
{
    const size_t nbytes = n * elem;
    size_t i = 0, k;
    pac_u64 o = 0, a = ~(pac_u64) 0, d = 0;
    pac_u64 vo = 0, va = ~(pac_u64) 0, vd = 0;
    const pac_u64 smask = elem == 8 ? (~(pac_u64) 0 >> 1)
                                    : (elem == 4 ? (((pac_u64) 0x7fffffffUL << 32) | 0x7fffffffUL)
                                                 : (((pac_u64) 0x7fff7fffUL << 32) | 0x7fff7fffUL));
#if PAC_HAVE_AVX2
    if (isa == 2 || isa == 3)
        pac_scan_invariance_avx2(in, nbytes, smask, &o, &a, &d, &i);
#endif
#if PAC_HAVE_SSE2
    if (isa == 1)
        pac_scan_invariance_sse2(in, nbytes, smask, &o, &a, &d, &i);
#endif
#if PAC_HAVE_NEON
    if (isa == 4)
        pac_scan_invariance_neon(in, nbytes, smask, &o, &a, &d, &i);
#endif
    (void) isa;
    /* n is a multiple of 8 and elem >= 2, so the tail is whole 64-bit words */
    for (; i + 8 <= nbytes; i += 8) {
        pac_u64 x = pac_load64(in + i);
        o |= x;
        a &= x;
        d |= x ^ ((x >> 1) & smask);
    }
    for (k = 0; k < 8; k += elem) {
        vo |= o >> (8 * k);
        va &= a >> (8 * k);
        vd |= d >> (8 * k);
    }
    if (elem < 8) {
        pac_u64 mask = ((pac_u64) 1 << (8 * elem)) - 1;
        vo &= mask;
        va &= mask;
        vd &= mask;
    }
    *or_out = vo;
    *and_out = va;
    *diff_out = vd;
}

static double pac_binary_entropy(double p)
{
    if (p <= 1e-6 || p >= 1.0 - 1e-6)
        return 0.0;
    return (-p * log(p) - (1.0 - p) * log(1.0 - p)) / log(2.0);
}

/*
 * Estimate the compression saving (%) of a bit plane p[0..len) from a
 * strided 4-byte sample (128 samples, odd stride to avoid power-of-2 phase
 * locking).  Combines 4-byte constant runs with the minimum of bit-density
 * entropy H2(p1), Miller-Madow-corrected nibble entropy, and XOR-against-
 * previous-plane entropy H2(p_xor); *by_prev_out is set when p_xor won.
 */
static double pac_probe_plane(const unsigned char *p, const unsigned char *prev, size_t len,
                              int *by_prev_out)
{
    size_t nsamples = 128, i, step, ones = 0, xor_ones = 0, run4 = 0, k_obs = 0, total_nib;
    unsigned long nib[16] = {0};
    double match_frac, p1, h_p1, h_pxor = 1.0, h_nib, h_min, sum_clogc = 0.0, h0;
    *by_prev_out = 0;
    if (len < 256)
        return 100.0;
    if (len < 512)
        nsamples = 64;
    step = len / nsamples;
    if (step > 4 && (step & 1) == 0)
        step--;
    if (prev) {
        for (i = 0; i < nsamples; i++) {
            pac_u32 w, wp;
            memcpy(&w, p + i * step, 4);
            memcpy(&wp, prev + i * step, 4);
            ones += (size_t) pac_popcount32(w);
            xor_ones += (size_t) pac_popcount32(w ^ wp);
            if (w == (pac_u32) ((unsigned char) w) * 0x01010101UL)
                run4++;
            nib[w & 0xf]++;
            nib[(w >> 4) & 0xf]++;
            nib[(w >> 8) & 0xf]++;
            nib[(w >> 12) & 0xf]++;
            nib[(w >> 16) & 0xf]++;
            nib[(w >> 20) & 0xf]++;
            nib[(w >> 24) & 0xf]++;
            nib[(w >> 28) & 0xf]++;
        }
    } else {
        for (i = 0; i < nsamples; i++) {
            pac_u32 w;
            memcpy(&w, p + i * step, 4);
            ones += (size_t) pac_popcount32(w);
            if (w == (pac_u32) ((unsigned char) w) * 0x01010101UL)
                run4++;
            nib[w & 0xf]++;
            nib[(w >> 4) & 0xf]++;
            nib[(w >> 8) & 0xf]++;
            nib[(w >> 12) & 0xf]++;
            nib[(w >> 16) & 0xf]++;
            nib[(w >> 20) & 0xf]++;
            nib[(w >> 24) & 0xf]++;
            nib[(w >> 28) & 0xf]++;
        }
    }
    match_frac = (double) run4 / (double) nsamples;
    p1 = (double) ones / (double) (nsamples * 32);
    h_p1 = pac_binary_entropy(p1);
    if (prev)
        h_pxor = pac_binary_entropy((double) xor_ones / (double) (nsamples * 32));
    total_nib = nsamples * 8;
    for (i = 0; i < 16; i++) {
        if (nib[i]) {
            sum_clogc += (double) nib[i] * log((double) nib[i]);
            k_obs++;
        }
    }
    h0 = (log((double) total_nib) - sum_clogc / (double) total_nib) / log(2.0);
    if (k_obs > 1) {
        h0 += (double) (k_obs - 1) / (2.0 * (double) total_nib * log(2.0));
        if (h0 > 4.0)
            h0 = 4.0;
    }
    h_nib = h0 / 4.0;
    h_min = h_p1 < h_nib ? h_p1 : h_nib;
    if (h_pxor < h_min) {
        h_min = h_pxor;
        *by_prev_out = 1;
    }
    return 100.0 * (1.0 - (1.0 - match_frac) * h_min);
}

/*
 * Parse the transposed block s[start_from..n), bit planes of `plane` bytes, into
 * enc->seq in zstd's form with explicit block delimiters: a plane of at
 * least 1024 bytes that is more than 1/20 literals gets a zstd block of its
 * own (its own Huffman table), the others are grouped up to zstd's block size.
 * enc->plane_{zero,one,eq} mark invariant planes to skip scanning.  Returns the
 * number of sequences; *lits_out = the literal bytes.
 */
static size_t pac_build(pac_encoder *enc, const unsigned char *s, size_t n, size_t plane,
                        size_t start_from, size_t *lits_out)
{
    ZSTD_Sequence *seq = enc->seq;
    size_t ns = 0, block_seq_start = 0, lit = 0, zblock = 0, total = 0, from;
    int alone = 0; /* the zstd block being built is a dense plane on its own */
    const size_t piece = plane < ZSTD_BLOCKSIZE_MAX ? plane : ZSTD_BLOCKSIZE_MAX;
    /* LZ4 offsets are 16 bits */
    const size_t use_plane = !(enc->codec == PAC_LZ4 && plane > 65535);
    for (from = start_from; from < n; from += piece) {
        size_t to = from + piece < n ? from + piece : n;
        size_t lit0 = lit, l = lit, k, lits, matched = 0;
        size_t p_idx = from / plane;
        int dense, skipped = 0;
        if (p_idx > 0 && from > start_from && (to - 1) / plane == p_idx && p_idx < 64 &&
            to - from >= 4) {
            pac_u64 pbit = (pac_u64) 1 << p_idx;
            int is_z = (enc->plane_zero & pbit) != 0;
            int is_o = (enc->plane_one & pbit) != 0;
            int is_eq = (enc->plane_eq & pbit) != 0;
            if ((is_z && ((enc->plane_zero >> (p_idx - 1)) & 1)) ||
                (is_o && ((enc->plane_one >> (p_idx - 1)) & 1))) {
                seq[ns].litLength = (unsigned) lit;
                seq[ns].matchLength = (unsigned) (to - from);
                seq[ns].offset = 1;
                seq[ns].rep = 0;
                k = 1;
                l = 0;
                matched = to - from;
                skipped = 1;
            } else if (use_plane && is_eq && from - start_from >= plane && s[from] != s[from - 1]) {
                seq[ns].litLength = (unsigned) lit;
                seq[ns].matchLength = (unsigned) (to - from);
                seq[ns].offset = (unsigned) plane;
                seq[ns].rep = 0;
                k = 1;
                l = 0;
                matched = to - from;
                skipped = 1;
            }
        }
        if (!skipped) {
            size_t dist = (use_plane && from - start_from >= plane) ? plane : 0;
            k = enc->parse(s + start_from, from - start_from, to - start_from, dist, seq + ns, &l, &matched);
        }
        lits = (to - from) - matched;
        /* its own Huffman table only pays for a plane of some size */
        dense = to - from >= 1024 && lits * 20 > to - from;
        if (zblock && (alone || dense || zblock + (to - from) > ZSTD_BLOCKSIZE_MAX)) {
            /* close the zstd block: the delimiter goes before this piece's sequences */
            memmove(seq + ns + 1, seq + ns, k * sizeof(*seq));
            seq[ns].offset = 0;
            seq[ns].matchLength = 0;
            seq[ns].rep = 0;
            seq[ns].litLength = (unsigned) lit0;
            /* which the first sequence of the piece then no longer carries */
            if (k)
                seq[ns + 1].litLength -= (unsigned) lit0;
            else
                l -= lit0;
            ns++;
            block_seq_start = ns;
            zblock = 0;
        } else if (ns > block_seq_start && k > 0 && seq[ns].litLength == 0 &&
                   seq[ns].matchLength > 0 && seq[ns].offset == seq[ns - 1].offset) {
            /* merge adjacent same-offset continuations inside the same zstd block */
            seq[ns - 1].matchLength += seq[ns].matchLength;
            if (k > 1)
                memmove(seq + ns, seq + ns + 1, (k - 1) * sizeof(*seq));
            k--;
        }
        ns += k;
        lit = l;
        zblock += to - from;
        alone = dense;
        total += lits;
    }
    seq[ns].offset = 0;
    seq[ns].matchLength = 0;
    seq[ns].rep = 0;
    seq[ns].litLength = (unsigned) lit;
    ns++;
    *lits_out = total;
    return ns;
}

static unsigned char *pac_lz4_len(unsigned char *op, size_t len)
{
    while (len >= 255) {
        *op++ = 255;
        len -= 255;
    }
    *op++ = (unsigned char) len;
    return op;
}

/*
 * The parse (as pac_build leaves it: the delimiters only carry literals) as
 * one LZ4 block.  LZ4's end-of-block rules: the last 5 bytes are literals and
 * no match starts in the last 12 bytes.  Returns the size, 0 if dst is too
 * small for the worst case.
 */
static size_t pac_emit_lz4(const ZSTD_Sequence *seq, size_t ns, const unsigned char *s, size_t n,
                           unsigned char *dst, size_t cap)
{
    unsigned char *op = dst;
    size_t anchor = 0, ip = 0, k, lit; /* anchor: start of the pending literals; ip: position */
    if (cap < n + n / 255 + 16)
        return 0; /* literals only: a match never costs more than the bytes it covers */
    for (k = 0; k < ns; k++) {
        size_t orig = seq[k].matchLength, m = orig, ml;
        ip += seq[k].litLength;
        if (!orig)
            continue;
        if (ip + m + 5 > n)
            m = ip + 5 < n ? n - 5 - ip : 0;
        if (m < 4 || ip + 12 > n) { /* stays literal */
            ip += orig;
            continue;
        }
        lit = ip - anchor;
        ml = m - 4;
        *op++ = (unsigned char) (((lit < 15 ? lit : 15) << 4) | (ml < 15 ? ml : 15));
        if (lit >= 15)
            op = pac_lz4_len(op, lit - 15);
        memcpy(op, s + anchor, lit);
        op += lit;
        *op++ = (unsigned char) (seq[k].offset & 255);
        *op++ = (unsigned char) (seq[k].offset >> 8);
        if (ml >= 15)
            op = pac_lz4_len(op, ml - 15);
        anchor = ip + m;
        ip += orig;
    }
    lit = n - anchor;
    *op++ = (unsigned char) ((lit < 15 ? lit : 15) << 4);
    if (lit >= 15)
        op = pac_lz4_len(op, lit - 15);
    memcpy(op, s + anchor, lit);
    op += lit;
    return (size_t) (op - dst);
}

/* Write a standard Zstd frame header with singleSegment=1, contentSizeFlag=1 (n < 2^32) */
static size_t pac_write_zstd_fh(unsigned char *dst, size_t n)
{
    pac_u32 fcs_code = (pac_u32) ((n >= 256) + (n >= 65536 + 256));
    unsigned char fhd = (unsigned char) ((1 << 5) | (fcs_code << 6));
    pac_put_le32(dst, 0xFD2FB528UL);
    dst[4] = fhd;
    if (fcs_code == 0) {
        dst[5] = (unsigned char) n;
        return 6;
    } else if (fcs_code == 1) {
        pac_put_le16(dst + 5, (pac_u16) (n - 256));
        return 7;
    } else {
        pac_put_le32(dst + 5, (pac_u32) n);
        return 9;
    }
}

static size_t pac_write_zstd_rle_frame(unsigned char *dst, size_t cap, size_t n, unsigned char val)
{
    size_t pos, done = 0;
    if (cap < ZSTD_FRAMEHEADERSIZE_MAX)
        return 0;
    pos = pac_write_zstd_fh(dst, n);
    while (done < n) {
        size_t b = n - done < ZSTD_BLOCKSIZE_MAX ? n - done : ZSTD_BLOCKSIZE_MAX;
        pac_u32 last = (done + b == n) ? 1U : 0U;
        if (cap - pos < 4)
            return 0;
        pac_put_le24(dst + pos, last | (1U << 1) | ((pac_u32) b << 3));
        dst[pos + 3] = val;
        pos += 4;
        done += b;
    }
    return pos;
}

static int pac_ensure_wl(pac_encoder *enc, size_t n)
{
    int wl = 10;
    while (((size_t) 1 << wl) < n)
        wl++;
    if (wl <= enc->window_log)
        return 1;
    /* parameters can only be set between frames: end the session of the last one */
    ZSTD_CCtx_reset(enc->cctx, ZSTD_reset_session_only);
    ZSTD_CCtx_reset(enc->cctx_tail, ZSTD_reset_session_only);
    if (ZSTD_isError(ZSTD_CCtx_setParameter(enc->cctx, ZSTD_c_windowLog, wl)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(enc->cctx_tail, ZSTD_c_windowLog, wl)))
        return 0;
    enc->window_log = wl;
    return 1;
}

/* One transposed block (in enc->tmp, n bytes, planes of `plane` bytes) to
 * one LZ4 block or zstd frame.  Returns the size, 0 on error. */
static size_t pac_encode_block(pac_encoder *enc, unsigned char *dst, size_t cap, size_t n,
                               size_t plane)
{
    size_t lits, ns, r;
    const size_t nplanes = n / plane;
    const pac_u64 nmask = nplanes >= 64 ? ~(pac_u64) 0 : (((pac_u64) 1 << nplanes) - 1);
    if (enc->codec == PAC_LZ4) {
        ns = pac_build(enc, enc->tmp, n, plane, 0, &lits);
        return pac_emit_lz4(enc->seq, ns, enc->tmp, n, dst, cap);
    }
    if (cap < ZSTD_FRAMEHEADERSIZE_MAX)
        return 0; /* zstd 1.5.7 ZSTD_compressSequences omits FORWARD_IF_ERROR on ZSTD_writeFrameHeader */
    if (plane >= 5 && (enc->plane_zero & nmask) == nmask)
        return pac_write_zstd_rle_frame(dst, cap, n, 0x00);
    if (plane >= 5 && (enc->plane_one & nmask) == nmask)
        return pac_write_zstd_rle_frame(dst, cap, n, 0xff);
    if (!pac_ensure_wl(enc, n)) /* the window must reach one plane back, whatever zstd would pick */
        return 0;
    if (n >= 8192 && plane >= 2048) {
        size_t k = 0;
        int stop_by_prev = 0;
        while (k < nplanes) {
            const unsigned char *prev = k > 0 ? (enc->tmp + (k - 1) * plane) : NULL;
            int by_prev = 0;
            if (pac_probe_plane(enc->tmp + k * plane, prev, plane, &by_prev) >= 2.0) {
                stop_by_prev = by_prev;
                break;
            }
            k++;
        }
        if (stop_by_prev && k > 0)
            k--;
        if (k > 0) {
            size_t raw_bytes = k * plane, tail_bytes = n - raw_bytes;
            size_t pos = pac_write_zstd_fh(dst, n), raw_done = 0;
            while (raw_done < raw_bytes) {
                size_t b = raw_bytes - raw_done;
                pac_u32 last = 0;
                if (b > ZSTD_BLOCKSIZE_MAX)
                    b = ZSTD_BLOCKSIZE_MAX;
                if (tail_bytes == 0 && raw_done + b == raw_bytes)
                    last = 1;
                if (cap - pos < 3 + b)
                    return 0;
                pac_put_le24(dst + pos, last | (0U << 1) | ((pac_u32) b << 3));
                pos += 3;
                memcpy(dst + pos, enc->tmp + raw_done, b);
                pos += b;
                raw_done += b;
            }
            if (tail_bytes == 0)
                return pos;
            ns = pac_build(enc, enc->tmp, n, plane, raw_bytes, &lits);
            {
                unsigned char s0 = dst[pos - 2], s1 = dst[pos - 1];
                if (cap - pos + 2 < ZSTD_FRAMEHEADERSIZE_MAX)
                    return 0;
                r = ZSTD_compressSequences(enc->cctx_tail, dst + pos - 2, cap - pos + 2,
                                           enc->seq, ns, enc->tmp + raw_bytes, tail_bytes);
                dst[pos - 2] = s0;
                dst[pos - 1] = s1;
                if (ZSTD_isError(r) || r < 2)
                    return 0;
                return pos + (r - 2);
            }
        }
    }
    ns = pac_build(enc, enc->tmp, n, plane, 0, &lits);
    if (lits == 1)
        return pac_write_zstd_rle_frame(dst, cap, n, enc->tmp[0]);
    r = ZSTD_compressSequences(enc->cctx, dst, cap, enc->seq, ns, enc->tmp, n);
    return ZSTD_isError(r) ? 0 : r;
}

/* the lower `keep` bytes of each of n little-endian elements of `elem` bytes */
static void pac_pack(unsigned char *PAC_RESTRICT out, const unsigned char *PAC_RESTRICT in,
                     size_t n, size_t elem, size_t keep)
{
    size_t i = 0;
#if defined(__SSE2__)
    if (elem == 2 && keep == 1) {
        const __m128i m = _mm_set1_epi16(0x00ff);
        for (; i + 16 <= n; i += 16) {
            __m128i a = _mm_and_si128(_mm_loadu_si128((const __m128i *) (in + 2 * i)), m);
            __m128i b = _mm_and_si128(_mm_loadu_si128((const __m128i *) (in + 2 * i + 16)), m);
            _mm_storeu_si128((__m128i *) (out + i), _mm_packus_epi16(a, b));
        }
    } else if (elem == 4 && keep == 2) {
        for (; i + 8 <= n; i += 8) { /* sign-extend the low 16 bits, so that the signed pack keeps them */
            __m128i a = _mm_loadu_si128((const __m128i *) (in + 4 * i));
            __m128i b = _mm_loadu_si128((const __m128i *) (in + 4 * i + 16));
            a = _mm_srai_epi32(_mm_slli_epi32(a, 16), 16);
            b = _mm_srai_epi32(_mm_slli_epi32(b, 16), 16);
            _mm_storeu_si128((__m128i *) (out + 2 * i), _mm_packs_epi32(a, b));
        }
    } else if (elem == 4 && keep == 1) {
        const __m128i m = _mm_set1_epi32(0xff);
        for (; i + 16 <= n; i += 16) {
            __m128i a = _mm_and_si128(_mm_loadu_si128((const __m128i *) (in + 4 * i)), m);
            __m128i b = _mm_and_si128(_mm_loadu_si128((const __m128i *) (in + 4 * i + 16)), m);
            __m128i c = _mm_and_si128(_mm_loadu_si128((const __m128i *) (in + 4 * i + 32)), m);
            __m128i d = _mm_and_si128(_mm_loadu_si128((const __m128i *) (in + 4 * i + 48)), m);
            _mm_storeu_si128((__m128i *) (out + i),
                             _mm_packus_epi16(_mm_packs_epi32(a, b), _mm_packs_epi32(c, d)));
        }
    }
#endif
    for (; i < n; i++)
        memcpy(out + keep * i, in + elem * i, keep);
}

/*
 * The bit transpose of n elements of elem bytes into enc->tmp.  When
 * enc->narrow is enabled on elem > 1 (little endian), scans for all-zero,
 * all-one, and eq-previous planes, transposes only the active lower bytes,
 * materializes invariant upper planes into enc->tmp, and records
 * enc->plane_{zero,one,eq}.  Returns 1 on success, 0 on error.
 */
static int pac_transpose(pac_encoder *enc, const unsigned char *in, size_t n, size_t elem)
{
    size_t keep = elem;
    const size_t nplanes = 8 * elem;
    const size_t plane = n / 8;
    const int use_eq = !(enc->codec == PAC_LZ4 && plane > 65535);
    enc->plane_zero = 0;
    enc->plane_one = 0;
    enc->plane_eq = 0;
    if (enc->narrow && elem > 1 && PAC_LITTLE_ENDIAN) {
        pac_u64 vo = 0, va = 0, vd = 0;
        const pac_u64 nmask = elem == 8 ? ~(pac_u64) 0 : (((pac_u64) 1 << nplanes) - 1);
        size_t p, trail = 0, need_planes;
        pac_scan_invariance(enc->isa, in, n, elem, &vo, &va, &vd);
        for (p = 0; p < nplanes; p++) {
            if (((vo >> p) & 1) == 0)
                enc->plane_zero |= ((pac_u64) 1 << p);
            else if (((va >> p) & 1) == 1)
                enc->plane_one |= ((pac_u64) 1 << p);
            else if (p > 0 && use_eq && ((vd >> (p - 1)) & 1) == 0)
                enc->plane_eq |= ((pac_u64) 1 << p);
        }
        if (vo == 0 || va == nmask) {
            if (enc->codec == PAC_LZ4 || plane < 5)
                memset(enc->tmp, vo == 0 ? 0 : 0xff, n * elem);
            return 1;
        }
        for (p = nplanes - 1; p > 0; p--) {
            pac_u64 pbit = (pac_u64) 1 << p;
            if ((enc->plane_zero | enc->plane_one | enc->plane_eq) & pbit)
                trail++;
            else
                break;
        }
        need_planes = nplanes - trail;
        keep = need_planes <= 8 ? 1 : (need_planes <= 16 ? 2 : (need_planes <= 32 ? 4 : 8));
        if (keep > elem)
            keep = elem;
        if (keep < elem)
            pac_pack(enc->pack, in, n, elem, keep);
    }
    if (enc->transpose((char *) enc->tmp, (const char *) (keep == elem ? in : enc->pack),
                       (char *) enc->scratch, n, keep) != 0)
        return 0;
    if (keep < elem) {
        size_t p;
        for (p = 8 * keep; p < nplanes; p++) {
            pac_u64 pbit = (pac_u64) 1 << p;
            if (enc->plane_zero & pbit)
                memset(enc->tmp + p * plane, 0, plane);
            else if (enc->plane_one & pbit)
                memset(enc->tmp + p * plane, 0xff, plane);
            else
                memcpy(enc->tmp + p * plane, enc->tmp + (p - 1) * plane, plane);
        }
    }
    return 1;
}

static int pac_args_ok(size_t elem_size, size_t block_size)
{
    if (elem_size != 1 && elem_size != 2 && elem_size != 4 && elem_size != 8)
        return 0;
    /* 128 MB: a zstd window (2^27) that every 64-bit decoder accepts */
    return block_size != 0 && block_size % (PAC_MULT * elem_size) == 0 &&
           block_size <= ((size_t) 1 << 27);
}

size_t pac_bound(size_t src_size, size_t elem_size, size_t block_size)
{
    size_t blocks, per, lz;
    if (!pac_args_ok(elem_size, block_size) || src_size % elem_size)
        return 0;
    blocks = src_size / block_size + 1;
    per = ZSTD_compressBound(block_size);
    lz = block_size + block_size / 255 + 16;
    if (lz > per)
        per = lz;
    /* a zstd block header for each of up to 64 planes and for each 128 kB */
    per += 256 + 3 * (block_size >> 17);
    return 12 + blocks * (4 + per) + PAC_MULT * elem_size;
}

size_t pac_compress_chunk(pac_encoder *enc, void *dst_, size_t cap, const void *src_,
                          size_t size, size_t elem, size_t block)
{
    unsigned char *dst = (unsigned char *) dst_;
    const unsigned char *src = (const unsigned char *) src_;
    size_t n = 12, done = 0;
    if (!enc || !enc->transpose || !pac_args_ok(elem, block) || size % elem || cap < 12)
        return 0;
    if (!pac_reserve(enc, block))
        return 0;
    pac_put_be(dst, (pac_u64) size, 8);
    pac_put_be(dst + 8, (pac_u64) block, 4);
    while (done < size) {
        size_t b = size - done < block ? size - done : block;
        size_t left = b % (PAC_MULT * elem), nb = b - left;
        if (nb) {
            size_t c;
            if (!pac_transpose(enc, src + done, nb / elem, elem) || cap - n < 4)
                return 0;
            c = pac_encode_block(enc, dst + n + 4, cap - n - 4, nb, nb / (8 * elem));
            if (!c)
                return 0;
            pac_put_be(dst + n, (pac_u64) c, 4);
            n += 4 + c;
        }
        if (left) { /* fewer than 8 elements at the end are stored as they are */
            if (cap - n < left)
                return 0;
            memcpy(dst + n, src + done + nb, left);
            n += left;
        }
        done += b;
    }
    return n;
}

size_t pac_repack_chunk(pac_encoder *enc, void *dst_, size_t cap, const void *src_,
                        size_t src_size, int src_codec, size_t elem, size_t block)
{
    unsigned char *dst = (unsigned char *) dst_;
    const unsigned char *sp = (const unsigned char *) src_;
    const unsigned char *send = sp + src_size;
    const size_t nplanes = 8 * elem;
    pac_u64 total, done = 0;
    size_t sblock, n = 12;
    if (!enc || !pac_args_ok(elem, block) || cap < 12 || src_size < 12)
        return 0;
    if (src_codec != PAC_LZ4 && src_codec != PAC_ZSTD)
        return 0;
    if (src_codec == PAC_ZSTD && !enc->dctx) {
        enc->dctx = ZSTD_createDCtx();
        if (!enc->dctx)
            return 0;
    }
    total = pac_get_be(sp, 8);
    sblock = (size_t) pac_get_be(sp + 8, 4);
    sp += 12;
    if (sblock == 0) /* "the filter's default": 8192 bytes, in whole multiples of 8 elements */
        sblock = (8192 / elem / PAC_MULT) * PAC_MULT * elem;
    if (!pac_args_ok(elem, sblock) || block % sblock || total % elem)
        return 0;
    if (!pac_reserve(enc, block))
        return 0;
    pac_put_be(dst, total, 8);
    pac_put_be(dst + 8, (pac_u64) block, 4);
    while (done < total) {
        size_t b = total - done < block ? (size_t) (total - done) : block;
        size_t left = b % (PAC_MULT * elem), nb = b - left;
        if (nb) {
            const size_t plane = nb / nplanes;
            size_t filled = 0, remaining = nb, c;
            enc->plane_zero = 0;
            enc->plane_one = 0;
            enc->plane_eq = 0;
            while (remaining) { /* the source blocks of this block: decode, regroup the planes */
                size_t sb = remaining < sblock ? remaining : sblock, csize, got, p, k;
                unsigned char *out = sb == nb ? enc->tmp : enc->scratch;
                if ((size_t) (send - sp) < 4)
                    return 0;
                csize = (size_t) pac_get_be(sp, 4);
                sp += 4;
                if ((size_t) (send - sp) < csize || csize > 0x7fffffffUL)
                    return 0;
                if (src_codec == PAC_LZ4) {
                    int r = LZ4_decompress_safe((const char *) sp, (char *) out, (int) csize,
                                                (int) sb);
                    got = r < 0 ? 0 : (size_t) r;
                } else {
                    got = ZSTD_decompressDCtx(enc->dctx, out, sb, sp, csize);
                }
                if (got != sb) /* zstd's error codes are huge values */
                    return 0;
                sp += csize;
                if (sb != nb) {
                    p = sb / nplanes;
                    for (k = 0; k < nplanes; k++)
                        memcpy(enc->tmp + k * plane + filled, out + k * p, p);
                    filled += p;
                }
                remaining -= sb;
            }
            if (cap - n < 4)
                return 0;
            c = pac_encode_block(enc, dst + n + 4, cap - n - 4, nb, plane);
            if (!c)
                return 0;
            pac_put_be(dst + n, (pac_u64) c, 4);
            n += 4 + c;
        }
        if (left) {
            if ((size_t) (send - sp) < left || cap - n < left)
                return 0;
            memcpy(dst + n, sp, left);
            sp += left;
            n += left;
        }
        done += b;
    }
    return n;
}
