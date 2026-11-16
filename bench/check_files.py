"""Check that pac-zstd accepts your HDF5 files.

    python3 bench/check_files.py PATH [PATH ...]      (files, or folders searched for *.h5 *.hdf5 *.nxs)

For every dataset of at least two dimensions with 1, 2, 4 or 8 byte elements, three chunks or
frames (first, middle, last):

  - stored with the bitshuffle filter (lz4 or zstd): the stored chunk goes through pac_repack_chunk
    to zstd and to lz4, and the pixels through pac_compress_chunk;
  - stored any other way: the pixels (read by h5py) through pac_compress_chunk, zstd and lz4.

Each result is decoded with the reference decoder (LZ4 or zstd, then kcb's untranspose) and compared
with the pixels.  Prints one line per dataset and a summary; the exit code is the number of
failures.  Needs numpy, h5py, hdf5plugin and build/libpacbench.so (make).
"""
import ctypes
import os
import struct
import sys

import h5py
import hdf5plugin
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
vp, sz, ci = ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int
lib = ctypes.CDLL(os.path.join(HERE, "..", "build", "libpacbench.so"))
lib.pac_create.restype = vp
lib.pac_create.argtypes = [ci]
lib.pac_bound.restype = sz
lib.pac_bound.argtypes = [sz, sz, sz]
lib.pac_compress_chunk.restype = sz
lib.pac_compress_chunk.argtypes = [vp, vp, sz, vp, sz, sz, sz]
lib.pac_repack_chunk.restype = sz
lib.pac_repack_chunk.argtypes = [vp, vp, sz, vp, sz, ci, sz, sz]
lib.ref_decode.restype = sz
lib.ref_decode.argtypes = [ci, vp, sz, sz, vp, sz]
LZ4, ZSTD, BLOCK = 1, 2, 253952
enc = {LZ4: lib.pac_create(LZ4), ZSTD: lib.pac_create(ZSTD)}
MAX_BYTES = 256 << 20  # the largest chunk or frame looked at


def files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, names in os.walk(p):
                dirs[:] = [d for d in dirs if not d.startswith(".")]
                for n in sorted(names):
                    if n.endswith((".h5", ".hdf5", ".nxs")):
                        yield os.path.join(root, n)
        else:
            yield p


def bitshuffle_codec(ds):
    """LZ4 or ZSTD if the dataset's only filter is bitshuffle with that compressor, else None"""
    pl = ds.id.get_create_plist()
    if pl.get_nfilters() != 1:
        return None
    f = pl.get_filter(0)
    if f[0] != 32008 or len(f[2]) < 5 or f[2][4] not in (2, 3):
        return None
    return LZ4 if f[2][4] == 2 else ZSTD


def roundtrip(pixels, item, block):
    """pixels (bytes) -> chunk -> pixels, both codecs; returns (ok, zstd chunk size)"""
    n = len(pixels)
    cap = lib.pac_bound(n, item, block)
    buf, out = ctypes.create_string_buffer(cap), ctypes.create_string_buffer(n)
    ok, zsize = True, 0
    for codec in (ZSTD, LZ4):
        m = lib.pac_compress_chunk(enc[codec], buf, cap, pixels, n, item, block)
        ok &= m > 0 and lib.ref_decode(codec, buf, m, item, out, n) == n and out.raw == pixels
        if codec == ZSTD:
            zsize = m
    return ok, zsize


def check(ds):
    """Returns (what was done, ok, stored bytes, pac zstd bytes, pixel bytes) or None if not looked at."""
    item = ds.dtype.itemsize
    if ds.ndim < 2 or item not in (1, 2, 4, 8) or ds.dtype.kind not in "iuf" or ds.size * item < 4096:
        return None
    codec = bitshuffle_codec(ds) if ds.chunks else None
    ok, stored, packed, pixels_total = True, 0, 0, 0
    if codec:
        nchunks = ds.id.get_num_chunks()
        if nchunks == 0:
            return None
        chunk_bytes = int(np.prod(ds.chunks)) * item
        if chunk_bytes > MAX_BYTES:
            return None
        pix = ctypes.create_string_buffer(chunk_bytes)
        for k in sorted({0, nchunks // 2, nchunks - 1}):
            info = ds.id.get_chunk_info(k)
            chunk = ds.id.read_direct_chunk(info.chunk_offset)[1]
            if info.filter_mask or len(chunk) < 12:
                continue
            sblock = struct.unpack(">I", chunk[8:12])[0] or (8192 // item // 8) * 8 * item
            if lib.ref_decode(codec, chunk, len(chunk), item, pix, chunk_bytes) != chunk_bytes:
                ok = False
                continue
            # the reference decoder against h5py + hdf5plugin, where the chunk is inside the dataset
            sel = tuple(slice(o, o + c) for o, c in zip(info.chunk_offset, ds.chunks))
            if all(o + c <= s for o, c, s in zip(info.chunk_offset, ds.chunks, ds.shape)):
                ok &= ds[sel].tobytes() == pix.raw
            block = BLOCK if BLOCK % sblock == 0 else sblock
            cap = lib.pac_bound(chunk_bytes, item, block)
            buf, out = ctypes.create_string_buffer(cap), ctypes.create_string_buffer(chunk_bytes)
            for target in (ZSTD, LZ4):
                m = lib.pac_repack_chunk(enc[target], buf, cap, chunk, len(chunk), codec, item, block)
                ok &= m > 0 and lib.ref_decode(target, buf, m, item, out, chunk_bytes) == chunk_bytes and out.raw == pix.raw
                if target == ZSTD:
                    packed += m
            ok &= roundtrip(pix.raw, item, BLOCK)[0]
            stored += len(chunk)
            pixels_total += chunk_bytes
        return "repack + compress", ok, stored, packed, pixels_total
    n0 = ds.shape[0]
    frame_bytes = ds.size * item // n0
    if frame_bytes > MAX_BYTES:
        return None
    for i in sorted({0, n0 // 2, n0 - 1}) if frame_bytes >= 4096 else [slice(None)]:
        pixels = np.ascontiguousarray(ds[i]).tobytes()
        if len(pixels) % (8 * item) and len(pixels) < 8 * item:
            continue
        good, zsize = roundtrip(pixels, item, BLOCK)
        ok &= good
        packed += zsize
        pixels_total += len(pixels)
    stored = int(ds.id.get_storage_size() * pixels_total / max(1, ds.size * item))
    return "compress", ok, stored, packed, pixels_total


nfail = nok = nskip = 0
for fname in files(sys.argv[1:]):
    try:
        h = h5py.File(fname, "r")
    except OSError as e:
        print("%s: cannot open (%s)" % (fname, e))
        nskip += 1
        continue
    found = []
    h.visititems(lambda name, obj: found.append(name) if isinstance(obj, h5py.Dataset) else None)
    for name in found:
        ds = h[name]
        try:
            r = check(ds)
        except Exception as e:  # a dataset h5py cannot read (missing filter, external link, ...)
            print("%s %s: not read (%s: %s)" % (fname, name, type(e).__name__, str(e)[:60]))
            nskip += 1
            continue
        if r is None:
            continue
        what, ok, stored, packed, pixels = r
        nok += ok
        nfail += not ok
        print("%-4s %s %s %s %s: %s; ratio stored %.3g, pac zstd %.3g" % (
            "ok" if ok else "FAIL", fname, name, "x".join(map(str, ds.shape)), ds.dtype, what,
            pixels / max(1, stored), pixels / max(1, packed)), flush=True)
    h.close()
print("\n%d datasets ok, %d failed, %d not read" % (nok, nfail, nskip))
sys.exit(nfail)
