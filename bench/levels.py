"""pac zstd against stock bitshuffle-zstd at every zstd level.

    python3 bench/levels.py [--core K] [--out bench/results/levels.json] [--only CASE ...]
    python3 bench/plot_levels.py                   # docs/levels.png, from that file

Three files, each as uint16 and as uint32 (values cast, not rescaled; gaps as written), 248 kB
blocks for both.  Stock = kcb bit transpose, then ZSTD_compressCCtx at the level, per block.
Detector gaps are the type's maximum, 2^n-1 (as sweep.py "max"): the ID11 masked pixels, and
thaumatin's own 65535 (4294967295 in the uint32 case).
Encode speed: best of 3 passes over the frames (1 pass from level 16), MB/s of pixels, one core.
Decoding is not timed; the first frame of every encoder is decoded and compared.
Needs numpy, h5py, hdf5plugin.
"""
import argparse
import ctypes
import json
import os
import platform
import time

import h5py
import hdf5plugin  # noqa: F401
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ID11 = "/entry_0000/ESRF-ID11/eiger/data"
#        case: (file, dataset, frames, cast to)
CASES = {
    "silicon uint16": ("silicon_u16_zstd2_256k.h5", ID11, 8, None),
    "silicon uint32": ("silicon_u16_zstd2_256k.h5", ID11, 8, "uint32"),
    "thaumatin uint16": ("thaumatin_9_1_000001.h5", "data", 4, None),
    "thaumatin uint32": ("thaumatin_9_1_000001.h5", "data", 4, "uint32"),
    "CeO2 uint16": ("ceo2_u32_zstd2_256k.h5", ID11, 8, "uint16"),
    "CeO2 uint32": ("ceo2_u32_zstd2_256k.h5", ID11, 8, None),
}
ZSTD, BLOCK = 2, 253952  # PAC_ZSTD_BLOCK
LEVELS = list(range(-20, 0)) + list(range(1, 23))

p = argparse.ArgumentParser()
p.add_argument("--data", default=os.path.join(HERE, "data"))
p.add_argument("--lib", default=os.path.join(HERE, "..", "build", "libpacbench.so"))
p.add_argument("--out", default=os.path.join(HERE, "results", "levels.json"))
p.add_argument("--core", type=int, default=None, help="pin to this core")
p.add_argument("--only", nargs="*", help="only the cases whose name contains one of these")
args = p.parse_args()
if args.core is not None:
    if not hasattr(os, "sched_setaffinity"):
        raise SystemExit("--core requires os.sched_setaffinity (not available on this platform)")
    os.sched_setaffinity(0, {args.core})

vp, sz, ci = ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int
lib = ctypes.CDLL(os.path.abspath(args.lib))
lib.pac_create.restype = vp
lib.pac_create.argtypes = [ci]
lib.pac_bound.restype = sz
lib.pac_bound.argtypes = [sz, sz, sz]
lib.pac_compress_chunk.restype = sz
lib.pac_compress_chunk.argtypes = [vp, vp, sz, vp, sz, sz, sz]
lib.pac_isa.restype = ctypes.c_char_p
lib.pac_isa.argtypes = [vp]
lib.ref_encode.restype = sz
lib.ref_encode.argtypes = [ci, ci, vp, sz, vp, sz, sz, sz]
lib.ref_decode.restype = sz
lib.ref_decode.argtypes = [ci, vp, sz, sz, vp, sz]
enc = lib.pac_create(ZSTD)
try:  # the zstd inside libpacbench.so is static; its range is only known if exported
    lib.ZSTD_minCLevel.restype = lib.ZSTD_maxCLevel.restype = ci
    zrange = [lib.ZSTD_minCLevel(), lib.ZSTD_maxCLevel()]
except AttributeError:
    zrange = None


def load(case):
    fname, dsname, nframes, cast = CASES[case]
    with h5py.File(os.path.join(args.data, fname), "r") as h:
        ds = h[dsname]
        idx = [int(i) for i in np.unique(np.linspace(0, ds.shape[0] - 1, nframes).round())]
        frames = np.stack([ds[i] for i in idx])
    gap = (frames == 65535) if fname.startswith("thaumatin") else None
    if cast:
        assert frames.max() <= np.iinfo(cast).max
        frames = frames.astype(cast)
    if gap is None:  # ID11: gaps are 0 in the file, the detector mask says where they are
        with h5py.File(os.path.join(args.data, "mask.h5"), "r") as h:
            gap = np.broadcast_to(h["mask"][()] != 0, frames.shape)
    frames[gap] = np.iinfo(frames.dtype).max
    return frames


def best(fn, reps=3):
    t = 1e30
    for _ in range(reps):
        t0 = time.perf_counter()
        fn()
        t = min(t, time.perf_counter() - t0)
    return t


def cpu_name():
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def pac_version():
    for line in open(os.path.join(HERE, "..", "src", "pac.h")):
        if line.startswith("#define PAC_VERSION"):
            return line.split('"')[1]
    return "unknown"


out_path = os.path.abspath(args.out)
previous = json.load(open(out_path)) if args.only and os.path.exists(out_path) else None
results = {"machine": cpu_name(), "date": time.strftime("%Y-%m-%d"), "version": pac_version(),
           "core": args.core, "gaps": "2^n-1", "block_kb": BLOCK // 1024, "zstd_library_levels": zrange, "zstd_levels_used": [LEVELS[0], LEVELS[-1]],
           "cases": {}}
print("%s, pac %s, core %s, zstd levels %s to %s (library: %s)" % (results["machine"], results["version"], args.core, LEVELS[0], LEVELS[-1], zrange))
t_start = time.time()
for case in CASES:
    if args.only and not any(o in case for o in args.only):
        continue
    frames = load(case)
    raw = frames.reshape(len(frames), -1)
    n, npix = raw.shape
    item, fb = raw.dtype.itemsize, raw[0].nbytes
    cap = max(lib.pac_bound(fb, item, BLOCK), 2 * fb + 65536)
    buf, out = np.zeros(cap, np.uint8), np.zeros(fb, np.uint8)
    mb = n * fb / 1e6
    res = {"dtype": str(raw.dtype), "frames": n, "shape": list(frames.shape[1:]), "results": []}
    print("\n%s: %d frames of %s %s" % (case, n, "x".join(map(str, frames.shape[1:])), raw.dtype))
    print("%-16s %6s %9s %10s" % ("encoder", "level", "ratio", "enc MB/s"))

    def measure(one, reps):
        packed = []
        for i in range(n):
            m = one(i)
            assert m > 0
            packed.append(buf[:m].tobytes())
        assert lib.ref_decode(ZSTD, packed[0], len(packed[0]), item, out.ctypes.data, fb) == fb
        assert np.array_equal(out, raw[0].view(np.uint8)), "decoded pixels differ"
        t_enc = best(lambda: [one(i) for i in range(n)], reps)
        return mb * 1e6 / sum(len(c) for c in packed), mb / t_enc

    one = lambda i: lib.pac_compress_chunk(enc, buf.ctypes.data, cap, raw[i].ctypes.data, fb, item, BLOCK)
    ratio, e = measure(one, 3)
    res["results"].append({"encoder": "pac zstd", "ratio": ratio, "enc_mbs": e})
    print("%-16s %6s %9.3f %10.0f" % ("pac zstd", "-", ratio, e), flush=True)
    for lv in LEVELS:
        one = lambda i: lib.ref_encode(ZSTD, lv, buf.ctypes.data, cap, raw[i].ctypes.data, fb, item, BLOCK)
        ratio, e = measure(one, 1 if lv >= 16 else 3)
        res["results"].append({"encoder": "stock zstd", "level": lv, "ratio": ratio, "enc_mbs": e})
        print("%-16s %6d %9.3f %10.0f" % ("stock zstd", lv, ratio, e), flush=True)
    results["cases"][case] = res

if previous:  # --only: the other cases of the file are kept
    previous["cases"].update(results["cases"])
    results = previous
results["run_seconds"] = round(time.time() - t_start)
os.makedirs(os.path.dirname(out_path), exist_ok=True)
json.dump(results, open(out_path, "w"), indent=1)
print("\nwrote", args.out, "in %d s" % (time.time() - t_start))
