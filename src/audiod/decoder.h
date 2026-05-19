#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../common/track.h"

/* Decode an entire audio file to a locked PCM buffer.
 *
 * On success:
 *   *pcm_out       → mmap(MAP_ANONYMOUS) + mlock() buffer of interleaved int16_t samples
 *   *frames_out    → total sample frames (samples / channels)
 *   *rate_out      → sample rate in Hz
 *   *channels_out  → channel count
 *
 * Returns 0 on success, -1 on failure.
 * Caller must decoder_free() the buffer when done. */
int decoder_decode(const char   *path,
                   int16_t     **pcm_out,
                   size_t       *frames_out,
                   uint32_t     *rate_out,
                   uint32_t     *channels_out);

/* Release a buffer returned by decoder_decode(). */
void decoder_free(int16_t *pcm, size_t frames, uint32_t channels);

/* Read basic metadata without full decode. */
int decoder_probe(const char *path, TrackInfo *info_out);
