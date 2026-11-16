"""What an empty frame costs, and what the detector gaps cost, with each encoder.

    python3 bench/empty_frame.py [--data bench/data]

Frames of zeros in the shapes of the test data, with the gaps at 0 and at the type's maximum (the
gap positions are those of the data: mask.h5 for the Eiger2 4M, the 65535 pixels of the first
thaumatin frame for the Eiger2 16M).  Prints a markdown table of chunk sizes in bytes.
Needs numpy, h5py, hdf5plugin and build/libpacbench.so.
"""
import argparse
import ctypes
import os

import h5py
import hdf5plugin
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
p = argparse.ArgumentParser()
p.add_argument("--data", default=os.path.join(HERE, "data"))
args = p.parse_args()
vp, sz, ci = ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int
lib = ctypes.CDLL(os.path.join(HERE, "..", "build", "libpacbench.so"))
lib.pac_create.restype = vp
lib.pac_create.argtypes = [ci]
lib.pac_compress_chunk.restype = sz
lib.pac_compress_chunk.argtypes = [vp, vp, sz, vp, sz, sz, sz]
lib.ref_encode.restype = sz
lib.ref_encode.argtypes = [ci, ci, vp, sz, vp, sz, sz, sz]
lib.ref_decode.restype = sz
lib.ref_decode.argtypes = [ci, vp, sz, sz, vp, sz]
LZ4, ZSTD, BLOCK, LZ4_BLOCK = 1, 2, 253952, 57344  # PAC_ZSTD_BLOCK, PAC_LZ4_BLOCK
enc = {LZ4: lib.pac_create(LZ4), ZSTD: lib.pac_create(ZSTD)}

with h5py.File(os.path.join(args.data, "mask.h5"), "r") as h:
    gaps4m = h["mask"][()] != 0
with h5py.File(os.path.join(args.data, "thaumatin_9_1_000001.h5"), "r") as h:
    gaps16m = h["data"][0] == 65535
CASES = [("Eiger2 4M, uint16", gaps4m, np.uint16), ("Eiger2 4M, uint32", gaps4m, np.uint32),
         ("Eiger2 16M, uint16", gaps16m, np.uint16)]
ENCODERS = [("stock lz4, 8 kB", lambda b, c, s, n, i: lib.ref_encode(LZ4, 0, b, c, s, n, i, 8192)),
            ("pac lz4, 56 kB", lambda b, c, s, n, i: lib.pac_compress_chunk(enc[LZ4], b, c, s, n, i, LZ4_BLOCK)),
            ("stock zstd level 2, 248 kB", lambda b, c, s, n, i: lib.ref_encode(ZSTD, 2, b, c, s, n, i, BLOCK)),
            ("pac zstd, 248 kB", lambda b, c, s, n, i: lib.pac_compress_chunk(enc[ZSTD], b, c, s, n, i, BLOCK))]
print("| empty frame | pixels | " + " | ".join(n for n, _ in ENCODERS) + " |")
print("|---|---:|" + "---:|" * len(ENCODERS))
for name, gaps, dtype in CASES:
    for label, value in (("gaps 0", 0), ("gaps at maximum", np.iinfo(dtype).max)):
        frame = np.zeros(gaps.shape, dtype)
        frame[gaps] = value
        n, item = frame.nbytes, frame.dtype.itemsize
        buf, out = np.zeros(2 * n + 65536, np.uint8), np.zeros(n, np.uint8)
        sizes = []
        for ename, f in ENCODERS:
            m = f(buf.ctypes.data, buf.size, frame.ctypes.data, n, item)
            codec = LZ4 if "lz4" in ename else ZSTD
            assert m > 0 and lib.ref_decode(codec, buf.ctypes.data, m, item, out.ctypes.data, n) == n
            assert np.array_equal(out, frame.reshape(-1).view(np.uint8))
            sizes.append(m)
        print("| %s, %s | %d | " % (name, label, frame.size) + " | ".join("%d" % s for s in sizes) + " |")
