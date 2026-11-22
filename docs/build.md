# Building and embedding

The API is documented in [src/pac.h](../src/pac.h); the README has an annotated example.

## Build and test

    git submodule update --init
    make              # build/libpac.a and build/libpac.so, with lz4, zstd and kcb inside;
                      # build/libpacbench.so for the scripts in bench/
    make check        # about 300000 round trips, on every instruction set this CPU has
    build/test_pac 1 5 quick     # the same on small inputs only, for slow machines
    make check-c89    # src/pac.c as strict C89 and as C++98
    make check-rfc    # validate emitted Zstd frames and LZ4 blocks
    make check-compat # decode corpus against 2022+ bitshuffle/zstd/lz4 and Go decoders
    make check-asan   # the tests under the address and undefined-behaviour sanitizers
    make coverage     # line and branch coverage of src/ from tests/test_pac.c
    make fuzz         # libFuzzer on pac_repack_chunk and pac_compress_chunk (requires clang)
    make fuzz-coverage # llvm-cov source coverage over the fuzz corpora (requires clang + llvm-cov)
    make fuzz-replay  # replay the fuzz corpora under ASan + UBSan (gcc or clang)

On Windows, `tests/build_vc9.bat` builds and runs the tests with Visual C++ 2008 (the Python 2.7
compiler, 32 bit); the `msvc` job in `.github/workflows/ci.yml` shows the commands for a current
MSVC.

## Coverage and fuzzing

- `make coverage` compiles `src/pac.c` with `--coverage` into `build-cov/`, runs `test_pac`, and
  prints line and branch coverage (`gcovr`, with an HTML report at `build-cov/coverage.html`, or
  `gcov -b` if `gcovr` is not installed).
- `make fuzz` builds `build-fuzz/fuzz_repack` and `build-fuzz/fuzz_compress` with `clang`
  (`-fsanitize=fuzzer,address,undefined`), seeds `build-fuzz/corpus_repack/` and
  `build-fuzz/corpus_compress/`, and runs each target for `FUZZ_TIME` seconds (default `60`).
  Pass `FUZZ_EXTRA="-jobs=4 -workers=4"` for parallel workers. Fuzzing is kept out of CI.
- `make fuzz-coverage` builds standalone coverage-instrumented binaries (`-fprofile-instr-generate
  -fcoverage-mapping`), replays the fuzz corpora, and writes an HTML report to
  `build-fuzz/coverage/index.html`.
- `make fuzz-replay` replays the corpora under ASan + UBSan using either `gcc` or `clang`.

## In another code base

Copy `src/pac.c`, `src/pac.h` and `src/pac_parse.inl` and compile them against your own lz4 and
zstd:

- **zstd** must be built in, not linked from the system's shared library: the encoder includes
  `zstd.h` with `ZSTD_STATIC_LINKING_ONLY` and needs `ZSTD_compressSequences`, explicit block
  delimiters and repcode resolution, which zstd keeps in its experimental API.  Tested with zstd
  1.5.7.
- **lz4**: `lz4.h`, any version, used by the repack to decode the input.  Tested with 1.9.4.
- **A bit transpose** with the signature of `bitshuf_encode_block` of
  [kcb](https://github.com/kalcutter/bitshuffle): define `PAC_WITH_KCB` to use that one, or pass
  your own with `pac_set_transpose()`.  `pac_repack_chunk` needs none.

The source keeps to what old MSVC accepts: declarations first, no `inline` or `restrict` keywords
(macros where they help), no `//` comments, 64-bit integers.  The SIMD kernels are compiled once
per instruction set with target attributes (gcc, clang), as kcb does, and the encoder picks the
best one the CPU has when it is created.  MSVC has no target attributes, so a `cl` build has only
the SSE2 kernel, or the one `/arch` selects, which the whole binary then needs.  kcb has no NEON
or VSX transpose: on those machines the bit transpose is its portable C code.

## Decoder compatibility floor

Written bitshuffle-zstd chunks target existing readers down to **bitshuffle 0.4.0** (released
2022-02-24, the first bitshuffle release with Zstandard support) and **zstd v1.5.2** (released
2022-01-20, also bundled by 2022 `hdf5plugin` builds):

- **`libzstd` `<= v1.5.2` (fixed in `v1.5.4`, Feb 2023):** rejects valid RFC 8878 compressed
  blocks of exact size `128 KB` (`131072` B) and 0-literal/0-sequence `Compressed_Block`s; neither is
  emitted here (`ZSTD_compressSequences` keeps compressed blocks `< 128 KB`, and raw/RLE prefixes
  use `bt_raw`/`bt_rle`).
- **`libzstd` `<= v1.5.5` (fixed in `v1.5.6`, Mar 2024):** magicless-frame decoder rejects some
  valid frames; emitted chunks always carry the standard `0xFD2FB528` magic frame header.
- **`zstd` CLI `<= v1.4.3` (fixed in `v1.4.4`, Nov 2019; CLI only, not `libzstd`):** rejects
  multi-block frames whose first block is a `131072`-byte `bt_rle` block (`should consume all input
  error`). `libzstd` of that era decodes them cleanly.
