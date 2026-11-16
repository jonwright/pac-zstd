"""Fetch the public data sets of the benchmarks from Zenodo (about 2.6 GB).

    python3 bench/get_data.py [--dir bench/data] [--s3dxrd]

A file that is already there with the right md5 is not fetched again (Zenodo is a free public
service), and an interrupted download resumes.  No dependencies beyond Python 3.

  thaumatin_9_1_000001.h5      Eiger2 16M, uint16, protein crystal, gaps at 65535   doi 10.5281/zenodo.2412270
  silicon_u16_zstd2_256k.h5    Eiger2 4M CdTe, uint16, sparse, gaps at 0            doi 10.5281/zenodo.23361659
  ceo2_u32_zstd2_256k.h5       Eiger2 4M CdTe, uint32, dense powder rings, gaps 0   doi 10.5281/zenodo.23361659
  mask.h5                      the detector mask of the Eiger2 4M                   doi 10.5281/zenodo.23361659

--s3dxrd also fetches NMC811_rptZ45999.7z (18 GB; Battery Imaging Library, S3DXRD at ESRF ID11,
data set 1, doi 10.5281/zenodo.19453256) and extracts one scan file from it with 7z:
NMC811_rptZ45999/scan0040/eiger_0000.h5 (Eiger2 4M CdTe, uint32, bitshuffle-lz4 as the detector
software wrote it).  The archive can be deleted afterwards.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
FILES = {  # name: (url, md5)
    "thaumatin_9_1_000001.h5": ("https://zenodo.org/api/records/2412270/files/%s/content", "f5ea1714a4fea5c053ab7fc65c812e6f"),
    "silicon_u16_zstd2_256k.h5": ("https://zenodo.org/api/records/23361660/files/%s/content", "cb3fe7636eabf8d2dffd9af692b1917e"),
    "ceo2_u32_zstd2_256k.h5": ("https://zenodo.org/api/records/23361660/files/%s/content", "39872d5b45ea89adead86b11a36022f1"),
    "mask.h5": ("https://zenodo.org/api/records/23361660/files/%s/content", "52665c08ca33aef9d5897b1d5f04a298"),
}


S3DXRD = ("NMC811_rptZ45999.7z", "https://zenodo.org/api/records/19453256/files/%s/content",
          "5d522013d5560a429969ba32370684c3", "NMC811_rptZ45999/scan0040/eiger_0000.h5")


def md5(fname):
    h = hashlib.md5()
    with open(fname, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(dst, names=None):
    os.makedirs(dst, exist_ok=True)
    for name in names or FILES:
        url, want = FILES[name]
        out = os.path.join(dst, name)
        if os.path.exists(out) and md5(out) == want:
            print("%s: there" % name)
            continue
        tmp = out + ".part"
        have = os.path.getsize(tmp) if os.path.exists(tmp) else 0
        req = urllib.request.Request(url % name, headers={"User-Agent": "pac-zstd-bench"})
        if have:
            req.add_header("Range", "bytes=%d-" % have)
        print("%s: downloading%s" % (name, " (resuming at %d bytes)" % have if have else ""))
        with urllib.request.urlopen(req) as r:
            resumed = have and getattr(r, "status", None) == 206  # a server that ignores Range sends it all
            with open(tmp, "ab" if resumed else "wb") as fh:
                shutil.copyfileobj(r, fh, 1 << 20)
        if md5(tmp) != want:
            os.remove(tmp)
            sys.exit("%s: the md5 is not %s, not kept" % (name, want))
        os.rename(tmp, out)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--dir", default=os.path.join(HERE, "data"))
    p.add_argument("--s3dxrd", action="store_true", help="also the 18 GB s3DXRD archive, and extract one scan file")
    p.add_argument("names", nargs="*", help="only these files")
    a = p.parse_args()
    fetch(a.dir, a.names)
    if a.s3dxrd:
        archive, url, want, member = S3DXRD
        if os.path.exists(os.path.join(a.dir, member)):
            print("%s: there" % member)
        else:
            FILES[archive] = (url, want)
            fetch(a.dir, [archive])
            subprocess.check_call(["7z", "x", "-y", "-o" + a.dir, os.path.join(a.dir, archive), member])
