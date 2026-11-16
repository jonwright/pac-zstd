"""The repack figure and the tables of docs/benchmarks.md, from the measurements of bench/sweep.py.

    python3 bench/plot.py [--results bench/results/results.json] [--out docs]

Writes repack_gain.png (how many times smaller a detector file gets) and results.md (the tables).
Needs matplotlib.
"""
import argparse
import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
p = argparse.ArgumentParser()
p.add_argument("--results", default=os.path.join(HERE, "results", "results.json"))
p.add_argument("--out", default=os.path.join(HERE, "..", "docs"))
args = p.parse_args()
R = json.load(open(args.results))
os.makedirs(args.out, exist_ok=True)

INK, MUTED, GRID, AXIS, SURF = "#1a1a19", "#6b6a63", "#e1e0d9", "#c3c2b7", "#fcfcfb"
BLUE, ORANGE = "#2a78d6", "#eb6834"
#         encoder: (colour, marker, line style, heavy, legend)
SERIES = {
    "stock lz4": (BLUE, "o", (0, (4, 2)), False, "bitshuffle-lz4, stock (LZ4_compress_default)"),
    "pac lz4": (BLUE, "D", "-", True, "bitshuffle-lz4, plane-aware"),
    "stock zstd": (ORANGE, "o", (0, (4, 2)), False, "bitshuffle-zstd, stock (zstd level 2)"),
    "pac zstd": (ORANGE, "D", "-", True, "bitshuffle-zstd, plane-aware"),
}
plt.rcParams.update({"font.size": 9, "font.family": "DejaVu Sans", "text.color": INK, "axes.labelcolor": INK,
                     "xtick.color": MUTED, "ytick.color": MUTED, "axes.edgecolor": AXIS})
note = "%s, one core, frames in memory.  Measured %s, pac %s." % (R["machine"], R["date"], R["version"])
default = R["default_block_kb"]  # {"lz4": 56, "zstd": 248}


def style(ax):
    ax.set_facecolor(SURF)
    ax.grid(True, color=GRID, lw=0.6)
    ax.set_axisbelow(True)
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)


def repack_figure():
    cases = sorted(R["cases"], key=lambda c: R["cases"][c]["repack"]["gain"])
    gains = [R["cases"][c]["repack"]["gain"] for c in cases]
    fig, ax = plt.subplots(figsize=(8.6, 0.52 * len(cases) + 1.9), facecolor=SURF)
    style(ax)
    ax.grid(axis="y", visible=False)
    ax.barh(range(len(cases)), gains, height=0.56, color=ORANGE, zorder=3)
    ax.axvline(1.0, color=INK, lw=1.0, zorder=4)
    for k, (c, g) in enumerate(zip(cases, gains)):
        r = R["cases"][c]["repack"]
        ax.text(g + 0.05, k, "%.2f x smaller   (ratio %.3g to %.3g)" % (g, r["ratio_lz4_8k"], r["ratio_zstd"]),
                va="center", fontsize=8.5, color=INK)
    ax.set_yticks(range(len(cases)))
    ax.set_yticklabels([c.replace(" NMC811", "") for c in cases], color=INK)
    ax.tick_params(axis="y", length=0)
    ax.set_xlim(0, max(gains) * 1.75)
    ax.set_xlabel("file size as written by the detector (lz4, 8 kB blocks) / size after the repack (zstd, %d kB blocks)" % default["zstd"])
    ax.set_title("Repacking detector files: how many times smaller", loc="left", fontsize=12, fontweight="bold")
    fig.text(0.02, 0.01, "pac_repack_chunk, no bit transpose.  " + note, fontsize=7.5, color=MUTED)
    fig.subplots_adjust(left=0.27, right=0.98, top=0.9, bottom=0.2)
    out = os.path.join(args.out, "repack_gain.png")
    fig.savefig(out, dpi=120, facecolor=SURF)
    plt.close(fig)
    print("wrote", out)


repack_figure()


def gbs(c, gpix):
    """Gpixel/s to GB/s of uncompressed pixels (the raw data rate)."""
    return gpix * {"uint8": 1, "uint16": 2, "uint32": 4, "uint64": 8}[c["dtype"]]


lines = ["# Results", "", note, "",
         "Written by `bench/plot.py` from `bench/results/%s` (`bench/sweep.py`).  Speeds in GB/s" % os.path.basename(args.results),
         "of uncompressed pixels (the raw data rate) on one core; every chunk decoded and compared with the pixels.",
         "", "## Pixels to chunk, at 8 kB and at the default block (lz4 %d kB, zstd %d kB)" % (default["lz4"], default["zstd"]), "",
         "| test case | encoder | 8 kB: ratio | encode GB/s | decode GB/s | default: ratio | encode GB/s | decode GB/s |",
         "|---|---|---:|---:|---:|---:|---:|---:|"]
for case, c in R["cases"].items():
    for name in SERIES:
        a = [s for s in c["sweep"] if s["encoder"] == name and s["block_kb"] == 8][0]
        b = [s for s in c["sweep"] if s["encoder"] == name and s["block_kb"] == default[name.split()[1]]][0]
        lines.append("| %s | %s | %.3g | %.2f | %.2f | %.3g | %.2f | %.2f |" % (
            case if name == "stock lz4" else "", name, a["ratio"], gbs(c, a["enc_gpix"]), gbs(c, a["dec_gpix"]),
            b["ratio"], gbs(c, b["enc_gpix"]), gbs(c, b["dec_gpix"])))
lines += ["", "## Raw-literal threshold, pac zstd %d kB (pac_set_raw_literals)" % default["zstd"], "",
          "| test case | " + " | ".join("%d %%: ratio | encode GB/s" % r["percent"] for r in next(iter(R["cases"].values()))["raw_literals"]) + " |",
          "|---|" + "---:|---:|" * len(next(iter(R["cases"].values()))["raw_literals"])]
for case, c in R["cases"].items():
    lines.append("| %s | " % case + " | ".join("%.3g | %.2f" % (r["ratio"], gbs(c, r["enc_gpix"])) for r in c["raw_literals"]) + " |")
lines += ["", "## Repack: stock lz4 with 8 kB blocks to zstd with %d kB blocks, no bit transpose" % default["zstd"], "",
          "| test case | ratio lz4 8 kB | ratio zstd %d kB | times smaller | raw GB/s | GB/s of lz4 read |" % default["zstd"],
          "|---|---:|---:|---:|---:|---:|"]
for case, c in R["cases"].items():
    r = c["repack"]
    lines.append("| %s | %.3g | %.3g | %.2f | %.2f | %.3f |" % (
        case, r["ratio_lz4_8k"], r["ratio_zstd"], r["gain"], r["pixel_gb"], r["read_gb"]))
blocks = sorted(set(t["block_kb"] for c in R["cases"].values() for t in c["transpose"]))
lines += ["", "## The bit transpose alone (kcb, block by block), GB/s", "",
          "| test case | " + " | ".join("%d kB" % kb for kb in blocks) + " |", "|---|" + "---:|" * len(blocks)]
for case, c in R["cases"].items():
    lines.append("| %s | " % case + " | ".join("%.1f" % gbs(c, t["gpix"]) for t in sorted(c["transpose"], key=lambda t: t["block_kb"])) + " |")
METHODS = ("repack", "decode + pac zstd", "decode + stock zstd-2")
lines += ["", "## Transcoding lz4 8 kB to zstd", "",
          "From each frame as stock lz4 with 8 kB blocks to zstd at each block size: `repack` is pac_repack_chunk; the",
          "others decode and untranspose, then encode with plane-aware zstd or with stock zstd level 2.  Ratio, GB/s.", ""]
for case, c in R["cases"].items():
    lines += ["### %s" % case, "", "| block kB | " + " | ".join(METHODS) + " |", "|---:|" + "---|" * len(METHODS)]
    for kb in blocks:
        cells = []
        for m in METHODS:
            t = [t for t in c["transcode"] if t["method"] == m and t["block_kb"] == kb][0]
            cells.append("%.3g, %.2f" % (t["ratio"], gbs(c, t["gpix"])))
        lines.append("| %d | " % kb + " | ".join(cells) + " |")
    lines.append("")
lines += ["", "## Every block size", "", "Each cell: ratio, encode GB/s, decode GB/s.", ""]
for case, c in R["cases"].items():
    lines += ["### %s" % case, "", "| block kB | " + " | ".join(SERIES) + " |",
              "|---:|" + "---|" * len(SERIES)]
    for kb in sorted(set(s["block_kb"] for s in c["sweep"])):
        cells = []
        for name in SERIES:
            a = [s for s in c["sweep"] if s["encoder"] == name and s["block_kb"] == kb][0]
            cells.append("%.3g, %.2f, %.2f" % (a["ratio"], gbs(c, a["enc_gpix"]), gbs(c, a["dec_gpix"])))
        lines.append("| %d | " % kb + " | ".join(cells) + " |")
    lines.append("")
out = os.path.join(args.out, "results.md")
open(out, "w").write("\n".join(lines) + "\n")
print("wrote", out)
