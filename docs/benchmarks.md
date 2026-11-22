# Benchmarks

Everything here is made by the scripts in `bench/` from public data sets on Zenodo (see
[Reproducing](#reproducing)).  Unless stated otherwise: AMD EPYC 9454, one core, gcc 13.3 `-O3`,
frames in memory.  Full tables: [results.md](results.md); raw numbers in `bench/results/`.

**Test cases.**  Three data sets, each with the detector gaps at 0 and at the type's maximum (as
the detector writes them), the CeO2 frames both as the uint32 they were stored as and as uint16
(their counts fit), and one s3DXRD scan file exactly as the detector software wrote it (uint32
counts, gaps at 65535).  Speeds are in GB/s of uncompressed pixels (the raw data rate) per
core, to compare with a network link or a detector's output.

**Reference.**  "stock" is the usual way to write these chunks, built from the same vendored
libraries: kcb's bit transpose, then `LZ4_compress_default` or zstd level 2 on each block.  So a
stock and a plane-aware encoder differ only in how the transposed block is compressed.  Every chunk
was decoded (LZ4 or zstd, then kcb's untranspose) and compared with the pixels, and at the default
block size also read back through h5py and hdf5plugin.  Decoding is timed with those same stock
decoders; the speed of other readers will differ.

## Encoding, at the default block sizes

Ratio / encode GB/s per core:

| | stock lz4, 8 kB (detector today) | plane-aware lz4, 56 kB | stock zstd level 2, 248 kB | plane-aware zstd, 248 kB |
|---|---|---|---|---|
| thaumatin, gaps 65535 | 21.7 / 3.16 | 22.5 / 4.58 | 50.0 / 2.12 | 67.1 / 3.22 |
| thaumatin, gaps 0 | 27.5 / 3.49 | 25.1 / 5.50 | 62.9 / 2.24 | 78.6 / 3.98 |
| silicon, gaps 0 | 59.7 / 4.73 | 62.0 / 7.49 | 189 / 3.13 | 247 / 6.00 |
| silicon, gaps 65535 | 39.2 / 4.07 | 41.5 / 5.28 | 129 / 2.89 | 147 / 4.04 |
| CeO2 uint32, gaps 0 | 4.55 / 3.79 | 4.57 / 5.97 | 4.89 / 1.80 | 5.12 / 2.37 |
| CeO2 uint32, gaps 2^32-1 | 4.45 / 3.52 | 4.50 / 4.75 | 4.85 / 1.76 | 5.09 / 2.12 |
| CeO2 uint16, gaps 0 | 2.31 / 2.77 | 2.32 / 3.89 | 2.45 / 1.19 | 2.57 / 1.43 |
| CeO2 uint16, gaps 65535 | 2.26 / 2.50 | 2.28 / 3.30 | 2.44 / 1.16 | 2.56 / 1.31 |
| s3DXRD, uint32, gaps 65535 | 33.0 / 4.16 | 31.1 / 6.13 | 83.5 / 2.75 | 100 / 4.44 |

Plane-aware lz4 is the fastest encoder everywhere (1.1 to 1.5 times stock lz4 at 56 kB), but its
ratio is 1 to 20 % below stock lz4, which also finds other repeats; with zstd the
entropy coder makes up for that and more.  On dense frames plane-aware zstd automatically emits
leading incompressible bit planes as raw Zstd blocks (`bt_raw`) to keep encode speed high while
preserving the ratio on compressible planes.

## Gaps at zero or at the maximum, and empty frames

Detectors write their gaps and bad pixels as the type's maximum.  After plane-aware zstd (248 kB
blocks) that costs, per frame:

| | gaps at maximum | gaps at 0 | cost of the gaps |
|---|---:|---:|---:|
| thaumatin (Eiger2 16M, uint16) | 539 kB | 460 kB | 79 kB, 17 % |
| silicon (Eiger2 4M, uint16) | 60.9 kB | 36.2 kB | 24.7 kB, 68 % |
| CeO2 (Eiger2 4M, uint32) | 3516 kB | 3492 kB | 24 kB, 0.7 % |

A saturated mask also takes away the narrow transpose (the upper bytes are no longer zero), which
is why the encoders are slower in those rows.

An empty frame (`bench/empty_frame.py`, [empty_frame.md](empty_frame.md)), bytes:

| empty frame | stock lz4, 8 kB | plane-aware lz4, 56 kB | stock zstd level 2, 248 kB | plane-aware zstd, 248 kB |
|---|---:|---:|---:|---:|
| Eiger2 4M, uint16, gaps 0 | 51321 | 44160 | 1086 | 2417 |
| Eiger2 4M, uint16, gaps at maximum | 100152 | 82282 | 11049 | 13750 |
| Eiger2 4M, uint32, gaps 0 | 102631 | 103284 | 2138 | 6974 |
| Eiger2 4M, uint32, gaps at maximum | 156381 | 141440 | 24492 | 19289 |
| Eiger2 16M, uint16, gaps 0 | 207638 | 178660 | 4298 | 9590 |
| Eiger2 16M, uint16, gaps at maximum | 507798 | 269424 | 124407 | 45080 |

## Block size

The defaults were chosen from a sweep of 8 to 512 kB (every value in [results.md](results.md#every-block-size)):

- The bit transpose is 30 to 40 % slower when a bit plane is a multiple of 4096 bytes long (uint16
  blocks of 64, 128, 192, 256, 384 and 512 kB; uint32 blocks of 128, 256, 384 and 512 kB),
  probably because its output streams then fall in the same set of the L1 cache.  So `PAC_ZSTD_BLOCK` is 248 kB and `PAC_LZ4_BLOCK` 56 kB, both multiples of 8 kB
  so that the detector's blocks can be regrouped.
- Zstandard: the ratio keeps rising with the block size, and plane-aware zstd is poor below about
  64 kB (every plane becomes a tiny zstd block).
- LZ4: the ratio is flat from about 48 kB and the speed peaks just below 64 kB.  For uint16 a
  block above 512 kB has planes longer than LZ4's 65535-byte offset.

## Repacking detector files (lz4 8 kB to zstd 248 kB)

Ratios and speed per core are in the README, next to decoding the lz4 and writing stock zstd
level 2; every output block size is in [results.md](results.md#transcoding-lz4-8-kb-to-zstd).
In GB/s of lz4 read per core: 0.08 to 0.14 for sparse frames, 0.47 to 0.58 for dense ones.

**Threads**, one encoder each (`bench/bench_repack.py --as-detector`,
`bench/results/repack_epyc9454.txt`, 48 cores): 8 threads repack 17 to 32 GB/s of pixels, 32
threads 46 to 94.

**Against a network link** (25 GbE = 3.1 GB/s, 100 GbE = 12.5 GB/s) the number that matters is the
bytes that cross it, the lz4 chunks:

- dense frames (CeO2 uint32): 0.46 GB/s of lz4 per core, so about 7 cores keep up with 25 GbE
  (measured: 3.7 GB/s on 8 threads) and about 40 with 100 GbE (10.0 GB/s on 32 threads);
- sparse frames (thaumatin): 0.15 GB/s of lz4 per core, so about 21 cores for a 25 GbE link full of
  lz4 (measured: 3.2 GB/s on 32 threads).  A link full of 22-fold compressed frames is 69 GB/s of
  pixels, more than a detector delivers; counted in pixels, 1 core does 25 Gb/s and 4 cores
  100 Gb/s.

**8-bit at 1.1 kHz** (Eiger2 4M: 4.9 GB/s).  There is no real 8-bit data on Zenodo, so
`bench_repack.py --uint8 2` divides the counts by 2 and clips at 255 (0.1 % of CeO2 pixels
clipped).  One thread per row:

| frames | lz4 ratio | zstd ratio | raw GB/s, 1 thread | 4 threads |
|---|---|---|---|---|
| silicon / 2 (sparse) | 155 | 1604 | 7.7 | 27.2 |
| CeO2 / 2 (dense) | 1.37 | 1.53 | 0.86 | 3.4 |

Sparse 8-bit frames need a single core; dense ones need about 6 cores (at the pre-hybrid baseline).

**Compute for space**: per TB of lz4 on disk, thaumatin takes 1.9 core-hours and frees 0.68 TB;
silicon 2.8 to 3.8 core-hours for 0.73 to 0.76 TB; the s3DXRD scan 2.3 core-hours for 0.67 TB;
CeO2 0.5 to 0.6 core-hours for 0.10 to 0.12 TB.  That is 3 to 5 core-hours per TB freed, on sparse
and dense data alike.

## Other machines

**POWER9** (`bench/results/results_power9.json`): off x86, kcb's transpose is plain C and sets the
speed.  Every encoder runs at 0.25 to 1.1 GB/s and the plane-aware ones are within 10 % of
stock (lz4 a little slower, zstd the same or faster), so the gain is in ratio only (zstd 248 kB:
thaumatin 67 against 50, silicon 247 against 189, CeO2 uint16 2.57 against 2.45).  There is no
cache cliff from 8 to 512 kB, so the narrow transpose is on by default only on x86.

## Reproducing

    git submodule update --init
    make                              # build/libpacbench.so
    python3 bench/get_data.py         # 2.6 GB from Zenodo into bench/data, checked by md5
    python3 bench/sweep.py --core 20  # about 15 minutes; writes bench/results/results.json
    python3 bench/plot.py             # docs/repack_gain.png and docs/results.md
    python3 bench/plot_summary.py     # docs/pareto.png
    python3 bench/bench_repack.py bench/data/thaumatin_9_1_000001.h5 data --as-detector
    python3 bench/empty_frame.py      # docs/empty_frame.md

    python3 bench/check_files.py /my/data   # do my own HDF5 files go through, and how small do they get?

`python3 bench/get_data.py --s3dxrd` adds the optional test case: it fetches an 18 GB archive of
the Battery Imaging Library (S3DXRD at ESRF ID11, data set 1,
[10.5281/zenodo.19453256](https://doi.org/10.5281/zenodo.19453256)) and extracts one scan file,
uint32 frames exactly as the detector software wrote them.

Python needs numpy, h5py, hdf5plugin and matplotlib.  The data: `thaumatin_9_1_000001.h5` (Eiger2
16M, [10.5281/zenodo.2412270](https://doi.org/10.5281/zenodo.2412270)); `silicon_u16_zstd2_256k.h5`,
`ceo2_u32_zstd2_256k.h5` and `mask.h5` (Eiger2 4M CdTe at ESRF ID11,
[10.5281/zenodo.23361659](https://doi.org/10.5281/zenodo.23361659)).

## Status

A working prototype, measured on four public data sets.

- Parse kernels: SSE2, AVX2 and AVX-512BW on x86; VSX on POWER (little endian), tested on POWER9;
  NEON on AArch64, which passes the tests under qemu-aarch64 and has not been run on ARM hardware.
  The portable C path passes on x86 (64 and 32 bit), POWER9 and a 32-bit Raspberry Pi (ARMv7).
  The narrow-transpose packing is SSE2 or plain C.  Big-endian is not tested (kcb refuses it).
- Windows: the tests pass built with Visual C++ 2008 (32 bit) and VS 2019; see
  [build.md](build.md).
- CI: Linux x86 (gcc, clang, sanitizers, an old gcc), AArch64, macOS, 32-bit x86, ARMv7 and
  ppc64le in containers, MSVC 64 and 32 bit.
- The tests are round trips over synthetic patterns, sizes and block sizes, and damaged input to
  the repack, also under the sanitizers.  No fuzzing campaign.
- An all-zero frame costs more than with stock zstd (see the empty-frame table above).
