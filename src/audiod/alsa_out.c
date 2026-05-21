/* 🐧 Kernel & HAL Hacker — tinyalsa output driver for MT6359 codec.
 *
 * Audio path: CPU → tinyalsa PCM → I2S → MT6359 PMIC codec → 3.5 mm HP jack
 *
 * Mixer controls must be set once at open time and left alone.  Re-touching
 * the mixer during playback causes audible glitches and unnecessary mA draw
 * from the PMIC's digital section. */

#include "alsa_out.h"
#include "../common/log.h"

#include <tinyalsa/asoundlib.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── MT6359 HP mixer path ──────────────────────────────────────────────── */
/* Control names are codec-driver strings from the AOSP vendor tree for
 * MT6785 / rosemary.  Adjust if your build differs. */
static const struct {
    const char *name;
    int         value;   /* 0 = off, 1 = on, >1 = enum index */
} kMixerPath[] = {
    { "Headphone Switch",          1 },
    { "Audio_Amp_R_Switch",        1 },
    { "Audio_Amp_L_Switch",        1 },
    { "HPL Mux",                   1 },   /* enum: 1 = "Audio Playback" */
    { "HPR Mux",                   1 },
    { "DAC R2 Switch",             1 },
    { "DAC L2 Switch",             1 },
    { "HPOUT_L_SEL",               1 },
    { "HPOUT_R_SEL",               1 },
    /* Volume — leave at a safe non-clipping level; user controls via HW keys */
    { "Headphone Volume",         14 },   /* 14/15 = ~−6 dBFS */
};

static void apply_mixer_path(unsigned int card)
{
    struct mixer *mx = mixer_open(card);
    if (!mx) { LOGW("alsa: mixer_open(card %u) failed", card); return; }

    for (size_t i = 0; i < sizeof(kMixerPath) / sizeof(kMixerPath[0]); i++) {
        struct mixer_ctl *ctl = mixer_get_ctl_by_name(mx, kMixerPath[i].name);
        if (!ctl) {
            LOGD("alsa: mixer ctl '%s' not found (skip)", kMixerPath[i].name);
            continue;
        }
        enum mixer_ctl_type t = mixer_ctl_get_type(ctl);
        int rc = (t == MIXER_CTL_TYPE_BOOL || t == MIXER_CTL_TYPE_INT)
                     ? mixer_ctl_set_value(ctl, 0, kMixerPath[i].value)
                     : mixer_ctl_set_enum_by_string(ctl,
                           mixer_ctl_get_enum_string(ctl, kMixerPath[i].value));
        if (rc != 0)
            LOGW("alsa: failed to set '%s' = %d", kMixerPath[i].name, kMixerPath[i].value);
    }
    mixer_close(mx);
}

/* ── AlsaOut context ───────────────────────────────────────────────────── */

struct AlsaOut {
    struct pcm        *pcm;
    struct pcm_config  cfg;
    unsigned int       card;
    unsigned int       device;
};

AlsaOut *alsa_out_open(unsigned int card, unsigned int device,
                       unsigned int rate, unsigned int channels,
                       unsigned int bits)
{
    /* Only 16-bit output is used; the codec does its own DAC filtering. */
    (void)bits;

    apply_mixer_path(card);

    AlsaOut *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->card   = card;
    out->device = device;

    out->cfg = (struct pcm_config){
        .channels     = channels,
        .rate         = rate,
        .period_size  = 4096,
        .period_count = 8,   /* 8×4096 = ~742ms buffer; CPU sleeps longer between refills */
        .format       = PCM_FORMAT_S16_LE,
        /* start_threshold = 0 means hardware starts as soon as first period
         * is written, minimising initial latency. */
        .start_threshold  = 0,
        .stop_threshold   = 0,
        .silence_threshold = 0,
        .avail_min        = 0,
    };

    out->pcm = pcm_open(card, device, PCM_OUT | PCM_NORESTART, &out->cfg);
    if (!out->pcm || !pcm_is_ready(out->pcm)) {
        LOGE("alsa: pcm_open(card=%u, dev=%u) failed: %s",
             card, device, pcm_get_error(out->pcm));
        pcm_close(out->pcm);
        free(out);
        return NULL;
    }

    LOGI("alsa: opened card=%u dev=%u  %u Hz / %u ch / 16-bit  period=%u×%u",
         card, device, rate, channels, out->cfg.period_size, out->cfg.period_count);
    return out;
}

int alsa_out_write(AlsaOut *out, const int16_t *frames, size_t frame_count)
{
    /* pcm_writei counts in frames.  PCM_NORESTART means it will not restart
     * after xrun; caller should reopen on -EPIPE. */
    int rc = pcm_writei(out->pcm, frames, (unsigned int)frame_count);
    if (rc < 0) {
        LOGE("alsa: pcm_writei: %s", pcm_get_error(out->pcm));
        return -1;
    }
    return 0;
}

int alsa_out_pause(AlsaOut *out)
{
    /* Hardware pause keeps the codec warm so resume has no pop/click. */
    int rc = pcm_ioctl(out->pcm, SNDRV_PCM_IOCTL_PAUSE, 1);
    if (rc != 0) LOGW("alsa: pause ioctl: %s", strerror(errno));
    return rc;
}

int alsa_out_resume(AlsaOut *out)
{
    int rc = pcm_ioctl(out->pcm, SNDRV_PCM_IOCTL_PAUSE, 0);
    if (rc != 0) LOGW("alsa: resume ioctl: %s", strerror(errno));
    return rc;
}

void alsa_out_close(AlsaOut *out)
{
    if (!out) return;
    pcm_close(out->pcm);
    free(out);
}
