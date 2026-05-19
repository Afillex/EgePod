#pragma once
#include "fb.h"
#include "../common/ipc.h"
#include "../common/track.h"

typedef struct {
    TrackInfo   track;
    PlayerState state;
    uint32_t    position_ms;
    uint32_t    battery_pct;
} UiState;

/* Draw a full screen frame into the back buffer.
 * Call fb_flip() afterwards to display it. */
void render_frame(FbCtx *fb, const UiState *st);
