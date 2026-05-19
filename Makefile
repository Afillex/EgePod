include build/toolchain.mk

OUT    := out/$(API)
TARGETS := $(OUT)/egepod_audiod $(OUT)/egepod_pwrd $(OUT)/egepod_uid

.PHONY: all clean install vendor-fetch test

all: vendor-check $(TARGETS)

# ── audiod ──────────────────────────────────────────────────────────────────
AUDIOD_SRC := src/audiod/main.c   \
              src/audiod/player.c \
              src/audiod/decoder.c \
              src/audiod/alsa_out.c \
              src/audiod/index.c

$(OUT)/egepod_audiod: $(AUDIOD_SRC) | $(OUT)
	$(CC) $(CFLAGS) $(AUDIOD_SRC) $(LDFLAGS) -o $@
	@echo "  LD  $@"

# ── pwrd ────────────────────────────────────────────────────────────────────
PWRD_SRC := src/pwrd/main.c   \
            src/pwrd/cpu.c    \
            src/pwrd/rfkill.c

$(OUT)/egepod_pwrd: $(PWRD_SRC) | $(OUT)
	$(CC) $(CFLAGS) $(PWRD_SRC) $(LDFLAGS) -o $@
	@echo "  LD  $@"

# ── uid ─────────────────────────────────────────────────────────────────────
UID_SRC := src/uid/main.c       \
           src/uid/fb.c         \
           src/uid/input.c      \
           src/uid/render.c     \
           src/uid/ipc_client.c

$(OUT)/egepod_uid: $(UID_SRC) | $(OUT)
	$(CC) $(CFLAGS) $(UID_SRC) $(LDFLAGS) -o $@
	@echo "  LD  $@"

$(OUT):
	mkdir -p $(OUT)

# ── vendor ──────────────────────────────────────────────────────────────────
vendor-check:
	@[ -f vendor/dr_flac.h ]      || (echo "ERROR: run 'make vendor-fetch' first" && exit 1)
	@[ -f vendor/dr_wav.h ]       || (echo "ERROR: run 'make vendor-fetch' first" && exit 1)
	@[ -f vendor/minimp3.h ]      || (echo "ERROR: run 'make vendor-fetch' first" && exit 1)
	@[ -f vendor/minimp3_ex.h ]   || (echo "ERROR: run 'make vendor-fetch' first" && exit 1)
	@[ -d vendor/tinyalsa ]       || (echo "ERROR: run 'make vendor-fetch' first" && exit 1)

vendor-fetch:
	@echo "Fetching vendor dependencies…"
	curl -fsSL https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h    -o vendor/dr_flac.h
	curl -fsSL https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h     -o vendor/dr_wav.h
	curl -fsSL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h      -o vendor/minimp3.h
	curl -fsSL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3_ex.h   -o vendor/minimp3_ex.h
	[ -d vendor/tinyalsa ] || git clone --depth=1 https://github.com/tinyalsa/tinyalsa vendor/tinyalsa
	$(MAKE) -C vendor/tinyalsa \
	    CC=$(CC) AR=$(AR) \
	    CFLAGS="$(CFLAGS)" \
	    lib
	@echo "Vendor dependencies ready."

# ── install (push to device via ADB) ────────────────────────────────────────
install: all
	adb root
	adb remount
	adb push $(OUT)/egepod_audiod /system/bin/
	adb push $(OUT)/egepod_pwrd   /system/bin/
	adb push $(OUT)/egepod_uid    /system/bin/
	adb push init/egepod.rc       /system/etc/init/
	adb push init/alsa_mixer.conf /system/etc/egepod/
	adb shell "chmod 755 /system/bin/egepod_*"
	adb shell sync

# ── test (decode a sample on macOS using host compiler) ──────────────────────
test:
	@echo "Building decoder test (native host)…"
	clang -O2 -std=c11 -D_GNU_SOURCE \
	      -I vendor \
	      -I src \
	      -DDECODER_TEST \
	      src/audiod/decoder.c \
	      -o out/decoder_test_host
	@echo "Run: ./out/decoder_test_host <audio_file>"

clean:
	rm -rf out/
