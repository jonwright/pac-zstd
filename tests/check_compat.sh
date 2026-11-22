#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

COMPAT_DIR="${COMPAT_DIR:-build-compat}"
SRC_DIR="$COMPAT_DIR/src"
CORPUS="$COMPAT_DIR/compat_corpus.bin"
CC="${CC:-cc}"
JOBS="${JOBS:-4}"

mkdir -p "$COMPAT_DIR" "$SRC_DIR"

# 1. Build corpus generator and generate the test corpus
make -j"$JOBS" build/gen_compat_corpus
build/gen_compat_corpus "$CORPUS"

fetch_tarball() {
    local name="$1"
    local url="$2"
    local tgz="$SRC_DIR/$name.tar.gz"
    local out="$SRC_DIR/$name"
    if [ ! -d "$out" ]; then
        echo "Downloading $name..."
        curl -fsSL "$url" -o "$tgz"
        mkdir -p "$out"
        tar -xzf "$tgz" -C "$out" --strip-components=1
    fi
}

fetch_tarball "bitshuffle-0.4.0" "https://github.com/kiyo-masui/bitshuffle/archive/refs/tags/0.4.0.tar.gz"
fetch_tarball "zstd-1.5.2"       "https://github.com/facebook/zstd/archive/refs/tags/v1.5.2.tar.gz"
fetch_tarball "lz4-1.9.3"        "https://github.com/lz4/lz4/archive/refs/tags/v1.9.3.tar.gz"
fetch_tarball "lz4-1.10.0"       "https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz"

# 2. Verify with bitshuffle 0.4.0 + zstd v1.5.2 + lz4 v1.9.3
BSHUF_DIR="$SRC_DIR/bitshuffle-0.4.0"
ZSTD152_DIR="$SRC_DIR/zstd-1.5.2"
LZ4_193_DIR="$SRC_DIR/lz4-1.9.3"

if [ ! -f "$COMPAT_DIR/verify_bshuf040" ]; then
    echo "Building verify_bshuf040 (bitshuffle 0.4.0 + zstd v1.5.2 + lz4 v1.9.3)..."
    mkdir -p "$COMPAT_DIR/obj_040"
    for f in "$ZSTD152_DIR"/lib/common/*.c "$ZSTD152_DIR"/lib/compress/*.c "$ZSTD152_DIR"/lib/decompress/*.c; do
        obj="$COMPAT_DIR/obj_040/zstd_$(basename "${f%.c}.o")"
        "$CC" -O2 -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -I"$ZSTD152_DIR/lib" -I"$ZSTD152_DIR/lib/common" -c "$f" -o "$obj"
    done
    "$CC" -O2 -I"$LZ4_193_DIR/lib" -c "$LZ4_193_DIR/lib/lz4.c" -o "$COMPAT_DIR/obj_040/lz4.o"
    for f in bitshuffle.c bitshuffle_core.c iochain.c; do
        "$CC" -O2 -std=gnu99 -DZSTD_SUPPORT -I"$BSHUF_DIR/src" -I"$ZSTD152_DIR/lib" -I"$LZ4_193_DIR/lib" \
            -c "$BSHUF_DIR/src/$f" -o "$COMPAT_DIR/obj_040/${f%.c}.o"
    done
    "$CC" -O2 -DCOMPAT_USE_BSHUF -DZSTD_SUPPORT -I"$BSHUF_DIR/src" -Itests tests/verify_compat.c \
        "$COMPAT_DIR"/obj_040/*.o -o "$COMPAT_DIR/verify_bshuf040" -lm
fi
"$COMPAT_DIR/verify_bshuf040" "$CORPUS" "bitshuffle 0.4.0 + zstd v1.5.2 + lz4 v1.9.3"

# 3. Verify with lz4 v1.10.0
LZ4_1100_DIR="$SRC_DIR/lz4-1.10.0"
if [ ! -f "$COMPAT_DIR/verify_lz4_1100" ]; then
    echo "Building verify_lz4_1100 (lz4 v1.10.0)..."
    "$CC" -O2 -I"$LZ4_1100_DIR/lib" -c "$LZ4_1100_DIR/lib/lz4.c" -o "$COMPAT_DIR/lz4_1100.o"
    "$CC" -O2 -std=gnu99 -c third_party/kcb/src/bitshuffle.c -o "$COMPAT_DIR/kcb.o"
    "$CC" -O2 -DCOMPAT_USE_LZ4_ONLY -I"$LZ4_1100_DIR/lib" -Itests tests/verify_compat.c \
        "$COMPAT_DIR/lz4_1100.o" "$COMPAT_DIR/kcb.o" -o "$COMPAT_DIR/verify_lz4_1100" -lm
fi
"$COMPAT_DIR/verify_lz4_1100" "$CORPUS" "lz4 v1.10.0"

# 4. Verify with independent pure-Go decoders (klauspost/compress/zstd + pierrec/lz4/v4)
echo "Building and running verify_go..."
(cd tests/verify_go && go build -o "../../$COMPAT_DIR/verify_go" .)
"$COMPAT_DIR/verify_go" "$CORPUS"
