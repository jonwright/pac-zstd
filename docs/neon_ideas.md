# Optimisation Ideas & Profiling Notes

Notes from a read-only profiling, sequence-level X-ray, and ablation study on `pac-zstd`, prioritised around detector data rates, dense-frame storage economics, and keeping this repository minimal.

Measured on an Intel Core i3-7100 @ 3.90 GHz (`avx2` parse kernel, single core, Zstd 1.5.7, 248 kB blocks) across `silicon` (4M `u16`/`u32`), `thaumatin` (16M `u16`), `CeO2` (4M `u16`/`u32`), and synthesised `u8` frames (`counts // 2` clipped to 255), testing mask values `0`, `2^n - 1`, `2^n - 2`, and `2^(n-1) - 2` (`INT_MAX - 1`). Every experimental variant was decoded with `ZSTD_decompress` and verified bit-for-bit against the original pixels and `PAC_VALIDATE_SEQUENCES`.

---

## 1. Operational Priorities & the Dense-Frame Ratio Ceiling

Typical incoming detector rates set the throughput target:

| Pixel type | Nominal rate | 4M raw data rate | 16M raw data rate |
|---|---:|---:|---:|
| `uint8` | ~1,000 fps | ~4.5 GB/s | ~18 GB/s |
| `uint16` | ~500 fps | ~4.5 GB/s | ~18 GB/s |
| `uint32` | ~200 fps | ~3.6 GB/s | ~14 GB/s |

- **Storage cost is set by high-count / dense / barely compressible frames**, not by sparse frames (already 50×–1,600× smaller). Optimising throughput and worst-case behaviour on dense/noisy frames comes first; extra ratio on already-sparse frames is secondary.
- **Masks** are `0` on ESRF ID11 data and `2^n - 1` (`255`, `65535`, `2^32 - 1`) for other facilities (`2^n - 2` and `INT_MAX - 1` behave similarly for bit-plane structure).
- **Simplicity is a feature:** `pac-zstd` should remain a tiny C89 single-file library (`src/pac.c` + `src/pac_parse.inl`) wrapping vendored `lz4` and `zstd`. Custom plane-aware LZ4 decompression already lives in `bslz4_to_sparse` and does not belong here.

### Why dense ratio is already at the bitshuffle-zstd ceiling

Per-plane measurements on `CeO2 u16` (`gaps = 65535`, 8.94 MB frame; `pac = 3,501,737 B` [2.56×] vs `stock zstd-2 = 3,674,953 B` [2.44×]):

| Bit planes | Bit density `p` | Order-0 entropy `H0` | `pac` literals | `pac` sequences | Sum of solo planes | Share of frame |
|---|---:|---:|---:|---:|---:|---:|
| `0..4` | 0.530–0.546 | 7.77–7.84 b/B | 2,646,676 B | 80 | 2,646,581 B | **~76%** |
| `5..6` | 0.489–0.618 | 5.61–6.49 b/B | 934,878 B | 18,551 | 774,263 B | **~22%** |
| `7..15` | 0.060–0.077 | 0.43–1.08 b/B | 107,778 B | 41,259 | 173,485 B | **~2%** |

*(With 5.6% masked `0xFFFF` pixels, `p ≈ 0.53` corresponds to `p ≈ 0.502` on valid pixels.)*

The lower 5 planes (`0..4`) are pure Poisson shot noise carrying nearly 8 bits/byte of irreducible entropy. `pac 0%` already compresses `CeO2 u16`/`u32` slightly **smaller** than `stock zstd -19` and `-22` (`2.56` vs `2.53`; `5.09` vs `5.04`, see [benchmarks.md](benchmarks.md)). Inside the standard bitshuffle-zstd format, **no lossless entropy trick can recover significant space on dense frames** — the real low-hanging fruit on barely compressible data is **encoding speed** at ~0–1.5% ratio cost, plus graceful behaviour when flux climbs even higher.

---

## 2. Poisson-Guided Adaptive Compression

Because photon counts follow `Poisson(λ)`, a transposed block naturally splits into three bit-plane zones whose boundary `log2(√λ)` can be estimated directly from a tiny pixel or per-plane sample (combined with the block's bit-invariance scan):

1. **Zone 1 — Shot-noise floor (`b < log2(√λ)`, `p ≈ 0.5`):** Incompressible randomness (`H0 ≈ 7.8–8.0 b/B`). Skip both `pac_parse` and Huffman; emit the plane directly as a raw Zstd block (`bt_raw`: 3-byte header + `memcpy`). *(Note: HDF5 bitshuffle readers always expect transposed planes, so the bit transpose itself still runs on the active lower bytes.)*
2. **Zone 2 — Transition planes (`log2(√λ) ≤ b ≤ b_max`):** Skewed bit densities with structure and Bragg peaks. Run full `pac_parse` + per-plane Zstd Huffman, preserving the compression where `pac` beats `zstd -22`.
3. **Zone 3 — Empty / mask-only upper planes (`b > b_max`):** All-zero, all-one, or identical to plane `b - 1`. Skip upper-byte transpose (`elem > 1`) and `pac_parse`; emit precomputed sequences directly.

**Worst-case behaviour (`λ >> 100`):** When counts are so high that *every* plane falls into Zone 1, the encoder automatically emits every plane as `bt_raw` inside a valid Zstd frame, bypassing both `pac_parse` and `ZSTD_compressSequences` completely and running at transpose + `memcpy` speed.

---

## 3. Prioritised Optimisations

Ranked by value on high-count/worst-case workloads relative to code size and simplicity.

### Priority 1 — Single-frame hybrid per-plane raw skip (replaces the whole-block `pac_set_raw_literals` cliff)

Today [`pac_raw_literals`](../src/pac.c#L583-L596) makes one binary Huffman-vs-raw choice for the entire 248 kB block. On `CeO2`, setting `--raw 10` throws away Huffman on compressible mid-planes (`5..9`) just to skip Huffman on noisy planes (`0..4`), costing **6.6–8.6% ratio**.

Instead, probe a tiny sample (e.g., 64–256 bytes) of leading planes `0, 1, ...` for compressibility (bit density / order-0 entropy saving `< T%`):

- Emit leading incompressible planes `0..k-1` directly as `bt_raw` Zstd blocks (`3-byte header + memcpy`) under a standard Zstd frame header (`singleSegment=1`, `ZSTD_getFrameContentSize == nb`). Because `bt_raw` blocks leave Zstd's repcodes and FSE/Huffman tables untouched, they compose cleanly with subsequent compressed blocks.
- Splice `ZSTD_compressSequences` on planes `k..nplanes-1` as the remaining blocks of the **same single RFC 8878 Zstd frame** using `ZSTD_f_zstd1_magicless` + `contentSizeFlag=0`, temporarily saving/restoring the 2 overlapping bytes before the splice offset — zero extra copy of the compressed suffix, ~40–60 lines of C, zero custom entropy coder.

Measured end-to-end vs baseline `pac` (combined with Priority 2 + 3):

| Case | `pac 0%` MB/s (ratio) | `pac 10%` MB/s (ratio) | Hybrid `T=5–10%` MB/s (ratio) | Ratio vs `pac 0%` | Speedup vs `pac 0%` |
|---|---:|---:|---:|---:|---:|
| `CeO2 u32`, `gaps=max` | 1,471 (5.09) | 2,148 (4.74, -6.9%) | **2,445 (5.02)** | **-1.4%** | **1.66×** *(faster than `pac 10%`)* |
| `CeO2 u32`, `intmax-1` | 1,469 (5.09) | 2,133 (4.74, -6.9%) | **2,384 (5.03)** | **-1.2%** | **1.62×** *(faster than `pac 10%`)* |
| `CeO2 u32`, `gaps=0` | 1,709 (5.12) | 2,696 (4.78, -6.6%) | **2,735 (5.05)** | **-1.4%** | **1.60×** |
| `CeO2 u16`, `gaps=max` | 965 (2.56) | 1,569 (2.38, -7.0%) | **1,590 (2.53)** | **-1.2%** | **1.65×** *(faster than `pac 10%`)* |
| `CeO2 u16`, `gaps=0` | 1,049 (2.57) | 1,771 (2.40, -6.6%) | **1,765 (2.54)** | **-1.2%** | **1.68×** |
| `CeO2 u8`, `gaps=0` | 763 (1.53) | 1,409 (1.40, -8.5%) | **1,302 (1.50)** | **-2.0%** | **1.71×** |
| `CeO2 u8`, `gaps=max` | 704 (1.52) | 1,251 (1.39, -8.6%) | **1,183 (1.49)** | **-2.0%** | **1.68×** |
| `silicon u16` / `u32` (sparse) | 3,713 / 2,945 | — | **4,292 / 4,991** | **0.00%** | **1.16× / 1.69×** |

With `T = 2%`, `CeO2 u16`/`u32` lose only **-0.6% to -0.8% ratio** (`2.54–2.55`, `5.05–5.09`) at **1.5–1.6×** speed (`1,541–1,669` MB/s on `u16`, `2,265–2,583` MB/s on `u32`). On `silicon`, the probe sees compressibility on plane 0 immediately, giving 0.00% ratio loss and `<1%` overhead.

---

### Priority 2 — Zero-risk hot-path cleanups & in-window `pac_parse`

Bit-for-bit identical Zstd sequences and output to current `pac`:

1. **Skip `enc->hist` sampling when unused:** [`pac_build`](../src/pac.c#L457-L488) currently zeros and populates `enc->hist[256]` unconditionally, wasting **1.7–5.0%** of total runtime on `raw_percent = 0` (and **9.2%** on `CeO2 u8` at `raw_percent = 100`).
2. **Free `pac_build` housekeeping:** Accumulate matched bytes inside `parse` instead of re-scanning `seq` to compute `lits`; grow `enc->window_for` monotonically instead of calling `ZSTD_CCtx_reset` on every frame's smaller tail block.
3. **In-window bitmask [`pac_parse`](../src/pac_parse.inl#L167-L200):** Keep the raw 64-bit equality masks `e1` / `ep`, resolve short matches ending inside the 64-byte window via `pac_ctz64(~(e1 >> j))` without calling `match_len`, stay inside the same 64-byte mask while `k < 61` instead of reloading SIMD vectors, and split out a dedicated `plane == 0` loop.
   - **1.3× to 1.5× faster isolated `parse`** across all datasets (`u8`, `u16`, `u32`).
   - Together with the cleanups above, gives **+6–12% end-to-end** on dense `CeO2 u8`/`u16`/`u32` before touching anything else.

---

### Priority 3 — Mask-aware SIMD narrow transpose & bit-plane invariance (`elem > 1`)

Baseline profiling shows the scalar `OR`-loop in [`pac_transpose`](../src/pac.c#L679-L685) consumes **22–28%** of total runtime on `gaps = 0`, and **13–18%** wasted work when `gaps != 0` disables `narrow` and forces a full `kcb` transpose (which jumps to **29–39%**).

Replace the scalar scan with a single AVX2/SSE2 pass computing three per-element bitmasks across the block: `or_acc |= x`, `and_acc &= x`, `diff_acc |= x ^ (x >> 1)`:

- Bit `b` of `or_acc == 0` → plane `b` is all `0x00`.
- Bit `b` of `and_acc == 1` → plane `b` is all `0xFF`.
- Bit `b` of `diff_acc == 0` → **plane `b + 1` is byte-for-byte identical to plane `b`**.

On real masked data (`0`, `2^n - 1`, `2^n - 2`, `INT_MAX - 1`):
- **`CeO2 u32`:** **568/568 blocks** (all mask modes) only need **2 bytes transposed** instead of 4; **65–68%** of planes skip `pac_parse`. Combined with Priority 2: **1.16–1.18× faster** (`1,712–1,724` vs `1,467–1,473` MB/s).
- **`silicon u16`/`u32` and `thaumatin u16`:** **280/288**, **560/568**, and **531/572** blocks only need **1 byte transposed**, and **69–90%** of planes skip `pac_parse`. Combined with Priority 2: **1.41–1.67× faster** on masked frames (`3,442–4,929` MB/s) and **1.16–1.18× faster** on `gaps = 0`, with **0% ratio loss**.

**Two essential guards:**
- **Gate on `elem > 1`:** `u8` (`keep == 1`) cannot narrow further, so scanning `in` adds a 4–8% pass (`silicon u8` dropped 5609 → 5343 MB/s in testing). Skip the invariance scan completely for `elem == 1`.
- **Boundary planes after unmaterialized `eq_prev` planes:** Under `INT_MAX - 1` (`0x7FFFFFFE`), plane 31 is all-zero immediately after unmaterialized plane 30. Calling `pac_parse` on plane 31 with `plane != 0` reads stale bytes in `enc->tmp[30*plane..]` (triggering `PAC_VALIDATE_SEQUENCES` errors). Emit `{lit + 1, len - 1, offset = 1}` directly whenever a zero/one boundary plane follows an unmaterialized plane.

---

### Priority 4 — Sequence merging, direct RLE zero blocks & stock-LZ4 repack cleanups

Tiny, zero-risk changes (~15 lines each) using the vendored decoders directly:

1. **Merge adjacent same-offset sequences across plane boundaries** inside each 128 kB Zstd block (zero/mask planes become 1 sequence instead of 16–32, allowing `ZSTD_maybeRLE` to trigger) + **emit 17-byte Zstd RLE frames** directly when `or_acc == 0` (skipping transpose, parse, and `ZSTD_compressSequences`):
   - **0 regressions**, `+0.1%` dense, **+4.1% to +19.9%** sparse `gaps = 0` (`silicon u8`: `1645 → 1973`, `u32`: `415 → 473`, `u16`: `250 → 260`), **-68% to -79%** empty `gaps = 0` frames (see [empty_frame.md](empty_frame.md)).
2. **Persistent `ZSTD_DCtx` in [`pac_repack_chunk`](../src/pac.c#L806):** Replace stateless `ZSTD_decompress()` (~1,000 `130 KB` allocations per frame) with `ZSTD_decompressDCtx`: **+2–8%** on `PAC_ZSTD` repack.
3. **Lazy plane-state tracking (`ZERO` / `ONE` / `EQ_PREV`) during repack scatter:** Keep stock `LZ4_decompress_safe` / `ZSTD_decompressDCtx`, skip scatter `memcpy` and `pac_parse` on invariant slices/planes: **1.08–1.11×** on dense `CeO2` repack and **1.17–1.25×** on sparse repack.

---

### Priority 5 — Lower priority for dense storage: 2D stride (`offset = stride`) & Zstd entropy level 19

- **2D row/module stride (`lcm(W, 8)/8`: 517 B on Eiger 4M, 130/1037 B on Eiger 16M):**
  Naively adding `offset = stride` regresses `-18%` on `gaps = 0` because at `[non-zero] → [0x00, 0x00, ...]` edges, `stride` matches 1 byte before `offset = 1` can start, hijacking zero runs and breaking `rep = 1`. Adding a **`no_hijack` check** (reject `stride` when `l1 == 0 && base[pos] == base[pos+1]` unless it extends >4 bytes past the run) + frame-cached edge-correlation auto-detect (`0` when unmasked):
  - Dense `CeO2` (`gaps = max`): **+0.4–0.5% ratio** (`2.56 → 2.57`, `5.09 → 5.11`).
  - Sparse masked (`gaps = max / max-1 / intmax-1`): **+25–47%** on `silicon u16`/`u32`, **+170–179%** on `silicon u8`, **+4.5–5.5%** on `thaumatin u16`, **-64–72%** on empty masked frames — **beating `stock zstd -19` on 27 of 28 test cases** at baseline `pac` speed.
  - Lower priority here because dense storage gains only ~0.4% and adding a 3rd match offset + stride detection adds code complexity to `pac_parse.inl`.
- **`ZSTD_c_compressionLevel = 19` (or `ZSTD_btultra2` with minimal `hashLog=6, chainLog=6`):**
  Enables cross-block entropy repeat selection on small blocks: `+0.1–0.7%` on `CeO2` / `thaumatin`, `+2–8%` on `silicon`, at ~20–25% encode-speed cost.

---

## 4. Baseline Profiling Snapshot

Measured stage breakdown of [`pac_compress_chunk`](../src/pac.c#L726) (i3-7100, `avx2`, `raw_percent = 0`):

| Case | MB/s | `narrow` | `pack` | `transp` | `memset` | `parse0` | `parseN` | `hist` | `zstd_seq` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `CeO2 u8` `gaps=0` / `max` | 731 / 605 | 0% | 0% | 10% / 9% | 0% | 1% | 16% | 5% | **68% / 69%** |
| `CeO2 u16` `gaps=0` / `max` | 1,025 / 942 | 7% | 1% / 0% | 14% | 0% | 0% | 14% / 16% | 4% | **59% / 58%** |
| `CeO2 u32` `gaps=0` / `max` | 1,602 / 1,163 | 11% | 5% / 0% | 10% / 23% | 2% / 0% | 0% | 11% / 13% | 3% | **55% / 48%** |
| `silicon u8` `gaps=0` / `max` | 5,049 / 3,934 | 0% | 0% | **63% / 49%** | 0% | 3% / 10% | 25% / 13% | 1% / 2% | 7% / 24% |
| `silicon u16` `gaps=0` / `max` | 3,369 / 2,353 | **22% / 15%** | 10% / 0% | **18% / 33%** | 3% / 0% | 12% / 10% | 9% / 14% | 2% | 23% / 26% |
| `silicon u32` `gaps=0` / `max` | 4,561 / 2,794 | **28% / 17%** | 10% / 0% | **11% / 39%** | 7% / 0% | 8% / 6% | 6% / 13% | 2% | 21% / 20% |
| `thaumatin u16` `gaps=0` / `max` | 2,528 / 1,866 | **19% / 13%** | 8% / 0% | **13% / 29%** | 3% / 0% | 13% / 9% | 8% / 12% | 3% | 31% / 32% |

---

## 5. Tested Dead Ends & Non-Goals

- **Custom plane-aware LZ4 decoder in `pac_repack_chunk`:** Would recover another ~25–35% on sparse 8 kB LZ4 repack (`LZ4_decompress_safe` is 48–60% of time on `silicon gaps=0`), **not** on dense `CeO2` (`13–20%`), and duplicates code already in `bslz4_to_sparse`. Intentionally excluded.
- **Custom Huffman/FSE encoder replacing `ZSTD_compressSequences`:** Priority 1's hybrid `bt_raw` skip captures most of the dense-frame speedup (~1.6–1.7×) inside ~50 lines of standard Zstd framing without reimplementing Zstd bitstream/Huffman encoding.
- **Changing `minMatch` from 4:** Tested `min_run, min_plane ∈ {3, 4, 6, 8, 12, 16}`. `min_run = 8` gained +1.2%–2.0% on `thaumatin` (`0x00` literal ≈ 1 bit under Huffman) **but lost -1.8% to -9.8% on `silicon`**; `min_run = 3` gained +0.3–0.5% on `silicon` and lost -1.1% on `thaumatin`. `(4, 4)` is the best general default.
- **Changing the 5% `dense` Zstd block-split threshold or merging dense (`>95%` literal) planes:** Current 5% is near-optimal; merging consecutive dense planes into 128 kB Zstd blocks **hurt** `CeO2 u16`/`u32` by **-1.4% to -1.7%** because separate ~16 kB blocks let Huffman adapt per bit plane.
- **Fixed offsets `offset = 2` or `offset = 2 * plane`:** `0.0%` to `-0.3%` ratio, slower everywhere.
- **Bit-plane invariance scan on `u8` (`elem == 1`):** Cannot reduce `keep < 1`, costing 4–8% speed for nothing.
