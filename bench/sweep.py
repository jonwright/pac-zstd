"""Ratio and one-core speed against block size, and the lz4 -> zstd repack, for every test case.

    python3 bench/get_data.py                      # once: the files from Zenodo, into bench/data
    make                                           # build/libpacbench.so
    python3 bench/sweep.py [--core K] [--out bench/results/results.json] [--only CASE ...]
    python3 bench/plot.py                          # the figures and docs/results.md, from that file

Test cases: the three data sets, each with the detector gaps at 0 and at the type's maximum (as the
detector writes them), and CeO2 both as the uint32 it was stored as and as uint16 (its counts fit);
and, if it was fetched, one s3DXRD scan file exactly as the detector software wrote it.
With --only, the other cases already in the results file are kept.

For each case and block size, four encoders write the same frames as bitshuffle HDF5 chunks:

    stock lz4 / stock zstd-2   kcb bit transpose, then LZ4_compress_default / ZSTD level 2 per block
    pac lz4 / pac zstd         this library

All from the libraries vendored here (bench/ref_codec.c), so the pairs differ only in how the
transposed block is compressed.  Every chunk is decoded (LZ4 or zstd, then kcb's untranspose) and
compared with the pixels; at the default block sizes the pac chunks are also written to an HDF5
file and read back with h5py + hdf5plugin.  Speeds are the best of 3 passes over the frames, held
in memory, on one core.  Decoding is timed too, with the stock decoders (LZ4 or zstd, then kcb's
untranspose).

The bit transpose alone (kcb, block by block, as every encoder above does it) is timed at every
block size: the ceiling for any bitshuffle encoder.

Transcoding: each frame as stock lz4 with 8 kB blocks (what the detector software writes) to zstd
at every block size, three ways:

    repack                  pac_repack_chunk, no bit transpose
    decode + stock zstd-2   the usual way: decode, untranspose, transpose, zstd level 2
    decode + pac zstd       decode, untranspose, then pac_compress_chunk

The repack at 248 kB is also kept on its own ("repack"), as before.

Needs numpy, h5py, hdf5plugin.
"""
import argparse
import ctypes
import json
import os
import platform
import time

import h5py
import hdf5plugin
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ID11 = "/entry_0000/ESRF-ID11/eiger/data"
#        case: (file, dataset, frames, cast to, gaps)   gaps: "zero" or "max"
CASES = {
    "thaumatin, gaps 65535": ("thaumatin_9_1_000001.h5", "data", 8, None, "as written"),
    "thaumatin, gaps 0": ("thaumatin_9_1_000001.h5", "data", 8, None, "zero"),
    "silicon, gaps 0": ("silicon_u16_zstd2_256k.h5", ID11, 32, None, "as written"),
    "silicon, gaps 65535": ("silicon_u16_zstd2_256k.h5", ID11, 32, None, "max"),
    "CeO2 uint32, gaps 0": ("ceo2_u32_zstd2_256k.h5", ID11, 16, None, "as written"),
    "CeO2 uint32, gaps 2^32-1": ("ceo2_u32_zstd2_256k.h5", ID11, 16, None, "max"),
    "CeO2 uint16, gaps 0": ("ceo2_u32_zstd2_256k.h5", ID11, 16, "uint16", "as written"),
    "CeO2 uint16, gaps 65535": ("ceo2_u32_zstd2_256k.h5", ID11, 16, "uint16", "max"),
    # optional (get_data.py --s3dxrd, 18 GB): as the detector wrote it, counts in uint32, gaps at 65535
    "s3DXRD NMC811 uint32, gaps 65535": ("NMC811_rptZ45999/scan0040/eiger_0000.h5", ID11, 16, None, "as written"),
}
BLOCKS_KB = [8, 16, 24, 32, 48, 56, 64, 96, 128, 192, 248, 256, 384, 496, 512]
LZ4, ZSTD = 1, 2
DEFAULT_KB = {LZ4: 56, ZSTD: 248}  # PAC_LZ4_BLOCK, PAC_ZSTD_BLOCK

p = argparse.ArgumentParser()
p.add_argument("--data", default=os.path.join(HERE, "data"))
p.add_argument("--lib", default=os.path.join(HERE, "..", "build", "libpacbench.so"))
p.add_argument("--out", default=os.path.join(HERE, "results", "results.json"))
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
lib.pac_repack_chunk.restype = sz
lib.pac_repack_chunk.argtypes = [vp, vp, sz, vp, sz, ci, sz, sz]
lib.pac_isa.restype = ctypes.c_char_p
lib.pac_isa.argtypes = [vp]
lib.ref_encode.restype = sz
lib.ref_encode.argtypes = [ci, ci, vp, sz, vp, sz, sz, sz]
lib.ref_decode.restype = sz
lib.ref_decode.argtypes = [ci, vp, sz, sz, vp, sz]
lib.ref_transpose.restype = sz
lib.ref_transpose.argtypes = [vp, vp, sz, sz, sz]
enc = {LZ4: lib.pac_create(LZ4), ZSTD: lib.pac_create(ZSTD)}


def load(case):
    fname, dsname, nframes, cast, gaps = CASES[case]
    with h5py.File(os.path.join(args.data, fname), "r") as h:
        ds = h[dsname]
        idx = [int(i) for i in np.unique(np.linspace(0, ds.shape[0] - 1, nframes).round())]
        frames = np.stack([ds[i] for i in idx])
    if cast:
        assert frames.max() <= np.iinfo(cast).max
        frames = frames.astype(cast)
    top = np.iinfo(frames.dtype).max
    if gaps == "zero":  # thaumatin: the gaps (and any saturated pixel) are the type's maximum
        frames[frames == top] = 0
    elif gaps == "max":  # the ID11 files: gaps are 0, the detector mask says where they are
        with h5py.File(os.path.join(args.data, "mask.h5"), "r") as h:
            frames[:, h["mask"][()] != 0] = top
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
           "parse": lib.pac_isa(enc[LZ4]).decode(), "default_block_kb": {"lz4": DEFAULT_KB[LZ4], "zstd": DEFAULT_KB[ZSTD]}, "cases": {}}
print("%s, parse kernel %s, pac %s" % (results["machine"], results["parse"], results["version"]))
for case in CASES:
    if args.only and not any(o in case for o in args.only):
        continue
    if not os.path.exists(os.path.join(args.data, CASES[case][0])):
        print("\n%s: %s is not in %s, skipped" % (case, CASES[case][0], args.data))
        continue
    frames = load(case)
    shape = frames.shape[1:]
    raw = frames.reshape(len(frames), -1)
    n, npix = raw.shape
    item, fb = raw.dtype.itemsize, raw[0].nbytes
    cap = max(lib.pac_bound(fb, item, 512 * 1024), 2 * fb + 65536)
    buf, out = np.zeros(cap, np.uint8), np.zeros(fb, np.uint8)
    gpix = n * npix / 1e9
    top = np.iinfo(raw.dtype).max
    res = {"dtype": str(raw.dtype), "frames": n, "shape": list(shape), "sweep": [],
           "fraction_zero": float((raw == 0).mean()), "fraction_max": float((raw == top).mean())}
    print("\n%s: %d frames of %s %s; %.1f%% zero, %.1f%% at the maximum" % (
        case, n, "x".join(map(str, shape)), raw.dtype, 100 * res["fraction_zero"], 100 * res["fraction_max"]))
    print("%-24s %6s %9s %12s %12s" % ("encoder", "block", "ratio", "enc GB/s", "dec GB/s"))
    GB = item  # Gpixel/s to GB/s of pixels

    def measure(codec, ours, block):
        if ours:
            one = lambda i: lib.pac_compress_chunk(enc[codec], buf.ctypes.data, cap, raw[i].ctypes.data, fb, item, block)
        else:
            one = lambda i: lib.ref_encode(codec, 2, buf.ctypes.data, cap, raw[i].ctypes.data, fb, item, block)
        packed = []
        for i in range(n):
            m = one(i)
            assert m > 0
            packed.append(buf[:m].tobytes())
            assert lib.ref_decode(codec, packed[-1], m, item, out.ctypes.data, fb) == fb
            assert np.array_equal(out, raw[i].view(np.uint8)), "decoded pixels differ"
        t_enc = best(lambda: [one(i) for i in range(n)])
        t_dec = best(lambda: [lib.ref_decode(codec, c, len(c), item, out.ctypes.data, fb) for c in packed])
        return packed, fb * n / sum(len(c) for c in packed), gpix / t_enc, gpix / t_dec

    for kb in BLOCKS_KB:
        for codec, cname in ((LZ4, "lz4"), (ZSTD, "zstd")):
            for ours in (0, 1):
                name = ("pac " if ours else "stock ") + cname
                packed, ratio, e, d = measure(codec, ours, kb * 1024)
                res["sweep"].append({"encoder": name, "block_kb": kb, "ratio": ratio, "enc_gpix": e, "dec_gpix": d})
                print("%-24s %5dk %9.2f %12.3f %12.3f" % (name, kb, ratio, e * GB, d * GB), flush=True)
                if ours and kb == DEFAULT_KB[codec]:  # the reader everybody has
                    with h5py.File("pac_check", "w", driver="core", backing_store=False) as h:
                        dset = h.create_dataset("d", frames.shape, frames.dtype, chunks=(1,) + shape,
                                                **hdf5plugin.Bitshuffle(nelems=kb * 1024 // item, cname=cname, clevel=2))
                        for i, c in enumerate(packed):
                            dset.id.write_direct_chunk((i,) + (0,) * len(shape), c)
                        assert np.array_equal(dset[()], frames), "h5py + hdf5plugin read something else"

    # the bit transpose alone, at every block size
    res["transpose"] = []
    tr = np.zeros(fb, np.uint8)
    for kb in BLOCKS_KB:
        t = best(lambda: [lib.ref_transpose(tr.ctypes.data, raw[i].ctypes.data, fb, item, kb * 1024) for i in range(n)])
        res["transpose"].append({"block_kb": kb, "gpix": gpix / t})
        print("%-24s %5dk %9s %12.3f" % ("transpose only", kb, "", gpix / t * GB), flush=True)

    # transcoding stock lz4 8 kB to zstd at every block size, three ways
    lz4in = []
    for i in range(n):
        m = lib.ref_encode(LZ4, 0, buf.ctypes.data, cap, raw[i].ctypes.data, fb, item, 8192)
        lz4in.append(buf[:m].tobytes())
    res["transcode"] = []

    def decoded(c):
        assert lib.ref_decode(LZ4, c, len(c), item, out.ctypes.data, fb) == fb
        return out.ctypes.data

    for kb in BLOCKS_KB:
        ways = {
            "repack": lambda i: lib.pac_repack_chunk(enc[ZSTD], buf.ctypes.data, cap, lz4in[i], len(lz4in[i]), LZ4, item, kb * 1024),
            "decode + stock zstd-2": lambda i: lib.ref_encode(ZSTD, 2, buf.ctypes.data, cap, decoded(lz4in[i]), fb, item, kb * 1024),
            "decode + pac zstd": lambda i: lib.pac_compress_chunk(enc[ZSTD], buf.ctypes.data, cap, decoded(lz4in[i]), fb, item, kb * 1024),
        }
        for way, one in ways.items():
            size_out = 0
            for i in range(n):
                m = one(i)
                assert m > 0
                size_out += m
            m = one(0)  # check the first frame of every way
            chk = np.zeros(fb, np.uint8)
            assert lib.ref_decode(ZSTD, buf.ctypes.data, m, item, chk.ctypes.data, fb) == fb
            assert np.array_equal(chk, raw[0].view(np.uint8)), "transcode decoded to other pixels"
            t = best(lambda: [one(i) for i in range(n)])
            res["transcode"].append({"method": way, "block_kb": kb, "ratio": n * fb / size_out, "gpix": gpix / t})
            print("%-24s %5dk %9.2f %12.3f" % (way, kb, n * fb / size_out, gpix / t * GB), flush=True)

    # the repack: stock lz4 at 8 kB in, zstd at the default block out
    src = []
    for i in range(n):
        m = lib.ref_encode(LZ4, 0, buf.ctypes.data, cap, raw[i].ctypes.data, fb, item, 8192)
        src.append(buf[:m].tobytes())
    one = lambda i: lib.pac_repack_chunk(enc[ZSTD], buf.ctypes.data, cap, src[i], len(src[i]), LZ4, item, DEFAULT_KB[ZSTD] * 1024)
    size_out = 0
    for i in range(n):
        m = one(i)
        assert m > 0 and lib.ref_decode(ZSTD, buf.ctypes.data, m, item, out.ctypes.data, fb) == fb
        assert np.array_equal(out, raw[i].view(np.uint8))
        size_out += m
    t = best(lambda: [one(i) for i in range(n)])
    size_in = sum(len(c) for c in src)
    res["repack"] = {"ratio_lz4_8k": n * fb / size_in, "ratio_zstd": n * fb / size_out, "gain": size_in / size_out,
                     "gpix": gpix / t, "pixel_gb": n * fb / 1e9 / t, "read_gb": size_in / 1e9 / t}
    print("repack lz4 8k -> zstd %dk: ratio %.2f -> %.2f, %.2f times smaller, %.2f Gpixel/s (%.2f GB/s of pixels, %.3f GB/s of lz4 read)" % (
        DEFAULT_KB[ZSTD], res["repack"]["ratio_lz4_8k"], res["repack"]["ratio_zstd"], res["repack"]["gain"],
        res["repack"]["gpix"], res["repack"]["pixel_gb"], res["repack"]["read_gb"]))
    results["cases"][case] = res

if previous:  # --only: the other cases of the file are kept as they were
    previous["cases"].update(results["cases"])
    results = previous
os.makedirs(os.path.dirname(out_path), exist_ok=True)
json.dump(results, open(out_path, "w"), indent=1)
print("\nwrote", args.out)
