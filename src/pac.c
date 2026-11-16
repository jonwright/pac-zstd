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

/* ------------------------------------------------------------------ */
/* the parse, once per instruction set (pac_parse.inl)                 */

typedef size_t (*pac_parse_fn)(const unsigned char *PAC_RESTRICT base, size_t from, size_t to,
                               size_t plane, ZSTD_Sequence *PAC_RESTRICT seq, size_t *lit);

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
    int raw_percent;
    int narrow;
    int isa;
    pac_parse_fn parse;
    pac_transpose_fn transpose;
    ZSTD_CCtx *cctx;     /* literals: Huffman where it gains */
    ZSTD_CCtx *cctx_raw; /* literals: raw */
    size_t window_for;   /* the block size the zstd window was last set for */
    /* for blocks of up to cap_block bytes: */
    size_t cap_block;
    unsigned char *tmp;     /* the transposed block, PAC_PAD more bytes */
    unsigned char *scratch; /* for the transpose; a decoded source block in pac_repack_chunk */
    unsigned char *pack;    /* the lower bytes of the pixels of a block */
    ZSTD_Sequence *seq;
    /* every 16th literal byte of the block being parsed, for the choice of raw literals */
    unsigned long hist[256];
    unsigned long hist_n;
};

#if defined(PAC_WITH_KCB)
extern int bitshuf_encode_block(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);
#endif

static ZSTD_CCtx *pac_make_cctx(int raw_literals)
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
    if (raw_literals)
        e |= ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_literalCompressionMode,
                                                 ZSTD_ps_disable));
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
    enc->raw_percent = 0;
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
        enc->cctx_raw = pac_make_cctx(1);
        if (!enc->cctx || !enc->cctx_raw) {
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
    ZSTD_freeCCtx(enc->cctx_raw);
    free(enc->tmp);
    free(enc->scratch);
    free(enc->pack);
    free(enc->seq);
    free(enc);
}

void pac_set_raw_literals(pac_encoder *enc, int percent) { enc->raw_percent = percent; }
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

/*
 * Parse the transposed block s[0..n), bit planes of `plane` bytes, into
 * enc->seq in zstd's form with explicit block delimiters: a plane of at
 * least 1024 bytes that is more than 1/20 literals gets a zstd block of its
 * own (its own Huffman table), the others are grouped up to zstd's block size.  The planes from
 * byte zero_from on are known to be all zero.  Returns the number of
 * sequences; *lits_out = the literal bytes.
 */
static size_t pac_build(pac_encoder *enc, const unsigned char *s, size_t n, size_t plane,
                        size_t zero_from, size_t *lits_out)
{
    ZSTD_Sequence *seq = enc->seq;
    size_t ns = 0, lit = 0, zblock = 0, total = 0, from;
    int alone = 0; /* the zstd block being built is a dense plane on its own */
    const size_t piece = plane < ZSTD_BLOCKSIZE_MAX ? plane : ZSTD_BLOCKSIZE_MAX;
    /* LZ4 offsets are 16 bits */
    const size_t use_plane = !(enc->codec == PAC_LZ4 && plane > 65535);
    memset(enc->hist, 0, sizeof(enc->hist));
    enc->hist_n = 0;
    for (from = 0; from < n; from += piece) {
        size_t to = from + piece < n ? from + piece : n;
        size_t lit0 = lit, l = lit, k, q, lits = 0;
        int dense;
        if (from > zero_from && to - from >= 4) { /* zeros after zeros: one run, nothing to scan */
            seq[ns].litLength = (unsigned) lit;
            seq[ns].matchLength = (unsigned) (to - from);
            seq[ns].offset = 1;
            seq[ns].rep = 0;
            k = 1;
            l = 0;
        } else {
            k = enc->parse(s, from, to, use_plane && from >= plane ? plane : 0, seq + ns, &l);
        }
        for (q = 0; q < k; q++)
            lits += seq[ns + q].litLength;
        lits += l - lit0; /* the literals of this piece only */
        if (lits && enc->codec == PAC_ZSTD) { /* sample them: the runs between the matches */
            size_t cur = from, p;
            for (q = 0; q <= k; q++) {
                /* the first run began lit0 bytes before this piece; the last one ends the piece */
                const size_t end = q == k ? to : cur + seq[ns + q].litLength - (q == 0 ? lit0 : 0);
                for (p = cur; p < end; p += 16) {
                    enc->hist[s[p]]++;
                    enc->hist_n++;
                }
                if (q < k)
                    cur = end + seq[ns + q].matchLength;
            }
        }
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
            zblock = 0;
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

/*
 * Store the literals of the block just parsed raw?  Yes if Huffman coding would
 * save less than raw_percent % of them: the order-0 entropy of the sampled
 * literal bytes says how much it would save.
 */
static int pac_raw_literals(const pac_encoder *enc)
{
    double bits = 0, n = (double) enc->hist_n;
    int i;
    if (enc->raw_percent <= 0 || !enc->hist_n)
        return 0;
    if (enc->raw_percent >= 100)
        return 1;
    for (i = 0; i < 256; i++)
        if (enc->hist[i])
            bits += (double) enc->hist[i] * (log(n) - log((double) enc->hist[i]));
    bits /= n * log(2.0); /* per literal byte */
    return 100.0 * (1.0 - bits / 8.0) < (double) enc->raw_percent;
}

/* One transposed block (in enc->tmp, n bytes, planes of `plane` bytes) to
 * one LZ4 block or zstd frame.  Returns the size, 0 on error. */
static size_t pac_encode_block(pac_encoder *enc, unsigned char *dst, size_t cap, size_t n,
                               size_t plane, size_t zero_from)
{
    size_t lits, ns, r;
    ns = pac_build(enc, enc->tmp, n, plane, zero_from, &lits);
    if (enc->codec == PAC_LZ4)
        return pac_emit_lz4(enc->seq, ns, enc->tmp, n, dst, cap);
    if (enc->window_for != n) { /* the window must reach one plane back, whatever zstd would pick */
        int wl = 10;
        while (((size_t) 1 << wl) < n)
            wl++;
        /* parameters can only be set between frames: end the session of the last one */
        ZSTD_CCtx_reset(enc->cctx, ZSTD_reset_session_only);
        ZSTD_CCtx_reset(enc->cctx_raw, ZSTD_reset_session_only);
        if (ZSTD_isError(ZSTD_CCtx_setParameter(enc->cctx, ZSTD_c_windowLog, wl)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(enc->cctx_raw, ZSTD_c_windowLog, wl)))
            return 0;
        enc->window_for = n;
    }
    r = ZSTD_compressSequences(pac_raw_literals(enc) ? enc->cctx_raw : enc->cctx, dst, cap,
                               enc->seq, ns, enc->tmp, n);
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
 * The bit transpose of n elements of elem bytes into enc->tmp.  If the upper
 * bytes of every element are zero only the lower ones are transposed (packed
 * into enc->pack first): their planes are the same and come first, and the
 * planes of the zero bytes are zero.  Returns the number of bytes of tmp that
 * are not known to be zero, 0 on error.
 */
static size_t pac_transpose(pac_encoder *enc, const unsigned char *in, size_t n, size_t elem)
{
    size_t keep = elem;
    if (enc->narrow && elem > 1 && PAC_LITTLE_ENDIAN) {
        /* n is a multiple of 8, so the block is whole 64-bit words: the OR of all of them,
         * folded to one element */
        const size_t words = n * elem / 8;
        pac_u64 o0 = 0, o1 = 0, v = 0;
        size_t i;
        for (i = 0; i + 2 <= words; i += 2) {
            o0 |= pac_load64(in + 8 * i);
            o1 |= pac_load64(in + 8 * i + 8);
        }
        if (i < words)
            o0 |= pac_load64(in + 8 * i);
        o0 |= o1;
        for (i = 0; i < 8; i += elem)
            v |= o0 >> (8 * i);
        if (elem < 8)
            v &= ((pac_u64) 1 << (8 * elem)) - 1;
        keep = v >> 32 ? 8 : (v >> 16 ? 4 : (v >> 8 ? 2 : 1));
        if (keep < elem)
            pac_pack(enc->pack, in, n, elem, keep);
    }
    if (enc->transpose((char *) enc->tmp, (const char *) (keep == elem ? in : enc->pack),
                       (char *) enc->scratch, n, keep) != 0)
        return 0;
    if (keep != elem)
        memset(enc->tmp + n * keep, 0, n * (elem - keep));
    return n * keep;
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
            size_t nonzero = pac_transpose(enc, src + done, nb / elem, elem), c;
            if (!nonzero || cap - n < 4)
                return 0;
            c = pac_encode_block(enc, dst + n + 4, cap - n - 4, nb, nb / (8 * elem), nonzero);
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
                    got = ZSTD_decompress(out, sb, sp, csize);
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
            c = pac_encode_block(enc, dst + n + 4, cap - n - 4, nb, plane, nb);
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
