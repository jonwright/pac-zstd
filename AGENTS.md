# Repository Notes for Agents

- **Minimalism & C89/C++98:** Keep `src/pac.c`, `src/pac.h`, and `src/pac_parse.inl` small, simple, and compliant with `make check-c89` (C89 `-Wdeclaration-after-statement` + C++98, MSVC 2008 compatible).
- **Decoder Compatibility Floor:** All generated bitshuffle-zstd chunks must remain decodable by **bitshuffle 0.4.0** (released 2022-02-24, the first bitshuffle with Zstandard support) and the zstd commit it bundles, **`18d02cbf`** (`v1.4.7-403`, and so `1.4.7+`), with **LZ4 1.9.x**. Before merging encoder/framing changes into `main`, verify generated chunks decode against those versions (see `tests/check_compat.sh`; the offline matrix also sweeps `v1.4.7` .. `v1.5.7` + `dev`). The `bt_rle` first-block behaviour matters only for the pre-`1.4.4` `zstd` *CLI*; `libzstd` of that era decodes it cleanly.
- **CI & Large Data:** Keep libFuzzer out of `.github/workflows/ci.yml`. Skip the 18 GB `--s3dxrd` dataset in local benchmarks unless explicitly requested.
- **Benchmarking:** Use the scripts in `bench/` — never a one-off harness. One core (`--core N`), AMD EPYC 9454 (gcc 13.3 `-O3`), frames in memory; numbers go to `bench/results/`, figures to `docs/` (see `docs/benchmarks.md` → Reproducing). The harness computes itemsize/dtype/mask per case itself — do not re-implement it. Flow:
  ```
  git submodule update --init
  make                                   # build/libpacbench.so
  python3 bench/get_data.py              # 2.6 GB Zenodo -> bench/data
  python3 bench/sweep.py --core 20       # all cases x all blocks -> bench/results/results.json
  python3 bench/levels.py --core 20      # pac vs stock zstd at every level -> bench/results/levels.json
  python3 bench/bench_repack.py bench/data/thaumatin_9_1_000001.h5 data --as-detector
  python3 bench/plot.py                  # docs/repack_gain.png + docs/results.md
  python3 bench/plot_summary.py          # docs/pareto.png
  python3 bench/plot_levels.py           # docs/levels.png
  ```
  (`--s3dxrd` adds the optional 18 GB case; skip unless asked.)
