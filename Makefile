# pac-zstd: the library, its tests and the checks.
#
#   make            libpac.a and libpac.so (with lz4, zstd and kcb inside), libpacbench.so for bench/
#   make check      the tests, for every instruction set this CPU has
#   make check-c89  src/pac.c as strict C89 and as C++98
#   make check-asan the tests under the address and undefined-behaviour sanitizers
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

$(BUILD)/libpac.a: $(BUILD)/pac.o $(DEP_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/libpac.so: $(BUILD)/pac.o $(DEP_OBJ)
	$(CC) -shared -o $@ $^ -lm

# the encoder and the reference codec of the benchmarks (bench/ref_codec.c) in one library
$(BUILD)/libpacbench.so: bench/ref_codec.c $(BUILD)/pac.o $(DEP_OBJ)
	$(CC) $(CFLAGS) $(WARN) -fPIC -shared $(INC) -o $@ $^ -lm

$(BUILD)/test_pac: tests/test_pac.c $(BUILD)/libpac.a
	$(CC) $(CFLAGS) $(WARN) $(LDFLAGS) -Isrc $(INC) $< $(BUILD)/libpac.a -o $@ -lm

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

clean:
	rm -rf build build-asan

.PHONY: all check check-c89 check-asan clean
