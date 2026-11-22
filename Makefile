# pac-zstd: the library, its tests and the checks.
#
#   make               libpac.a and libpac.so (with lz4, zstd and kcb inside), libpacbench.so for bench/
#   make check         the tests, for every instruction set this CPU has
#   make check-c89     src/pac.c as strict C89 and as C++98
#   make check-asan    the tests under the address and undefined-behaviour sanitizers
#   make coverage      line and branch coverage of src/ from tests/test_pac.c
#   make fuzz          run libFuzzer targets (requires clang; FUZZ_TIME=60 per target)
#   make fuzz-coverage line and branch coverage of src/ over the fuzz corpora (requires clang + llvm-cov)
#   make fuzz-replay   replay the fuzz corpora under ASan + UBSan (works with gcc or clang)
#
# git submodule update --init   first.

CC ?= cc
CFLAGS ?= -O3
BUILD ?= build
TP = third_party
INC = -isystem $(TP)/zstd/lib -isystem $(TP)/lz4/lib
ZSTD_SRC = $(wildcard $(TP)/zstd/lib/common/*.c $(TP)/zstd/lib/compress/*.c $(TP)/zstd/lib/decompress/*.c)
ZSTD_DEF = -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0
DEP_OBJ = $(patsubst $(TP)/zstd/lib/%.c,$(BUILD)/zstd/%.o,$(ZSTD_SRC)) $(BUILD)/lz4.o $(BUILD)/kcb.o
WARN = -Wall -Wextra -Wno-unused-function
FUZZ_CC ?= clang
FUZZ_TIME ?= 60
FUZZ_EXTRA ?=
FUZZ_FLAGS ?= -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined
LLVM_COV ?= $(or $(shell command -v llvm-cov 2>/dev/null),$(firstword $(wildcard /usr/bin/llvm-cov-*)),llvm-cov)
LLVM_PROFDATA ?= $(or $(shell command -v llvm-profdata 2>/dev/null),$(firstword $(wildcard /usr/bin/llvm-profdata-*)),llvm-profdata)

all: $(BUILD)/libpac.a $(BUILD)/libpac.so $(BUILD)/libpacbench.so

$(BUILD)/zstd/%.o: $(TP)/zstd/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fPIC $(ZSTD_DEF) -c $< -o $@

$(BUILD)/lz4.o: $(TP)/lz4/lib/lz4.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/kcb.o: $(TP)/kcb/src/bitshuffle.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=gnu99 -fPIC -c $< -o $@

$(BUILD)/pac.o: src/pac.c src/pac.h src/pac_parse.inl
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(WARN) $(PACFLAGS) -fPIC -DPAC_WITH_KCB $(INC) -c $< -o $@

$(BUILD)/libdeps.a: $(DEP_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/libpac.a: $(BUILD)/pac.o $(DEP_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/libpac.so: $(BUILD)/pac.o $(DEP_OBJ)
	$(CC) -shared -o $@ $^ -lm

# the encoder and the reference codec of the benchmarks (bench/ref_codec.c) in one library
$(BUILD)/libpacbench.so: bench/ref_codec.c $(BUILD)/pac.o $(DEP_OBJ)
	$(CC) $(CFLAGS) $(WARN) -fPIC -shared $(INC) -o $@ $^ -lm

$(BUILD)/test_pac: tests/test_pac.c $(BUILD)/libpac.a
	$(CC) $(CFLAGS) $(WARN) $(LDFLAGS) -Isrc $(INC) $< $(BUILD)/libpac.a -o $@ -lm

$(BUILD)/gen_compat_corpus: tests/gen_compat_corpus.c tests/compat_format.h $(BUILD)/libpac.a
	$(CC) $(CFLAGS) $(WARN) $(LDFLAGS) -Isrc $(INC) $< $(BUILD)/libpac.a -o $@ -lm

$(BUILD)/edu_zstd.o: $(TP)/zstd/doc/educational_decoder/zstd_decompress.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -std=c99 -DZDEC_NO_DICTIONARY -DZDEC_NO_MESSAGE -I$(TP)/zstd/doc/educational_decoder -c $< -o $@

$(BUILD)/check_rfc: tests/check_rfc.c tests/compat_format.h $(BUILD)/edu_zstd.o $(BUILD)/kcb.o
	$(CC) $(CFLAGS) $(WARN) $(LDFLAGS) -std=c99 -I$(TP)/zstd/doc/educational_decoder $< $(BUILD)/edu_zstd.o $(BUILD)/kcb.o -o $@ -lm

check-rfc: $(BUILD)/gen_compat_corpus $(BUILD)/check_rfc
	$(BUILD)/gen_compat_corpus $(BUILD)/compat_corpus.bin
	$(BUILD)/check_rfc $(BUILD)/compat_corpus.bin

check-compat:
	CC="$(CC)" bash tests/check_compat.sh

# the full external-decoder version sweep (zstd 1.5.2..1.5.7 + dev HEAD, lz4 1.9.2..1.10.0 + dev HEAD).
# downloads whole trees and uses the non-deterministic git HEADs, so it is meant to run OFFLINE only.
check-compat-matrix:
	CC="$(CC)" COMPAT_MATRIX=1 bash tests/check_compat.sh

check: $(BUILD)/test_pac
	$(BUILD)/test_pac

check-c89:
	@mkdir -p $(BUILD)
	$(CC) -std=c89 -pedantic-errors -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function \
	    -O2 -DPAC_WITH_KCB $(INC) -c src/pac.c -o $(BUILD)/pac_c89.o
	$(CC) -std=c89 -pedantic-errors -Wall -Wextra -Wdeclaration-after-statement -Wno-unused-function \
	    -O2 -DPAC_RUNTIME_DISPATCH=0 $(INC) -c src/pac.c -o $(BUILD)/pac_c89_plain.o
	$(CXX) -std=c++98 -Wall -Wextra -Wno-unused-function -fsyntax-only -x c++ $(INC) src/pac.c
	@echo "src/pac.c is C89 and C++98"

check-asan:
	$(MAKE) BUILD=build-asan CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" \
	    PACFLAGS=-DPAC_VALIDATE_SEQUENCES build-asan/test_pac
	build-asan/test_pac

coverage:
	rm -f build-cov/*.gcda
	$(MAKE) BUILD=build-cov CFLAGS="-O2" PACFLAGS="-O0 -g --coverage" LDFLAGS="--coverage" build-cov/test_pac
	build-cov/test_pac
	@if command -v gcovr >/dev/null 2>&1; then \
	    gcovr -r . --filter 'src/' --txt - --print-summary \
	        --html-details build-cov/coverage.html build-cov; \
	    echo "HTML report: build-cov/coverage.html"; \
	else \
	    gcov -b build-cov/pac.gcno && mv -f *.gcov build-cov/; \
	fi

ifeq ($(BUILD),build)
build-fuzz/deps/libdeps.a:
	$(MAKE) BUILD=build-fuzz/deps CC="$(FUZZ_CC)" \
	    CFLAGS="-O2 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" \
	    build-fuzz/deps/libdeps.a

build-fuzz/cov_deps/libdeps.a:
	$(MAKE) BUILD=build-fuzz/cov_deps CC="$(FUZZ_CC)" CFLAGS="-O2" build-fuzz/cov_deps/libdeps.a
endif

build-fuzz/pac_fuzz.o: src/pac.c src/pac.h src/pac_parse.inl
	@mkdir -p build-fuzz
	$(FUZZ_CC) $(FUZZ_FLAGS) -fsanitize=fuzzer-no-link $(WARN) -DPAC_WITH_KCB -DPAC_VALIDATE_SEQUENCES $(INC) -c $< -o $@

build-fuzz/fuzz_repack: tests/fuzz_repack.c tests/fuzz_common.h src/pac.h build-fuzz/pac_fuzz.o build-fuzz/deps/libdeps.a
	$(FUZZ_CC) $(FUZZ_FLAGS) -fsanitize=fuzzer $(WARN) -Isrc $(INC) $< build-fuzz/pac_fuzz.o build-fuzz/deps/libdeps.a -o $@ -lm

build-fuzz/fuzz_compress: tests/fuzz_compress.c tests/fuzz_common.h src/pac.h build-fuzz/pac_fuzz.o build-fuzz/deps/libdeps.a
	$(FUZZ_CC) $(FUZZ_FLAGS) -fsanitize=fuzzer $(WARN) -Isrc $(INC) $< build-fuzz/pac_fuzz.o build-fuzz/deps/libdeps.a -o $@ -lm

build-fuzz/fuzz_seed: tests/fuzz_seed.c tests/fuzz_common.h src/pac.h build-fuzz/deps/libdeps.a
	@mkdir -p build-fuzz
	$(FUZZ_CC) $(FUZZ_FLAGS) $(WARN) -Isrc $(INC) $< build-fuzz/deps/libdeps.a -o $@ -lm

fuzz-build: build-fuzz/fuzz_repack build-fuzz/fuzz_compress build-fuzz/fuzz_seed

fuzz: fuzz-build
	@mkdir -p build-fuzz/corpus_repack build-fuzz/corpus_compress build-fuzz/artifacts
	build-fuzz/fuzz_seed build-fuzz/corpus_repack build-fuzz/corpus_compress
	build-fuzz/fuzz_repack -max_total_time=$(FUZZ_TIME) -max_len=65536 -artifact_prefix=build-fuzz/artifacts/repack- $(FUZZ_EXTRA) build-fuzz/corpus_repack
	build-fuzz/fuzz_compress -max_total_time=$(FUZZ_TIME) -max_len=65536 -artifact_prefix=build-fuzz/artifacts/compress- $(FUZZ_EXTRA) build-fuzz/corpus_compress

fuzz-replay:
	$(MAKE) BUILD=build-asan CC="$(CC)" \
	    CFLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined" \
	    PACFLAGS=-DPAC_VALIDATE_SEQUENCES build-asan/libpac.a
	@mkdir -p build-fuzz/corpus_repack build-fuzz/corpus_compress
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -DPAC_FUZZ_STANDALONE -Isrc $(INC) tests/fuzz_repack.c build-asan/libpac.a -o build-fuzz/replay_repack -lm
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -DPAC_FUZZ_STANDALONE -Isrc $(INC) tests/fuzz_compress.c build-asan/libpac.a -o build-fuzz/replay_compress -lm
	$(CC) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -Isrc $(INC) tests/fuzz_seed.c build-asan/libpac.a -o build-fuzz/replay_seed -lm
	build-fuzz/replay_seed build-fuzz/corpus_repack build-fuzz/corpus_compress
	build-fuzz/replay_repack build-fuzz/corpus_repack
	build-fuzz/replay_compress build-fuzz/corpus_compress

build-fuzz/pac_cov.o: src/pac.c src/pac.h src/pac_parse.inl
	@mkdir -p build-fuzz
	$(FUZZ_CC) -O0 -g -fprofile-instr-generate -fcoverage-mapping $(WARN) -DPAC_WITH_KCB -DPAC_VALIDATE_SEQUENCES $(INC) -c $< -o $@

build-fuzz/cov_repack: tests/fuzz_repack.c tests/fuzz_common.h src/pac.h build-fuzz/pac_cov.o build-fuzz/cov_deps/libdeps.a
	$(FUZZ_CC) -O1 -g -fprofile-instr-generate -fcoverage-mapping $(WARN) -DPAC_FUZZ_STANDALONE -Isrc $(INC) $< build-fuzz/pac_cov.o build-fuzz/cov_deps/libdeps.a -o $@ -lm

build-fuzz/cov_compress: tests/fuzz_compress.c tests/fuzz_common.h src/pac.h build-fuzz/pac_cov.o build-fuzz/cov_deps/libdeps.a
	$(FUZZ_CC) -O1 -g -fprofile-instr-generate -fcoverage-mapping $(WARN) -DPAC_FUZZ_STANDALONE -Isrc $(INC) $< build-fuzz/pac_cov.o build-fuzz/cov_deps/libdeps.a -o $@ -lm

build-fuzz/cov_seed: tests/fuzz_seed.c tests/fuzz_common.h src/pac.h build-fuzz/cov_deps/libdeps.a
	@mkdir -p build-fuzz
	$(FUZZ_CC) -O2 -g $(WARN) -Isrc $(INC) $< build-fuzz/cov_deps/libdeps.a -o $@ -lm

fuzz-coverage: build-fuzz/cov_repack build-fuzz/cov_compress build-fuzz/cov_seed
	@mkdir -p build-fuzz/corpus_repack build-fuzz/corpus_compress build-fuzz/coverage
	build-fuzz/cov_seed build-fuzz/corpus_repack build-fuzz/corpus_compress
	LLVM_PROFILE_FILE=build-fuzz/repack.profraw build-fuzz/cov_repack build-fuzz/corpus_repack
	LLVM_PROFILE_FILE=build-fuzz/compress.profraw build-fuzz/cov_compress build-fuzz/corpus_compress
	$(LLVM_PROFDATA) merge -sparse build-fuzz/repack.profraw build-fuzz/compress.profraw -o build-fuzz/fuzz.profdata
	$(LLVM_COV) report build-fuzz/cov_repack -object=build-fuzz/cov_compress -instr-profile=build-fuzz/fuzz.profdata src/pac.c src/pac_parse.inl
	$(LLVM_COV) show build-fuzz/cov_repack -object=build-fuzz/cov_compress -instr-profile=build-fuzz/fuzz.profdata -format=html -output-dir=build-fuzz/coverage src/pac.c src/pac_parse.inl
	@echo "HTML report: build-fuzz/coverage/index.html"

clean:
	rm -rf build build-asan build-cov build-fuzz build-compat

.PHONY: all check check-c89 check-asan check-rfc check-compat check-compat-matrix coverage fuzz fuzz-build fuzz-replay fuzz-coverage clean
