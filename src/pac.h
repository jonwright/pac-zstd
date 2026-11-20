/*
 * pac-zstd (pac): a fast, bit-plane-aware encoder for the bitshuffle HDF5
 * chunk format (filter 32008) with Zstandard (or LZ4) blocks, for detector
 * frames.
 *
 * The chunks are ordinary bitshuffle-lz4 / bitshuffle-zstd: any existing
 * reader decodes them.  Only the encoder is different: after the bit
 * transpose the repeats are at known places (a run of equal bytes, or the
 * same bytes as the bit plane before), so no hash-table search is made.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Jon Wright, ESRF.  Code written by Claude Fable 5.2.
 */
#ifndef PAC_H
#define PAC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAC_VERSION "0.2.0"

/* block compressor of a chunk */
#define PAC_LZ4 1
#define PAC_ZSTD 2

/* Default block sizes, in bytes; the block size is an argument of every call,
 * so these are only the recommended values.  Both are multiples of 8 kB (the
 * block the detector software writes, so that pac_repack_chunk can regroup its
 * blocks) and not multiples of 64 kB (bit planes whose length is a multiple of
 * 4096 bytes make the transpose slow).
 *   PAC_LZ4_BLOCK   56 kB: lz4 is the fast option; its speed peaks below 64 kB
 *                   (cache) and its ratio hardly grows beyond.
 *   PAC_ZSTD_BLOCK 248 kB: zstd is the small option; the ratio of 256 kB. */
#define PAC_LZ4_BLOCK 57344
#define PAC_ZSTD_BLOCK 253952

typedef struct pac_encoder pac_encoder;

/* The bit transpose of `size` elements of `elem_size` bytes, as
 * bitshuf_encode_block() of https://github.com/kalcutter/bitshuffle:
 * returns 0 on success.  `scratch` holds size * elem_size bytes. */
typedef int (*pac_transpose_fn)(char *out, const char *in, char *scratch, size_t size,
                                size_t elem_size);

/* codec = PAC_LZ4 or PAC_ZSTD.  One encoder per thread; it keeps its buffers
 * and can be used for any number of chunks.  NULL on failure. */
pac_encoder *pac_create(int codec);
void pac_free(pac_encoder *enc);

/* 1 (the default on x86, 0 elsewhere): when the upper bytes of every pixel of a block are zero
 * (uint32 counts below 65536, ...) only the lower bytes are transposed.
 * The chunk is the same either way. */
void pac_set_narrow(pac_encoder *enc, int on);

/* The transpose used by pac_compress_chunk.  The default is kcb's
 * bitshuf_encode_block if the library was built with it, else none. */
void pac_set_transpose(pac_encoder *enc, pac_transpose_fn fn);

/* An upper bound of the chunk for src_size bytes, or 0 if the arguments are
 * not valid (block_size must be a multiple of 8 * elem_size). */
size_t pac_bound(size_t src_size, size_t elem_size, size_t block_size);

/* Pixels to chunk: src_size bytes of elem_size-byte pixels (1, 2, 4 or 8)
 * become a bitshuffle HDF5 chunk with blocks of block_size bytes.
 * Returns the chunk size, or 0 on error (dst too small: see pac_bound). */
size_t pac_compress_chunk(pac_encoder *enc, void *dst, size_t dst_capacity, const void *src,
                          size_t src_size, size_t elem_size, size_t block_size);

/* An already bit-transposed block (nbytes bytes, a multiple of 8 * elem_size,
 * elem_size in {1, 2, 4, 8}, nbytes <= 128 MB) to one LZ4 block or zstd frame:
 * the trailing zero planes are stitched into one run, nothing is searched but
 * offset 1 and `plane`.  The block is returned to `dst` (no chunk header, the
 * raw LZ4 block or zstd frame), and is decodable by an existing reader.
 * Returns the compressed size, or 0 on error or if dst_capacity is too small
 * (enc can be reused for the next block). */
size_t pac_compress_block(pac_encoder *enc, void *dst, size_t dst_capacity,
                          const void *bitshuffled_src, size_t nbytes, size_t elem_size);

/* Chunk to chunk without any bit transpose: a bitshuffle chunk whose blocks
 * are src_codec (PAC_LZ4 as the detector writes, or PAC_ZSTD) is decoded
 * block by block, the bit planes of its blocks are regrouped into blocks of
 * block_size bytes (a multiple of the chunk's own block size, 8192 when its
 * header says 0) and encoded
 * with the encoder's codec.  Returns the new chunk size, or 0 on error
 * (corrupt input included). */
size_t pac_repack_chunk(pac_encoder *enc, void *dst, size_t dst_capacity, const void *src,
                        size_t src_size, int src_codec, size_t elem_size, size_t block_size);

/* The instruction set of the parse: "avx512bw", "avx2", "sse2", "neon", "vsx"
 * or "generic". */
const char *pac_isa(const pac_encoder *enc);

/* For tests: use variant 0 generic, 1 sse2, 2 avx2, 3 avx512bw, 4 neon, 5 vsx.
 * Returns 0 and changes nothing if it was not compiled or this CPU cannot run
 * it. */
int pac_force_isa(pac_encoder *enc, int isa);

#ifdef __cplusplus
}
#endif

#endif /* PAC_H */
