#pragma once
#include <stdint.h>

/* Save/restore playback position across restarts.
 * Written atomically (tmp + rename) so a crash during write leaves the
 * previous state intact.  On SIMULATE the file lives in /tmp. */

int persist_save(uint32_t track_idx, uint32_t position_ms, int was_playing);
int persist_load(uint32_t *track_idx, uint32_t *position_ms, int *was_playing);
