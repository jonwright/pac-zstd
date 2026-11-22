# Repository Notes for Agents

- **Minimalism & C89/C++98:** Keep `src/pac.c`, `src/pac.h`, and `src/pac_parse.inl` small, simple, and compliant with `make check-c89` (C89 `-Wdeclaration-after-statement` + C++98, MSVC 2008 compatible).
- **Decoder Compatibility Floor (future verification note):** All generated bitshuffle-zstd chunks must remain decodable by **bitshuffle 0.4.0** (released 2022-02-24) and **zstd v1.5.2** (released 2022-01-20, 2022-era `hdf5plugin` / `libzstd` floor). Before merging encoder/framing changes into `main`, verify generated chunks decode against those 2022 library versions (and check `bt_rle` first-block behaviour if compatibility with pre-`1.4.4` `zstd` CLI is desired).
- **CI & Large Data:** Keep libFuzzer out of `.github/workflows/ci.yml`. Skip the 18 GB `--s3dxrd` dataset in local benchmarks unless explicitly requested.
