#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct AlsaOut AlsaOut;

/* Open the PCM device and configure the headphone mixer path.
 * card/device: tinyalsa card and device indices (typically 0/0 for MT6359).
 * Returns NULL on failure. */
AlsaOut *alsa_out_open(unsigned int card,
                       unsigned int device,
                       unsigned int rate,
                       unsigned int channels,
                       unsigned int bits);

/* Write interleaved PCM frames. Blocks until the period buffer accepts data.
 * Returns 0 on success, -1 on underrun/error (caller should reopen). */
int alsa_out_write(AlsaOut *out, const int16_t *frames, size_t frame_count);

/* Drain pending samples and close the device. */
void alsa_out_close(AlsaOut *out);

/* Pause/resume without closing (avoids codec re-init overhead). */
int alsa_out_pause(AlsaOut *out);
int alsa_out_resume(AlsaOut *out);

/* Returns 1 if the open device matches rate/channels; 0 if it needs to be
 * reopened (e.g. the next track has a different sample rate). */
int alsa_out_matches_format(const AlsaOut *out, unsigned int rate,
                            unsigned int channels);
