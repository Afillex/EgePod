# Vendor dependencies

Run `make vendor-fetch` to download all dependencies automatically.

Or manually:

```bash
# ── tinyalsa ────────────────────────────────────────────────────────────────
git clone --depth=1 https://github.com/tinyalsa/tinyalsa vendor/tinyalsa

# ── dr_libs (FLAC + WAV single-header decoders, MIT) ────────────────────────
curl -fsSL https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h  -o vendor/dr_flac.h
curl -fsSL https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h   -o vendor/dr_wav.h

# ── minimp3 (MP3 single-header decoder, CC0) ─────────────────────────────────
curl -fsSL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h    -o vendor/minimp3.h
curl -fsSL https://raw.githubusercontent.com/lieff/minimp3/master/minimp3_ex.h -o vendor/minimp3_ex.h
```

## Notes

- **tinyalsa** must be cross-compiled for AArch64 using the same NDK toolchain.
  `make vendor-fetch` handles this automatically.
- **dr_flac.h** and **dr_wav.h** are single-header libraries — they are
  `#include`d with `#define DR_FLAC_IMPLEMENTATION` in `decoder.c` only, not
  in any headers, to avoid ODR violations.
- **minimp3_ex.h** is the extended API that provides `mp3dec_load_buf()` for
  full-file decode. `minimp3.h` must also be present (minimp3_ex.h includes it).
