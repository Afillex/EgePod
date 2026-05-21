/* 🐧 Kernel & HAL Hacker — pure C audio decode to locked PCM RAM.
 *
 * Single-header libraries live in vendor/:
 *   dr_flac.h   (FLAC)
 *   dr_wav.h    (WAV / PCM)
 *   minimp3_ex.h (MP3)
 *
 * Strategy: read the compressed file into RAM once, decode entirely to a
 * mlock()'d anonymous mapping.  eMMC goes idle after the initial read. */

#define DR_FLAC_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_EXT_IMPLEMENTATION

#include "decoder.h"
#include "../common/log.h"

/* vendor/ is on the include path (-Ivendor in Makefile) */
#include "dr_flac.h"
#include "dr_wav.h"
#include "minimp3_ex.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── helpers ───────────────────────────────────────────────────────────── */

static AudioFormat detect_format(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return FMT_UNKNOWN;
    dot++;
    if (!strcasecmp(dot, "flac")) return FMT_FLAC;
    if (!strcasecmp(dot, "wav"))  return FMT_WAV;
    if (!strcasecmp(dot, "mp3"))  return FMT_MP3;
    return FMT_UNKNOWN;
}

/* Read entire file into a heap buffer.  Returns buf + size. */
static uint8_t *slurp_file(const char *path, size_t *size_out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { LOGE("decoder: open(%s): %s", path, strerror(errno)); return NULL; }

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return NULL; }

    uint8_t *buf = malloc((size_t)st.st_size);
    if (!buf) { close(fd); return NULL; }

    /* Use a loop — a single read() may return fewer bytes than requested
     * if interrupted by a signal or due to I/O scheduling. */
    size_t total = 0;
    while (total < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + total, (size_t)st.st_size - total);
        if (n <= 0) {
            LOGE("decoder: read error on %s at %zu/%lld: %s",
                 path, total, (long long)st.st_size, strerror(errno));
            free(buf);
            close(fd);
            return NULL;
        }
        total += (size_t)n;
    }
    close(fd);
    *size_out = total;
    return buf;
}

/* Hard cap: 256 MB per track.  At 16-bit/96kHz/2ch that is ~22 minutes.
 * Beyond this, mlock would silently fail and the buffer risks being swapped,
 * causing audio dropouts under memory pressure. */
#define PCM_MAX_BYTES (256UL * 1024UL * 1024UL)

/* Allocate a locked anonymous mapping for PCM output.
 * MADV_HUGEPAGE reduces TLB pressure on the large PCM buffer (requires
 * CONFIG_TRANSPARENT_HUGEPAGE=madvise, set by cpu_apply_dap_policy). */
static int16_t *alloc_pcm(size_t n_frames, uint32_t channels)
{
    if (channels == 0 || channels > 8) {
        LOGE("decoder: alloc_pcm: invalid channels=%u", channels);
        return NULL;
    }
    /* Check for overflow BEFORE multiplying: n_frames * channels * 2 can wrap
     * on a malformed file with a huge frame count, bypassing the cap below. */
    if (n_frames == 0 ||
        n_frames > PCM_MAX_BYTES / ((size_t)channels * sizeof(int16_t))) {
        LOGE("decoder: alloc_pcm: %zu frames × %u ch zero or exceeds 256 MB cap",
             n_frames, channels);
        return NULL;
    }
    size_t bytes = n_frames * (size_t)channels * sizeof(int16_t);

    void *p = mmap(NULL, bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE,
                   -1, 0);
    if (p == MAP_FAILED) {
        LOGE("decoder: mmap(%zu): %s", bytes, strerror(errno));
        return NULL;
    }
    /* Hint the kernel to back this mapping with huge pages — reduces TLB
     * misses on a ~30 MB PCM buffer.  Silent no-op if hugepages unavailable. */
    madvise(p, bytes, MADV_HUGEPAGE);
    if (mlock(p, bytes) != 0)
        LOGW("decoder: mlock failed (%s) — PCM may be swapped under pressure",
             strerror(errno));
    return (int16_t *)p;
}

/* ── FLAC ──────────────────────────────────────────────────────────────── */

static int decode_flac(const uint8_t *data, size_t data_len,
                       int16_t **pcm_out, size_t *frames_out,
                       uint32_t *rate_out, uint32_t *channels_out)
{
    /* Open without decoding to read the header metadata first. */
    drflac *f = drflac_open_memory(data, data_len, NULL);
    if (!f) { LOGE("decoder: FLAC open failed"); return -1; }

    size_t   n  = (size_t)f->totalPCMFrameCount;
    uint32_t ch = f->channels;
    uint32_t sr = f->sampleRate;

    /* Allocate the locked mmap buffer, then decode directly into it —
     * avoids a second ~30 MB heap allocation and memcpy. */
    int16_t *locked = alloc_pcm(n, ch);
    if (!locked) { drflac_close(f); return -1; }

    drflac_uint64 decoded = drflac_read_pcm_frames_s16(f, (drflac_uint64)n, locked);
    drflac_close(f);

    if (decoded == 0) {
        LOGE("decoder: FLAC decode produced 0 frames");
        decoder_free(locked, n, ch);
        return -1;
    }

    *pcm_out      = locked;
    *frames_out   = (size_t)decoded;
    *rate_out     = sr;
    *channels_out = ch;
    return 0;
}

/* ── WAV ───────────────────────────────────────────────────────────────── */

static int decode_wav(const uint8_t *data, size_t data_len,
                      int16_t **pcm_out, size_t *frames_out,
                      uint32_t *rate_out, uint32_t *channels_out)
{
    drwav w;
    if (!drwav_init_memory(&w, data, data_len, NULL)) {
        LOGE("decoder: WAV open failed");
        return -1;
    }

    size_t   n  = (size_t)w.totalPCMFrameCount;
    uint32_t ch = w.channels;
    uint32_t sr = w.sampleRate;

    int16_t *locked = alloc_pcm(n, ch);
    if (!locked) { drwav_uninit(&w); return -1; }

    drwav_uint64 decoded = drwav_read_pcm_frames_s16(&w, (drwav_uint64)n, locked);
    drwav_uninit(&w);

    if (decoded == 0) {
        LOGE("decoder: WAV decode produced 0 frames");
        decoder_free(locked, n, ch);
        return -1;
    }

    *pcm_out      = locked;
    *frames_out   = (size_t)decoded;
    *rate_out     = sr;
    *channels_out = ch;
    return 0;
}

/* ── MP3 ───────────────────────────────────────────────────────────────── */

static int decode_mp3(const uint8_t *data, size_t data_len,
                      int16_t **pcm_out, size_t *frames_out,
                      uint32_t *rate_out, uint32_t *channels_out)
{
    /* mp3dec_ex prescan the Xing/VBRI header to know total frame count up
     * front, allowing us to allocate the locked mmap buffer once and decode
     * directly into it — no intermediate heap copy. */
    mp3dec_ex_t ex;
    if (mp3dec_ex_open_buf(&ex, data, data_len, MP3D_SEEK_TO_SAMPLE) != 0) {
        LOGE("decoder: MP3 open failed");
        return -1;
    }

    uint32_t ch = (uint32_t)ex.info.channels;
    uint32_t sr = (uint32_t)ex.info.hz;
    if (ch == 0 || ch > 2 || sr == 0) {
        LOGE("decoder: MP3 invalid stream: channels=%u hz=%u", ch, sr);
        mp3dec_ex_close(&ex);
        return -1;
    }

    /* ex.samples is total number of PCM samples (all channels interleaved) */
    size_t total_samples = ex.samples;
    size_t frames        = total_samples / ch;
    if (frames == 0) { mp3dec_ex_close(&ex); return -1; }

    int16_t *locked = alloc_pcm(frames, ch);
    if (!locked) { mp3dec_ex_close(&ex); return -1; }

    size_t decoded = mp3dec_ex_read(&ex, locked, total_samples);
    mp3dec_ex_close(&ex);

    if (decoded == 0) {
        LOGE("decoder: MP3 decode produced 0 samples");
        decoder_free(locked, frames, ch);
        return -1;
    }

    *pcm_out      = locked;
    *frames_out   = decoded / ch;
    *rate_out     = sr;
    *channels_out = ch;
    return 0;
}

/* ── public API ────────────────────────────────────────────────────────── */

int decoder_decode(const char *path,
                   int16_t **pcm_out, size_t *frames_out,
                   uint32_t *rate_out, uint32_t *channels_out)
{
    AudioFormat fmt = detect_format(path);
    if (fmt == FMT_UNKNOWN) {
        LOGE("decoder: unsupported format for %s", path);
        return -1;
    }

    size_t   file_len;
    uint8_t *file_buf = slurp_file(path, &file_len);
    if (!file_buf) return -1;

    /* eMMC can now go idle — we work entirely from file_buf in RAM. */
    int rc = -1;
    switch (fmt) {
    case FMT_FLAC: rc = decode_flac(file_buf, file_len, pcm_out, frames_out, rate_out, channels_out); break;
    case FMT_WAV:  rc = decode_wav (file_buf, file_len, pcm_out, frames_out, rate_out, channels_out); break;
    case FMT_MP3:  rc = decode_mp3 (file_buf, file_len, pcm_out, frames_out, rate_out, channels_out); break;
    default: break;
    }
    free(file_buf);

    if (rc == 0)
        LOGD("decoder: %s → %zu frames @ %u Hz / %u ch",
             path, *frames_out, *rate_out, *channels_out);
    return rc;
}

void decoder_free(int16_t *pcm, size_t frames, uint32_t channels)
{
    if (!pcm) return;
    size_t bytes = frames * channels * sizeof(int16_t);
    munlock(pcm, bytes);
    munmap(pcm, bytes);
}

int decoder_probe(const char *path, TrackInfo *info_out)
{
    AudioFormat fmt = detect_format(path);
    snprintf(info_out->path, sizeof(info_out->path), "%s", path);
    info_out->format = fmt;

    if (fmt == FMT_FLAC) {
        drflac *f = drflac_open_file(path, NULL);
        if (!f) return -1;
        if (f->sampleRate == 0) { drflac_close(f); return -1; }
        info_out->sample_rate     = f->sampleRate;
        info_out->channels        = f->channels;
        info_out->bits_per_sample = f->bitsPerSample;
        info_out->duration_ms     = (uint32_t)
            (((uint64_t)f->totalPCMFrameCount * 1000) / f->sampleRate);
        drflac_close(f);
        return 0;
    }
    if (fmt == FMT_WAV) {
        drwav w;
        if (!drwav_init_file(&w, path, NULL)) return -1;
        if (w.sampleRate == 0) { drwav_uninit(&w); return -1; }
        info_out->sample_rate     = w.sampleRate;
        info_out->channels        = w.channels;
        info_out->bits_per_sample = w.bitsPerSample;
        info_out->duration_ms     = (uint32_t)
            (((uint64_t)w.totalPCMFrameCount * 1000) / w.sampleRate);
        drwav_uninit(&w);
        return 0;
    }
    /* MP3 probe: read enough data for minimp3 to parse the first full frame.
     * 4 bytes is never sufficient — minimp3 needs the full frame (~400+ bytes)
     * to return valid hz/channels. Use 4096 to comfortably cover ID3 tags. */
    if (fmt == FMT_MP3) {
        FILE *fp = fopen(path, "rb");
        if (!fp) return -1;
        uint8_t hdr[4096];
        mp3dec_frame_info_t fi = {0};
        size_t n = fread(hdr, 1, sizeof(hdr), fp);
        fclose(fp);
        if (n > 0) {
            mp3dec_t mp3d; mp3dec_init(&mp3d);
            int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            mp3dec_decode_frame(&mp3d, hdr, (int)n, pcm, &fi);
        }
        if (fi.hz == 0 || fi.channels == 0) return -1;
        info_out->sample_rate     = (uint32_t)fi.hz;
        info_out->channels        = (uint32_t)fi.channels;
        info_out->bits_per_sample = 16;
        return 0;
    }
    return -1;
}
