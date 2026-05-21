include build/toolchain.mk

OUT     := out/$(API)
TARGETS := $(OUT)/egepod_audiod $(OUT)/egepod_pwrd $(OUT)/egepod_uid

.PHONY: all clean install vendor-fetch vendor-check test

all: vendor-check $(TARGETS)
	@echo ""
	@echo "Build complete → $(OUT)/"
	@ls -lh $(OUT)/egepod_*

# ── tinyalsa static library ────────────────────────────────────────────────────
# Compiled from vendor/tinyalsa/src/ directly rather than relying on
# tinyalsa's own Makefile target (avoids cmake/autotools dependency).
TINY_INC  := vendor/tinyalsa/include
TINY_SRCS := $(wildcard vendor/tinyalsa/src/*.c)
TINY_OBJS := $(TINY_SRCS:vendor/tinyalsa/src/%.c=$(OUT)/tinyalsa/%.o)
TINY_LIB  := $(OUT)/tinyalsa/libtinyalsa.a

$(OUT)/tinyalsa:
	mkdir -p $@

$(OUT)/tinyalsa/%.o: vendor/tinyalsa/src/%.c | $(OUT)/tinyalsa
	$(CC) $(CFLAGS) -I$(TINY_INC) -c $< -o $@

$(TINY_LIB): $(TINY_OBJS) | $(OUT)/tinyalsa
	$(AR) rcs $@ $^

# ── audiod ─────────────────────────────────────────────────────────────────────
AUDIOD_SRC := src/audiod/main.c   \
              src/audiod/player.c \
              src/audiod/decoder.c \
              src/audiod/alsa_out.c \
              src/audiod/index.c

$(OUT)/egepod_audiod: $(AUDIOD_SRC) $(TINY_LIB) | $(OUT)
	$(CC) $(CFLAGS) -I$(TINY_INC) $(AUDIOD_SRC) $(TINY_LIB) $(LDFLAGS) -lm -o $@
	$(STRIP) --strip-unneeded $@

# ── pwrd ───────────────────────────────────────────────────────────────────────
PWRD_SRC := src/pwrd/main.c   \
            src/pwrd/cpu.c    \
            src/pwrd/rfkill.c

$(OUT)/egepod_pwrd: $(PWRD_SRC) | $(OUT)
	$(CC) $(CFLAGS) $(PWRD_SRC) $(LDFLAGS) -o $@
	$(STRIP) --strip-unneeded $@

# ── uid ────────────────────────────────────────────────────────────────────────
UID_SRC := src/uid/main.c       \
           src/uid/fb.c         \
           src/uid/input.c      \
           src/uid/render.c     \
           src/uid/ipc_client.c

$(OUT)/egepod_uid: $(UID_SRC) | $(OUT)
	$(CC) $(CFLAGS) $(UID_SRC) $(LDFLAGS) -lm -o $@
	$(STRIP) --strip-unneeded $@

$(OUT):
	mkdir -p $(OUT)

# ── vendor dependencies ────────────────────────────────────────────────────────
vendor-check:
	@[ -f vendor/dr_flac.h ]              || { echo "ERROR: run 'make vendor-fetch'"; exit 1; }
	@[ -f vendor/minimp3.h ]              || { echo "ERROR: run 'make vendor-fetch'"; exit 1; }
	@[ -d vendor/tinyalsa/include ]       || { echo "ERROR: run 'make vendor-fetch'"; exit 1; }
	@[ -n "$(NDK)" ]                      || { echo "ERROR: export ANDROID_NDK_HOME=..."; exit 1; }
	@[ -f "$(CC)" ]                       || { echo "ERROR: NDK compiler not found: $(CC)"; exit 1; }

vendor-fetch:
	@echo "Fetching single-header audio decoders…"
	curl -fsSL https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h  -o vendor/dr_flac.h
	curl -fsSL https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h   -o vendor/dr_wav.h
	curl -fsSL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h    -o vendor/minimp3.h
	curl -fsSL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3_ex.h -o vendor/minimp3_ex.h
	@echo "Fetching tinyalsa…"
	[ -d vendor/tinyalsa/.git ] || \
	    git clone --depth=1 --branch v2.0.0 \
	        https://github.com/tinyalsa/tinyalsa vendor/tinyalsa
	@echo "Vendor dependencies ready."

# ── install via ADB (device must already be adb-rooted) ───────────────────────
install: all
	./build/flash.sh

# ── native decoder smoke-test (macOS host compiler, no NDK needed) ─────────────
test:
	clang -O2 -std=c11 -D_GNU_SOURCE -Ivendor -Isrc -DDECODER_TEST \
	    src/audiod/decoder.c -o out/decoder_test_host
	@echo "Run: ./out/decoder_test_host <audio_file>"

clean:
	rm -rf out/
