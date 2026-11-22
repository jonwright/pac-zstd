"""docs/levels.png: pac zstd against stock bitshuffle-zstd at every level, from bench/levels.py.

    python3 bench/plot_levels.py [--results bench/results/levels.json] [--out docs]

The uint16 cases only (uint32 tells the same story; it is in the README table).  Linear axes: the
ratio from 1, the speed from 1/10 of the fastest encoder of each panel (slower levels left out).
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
CASES = ["silicon uint16", "thaumatin uint16", "CeO2 uint16"]
PAC_DY = {0: 7, 5: -3, 10: -13}  # label offsets where the pac points coincide (sparse frames)

fig, axes = plt.subplots(1, 3, figsize=(14, 5.2), facecolor=SURF)
for i, (ax, case) in enumerate(zip(axes, CASES)):
    ax.set_facecolor(SURF)
    ax.grid(True, color=GRID, lw=0.6)
    ax.set_axisbelow(True)
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)
    rs = R["cases"][case]["results"]
    top = max(r["enc_mbs"] for r in rs)
    rs = [r for r in rs if r["enc_mbs"] >= top / 10]  # at most a 10x slowdown
    st = [r for r in rs if r["encoder"] == "stock zstd"]
    slowest = min(st, key=lambda r: r["enc_mbs"])["level"]
    ax.plot([r["enc_mbs"] / 1e3 for r in st], [r["ratio"] for r in st], "o", color=ORANGE, lw=1.2, ms=3,
            alpha=0.8, ls=(0, (4, 2)), zorder=3)
    for r in st:
        if r["level"] in LABELS or r["level"] == slowest:
            ax.annotate(str(r["level"]), (r["enc_mbs"] / 1e3, r["ratio"]), xytext=(4, -9), textcoords="offset points",
                        fontsize=7, color=MUTED)
    pac = [r for r in rs if r["encoder"] == "pac zstd"]
    for r in pac:
        ax.plot(r["enc_mbs"] / 1e3, r["ratio"], "D", color=ORANGE, ms=8, mec=INK, mew=0.6, zorder=5)
    groups = {}  # settings that give the same chunks share one label
    for r in pac:
        groups.setdefault(round(r["ratio"], 4), []).append(r)
    for g in groups.values():
        r = g[0]
        if "raw_percent" in r:
            lbl = "pac %s %%" % ", ".join(str(x["raw_percent"]) for x in g)
            dy = PAC_DY.get(r["raw_percent"], 4)
        else:
            lbl = "pac"
            dy = 4
        ax.annotate(lbl, (r["enc_mbs"] / 1e3, r["ratio"]),
                    xytext=(-9, dy), textcoords="offset points", fontsize=8,
                    fontweight="bold", ha="right", zorder=6)
    ax.set_xlim(top / 1e4, top / 1e3 * 1.06)
    ax.set_ylim(1, max(r["ratio"] for r in rs) * 1.1)
    ax.set_title(case, loc="left", fontsize=10, fontweight="bold")
    ax.set_xlabel("encode speed, GB/s of pixels")
    if i == 0:
        ax.set_ylabel("compression ratio")
fig.suptitle("pac zstd against every zstd level (dashed line, stock bitshuffle-zstd; numbers are levels)\n"
             "x axis: from 1/10 of the fastest encoder of each panel; slower levels are left out",
             x=0.02, y=0.98, ha="left", fontsize=12, fontweight="bold")
fig.text(0.02, 0.015, note, fontsize=7.5, color=MUTED)
fig.subplots_adjust(left=0.05, right=0.99, top=0.82, bottom=0.14, wspace=0.16)
out = os.path.join(args.out, "levels.png")
fig.savefig(out, dpi=120, facecolor=SURF)
print("wrote", out)
