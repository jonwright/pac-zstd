# Plan: Integrating, Testing, and Benchmarking `pac-zstd` within `c-blosc2` (`_neon` revision)

This document revises `pac_for_blosc2.md` after auditing the code references and memory-safety/build/benchmarking assumptions in both `pac-zstd` and `c-blosc2`. It is designed to be executed on a dedicated build/benchmark machine.

*Drafted by Google Hydrogen, modified by Google Neon for authorship; all implementation work is done by ESRF DeepSeek.*

---

## 0. Assessment: Is This Worth Executing?

**Yes — strongly recommended as an experimental integration and benchmark, and viable as an opt-in fast path in `c-blosc2`.**

- **Zero wire-format risk & ecosystem reach (no new codec/decoder):** `pac-zstd` emits standard Zstandard frames (`ZSTD_compressSequences`), and `c-blosc2`'s `bitshuffle()` (`blosc/bitshuffle-generic.c` and SIMD variants) produces the exact same `[elem_size][8][size/8]` bit-plane layout as `kcb`/`bitshuffle`. By hooking into `c-blosc2`'s existing `BLOSC_BITSHUFFLE + BLOSC_ZSTD` path, the `pac-zstd` idea can be shared across the entire Blosc2 ecosystem (`python-blosc2`, `hdf5plugin.Blosc2` / HDF5 filter 32026, PyTables, Zarr/Blosc2) without registering a new codec ID or shipping a new decoder—any unmodified `c-blosc2` reader already decodes the chunks.
- **Clear expected win:** Replacing Zstd's hash-table matchfinder with `pac-zstd`'s SIMD bit-plane parser (`offset = 1` and `offset = plane`) targets the main bottleneck of `BLOSC_BITSHUFFLE + BLOSC_ZSTD` compression on scientific/detector integer data while preserving Zstd's fast decode speed and entropy coding.
- **Two real caveats to design around:**
  1. **Workload scope:** `pac-zstd` only searches offsets `1` and `plane`. It shines on bitshuffled integer/detector data, but stock `ZSTD_compressCCtx` can beat it in ratio on non-bitshuffled data, high-entropy floats, or data with other strides. Keep it **opt-in** (env toggle / flag) rather than unconditionally replacing high-`clevel` Zstd.
  2. **Static Zstd requirement:** `pac-zstd` uses `ZSTD_STATIC_LINKING_ONLY` (`ZSTD_compressSequences`, `ZSTD_c_blockDelimiters`, `ZSTD_c_repcodeResolution`). This works out of the box in `c-blosc2`'s default `BLOSC_DEPENDENCY_MODE=BUNDLED` mode (which fetches `zstd` 1.5.7 as `libzstd_static`), but **will fail to link** against system shared `libzstd.so` (`BLOSC_DEPENDENCY_MODE=EXTERNAL` / `PREFER_EXTERNAL_ZSTD=ON`). Gate the feature in CMake accordingly.

---

## 1. Problems Found in the Original Plan (and Fixes)

1. **Critical memory-safety bug (`PAC_PAD = 64` out-of-bounds read):**
   - The original plan proposed passing `bitshuffled_src` directly into `pac_build()`.
   - `pac-zstd/src/pac_parse.inl:155` requires `base[to .. to + 63]` to be readable because `eq64(base + i, ...)` unconditionally loads 64 bytes even when `to - i < 64`. Passing an unpadded pointer directly causes a **heap-buffer-overflow** (up to 63 bytes past `nbytes`; fails ASan and risks segfaults at page boundaries, including in `c-blosc2` for small blocks where `3 * ebsize < 64`).
   - **Fix:** Either (a) handle the final `< 64` bytes of the block safely in `pac_parse.inl` using a 64-byte stack tail buffer (zero extra full-block `memcpy`, safe for all callers), or (b) `memcpy` into `enc->tmp` (allocated with `+ PAC_PAD`) inside `pac_compress_block()`. Option (a) is cleaner and avoids an extra L2 memory pass per block.
2. **Incomplete dispatch guard in `zstd_wrap_compress()`:**
   - The original plan assumed `BLOSC_BITSHUFFLE` always yields `neblock == bsize`. In fact, `split_block()` (`c-blosc2/blosc/stune.c:197`) returns `1` unconditionally when `context->splitmode == BLOSC_ALWAYS_SPLIT`, and `context->filter_flags & BLOSC_DOBITSHUFFLE` remains set even if another filter (e.g., `BLOSC_DELTA`) runs *after* `BLOSC_BITSHUFFLE`.
   - **Fix:** Dispatch to `pac_compress_block()` only when:
     - `BLOSC_BITSHUFFLE` is the **last active filter** (`last_filter(context, 'c')` has `context->filters[idx] == BLOSC_BITSHUFFLE`),
     - the block is **not split** (`(context->header_flags & 0x10) != 0`, i.e., `dont_split` is set / `nstreams == 1`),
     - `!context->use_dict`,
     - `context->typesize` is `1`, `2`, `4`, or `8`, and
     - `input_length > 0 && input_length <= (1u << 27) && (input_length % (8 * (size_t)context->typesize)) == 0`.
3. **Thread-context lifecycle omission:**
   - `free_thread_context()` (`c-blosc2/blosc/blosc2.c:2648`) delegates to `destroy_thread_context()` (`blosc2.c:2629`), and initialization happens in `init_thread_context()` (`blosc2.c:2583`). Initialize `thread_context->pac_enc = NULL;` in `init_thread_context()` and call `pac_free(thread_context->pac_enc);` in `destroy_thread_context()`.
4. **Unused per-thread scratch allocation in `pac_reserve()`:**
   - `pac_reserve()` (`pac-zstd/src/pac.c:404`) allocates `tmp`, `scratch`, `pack`, and `seq` (~3x `blocksize` plus sequences per thread). If `pac_compress_block()` does not run transpose/pack (and does not copy into `enc->tmp`), add a `pac_reserve_seq()` helper that only allocates `enc->seq` to avoid wasting tens of MB across Blosc2's thread pool.
5. **CMake & dependency caveats:**
   - `pac-zstd/src/pac.c:22` unconditionally includes `"lz4.h"` (used by `pac_repack_chunk`); `c-blosc2` always builds with LZ4, so `lz4.h` is available on the include path (and `libm` for `log()` in `pac_raw_literals()` is already linked on Linux in `c-blosc2/blosc/CMakeLists.txt:176`).
   - Gate `HAVE_PAC_ZSTD` in CMake on `NOT DEACTIVATE_ZSTD AND (ZSTD_FETCHED OR TARGET zstd::libzstd_static)` behind an explicit option (e.g., `option(BLOSC_ENABLE_PAC_ZSTD ... OFF)`).
6. **Code reference & benchmark inaccuracies fixed:**
   - Instrumentation flag is `BLOSC2_INSTR_CODEC` (`c-blosc2/include/blosc2.h:286`, set via `cparams.instr_codec`), not `BLOSC_INSTR_CODEC`.
   - `blosc_stune_next_blocksize()` (`c-blosc2/blosc/stune.c:66-109`) selects `32 KB` (`clevel=1`) up to **`1 MB`** (`clevel=9`) for `BLOSC_ZSTD` (`is_HCR == true`), not `512 KB`.
   - `c-blosc2/bench/bench_bitshuffle_pair.c` is a `dlopen` microbenchmark for `bshuf_trans_bit_elem_NEON` only—not a model for a Blosc2 real-data compression benchmark.
   - `pac-zstd/bench/get_data.py` downloads **HDF5 (`.h5`)** files (`thaumatin_9_1_000001.h5`, `silicon_u16_zstd2_256k.h5`, `ceo2_u32_zstd2_256k.h5`, plus optional 18 GB `--s3dxrd`). A pure C binary linking `blosc2_static` cannot read `.h5` without `libhdf5`; instead, use `h5py` + `hdf5plugin` to read the frames and benchmark directly through `python-blosc2` (built against the patched `c-blosc2`) or `hdf5plugin.Blosc2`, or dump raw `.bin` frames once if a pure C harness is desired.
   - `test_zstd_bitshuffle_1` and `test_zstd_bitshuffle_n` are commented out in `c-blosc2/bench/CMakeLists.txt:125-147`, so run `b2bench` directly. Also note `b2bench` always uses `blocksize = 0` (confounding `clevel` with `blocksize`) and generates a 32-bit hash independent of `elsize`.

---

## 2. Step-by-Step Implementation Plan

### Step 1: Add `pac_compress_block()` in `pac-zstd`

In `pac-zstd/src/pac.h` and `pac-zstd/src/pac.c`:

1. **Eliminate the 64-byte overread on unpadded inputs** (choose one):
   - **Preferred (zero-copy):** In `pac_parse.inl`, pass `size_t total_len` (or guard the last `< 64` bytes when `to - i < 64` and `to == total_len`) by copying the remaining `< 64` bytes into a zero-padded `unsigned char tail[64]` stack buffer before calling `eq64`. This removes the `PAC_PAD` requirement for `pac_compress_block()` with negligible overhead (runs at most once per block).
   - **Alternative (minimal diff to `pac_parse.inl`):** Copy `bitshuffled_src` into `enc->tmp` (`memcpy(enc->tmp, bitshuffled_src, nbytes)`) inside `pac_compress_block()`. Measure if the extra `memcpy` is noticeable.
2. **Add `pac_reserve_seq(pac_encoder *enc, size_t block)`** (when using the zero-copy path) so `pac_compress_block()` only allocates `enc->seq`.
3. **Expose public API in `pac.h`:**
   ```c
   /* Encode an already bitshuffled block (nbytes must be a multiple of 8 * elem_size,
    * elem_size in {1, 2, 4, 8}, nbytes <= 128 MB). Returns compressed size, or 0 on error
    * or if dst_capacity is too small. */
   size_t pac_compress_block(pac_encoder *enc, void *dst, size_t dst_capacity,
                             const void *bitshuffled_src, size_t nbytes, size_t elem_size);
   ```
4. **Optional trailing-zero-plane scan (recommended):**
   - Before calling `pac_build()`, scan backwards from `bitshuffled_src + nbytes` in 64-bit words to find the last non-zero byte, round up to a `plane` boundary, and pass that as `zero_from`. On sparse detector frames (`uint16`/`uint32` with zero upper bit planes), this lets `pac_build()` emit single-sequence runs for all trailing zero planes without scanning them.
5. **Validate in `pac-zstd` first:**
   - Add a unit test in `pac-zstd/tests/` calling `pac_compress_block()` on an exact-size `malloc(nbytes)` buffer (no padding!) and run `make check-asan` to prove there are zero out-of-bounds reads, plus test `dst_capacity` too small (verifying `0` return and clean reuse of `enc` on the next call).

### Step 2: Integrate into `c-blosc2` (Option A — Direct Hook in `zstd_wrap_compress`)

Option A is strongly preferred over `blosc2_register_codec()` (Option B) because it preserves `BLOSC_ZSTD_FORMAT`, reuses Blosc2's per-thread `thread_context` (avoiding `_Thread_local` memory leaks), and requires zero decompression changes.

1. **CMake (`c-blosc2/CMakeLists.txt` and `c-blosc2/blosc/CMakeLists.txt`):**
   - Add `option(BLOSC_ENABLE_PAC_ZSTD "Enable pac-zstd bit-plane Zstd encoder (requires static zstd)" OFF)` and a path variable `BLOSC_PAC_ZSTD_SOURCE_DIR` (or copy `pac.c`, `pac.h`, `pac_parse.inl` into `c-blosc2/blosc/`).
   - Enable `HAVE_PAC_ZSTD` only when `BLOSC_ENABLE_PAC_ZSTD AND NOT DEACTIVATE_ZSTD AND (ZSTD_FETCHED OR TARGET zstd::libzstd_static)` (fail with a clear CMake message if `BLOSC_ENABLE_PAC_ZSTD=ON` with shared external zstd).
   - Add `pac.c` to `SOURCES` (without `PAC_WITH_KCB`) and add `HAVE_PAC_ZSTD` to compile definitions.
   - Build with `BLOSC_DEPENDENCY_MODE=BUNDLED` (default) so `libzstd_static` (v1.5.7) is used.
2. **Thread state (`c-blosc2/blosc/context.h`, `c-blosc2/blosc/blosc2.c`):**
   - In `struct thread_context` (`context.h:153`), under `#if defined(HAVE_ZSTD) && defined(HAVE_PAC_ZSTD)`, add `struct pac_encoder *pac_enc;` (forward-declared or via `#include "pac.h"`).
   - In `init_thread_context()` (`blosc2.c:2605`), set `thread_context->pac_enc = NULL;`.
   - In `destroy_thread_context()` (`blosc2.c:2632`), call `if (thread_context->pac_enc != NULL) pac_free(thread_context->pac_enc);`.
3. **Runtime toggle & dispatch (`c-blosc2/blosc/blosc2.c`):**
   - Parse `BLOSC_PAC_ZSTD` (e.g., `0`/unset = off, `1` = on) and `BLOSC_PAC_RAW_LITERALS` (e.g., `0`, `5`, `10`, `100`) once (cached via `static` / `pthread_once` or per-encoder creation so `getenv()` is not called on every block).
   - In `zstd_wrap_compress()` (`blosc2.c:700`), check the full eligibility condition:
     - toggle enabled,
     - `!context->use_dict`,
     - `(context->header_flags & 0x10) != 0` (`dont_split` — guarantees unsplit block),
     - last active filter is `BLOSC_BITSHUFFLE` (via `last_filter(context, 'c')`),
     - `context->typesize` in `{1, 2, 4, 8}`,
     - `input_length > 0 && input_length <= (1u << 27) && (input_length % (8 * (size_t)context->typesize)) == 0`.
   - Lazily create `thread_context->pac_enc = pac_create(PAC_ZSTD)` and apply `pac_set_raw_literals()`.
   - Call `pac_compress_block(thread_context->pac_enc, output, maxout, input, input_length, (size_t)context->typesize)`.
   - Return `(int)code` (if `0`, `blosc_c` automatically falls back to storing the block uncompressed via `memcpy`).

---

## 3. Testing Plan (Run on Target Machine)

1. **ASan / UBSan build first:**
   - Configure `c-blosc2` with `-DBLOSC_ENABLE_PAC_ZSTD=ON -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"`.
   - Run `ctest` with `BLOSC_PAC_ZSTD=1` (verifies all existing Blosc2 unit tests, including bitshuffle, multi-threading, Blosc1 compat, split modes, and filter pipelines).
2. **Targeted edge-case unit test (small C test or `tests/` addition):**
   - Element sizes `typesize` = `1, 2, 4, 8` (hits `pac_compress_block`) and `3, 16, 32` (must fall back to `ZSTD_compressCCtx`).
   - Tail/leftover blocks where `nbytes % (8 * typesize) != 0` (must fall back cleanly).
   - `BLOSC_ALWAYS_SPLIT` with `BLOSC_BITSHUFFLE` (must fall back cleanly, not miscalculate `plane`).
   - Multi-filter pipelines: `BLOSC_DELTA -> BLOSC_BITSHUFFLE` (eligible) vs `BLOSC_BITSHUFFLE -> BLOSC_DELTA` (must fall back).
   - All-constant blocks (intercepted by `get_run` in `blosc_c`), all-zero upper bytes, and incompressible random bytes (`maxout` overflow / `memcpy` fallback + subsequent compressible block reusing the same `pac_encoder`).
   - **Cross-decoder check:** Compress with `BLOSC_PAC_ZSTD=1`, decompress in a separate process (or with `BLOSC_PAC_ZSTD=0`) using stock `c-blosc2`, and assert exact `memcmp`.
   - Multi-threading (`nthreads = 1, 2, 4, 8`) verifying per-`thread_context` isolation.
3. **`b2bench` smoke check:**
   - Run `./bench/b2bench zstd bitshuffle test 1` and `./bench/b2bench zstd bitshuffle test` with `BLOSC_PAC_ZSTD=1` and `BLOSC_PAC_ZSTD=0`.

---

## 4. Benchmarking Plan (Dedicated Machine)

Build `c-blosc2` in `Release` mode (`-DCMAKE_BUILD_TYPE=Release -DBLOSC_ENABLE_PAC_ZSTD=ON`) on the dedicated CPU machine. Pin threads / minimize background load, warm up before timing, and report medians across repeated runs.

### A. Real Detector Datasets (Primary Benchmark)

Fetch datasets via `python3 pac-zstd/bench/get_data.py` (`thaumatin_9_1_000001.h5`, `silicon_u16_zstd2_256k.h5`, `ceo2_u32_zstd2_256k.h5`, and optionally `--s3dxrd` for `NMC811_rptZ45999/scan0040/eiger_0000.h5`).

Write a small Python benchmark using `h5py` + `hdf5plugin` to load the `.h5` frames and `python-blosc2` (linked against the patched `c-blosc2`, or `hdf5plugin.Blosc2(cname='zstd', filters=hdf5plugin.Blosc2.BITSHUFFLE)` for end-to-end HDF5)—no `ctypes` wrapper needed (alternatively, dump raw `.bin` frames once if you want a pure C binary). Compare:

| Variant | Settings | Purpose |
|---|---|---|
| Stock `c-blosc2` Zstd | `BLOSC_BITSHUFFLE + BLOSC_ZSTD`, `clevel = 1, 2, 3, 5` (Zstd levels `1, 3, 5, 9`) | Baseline |
| `c-blosc2` + `pac-zstd` | `BLOSC_PAC_ZSTD=1`, `raw_literals = 0, 5, 10` | Target |
| Standalone `pac-zstd` | `pac_compress_chunk` with `narrow = 1` and `narrow = 0` | Upper-bound reference (quantifies `c-blosc2` bitshuffle + framing overhead) |

**Sweep dimensions & metrics:**
- **Blocksizes:** `blocksize = 0` (auto `stune`), `262144` (256 KiB), and `253952` (248 KiB = `PAC_ZSTD_BLOCK`) — testing fixed blocksizes avoids `stune` confounding `clevel` with `blocksize`, and `256 KiB` vs `248 KiB` isolates 4096-byte plane-aliasing effects on `c-blosc2`'s bitshuffle.
- **Threads:** `nthreads = 1` (single-core efficiency) and `nthreads = physical_cores` (scaling / memory-bandwidth saturation).
- **Metrics:**
  1. Compression ratio (`uncompressed / compressed`)
  2. Encode throughput (MB/s or GB/s)
  3. Decode throughput (`zstd_wrap_decompress` on `pac-zstd` chunks vs stock `zstd` chunks)
  4. **Filter vs. codec time breakdown:** Enable `cparams.instr_codec = true` (`BLOSC2_INSTR_CODEC`, `c-blosc2/blosc/blosc2.c:1535,1741`) to separate `bitshuffle()` time (`filter_speed`) from `pac_compress_block()` time (`cspeed`).

### B. Synthetic `b2bench` Comparison (Secondary)

Run `b2bench zstd bitshuffle single <nthreads> <size> <elsize> <rshift>` for `elsize` in `{2, 4, 8}`, `rshift` in `{8, 12, 16, 19, 24}`, comparing `BLOSC_PAC_ZSTD=0` vs `1` (noting that `b2bench` uses `blocksize = 0`, so `clevel` also changes `blocksize` from 32 KB to 1 MB).

### C. Follow-Up Optimization (If Profiling Warrants It)

If `BLOSC2_INSTR_CODEC` shows that `c-blosc2`'s `bitshuffle()` dominates total encode time on sparse `uint16`/`uint32` frames (where standalone `pac-zstd` with `narrow = 1` is significantly faster), the natural Phase 2 is porting `pac-zstd`'s `narrow` zero-byte-skipping optimization into `c-blosc2`'s bitshuffle filter.

---

## 5. Verified Key Files Reference

| Repository | File | Key Symbols / Role |
|---|---|---|
| `pac-zstd` | `src/pac.h` | Public API, `PAC_ZSTD_BLOCK` (`253952`), `pac_set_raw_literals`, new `pac_compress_block` |
| `pac-zstd` | `src/pac.c` | `pac_reserve`, `pac_build`, `pac_encode_block`, `ZSTD_compressSequences`, `PAC_PAD` (`64`) |
| `pac-zstd` | `src/pac_parse.inl` | SIMD bit-plane parse kernels (SSE2/AVX2/AVX-512/NEON/VSX); 64-byte tail-read fix |
| `pac-zstd` | `docs/build.md` | Static Zstd (`ZSTD_STATIC_LINKING_ONLY`) embedding notes (tested with zstd 1.5.7) |
| `pac-zstd` | `bench/get_data.py` | Zenodo HDF5 datasets (`thaumatin`, `silicon`, `ceo2`, optional `--s3dxrd`) |
| `c-blosc2` | `CMakeLists.txt` | `BLOSC_DEPENDENCY_MODE=BUNDLED`, `BLOSC_ZSTD_VERSION` (`1.5.7`), `libzstd_static`, `ZSTD_FETCHED` |
| `c-blosc2` | `blosc/CMakeLists.txt` | `blosc2_static`, `blosc2_shared`, `blosc_testing`, `LIBS` (`libzstd_static`, `lz4`, `m`) |
| `c-blosc2` | `blosc/context.h` | `struct thread_context` (`zstd_cctx`, `zstd_dctx`, new `pac_enc`) |
| `c-blosc2` | `blosc/blosc2.c` | `zstd_wrap_compress`, `zstd_wrap_decompress`, `pipeline_forward`, `blosc_c`, `init_thread_context`, `destroy_thread_context` |
| `c-blosc2` | `blosc/stune.c` | `blosc_stune_next_blocksize` (32 KB–1 MB for Zstd), `split_block` (`BLOSC_ALWAYS_SPLIT` edge case) |
| `c-blosc2` | `blosc/bitshuffle-generic.c` & `blosc/shuffle.c` | Confirms `[elem_size][8][size/8]` bit-plane layout compatibility |
| `c-blosc2` | `include/blosc2.h` | `BLOSC_ZSTD`, `BLOSC2_INSTR_CODEC`, `blosc2_instr`, `blosc2_codec` |
| `c-blosc2` | `bench/b2bench.c` & `bench/CMakeLists.txt` | Synthetic round-trip & throughput benchmark (Zstd bitshuffle CTest entries commented out) |
