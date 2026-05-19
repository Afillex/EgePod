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

    uint8_t *buf = malloc(st.st_size);
    if (!buf) { close(fd); return NULL; }

    ssize_t rd = read(fd, buf, st.st_size);
    close(fd);

    if (rd != st.st_size) {
        LOGE("decoder: short read on %s (%zd / %lld)", path, rd, (long long)st.st_size);
        free(buf);
        return NULL;
    }
    *size_out = (size_t)st.st_size;
    return buf;
}

/* Allocate a locked anonymous mapping for PCM output. */
static int16_t *alloc_pcm(size_t n_frames, uint32_t channels)
{
    size_t bytes = n_frames * channels * sizeof(int16_t);
    void *p = mmap(NULL, bytes,
                   PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE,
                   -1, 0);
    if (p == MAP_FAILED) {
        LOGE("decoder: mmap(%zu): %s", bytes, strerror(errno));
        return NULL;
    }
    if (mlock(p, bytes) != 0)
        LOGW("decoder: mlock failed (%s) — swap possible under memory pressure",
             strerror(errno));
    return (int16_t *)p;
}

/* ── FLAC ──────────────────────────────────────────────────────────────── */

static int decode_flac(const uint8_t *data, size_t data_len,
                       int16_t **pcm_out, size_t *frames_out,
                       uint32_t *rate_out, uint32_t *channels_out)
{
    drflac_uint64 frame_count;
    unsigned int  channels, rate;

    /* dr_flac decodes to int16 from memory buffer */
    int16_t *decoded = drflac_open_memory_and_read_pcm_frames_s16(
                           data, data_len, &channels, &rate, &frame_count, NULL);
    if (!decoded) { LOGE("decoder: FLAC decode failed"); return -1; }

    size_t bytes = (size_t)frame_count * channels * sizeof(int16_t);
    int16_t *locked = alloc_pcm((size_t)frame_count, channels);
    if (!locked) { drflac_free(decoded, NULL); return -1; }

    memcpy(locked, decoded, bytes);
    drflac_free(decoded, NULL);

    *pcm_out      = locked;
    *frames_out   = (size_t)frame_count;
    *rate_out     = rate;
    *channels_out = channels;
    return 0;
}

/* ── WAV ───────────────────────────────────────────────────────────────── */

static int decode_wav(const uint8_t *data, size_t data_len,
                      int16_t **pcm_out, size_t *frames_out,
                      uint32_t *rate_out, uint32_t *channels_out)
{
    drwav_uint64  frame_count;
    unsigned int  channels, rate;

    int16_t *decoded = drwav_open_memory_and_read_pcm_frames_s16(
                           data, data_len, &channels, &rate, &frame_count, NULL);
    if (!decoded) { LOGE("decoder: WAV decode failed"); return -1; }

    size_t bytes = (size_t)frame_count * channels * sizeof(int16_t);
    int16_t *locked = alloc_pcm((size_t)frame_count, channels);
    if (!locked) { drwav_free(decoded, NULL); return -1; }

    memcpy(locked, decoded, bytes);
    drwav_free(decoded, NULL);

    *pcm_out      = locked;
    *frames_out   = (size_t)frame_count;
    *rate_out     = rate;
    *channels_out = channels;
    return 0;
}

/* ── MP3 ───────────────────────────────────────────────────────────────── */

static int decode_mp3(const uint8_t *data, size_t data_len,
                      int16_t **pcm_out, size_t *frames_out,
                      uint32_t *rate_out, uint32_t *channels_out)
{
    mp3dec_t        mp3d;
    mp3dec_file_info_t info;

    mp3dec_init(&mp3d);
    if (mp3dec_load_buf(&mp3d, data, data_len, &info, NULL, NULL) != 0) {
        LOGE("decoder: MP3 decode failed");
        return -1;
    }
    if (!info.buffer || info.samples == 0) {
        free(info.buffer);
        return -1;
    }

    size_t frames = info.samples / (size_t)(info.channels ? info.channels : 2);
    int16_t *locked = alloc_pcm(frames, info.channels);
    if (!locked) { free(info.buffer); return -1; }

    memcpy(locked, info.buffer, info.samples * sizeof(int16_t));
    free(info.buffer);

    *pcm_out      = locked;
    *frames_out   = frames;
    *rate_out     = (uint32_t)info.hz;
    *channels_out = (uint32_t)info.channels;
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
    /* MP3 probe: parse just the first frame header */
    if (fmt == FMT_MP3) {
        FILE *fp = fopen(path, "rb");
        if (!fp) return -1;
        uint8_t hdr[4];
        mp3dec_frame_info_t fi = {0};   /* zero-init: safe if fread < 4 */
        if (fread(hdr, 1, 4, fp) == 4) {
            mp3dec_t mp3d; mp3dec_init(&mp3d);
            int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            mp3dec_decode_frame(&mp3d, hdr, 4, pcm, &fi);
        }
        fclose(fp);
        info_out->sample_rate     = (uint32_t)fi.hz;
        info_out->channels        = (uint32_t)fi.channels;
        info_out->bits_per_sample = 16;
        return 0;
    }
    return -1;
}
