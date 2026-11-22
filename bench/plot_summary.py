"""The Pareto plots of the README, from the committed measurements (nothing is run).

    python3 bench/plot_summary.py [--results bench/results/results.json] [--out docs]

docs/pareto.png, compression ratio against speed in GB/s of uncompressed pixels per core, on the
test cases as the detector writes them (gaps at the maximum).  Each line is one method through its
block sizes (8 to 496 kB, without those whose bit planes are a multiple of 4096 bytes long).  Three rows:

    encode      pixels to chunk, four encoders; the dashed line is the bit transpose alone
    decode      chunk to pixels with the stock decoders, for the chunks of the row above
    transcode   the detector's lz4 (8 kB) to zstd, three ways

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

INK, MUTED, GRID, AXIS, SURF = "#1a1a19", "#6b6a63", "#e1e0d9", "#c3c2b7", "#fcfcfb"
BLUE, ORANGE, TEAL = "#2a78d6", "#eb6834", "#1f9e89"
plt.rcParams.update({"font.size": 9, "font.family": "DejaVu Sans", "text.color": INK, "axes.labelcolor": INK,
                     "xtick.color": MUTED, "ytick.color": MUTED, "axes.edgecolor": AXIS})
CASES = ["thaumatin, gaps 65535", "silicon, gaps 65535", "s3DXRD NMC811 uint32, gaps 65535", "CeO2 uint16, gaps 65535"]
CASES = [c for c in CASES if c in R["cases"]]
ENCODERS = {  # encoder: colour, marker, line style, legend
    "stock lz4": (BLUE, "o", (0, (4, 2)), "bitshuffle-lz4, stock (LZ4_compress_default)"),
    "pac lz4": (BLUE, "D", "-", "bitshuffle-lz4, plane-aware"),
    "stock zstd": (ORANGE, "o", (0, (4, 2)), "bitshuffle-zstd, stock (zstd level 2)"),
    "pac zstd": (ORANGE, "D", "-", "bitshuffle-zstd, plane-aware"),
}
TRANSCODE = {
    "decode + stock zstd-2": (ORANGE, "o", (0, (4, 2)), "lz4 decode + untranspose,\nthen stock zstd level 2"),
    "decode + pac zstd": (TEAL, "s", (0, (1, 1.5)), "lz4 decode + untranspose,\nthen plane-aware zstd"),
    "repack": (ORANGE, "D", "-", "pac_repack_chunk\n(no bit transpose)"),
}
default = R["default_block_kb"]  # {"lz4": 56, "zstd": 248}
def slow(c, kb):
    """Bit planes a multiple of 4096 bytes long: the transpose is slow there (docs/benchmarks.md)."""
    item = {"uint16": 2, "uint32": 4}[c["dtype"]]
    return (kb * 1024 // (8 * item)) % 4096 == 0


def style(ax):
    ax.set_facecolor(SURF)
    ax.grid(True, color=GRID, lw=0.6)
    ax.set_axisbelow(True)
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)


def gbs(c, gpix):
    """Gpixel/s to GB/s of uncompressed pixels."""
    return gpix * {"uint16": 2, "uint32": 4}[c["dtype"]]


def curve(ax, c, pts, speed, colour, marker, ls, label):
    pts = sorted((s for s in pts if not slow(c, s["block_kb"])), key=lambda s: s["block_kb"])
    x, y = [gbs(c, s[speed]) for s in pts], [s["ratio"] for s in pts]
    ax.plot(x, y, color=colour, ls=ls, lw=1.2, alpha=0.8, zorder=2)
    ax.scatter(x, y, s=14, marker=marker, facecolor=colour if ls == "-" else SURF, edgecolor=colour, lw=0.9,
               zorder=3, label=label)
    for s, off, ha in ((pts[0], (-4, -9), "right"), (pts[-1], (4, 3), "left")):
        ax.annotate("%d kB" % s["block_kb"], (gbs(c, s[speed]), s["ratio"]), xytext=off, textcoords="offset points",
                    fontsize=6.5, color=colour, ha=ha)
    return x, y


def pick(pts, kb, **kw):
    return [s for s in pts if s["block_kb"] == kb and all(s[k] == v for k, v in kw.items())][0]


def arrow(ax, c, s, speed, text, dx, dy, colour, xmax, yrange):
    x, y = gbs(c, s[speed]), s["ratio"]
    ax.scatter([x], [y], s=70, marker="D", facecolor=colour, edgecolor=INK, lw=1.3, zorder=6)
    ax.annotate(text, (x, y), xytext=(x + dx * xmax, y + dy * yrange), fontsize=8.5, fontweight="bold", color=INK,
                ha="left" if dx > 0 else "center", va="center", zorder=7,
                arrowprops=dict(arrowstyle="-|>", color=INK, lw=1.2, shrinkA=2, shrinkB=6))


def limits(ax, xs, ys):
    lo, top = min(ys), max(ys)
    ax.set_xlim(0, 1.18 * max(xs))
    ax.set_ylim(lo - 0.06 * (top - lo), top + 0.1 * (top - lo))
    return 1.18 * max(xs), top - lo


def encode_or_decode(ax, c, speed):
    xs, ys = [], []
    for name, (colour, marker, ls, label) in ENCODERS.items():
        x, y = curve(ax, c, [s for s in c["sweep"] if s["encoder"] == name], speed, colour, marker, ls, label)
        xs += x
        ys += y
    det = pick(c["sweep"], 8, encoder="stock lz4")
    ax.scatter([gbs(c, det[speed])], [det["ratio"]], s=150, marker="*", color=INK, zorder=5,
               label="detector format today:\nstock lz4, 8 kB")
    if speed == "enc_gpix":
        tr = gbs(c, pick(c["transpose"], default["zstd"])["gpix"])
        ax.axvline(tr, color=MUTED, ls="--", lw=1.1, zorder=1, label="bit transpose alone (kcb)")
        xs.append(tr)
    xmax, yr = limits(ax, xs, ys)
    arrow(ax, c, pick(c["sweep"], default["zstd"], encoder="pac zstd"), speed, "PAC zstd\n%d kB" % default["zstd"],
          0.10, -0.16, ORANGE, xmax, yr)
    arrow(ax, c, pick(c["sweep"], default["lz4"], encoder="pac lz4"), speed, "PAC lz4 %d kB" % default["lz4"],
          -0.12, 0.32, BLUE, xmax, yr)


def transcode(ax, c):
    xs, ys = [], []
    for way, (colour, marker, ls, label) in TRANSCODE.items():
        x, y = curve(ax, c, [s for s in c["transcode"] if s["method"] == way], "gpix", colour, marker, ls, label)
        xs += x
        ys += y
    r_in = c["repack"]["ratio_lz4_8k"]
    ax.axhline(r_in, color=INK, ls=":", lw=1.0, zorder=1, label="ratio of the input\n(detector lz4, 8 kB)")
    ys.append(r_in)
    xmax, yr = limits(ax, xs, ys)
    arrow(ax, c, pick(c["transcode"], default["zstd"], method="repack"), "gpix", "PAC repack\n%d kB" % default["zstd"],
          0.10, -0.2, ORANGE, xmax, yr)


rows = [("encode: pixels to chunk, raw GB/s per core", lambda ax, c: encode_or_decode(ax, c, "enc_gpix")),
        ("decode: chunk to pixels, raw GB/s per core", lambda ax, c: encode_or_decode(ax, c, "dec_gpix")),
        ("transcode lz4 8 kB to zstd, raw GB/s per core", transcode)]
fig, axes = plt.subplots(len(rows), len(CASES), figsize=(3.6 * len(CASES) + 2.6, 3.9 * len(rows) + 1.2),
                         facecolor=SURF, squeeze=False)
for i, (xlabel, draw) in enumerate(rows):
    for j, case in enumerate(CASES):
        ax = axes[i, j]
        style(ax)
        c = R["cases"][case]
        draw(ax, c)
        if i == 0:
            ax.set_title("%s\n%s, %.0f %% zero" % (case.replace(" NMC811", ""), c["dtype"], 100 * c["fraction_zero"]),
                         loc="left", fontsize=9.5)
        ax.set_xlabel(xlabel, fontsize=8)
        if j == 0:
            ax.set_ylabel("compression ratio")
    h, l = axes[i, 0].get_legend_handles_labels()
    axes[i, -1].legend(h, l, loc="upper left", bbox_to_anchor=(1.03, 1.0), frameon=False, fontsize=7.5,
                       labelspacing=0.9)
H = fig.get_figheight()
fig.suptitle("Compression ratio against speed, one core (up and to the right is better)", x=0.01, y=1 - 0.15 / H,
             ha="left", va="top", fontsize=12, fontweight="bold")
fig.text(0.01, 1 - 0.5 / H,
         "%s, frames in memory, the same bit transpose (kcb) for all.  Each line joins one method's block sizes, "
         "8 to 496 kB,\nleaving out those whose bit planes are a multiple of 4096 bytes (the transpose is slow there).  Measured %s, pac %s."
         % (R["machine"], R["date"], R["version"]), fontsize=7.5, color=MUTED, va="top")
fig.subplots_adjust(left=0.045, right=0.835, top=1 - 1.25 / H, bottom=0.45 / H, hspace=0.38, wspace=0.22)
out = os.path.join(args.out, "pareto.png")
fig.savefig(out, dpi=110, facecolor=SURF)
plt.close(fig)
print("wrote", out)
