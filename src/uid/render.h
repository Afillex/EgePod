#pragma once
#include "fb.h"
#include "../common/ipc.h"
#include "../common/track.h"

/* ── Virtual canvas dimensions (design space) ─────────────────────────────
 * All layout constants and hit-test coords are in this 720×1280 space.
 * render_frame() scale-blits to the actual display at runtime.            */
#define UI_DESIGN_W  720
#define UI_DESIGN_H  1280

/* ── Hit-test layout constants shared with main.c ─────────────────────────
 * All values in virtual canvas pixels (720×1280 portrait).                */

#define UI_CTRL_Y          824
#define UI_CTRL_H           74

#define UI_BTN_SHUFFLE_X   180
#define UI_BTN_SHUFFLE_W    50
#define UI_BTN_PREV_X      252
#define UI_BTN_PREV_W       50
#define UI_BTN_PLAY_X      324
#define UI_BTN_PLAY_W       72
#define UI_BTN_NEXT_X      418
#define UI_BTN_NEXT_W       50
#define UI_BTN_REPEAT_X    490
#define UI_BTN_REPEAT_W     50

/* Library view tap zone */
#define UI_LIB_ENTER_H    130   /* tap y in [UI_LIB_ENTER_Y, +UI_LIB_ENTER_H) opens library */

/* Library view hit-test constants */
#define UI_LIB_ENTER_Y    920   /* tap below this on nowplaying → open library */
#define UI_LIB_BACK_H     110   /* tap y < this in library view → back          */
#define UI_LIB_ROW_H       88   /* height of each library row in pixels          */
#define UI_LIB_VISIBLE     13   /* number of visible rows at once                */

/* Brightness bar — hit zone taller than the visual bar for easier tapping */
#define UI_BRIGHT_Y       1060
#define UI_BRIGHT_H        80
#define UI_BRIGHT_X        36
#define UI_BRIGHT_W       648

#define MAX_LIB_TRACKS    256

typedef struct {
    char     title[64];
    char     artist[64];
    uint32_t duration_ms;
    uint32_t track_idx;
} LibEntry;

typedef struct {
    TrackInfo   track;
    PlayerState state;
    uint32_t    position_ms;
    int64_t     pos_ref_mono_ms;   /* monotonic ms when position_ms was last received */
    uint32_t    battery_pct;
    uint32_t    track_count;
    uint32_t    cur_track_idx;
    int         lib_mode;
    int         lib_scroll;
    uint32_t    library_loaded;
    LibEntry    library[MAX_LIB_TRACKS];
    uint32_t    brightness;   /* 0–100 */
} UiState;

void render_frame(FbCtx *fb, const UiState *st);
