#!/usr/bin/env bash
# External decoder compatibility for pac-zstd "compat" chunks.
#
# Generates a corpus with tests/gen_compat_corpus, then verifies that every chunk can be decoded by
# external libraries via bitshuffle 0.4.0 / kcb + LZ4_decompress_safe, and by independent pure-Go
# decoders (klauspost/compress/zstd + pierrec/lz4/v4).
#
# Two modes:
#   * default (make check-compat, what CI runs): the stable 2022 "floor" set only -- zstd v1.5.2 and
#     lz4 v1.9.3 + v1.10.0. Deterministic, no git HEADs, few downloads.
#   * full matrix (make check-compat-matrix, COMPAT_MATRIX=1): every zstd release from the 2022
#     floor to the latest plus the git HEAD (facebook/zstd dev), and every recent lz4 release
#     (1.9.2 .. 1.10.0) plus the git HEAD (lz4/lz4 dev). Downloads whole trees and tests the
#     non-deterministic dev HEADs, so it is intentionally run OFFLINE, never in CI.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

COMPAT_DIR="${COMPAT_DIR:-build-compat}"
# resolve to an absolute path: the Go build runs from the tests/verify_go subshell, so a relative
# COMPAT_DIR would make `go build -o "$COMPAT_DIR/verify_go"` land in the wrong place.
case "$COMPAT_DIR" in /*) ;; *) COMPAT_DIR="$ROOT_DIR/$COMPAT_DIR" ;; esac
SRC_DIR="$COMPAT_DIR/src"
CORPUS="$COMPAT_DIR/compat_corpus.bin"
CC="${CC:-cc}"
JOBS="${JOBS:-4}"

# Select the version sweep. Default = the CI-safe floor; COMPAT_MATRIX=1 = the full offline matrix.
# (There is no zstd v1.5.3 release.)
if [ "${COMPAT_MATRIX:-0}" = "1" ]; then
    ZSTD_FLOOR="1.5.2 1.5.4 1.5.5 1.5.6 1.5.7 dev"
    LZ4_FLOOR="1.9.2 1.9.3 1.9.4 1.10.0 dev"
    MODE="full matrix"
else
    ZSTD_FLOOR="1.5.2"
    LZ4_FLOOR="1.9.3 1.10.0"
    MODE="floor"
fi
ZSTD_VERSIONS="${ZSTD_VERSIONS:-$ZSTD_FLOOR}"
LZ4_VERSIONS="${LZ4_VERSIONS:-$LZ4_FLOOR}"
echo "check_compat mode=$MODE  zstd='$ZSTD_VERSIONS'  lz4='$LZ4_VERSIONS'"

mkdir -p "$COMPAT_DIR" "$SRC_DIR"
FAILED=0

# version helpers: map a version to a GitHub ref
zstd_ref()  { case "$1" in dev) echo "refs/heads/dev" ;; *) echo "refs/tags/v$1" ;; esac; }
lz4_ref()   { case "$1" in dev) echo "refs/heads/dev" ;; *) echo "refs/tags/v$1" ;; esac; }

fetch_tarball() {
    local name="$1" url="$2"
    local tgz="$SRC_DIR/$name.tar.gz" out="$SRC_DIR/$name"
    if [ ! -d "$out" ]; then
        echo "Downloading $name..."
        curl -fsSL "$url" -o "$tgz" || { echo "  SKIP: could not fetch $name"; return 1; }
        mkdir -p "$out"
        tar -xzf "$tgz" -C "$out" --strip-components=1
    fi
    return 0
}

# 1. Build corpus generator and generate the test corpus
make -j"$JOBS" build/gen_compat_corpus
build/gen_compat_corpus "$CORPUS"

# --- zstd sweep (bitshuffle 0.4.0 + zstd@VER + lz4 v1.9.3) ---
BSHUF_DIR="$SRC_DIR/bitshuffle-0.4.0"
LZ4_193_DIR="$SRC_DIR/lz4-1.9.3"
fetch_tarball "bitshuffle-0.4.0" "https://github.com/kiyo-masui/bitshuffle/archive/refs/tags/0.4.0.tar.gz"
fetch_tarball "lz4-1.9.3"        "https://github.com/lz4/lz4/archive/refs/tags/v1.9.3.tar.gz" || FAILED=1
for V in $ZSTD_VERSIONS; do
    ZDIR="$SRC_DIR/zstd-$V"
    fetch_tarball "zstd-$V" "https://github.com/facebook/zstd/archive/$(zstd_ref "$V").tar.gz" || { echo "zstd v$V: BUILD FAILED (fetch)"; FAILED=1; continue; }
    BIN="$COMPAT_DIR/verify_bshuf_zstd_$V"
    if [ ! -f "$BIN" ] || [ -n "${FORCE:-}" ]; then
        echo "Building verify_bshuf_zstd_$V (bitshuffle 0.4.0 + zstd v$V + lz4 v1.9.3)..."
        OBJ="$COMPAT_DIR/obj_zstd_$V"; mkdir -p "$OBJ"
        for f in "$ZDIR"/lib/common/*.c "$ZDIR"/lib/compress/*.c "$ZDIR"/lib/decompress/*.c; do
            "$CC" -O2 -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -I"$ZDIR/lib" -I"$ZDIR/lib/common" \
                -c "$f" -o "$OBJ/zstd_$(basename "${f%.c}.o")" || { echo "zstd v$V: compile FAIL $f"; FAILED=1; }
        done
        "$CC" -O2 -I"$LZ4_193_DIR/lib" -c "$LZ4_193_DIR/lib/lz4.c" -o "$OBJ/lz4.o"
        for f in bitshuffle.c bitshuffle_core.c iochain.c; do
            "$CC" -O2 -std=gnu99 -DZSTD_SUPPORT -I"$BSHUF_DIR/src" -I"$ZDIR/lib" -I"$LZ4_193_DIR/lib" \
                -c "$BSHUF_DIR/src/$f" -o "$OBJ/${f%.c}.o"
        done
        "$CC" -O2 -DCOMPAT_USE_BSHUF -DZSTD_SUPPORT -I"$BSHUF_DIR/src" -Itests tests/verify_compat.c \
            "$OBJ"/*.o -o "$BIN" -lm || { echo "zstd v$V: link FAIL"; FAILED=1; }
    fi
    if [ -x "$BIN" ]; then
        OUT=$("$BIN" "$CORPUS" "zstd v$V + lz4 v1.9.3 (bitshuffle 0.4.0)")
        echo "$OUT"
        echo "$OUT" | grep -q ", 0 failures" || FAILED=1
    fi
done

# --- lz4 sweep (kcb bitshuffle + lz4@VER via LZ4_decompress_safe) ---
for W in $LZ4_VERSIONS; do
    L4DIR="$SRC_DIR/lz4-$W"
    fetch_tarball "lz4-$W" "https://github.com/lz4/lz4/archive/$(lz4_ref "$W").tar.gz" || { echo "lz4 v$W: BUILD FAILED (fetch)"; FAILED=1; continue; }
    BIN="$COMPAT_DIR/verify_lz4_$W"
    if [ ! -f "$BIN" ] || [ -n "${FORCE:-}" ]; then
        echo "Building verify_lz4_$W (lz4 v$W + kcb bitshuffle)..."
        "$CC" -O2 -I"$L4DIR/lib" -c "$L4DIR/lib/lz4.c" -o "$COMPAT_DIR/lz4_${W}.o"
        "$CC" -O2 -std=gnu99 -c third_party/kcb/src/bitshuffle.c -o "$COMPAT_DIR/kcb.o"
        "$CC" -O2 -DCOMPAT_USE_LZ4_ONLY -I"$L4DIR/lib" -Itests tests/verify_compat.c \
            "$COMPAT_DIR/lz4_${W}.o" "$COMPAT_DIR/kcb.o" -o "$BIN" -lm || { echo "lz4 v$W: build FAIL"; FAILED=1; }
    fi
    if [ -x "$BIN" ]; then
        OUT=$("$BIN" "$CORPUS" "lz4 v$W (kcb bitshuffle)")
        echo "$OUT"
        echo "$OUT" | grep -q ", 0 failures" || FAILED=1
    fi
done

# --- independent pure-Go decoders (klauspost/compress/zstd + pierrec/lz4/v4), module heads ---
if command -v go >/dev/null 2>&1; then
    echo "Building and running verify_go..."
    (cd tests/verify_go && go build -o "$COMPAT_DIR/verify_go" .) || FAILED=1
    "$COMPAT_DIR/verify_go" "$CORPUS" || FAILED=1
else
    echo "go not found on PATH: skipping independent Go decoder check (install Go or extend PATH to test module-head decoders)."
    FAILED=1
fi

[ "$FAILED" -eq 0 ] && echo "COMPAT: all libraries matched." || { echo "COMPAT: FAILURES above."; exit 1; }
