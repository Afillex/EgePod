/* Simulation ALSA backend — compiled only when USE_ALSA_LIB=1.
 *
 * Priority:
 *   1. Try system libasound (works if the VM has a real soundcard)
 *   2. Fall back to writing raw s16le PCM to EGEPOD_AUDIO_OUT
 *      (default: /tmp/egepod_audio.raw)
 *
 * The raw file can be played on macOS:
 *   ffplay -f s16le -ar 44100 -ac 2 -nodisp /tmp/egepod_audio.raw
 * Or live via orb pipe:
 *   orb run -m egepod-sim cat /tmp/egepod_audio.raw | \
 *     ffplay -f s16le -ar 44100 -ac 2 -nodisp -
 */

#ifdef USE_ALSA_LIB

#include "alsa_out.h"
#include "../common/log.h"

#include <alsa/asoundlib.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>

#define AUDIO_OUT_DEFAULT "/tmp/egepod_audio.raw"

struct AlsaOut {
    snd_pcm_t    *pcm;      /* non-NULL → real ALSA device */
    FILE         *raw_file; /* non-NULL → file fallback */
    unsigned int  channels;
    unsigned int  rate;
};

/* ── real ALSA open ──────────────────────────────────────────────────────── */

static snd_pcm_t *try_alsa_open(unsigned int rate, unsigned int channels)
{
    const char *names[] = { "hw:0,0", "plughw:0,0", "default", "pulse", NULL };
    for (int i = 0; names[i]; i++) {
        snd_pcm_t *pcm = NULL;
        int rc = snd_pcm_open(&pcm, names[i], SND_PCM_STREAM_PLAYBACK, 0);
        if (rc < 0) continue;

        snd_pcm_hw_params_t *p;
        snd_pcm_hw_params_alloca(&p);
        snd_pcm_hw_params_any(pcm, p);
        snd_pcm_hw_params_set_access(pcm, p, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(pcm, p, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(pcm, p, channels);
        snd_pcm_hw_params_set_rate_near(pcm, p, &rate, 0);
        snd_pcm_uframes_t period = 4096;
        unsigned int periods = 4;
        snd_pcm_hw_params_set_period_size_near(pcm, p, &period, 0);
        snd_pcm_hw_params_set_periods_near(pcm, p, &periods, 0);
        rc = snd_pcm_hw_params(pcm, p);
        if (rc < 0) { snd_pcm_close(pcm); continue; }

        LOGI("alsa_sim: opened PCM '%s' @ %u Hz / %u ch", names[i], rate, channels);
        return pcm;
    }
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────────────── */

AlsaOut *alsa_out_open(unsigned int card, unsigned int device,
                       unsigned int rate, unsigned int channels,
                       unsigned int bits)
{
    (void)card; (void)device; (void)bits;

    AlsaOut *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->channels = channels;
    out->rate     = rate;

    out->pcm = try_alsa_open(rate, channels);
    if (out->pcm) return out;

    /* No soundcard — write raw PCM to file instead */
    const char *path = getenv("EGEPOD_AUDIO_OUT");
    if (!path) path = AUDIO_OUT_DEFAULT;

    out->raw_file = fopen(path, "wb");
    if (!out->raw_file) {
        LOGE("alsa_sim: cannot open audio out file %s: %s", path, strerror(errno));
        free(out);
        return NULL;
    }
    LOGI("alsa_sim: no soundcard — writing raw s16le PCM to %s  (%u Hz / %u ch)",
         path, rate, channels);
    LOGI("alsa_sim: play with:  ffplay -f s16le -ar %u -ac %u -nodisp %s",
         rate, channels, path);
    return out;
}

int alsa_out_write(AlsaOut *out, const int16_t *frames, size_t frame_count)
{
    if (out->pcm) {
        snd_pcm_sframes_t r = snd_pcm_writei(out->pcm, frames, frame_count);
        if (r == -EPIPE) { snd_pcm_recover(out->pcm, (int)r, 1); return 0; }
        return (r < 0) ? -1 : 0;
    }

    if (out->raw_file) {
        size_t n = fwrite(frames, sizeof(int16_t), frame_count * out->channels,
                          out->raw_file);
        fflush(out->raw_file);  /* keep the file current for the viewer */
        /* Pace writes to real-time so the player doesn't race through tracks */
        uint64_t ns = (uint64_t)frame_count * 1000000000ULL / out->rate;
        struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000ULL),
                               .tv_nsec = (long)(ns % 1000000000ULL) };
        nanosleep(&ts, NULL);
        return (n == frame_count * out->channels) ? 0 : -1;
    }

    /* Pacing: without any sink we'd spin flat-out.  Sleep proportionally. */
    {
        uint64_t ns = (uint64_t)frame_count * 1000000000ULL / out->rate;
        struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000ULL),
                               .tv_nsec = (long)(ns % 1000000000ULL) };
        nanosleep(&ts, NULL);
    }
    return 0;
}

int alsa_out_pause(AlsaOut *out)
{
    return out->pcm ? snd_pcm_pause(out->pcm, 1) : 0;
}

int alsa_out_resume(AlsaOut *out)
{
    return out->pcm ? snd_pcm_pause(out->pcm, 0) : 0;
}

void alsa_out_close(AlsaOut *out)
{
    if (!out) return;
    if (out->pcm)      { snd_pcm_drain(out->pcm); snd_pcm_close(out->pcm); }
    if (out->raw_file) fclose(out->raw_file);
    free(out);
}

#endif /* USE_ALSA_LIB */
