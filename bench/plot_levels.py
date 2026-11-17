"""docs/levels.png: pac zstd against stock bitshuffle-zstd at every level, from bench/levels.py.

    python3 bench/plot_levels.py [--results bench/results/levels.json] [--out docs]
"""
import argparse
import json
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
p = argparse.ArgumentParser()
p.add_argument("--results", default=os.path.join(HERE, "results", "levels.json"))
p.add_argument("--out", default=os.path.join(HERE, "..", "docs"))
args = p.parse_args()
R = json.load(open(args.results))

INK, MUTED, GRID, AXIS, SURF, ORANGE = "#1a1a19", "#6b6a63", "#e1e0d9", "#c3c2b7", "#fcfcfb", "#eb6834"
plt.rcParams.update({"font.size": 9, "font.family": "DejaVu Sans", "text.color": INK, "axes.labelcolor": INK,
                     "xtick.color": MUTED, "ytick.color": MUTED, "axes.edgecolor": AXIS})
note = "%s, one core, %d kB blocks, gaps 2^n-1.  Measured %s, pac %s." % (R["machine"], R["block_kb"], R["date"], R["version"])
LABELS = [-20, -10, -5, -1, 1, 3, 9, 15, 19, 22]
ROWS, COLS = ["silicon", "thaumatin", "CeO2"], ["uint16", "uint32"]

fig, axes = plt.subplots(3, 2, figsize=(10, 11), facecolor=SURF)
for i, row in enumerate(ROWS):
    for j, col in enumerate(COLS):
        ax = axes[i][j]
        ax.set_facecolor(SURF)
        ax.grid(True, color=GRID, lw=0.6)
        ax.set_axisbelow(True)
        for sp in ("top", "right"):
            ax.spines[sp].set_visible(False)
        rs = R["cases"]["%s %s" % (row, col)]["results"]
        st = [r for r in rs if r["encoder"] == "stock zstd"]
        ax.plot([r["enc_mbs"] for r in st], [r["ratio"] for r in st], "o", color=ORANGE, lw=1.2, ms=3,
                alpha=0.8, ls=(0, (4, 2)), zorder=3)
        for r in st:
            if r["level"] in LABELS:
                ax.annotate(str(r["level"]), (r["enc_mbs"], r["ratio"]), xytext=(4, -9), textcoords="offset points",
                            fontsize=7, color=MUTED)
        for r in [r for r in rs if r["encoder"] == "pac zstd"]:
            ax.plot(r["enc_mbs"], r["ratio"], "D", color=ORANGE, ms=8, mec=INK, mew=0.6, zorder=5)
            ax.annotate("pac %d %%" % r["raw_percent"], (r["enc_mbs"], r["ratio"]), xytext=(-9, {0: 7, 5: -3, 10: -13}[r["raw_percent"]]),
                        textcoords="offset points", fontsize=8, fontweight="bold", ha="right", zorder=6)
        ax.set_xscale("log")
        ax.set_title("%s, %s" % (row, col), loc="left", fontsize=10, fontweight="bold")
        if i == 2:
            ax.set_xlabel("encode speed, MB/s of pixels (log)")
        if j == 0:
            ax.set_ylabel("compression ratio")
fig.suptitle("pac zstd against every zstd level (dashed line, stock bitshuffle-zstd; numbers are levels)",
             x=0.02, y=0.985, ha="left", fontsize=12, fontweight="bold")
fig.text(0.02, 0.008, note, fontsize=7.5, color=MUTED)
fig.subplots_adjust(left=0.07, right=0.98, top=0.93, bottom=0.07, hspace=0.3, wspace=0.18)
out = os.path.join(args.out, "levels.png")
fig.savefig(out, dpi=120, facecolor=SURF)
print("wrote", out)
