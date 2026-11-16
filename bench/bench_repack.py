"""pac_repack_chunk on the chunks of an HDF5 file, on several threads.

    python3 bench/bench_repack.py FILE.h5 DATASET [--frames N] [--threads 1,4,8,16] [--codec zstd]
                                  [--as-detector] [--uint8 DIV] [--raw PCT]

The file's chunks must be bitshuffle-lz4 or bitshuffle-zstd; with --as-detector the frames are first
written again as the detector software does (stock bitshuffle-lz4, 8 kB blocks; needs
build/libpacbench.so), whatever the file holds.  The frames are read into memory first
(reading and writing files is not timed), then repacked to blocks of 248 kB (zstd) or 56 kB (lz4),
one encoder per thread.

--uint8 DIV (implies --as-detector) makes 8-bit frames, as for the detector's fastest mode: counts
divided by DIV and clipped at 255.  There is no real 8-bit data on Zenodo; silicon with DIV 2 is
the sparse case, CeO2 with DIV 2 the dense one (0.1 % of pixels clipped).
--raw PCT is pac_set_raw_literals (default 0; 5 or 10 trade a few % of ratio for speed on dense data).

Reports the rate in pixel bytes, in bytes read (the chunks as stored) and in bytes written, which is
what to compare with a network link or a disk.  Needs numpy, h5py, hdf5plugin; the first chunk of
the result is decoded with hdf5plugin and compared with the frame.
"""
import argparse
import ctypes
import glob
import os
import struct
import threading
import time

import h5py
import hdf5plugin
import numpy as np

p = argparse.ArgumentParser()
p.add_argument("file")
p.add_argument("dataset")
p.add_argument("--frames", type=int, default=64)
p.add_argument("--threads", default="1,4,8,16")
p.add_argument("--codec", default="zstd", choices=["zstd", "lz4"])
p.add_argument("--as-detector", action="store_true")
p.add_argument("--uint8", type=int, default=0, metavar="DIV")
p.add_argument("--raw", type=int, default=0, metavar="PCT")
p.add_argument("--lib", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "libpac.so"))
args = p.parse_args()
args.as_detector = args.as_detector or args.uint8 > 0


def read(ds, i):
    frame = ds[i]
    if args.uint8:
        frame = np.minimum(frame // args.uint8, 255).astype(np.uint8)
    return np.ascontiguousarray(frame)

vp, sz, ci = ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int
pac = ctypes.CDLL(os.path.abspath(args.lib))
pac.pac_create.restype = vp
pac.pac_create.argtypes = [ci]
pac.pac_bound.restype = sz
pac.pac_bound.argtypes = [sz, sz, sz]
pac.pac_repack_chunk.restype = sz
pac.pac_set_raw_literals.argtypes = [vp, ci]
pac.pac_repack_chunk.argtypes = [vp, vp, sz, vp, sz, ci, sz, sz]
LZ4, ZSTD = 1, 2
codec = ZSTD if args.codec == "zstd" else LZ4
BLOCK = 253952 if codec == ZSTD else 57344  # PAC_ZSTD_BLOCK, PAC_LZ4_BLOCK

with h5py.File(args.file, "r") as h:
    ds = h[args.dataset]
    idx = [int(i) for i in np.unique(np.linspace(0, ds.shape[0] - 1, args.frames).round())]
    first = read(ds, idx[0])
    item, fb = first.dtype.itemsize, first.nbytes
    if args.as_detector:
        bench = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(args.lib)), "libpacbench.so"))
        bench.ref_encode.restype = sz
        bench.ref_encode.argtypes = [ci, ci, vp, sz, vp, sz, sz, sz]
        tmp = np.zeros(2 * fb + 65536, np.uint8)
        stored, chunks = LZ4, []
        for i in idx:
            frame = read(ds, i)
            m = bench.ref_encode(LZ4, 0, tmp.ctypes.data, tmp.size, frame.ctypes.data, fb, item, 8192)
            chunks.append(tmp[:m].tobytes())
    else:
        filt = ds.id.get_create_plist().get_filter(0)
        assert filt[0] == 32008 and filt[2][4] in (2, 3), "not a bitshuffle-lz4/zstd dataset"
        stored = LZ4 if filt[2][4] == 2 else ZSTD
        chunks = [ds.id.read_direct_chunk((i,) + (0,) * (ds.ndim - 1))[1] for i in idx]
n = len(chunks)
sblock = struct.unpack(">I", chunks[0][8:12])[0] or 8192  # 0 = the filter's default
block = BLOCK if BLOCK % sblock == 0 else sblock
cap = pac.pac_bound(fb, item, block)
srcs = [ctypes.c_char_p(c) for c in chunks]
size_in = sum(len(c) for c in chunks)
if args.uint8:
    print("counts divided by %d and clipped at 255: %.3f %% of the first frame clipped" % (
        args.uint8, 100.0 * (first == 255).mean()))
print("%s: %d frames of %d bytes (%s), %s bitshuffle-%s with %d kB blocks, ratio %.2f" % (
    os.path.basename(args.file), n, fb, first.dtype, "written again as" if args.as_detector else "stored as",
    "lz4" if stored == LZ4 else "zstd", sblock // 1024, n * fb / size_in))


def run(nthreads):
    out_sizes = np.zeros(n, np.int64)
    bounds = np.linspace(0, n, nthreads + 1).astype(int)
    start = threading.Barrier(nthreads + 1)
    def work(t):
        enc = pac.pac_create(codec)
        pac.pac_set_raw_literals(enc, args.raw)
        buf = np.zeros(cap, np.uint8)
        start.wait()
        for i in range(bounds[t], bounds[t + 1]):
            out_sizes[i] = pac.pac_repack_chunk(enc, buf.ctypes.data, cap, srcs[i], len(chunks[i]), stored, item, block)
    ts = [threading.Thread(target=work, args=(t,)) for t in range(nthreads)]
    [t.start() for t in ts]
    start.wait()
    t0 = time.perf_counter()
    [t.join() for t in ts]
    dt = time.perf_counter() - t0
    assert out_sizes.min() > 0
    return dt, int(out_sizes.sum())


# one chunk through hdf5plugin's decoder
enc = pac.pac_create(codec)
pac.pac_set_raw_literals(enc, args.raw)
buf = np.zeros(cap, np.uint8)
m = pac.pac_repack_chunk(enc, buf.ctypes.data, cap, srcs[0], len(chunks[0]), stored, item, block)
bs = ctypes.CDLL(glob.glob(hdf5plugin.PLUGIN_PATH + "/libh5bshuf.so")[0])
f = bs.bshuf_decompress_zstd if codec == ZSTD else bs.bshuf_decompress_lz4
f.restype = ctypes.c_int64
f.argtypes = [vp, vp, sz, sz, sz]
out = np.zeros(fb, np.uint8)
assert f(buf.ctypes.data + 12, out.ctypes.data, fb // item, item, block // item) == m - 12
assert np.array_equal(out, first.reshape(-1).view(np.uint8))

print("%8s %12s %14s %14s %8s" % ("threads", "pixels GB/s", "read GB/s", "written GB/s", "ratio"))
for nt in [int(t) for t in args.threads.split(",")]:
    best = min(run(nt) for _ in range(3))
    dt, size_out = best
    print("%8d %12.2f %14.3f %14.3f %8.2f" % (nt, n * fb / 1e9 / dt, size_in / 1e9 / dt, size_out / 1e9 / dt, n * fb / size_out), flush=True)
