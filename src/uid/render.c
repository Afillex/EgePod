/* 🖥️ Kiosk UI Engineer — framebuffer renderer.
 *
 * Font: DejaVu Sans Bold via stb_truetype (EGEPOD_FONT_PATH or system path).
 * Falls back to font8x8 if no TTF is found.
 *
 * Layout (1080 × 2400, RGBX8888):
 *
 *   ┌──────────────────────────────────────┐  y=0
 *   │  EgePod                  🔋  14:32  │  header  h=100
 *   ├──────────────────────────────────────┤  y=100  accent stripe h=4
 *   │                                      │
 *   │         [album art 620×620]          │  y=120  centred
 *   │                                      │
 *   ├──────────────────────────────────────┤  y=760
 *   │           Title                      │  y=800  centered
 *   │        Artist — Album                │  y=860  centered
 *   ├──────────────────────────────────────┤  y=930
 *   │  0:42  [██████████░░░░░░░░]  5:11   │  progress bar  y=970
 *   ├──────────────────────────────────────┤  y=1040
 *   │        (|<)   ( ▶ )   (>|)          │  transport circles
 *   └──────────────────────────────────────┘  y=1280
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "render.h"
#include "font8x8.h"
#include "fb.h"
#include "../common/log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

/* ── colour palette ──────────────────────────────────────────────────────── */
#define C_BG          0xFF0D0D1A
#define C_SURFACE     0xFF141428
#define C_PANEL       0xFF1C1C35
#define C_ACCENT      0xFF6C63FF   /* violet */
#define C_HIGHLIGHT   0xFFFF4D8B   /* pink   */
#define C_TEXT_PRI    0xFFF0F0F0
#define C_TEXT_SEC    0xFF8888AA
#define C_TEXT_DIM    0xFF444466
#define C_PROGRESS    0xFF6C63FF
#define C_PROGRESS_BG 0xFF252540
#define C_BTN_MAIN    0xFF6C63FF
#define C_BTN_ALT     0xFF252540

/* ── TrueType font state ─────────────────────────────────────────────────── */
static stbtt_fontinfo  g_font;
static int             g_font_ok  = 0;
static unsigned char  *g_font_buf = NULL;

static const char *FONT_SEARCH[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
    NULL
};

static void font_init(void)
{
    if (g_font_ok) return;

    const char *paths[16];
    int n = 0;
    const char *env = getenv("EGEPOD_FONT_PATH");
    if (env) paths[n++] = env;
    for (int i = 0; FONT_SEARCH[i]; i++) paths[n++] = FONT_SEARCH[i];

    for (int i = 0; i < n; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        g_font_buf = malloc(sz);
        if (!g_font_buf) { fclose(f); continue; }
        fread(g_font_buf, 1, sz, f);
        fclose(f);
        if (stbtt_InitFont(&g_font, g_font_buf, 0)) {
            g_font_ok = 1;
            LOGI("render: TrueType font loaded from %s", paths[i]);
            return;
        }
        free(g_font_buf); g_font_buf = NULL;
    }
    LOGW("render: no TrueType font found, falling back to 8×8 bitmap");
}

/* ── pixel helpers ───────────────────────────────────────────────────────── */

static inline void blend_pixel(uint32_t *fb, uint32_t stride,
                                int px, int py, uint32_t fg, uint8_t alpha,
                                int fb_w, int fb_h)
{
    if (px < 0 || py < 0 || px >= fb_w || py >= fb_h) return;
    uint32_t bg  = fb[py * stride + px];
    uint32_t fr  = (fg >> 16) & 0xFF, fg2 = (fg >> 8) & 0xFF, fb2 = fg & 0xFF;
    uint32_t br  = (bg >> 16) & 0xFF, bg2 = (bg >> 8) & 0xFF, bb  = bg & 0xFF;
    uint32_t a   = alpha;
    uint32_t r   = (fr * a + br * (255 - a)) / 255;
    uint32_t g   = (fg2 * a + bg2 * (255 - a)) / 255;
    uint32_t b   = (fb2 * a + bb  * (255 - a)) / 255;
    fb[py * stride + px] = 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* Filled anti-aliased circle on raw fb buffer */
static void buf_fill_circle(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                             int cx, int cy, int r, uint32_t colour)
{
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r2 - dy * dy));
        int y  = cy + dy;
        if (y < 0 || y >= fb_h) continue;
        int x0 = cx - dx < 0 ? 0 : cx - dx;
        int x1 = cx + dx >= fb_w ? fb_w - 1 : cx + dx;
        uint32_t *row = fb + (size_t)y * stride;
        for (int x = x0; x <= x1; x++) row[x] = colour;
    }
}

/* Ring (hollow circle) */
static void buf_draw_ring(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                          int cx, int cy, int r_out, int thick, uint32_t colour)
{
    int r_in = r_out - thick;
    if (r_in < 0) r_in = 0;
    for (int dy = -r_out; dy <= r_out; dy++) {
        int out_dx = (int)sqrtf((float)(r_out * r_out - dy * dy));
        int in_dy  = (dy < -r_in) ? -r_in : (dy > r_in) ? r_in : dy;
        int in_dx  = (r_in > 0) ? (int)sqrtf((float)(r_in * r_in - in_dy * in_dy)) : 0;
        int y = cy + dy;
        if (y < 0 || y >= fb_h) continue;
        /* left segment */
        for (int x = cx - out_dx; x < cx - in_dx; x++) {
            if (x >= 0 && x < fb_w) fb[y * stride + x] = colour;
        }
        /* right segment */
        for (int x = cx + in_dx + 1; x <= cx + out_dx; x++) {
            if (x >= 0 && x < fb_w) fb[y * stride + x] = colour;
        }
    }
}

/* ── TTF draw helpers ────────────────────────────────────────────────────── */

static int ttf_str_width(const char *s, float px)
{
    if (!g_font_ok) return (int)strlen(s) * 8;
    float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int w = 0;
    for (const char *c = s; *c; c++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, *c, &adv, &lsb);
        w += (int)(adv * scale);
        if (*(c+1))
            w += (int)(stbtt_GetCodepointKernAdvance(&g_font, *c, *(c+1)) * scale);
    }
    return w;
}

static void ttf_draw(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                     int x, int y, const char *s, uint32_t color, float px)
{
    if (!g_font_ok) {
        int scale = (int)(px / 8);
        if (scale < 1) scale = 1;
        font_draw_str(fb, stride, x, y - (int)(px * 0.8f), s, color, scale);
        return;
    }
    float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int cx = x;
    for (const char *c = s; *c; c++) {
        int bw, bh, bx, by;
        uint8_t *bm = stbtt_GetCodepointBitmap(&g_font, scale, scale,
                                                *c, &bw, &bh, &bx, &by);
        for (int row = 0; row < bh; row++)
            for (int col = 0; col < bw; col++)
                blend_pixel(fb, stride, cx + bx + col, y + by + row,
                            color, bm[row * bw + col], fb_w, fb_h);
        stbtt_FreeBitmap(bm, NULL);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, *c, &adv, &lsb);
        cx += (int)(adv * scale);
        if (*(c+1))
            cx += (int)(stbtt_GetCodepointKernAdvance(&g_font, *c, *(c+1)) * scale);
    }
}

static void ttf_centred(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                        int x0, int x1, int y,
                        const char *s, uint32_t color, float px)
{
    int w = ttf_str_width(s, px);
    int x = x0 + (x1 - x0 - w) / 2;
    ttf_draw(fb, stride, fb_w, fb_h, x, y, s, color, px);
}

static void ttf_right(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                      int x_right, int y,
                      const char *s, uint32_t color, float px)
{
    int w = ttf_str_width(s, px);
    ttf_draw(fb, stride, fb_w, fb_h, x_right - w, y, s, color, px);
}

/* ── icon drawing ────────────────────────────────────────────────────────── */

/* Right-pointing triangle (play). cx,cy = visual center. sz = half-height. */
static void draw_icon_play(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                           int cx, int cy, int sz, uint32_t colour)
{
    for (int dy = -sz; dy <= sz; dy++) {
        int w = sz - abs(dy);
        int x0 = cx - sz / 3;
        for (int dx = 0; dx <= w; dx++) {
            int px = x0 + dx, py = cy + dy;
            if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                fb[py * stride + px] = colour;
        }
    }
}

/* Two vertical bars (pause). */
static void draw_icon_pause(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                            int cx, int cy, int sz, uint32_t colour)
{
    int bar_w = sz / 3, bar_h = sz * 2, gap = sz / 2;
    int x0 = cx - gap / 2 - bar_w;
    int x1 = cx + gap / 2;
    int y0 = cy - sz;
    for (int row = 0; row < bar_h; row++) {
        int y = y0 + row;
        if (y < 0 || y >= fb_h) continue;
        for (int col = 0; col < bar_w; col++) {
            int px;
            px = x0 + col;
            if (px >= 0 && px < fb_w) fb[y * stride + px] = colour;
            px = x1 + col;
            if (px >= 0 && px < fb_w) fb[y * stride + px] = colour;
        }
    }
}

/* Skip-back icon: vertical bar + left triangle */
static void draw_icon_prev(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                           int cx, int cy, int sz, uint32_t colour)
{
    int bar_w = sz / 4, bar_h = sz * 2;
    int x0    = cx - sz / 2 - bar_w / 2;
    int y0    = cy - sz;
    /* vertical bar */
    for (int row = 0; row < bar_h; row++) {
        int y = y0 + row;
        if (y < 0 || y >= fb_h) continue;
        for (int col = 0; col < bar_w; col++) {
            int px = x0 + col;
            if (px >= 0 && px < fb_w) fb[y * stride + px] = colour;
        }
    }
    /* left-pointing triangle */
    int tri_cx = cx + bar_w / 2;
    for (int dy = -sz; dy <= sz; dy++) {
        int w = sz - abs(dy);
        int tx0 = tri_cx - w;
        for (int dx = 0; dx <= w; dx++) {
            int px = tx0 + dx, py = cy + dy;
            if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                fb[py * stride + px] = colour;
        }
    }
}

/* Skip-next icon: right triangle + vertical bar */
static void draw_icon_next(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                           int cx, int cy, int sz, uint32_t colour)
{
    int bar_w = sz / 4, bar_h = sz * 2;
    int x0    = cx + sz / 2 - bar_w / 2;
    int y0    = cy - sz;
    /* vertical bar */
    for (int row = 0; row < bar_h; row++) {
        int y = y0 + row;
        if (y < 0 || y >= fb_h) continue;
        for (int col = 0; col < bar_w; col++) {
            int px = x0 + col;
            if (px >= 0 && px < fb_w) fb[y * stride + px] = colour;
        }
    }
    /* right-pointing triangle */
    int tri_cx = cx - bar_w / 2;
    for (int dy = -sz; dy <= sz; dy++) {
        int w = sz - abs(dy);
        int tx0 = tri_cx - sz / 3;
        for (int dx = 0; dx <= w; dx++) {
            int px = tx0 + dx, py = cy + dy;
            if (px >= 0 && px < fb_w && py >= 0 && py < fb_h)
                fb[py * stride + px] = colour;
        }
    }
}

/* ── misc helpers ────────────────────────────────────────────────────────── */

static void ms_to_str(uint32_t ms, char *buf, size_t len)
{
    uint32_t sec = ms / 1000;
    snprintf(buf, len, "%u:%02u", sec / 60, sec % 60);
}

static void trunc_str(const char *src, char *dst, size_t max_chars)
{
    size_t n = strlen(src);
    if (n <= max_chars) {
        memcpy(dst, src, n + 1);
    } else {
        memcpy(dst, src, max_chars - 2);
        dst[max_chars - 2] = '.';
        dst[max_chars - 1] = '.';
        dst[max_chars]     = '\0';
    }
}

/* Horizontal gradient fill: left colour → right colour */
static void fill_rect_grad(uint32_t *fb, uint32_t stride, int fb_w, int fb_h,
                            int x, int y, int w, int h,
                            uint32_t c0, uint32_t c1)
{
    for (int row = y; row < y + h && row < fb_h; row++) {
        uint32_t *line = fb + (size_t)row * stride;
        for (int col = x; col < x + w && col < fb_w; col++) {
            if (col < 0) continue;
            int t = (col - x) * 255 / (w > 1 ? w - 1 : 1);
            uint32_t r = ((c0 >> 16 & 0xFF) * (255 - t) + (c1 >> 16 & 0xFF) * t) / 255;
            uint32_t g = ((c0 >> 8  & 0xFF) * (255 - t) + (c1 >> 8  & 0xFF) * t) / 255;
            uint32_t b = ((c0       & 0xFF) * (255 - t) + (c1       & 0xFF) * t) / 255;
            line[col] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }
}

/* ── render_frame ────────────────────────────────────────────────────────── */

void render_frame(FbCtx *fb, const UiState *st)
{
    font_init();

    uint32_t *buf    = fb_back(fb);
    uint32_t  stride = fb->stride / sizeof(uint32_t);
    int       W      = (int)fb->width;
    int       H      = (int)fb->height;

    /* ── background ── */
    fb_clear(fb, C_BG);

    /* ── header ── */
    {
        int hdr_h = 100;
        fb_fill_rect(fb, 0, 0, W, hdr_h, C_SURFACE);

        /* App name */
        ttf_draw(buf, stride, W, H, 44, 70, "EgePod", C_TEXT_PRI, 50.0f);

        /* Battery */
        char batt[16];
        snprintf(batt, sizeof(batt), "BAT %u%%", st->battery_pct);
        ttf_right(buf, stride, W, H, W - 44, 52, batt, C_TEXT_SEC, 26.0f);

        /* Clock */
        {
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            char clk[8];
            snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
            ttf_right(buf, stride, W, H, W - 44, 88, clk, C_TEXT_PRI, 34.0f);
        }

        /* Accent stripe */
        fb_fill_rect(fb, 0, hdr_h, W, 4, C_ACCENT);
    }

    /* ── album art ── */
    {
        int art_size = 620;
        int art_x    = (W - art_size) / 2;
        int art_y    = 124;
        int brd      = 4;

        /* Outer glow ring */
        buf_draw_ring(buf, stride, W, H,
                      art_x + art_size / 2, art_y + art_size / 2,
                      art_size / 2 + 8, 3, C_ACCENT);

        /* Border */
        fb_fill_rect(fb, art_x,        art_y,        art_size, art_size, C_ACCENT);
        /* Panel fill */
        fb_fill_rect(fb, art_x + brd,  art_y + brd,
                     art_size - brd*2, art_size - brd*2, C_PANEL);

        /* Decorative inner ring */
        buf_draw_ring(buf, stride, W, H,
                      art_x + art_size / 2, art_y + art_size / 2,
                      art_size / 2 - 20, 2, C_TEXT_DIM);

        /* "NO COVER" label */
        ttf_centred(buf, stride, W, H, art_x, art_x + art_size,
                    art_y + art_size / 2 + 14, "NO COVER", C_TEXT_DIM, 30.0f);
    }

    /* ── track info ── */
    {
        int info_y = 800;
        char title[52], artist[36], album[36], sub[80];
        const char *t = st->track.title[0]  ? st->track.title  : "No Track";
        const char *a = st->track.artist[0] ? st->track.artist : "Unknown Artist";
        const char *l = st->track.album[0]  ? st->track.album  : "Unknown Album";
        trunc_str(t, title,  48);
        trunc_str(a, artist, 32);
        trunc_str(l, album,  32);
        snprintf(sub, sizeof(sub), "%s  \xe2\x80\x94  %s", artist, album);

        ttf_centred(buf, stride, W, H, 40, W - 40, info_y,
                    title, C_TEXT_PRI, 52.0f);
        ttf_centred(buf, stride, W, H, 40, W - 40, info_y + 72,
                    sub, C_TEXT_SEC, 30.0f);
    }

    /* ── progress bar ── */
    {
        int bar_x = 60, bar_y = 990, bar_w = W - 120, bar_h = 8;

        /* Time labels */
        char pos_s[8], dur_s[8];
        ms_to_str(st->position_ms,       pos_s, sizeof(pos_s));
        ms_to_str(st->track.duration_ms, dur_s, sizeof(dur_s));
        ttf_draw (buf, stride, W, H, bar_x,          bar_y - 32, pos_s, C_TEXT_SEC, 28.0f);
        ttf_right(buf, stride, W, H, bar_x + bar_w,  bar_y - 32, dur_s, C_TEXT_SEC, 28.0f);

        /* Track: rounded ends by overdrawing */
        fb_fill_rect(fb, bar_x, bar_y - 1, bar_w, bar_h + 2, C_PROGRESS_BG);
        fb_fill_rect(fb, bar_x, bar_y,     bar_w, bar_h,     C_PROGRESS_BG);

        uint32_t dur = st->track.duration_ms;
        if (dur > 0 && st->position_ms <= dur) {
            int filled = (int)((uint64_t)st->position_ms * bar_w / dur);
            if (filled > 0) {
                /* Gradient fill: accent → highlight */
                fill_rect_grad(buf, stride, W, H,
                               bar_x, bar_y, filled, bar_h,
                               C_ACCENT, C_HIGHLIGHT);
            }
            /* Playhead dot */
            int dot_cx = bar_x + (filled > 0 ? filled : 0);
            int dot_cy = bar_y + bar_h / 2;
            buf_fill_circle(buf, stride, W, H, dot_cx, dot_cy, 12, C_HIGHLIGHT);
            buf_fill_circle(buf, stride, W, H, dot_cx, dot_cy,  6, C_TEXT_PRI);
        }
    }

    /* ── transport controls ── */
    {
        int btn_y    = 1155;        /* circle centers */
        int cx       = W / 2;
        int spacing  = 220;

        /* PREV button: mid-size ring + icon */
        int prev_cx = cx - spacing;
        int next_cx = cx + spacing;
        int r_side  = 62;
        int r_main  = 80;

        /* Side button backgrounds */
        buf_fill_circle(buf, stride, W, H, prev_cx, btn_y, r_side, C_BTN_ALT);
        buf_draw_ring  (buf, stride, W, H, prev_cx, btn_y, r_side, 3, C_ACCENT);
        buf_fill_circle(buf, stride, W, H, next_cx, btn_y, r_side, C_BTN_ALT);
        buf_draw_ring  (buf, stride, W, H, next_cx, btn_y, r_side, 3, C_ACCENT);

        /* Main play/pause background */
        buf_fill_circle(buf, stride, W, H, cx, btn_y, r_main, C_BTN_MAIN);

        /* Icons */
        draw_icon_prev(buf, stride, W, H, prev_cx, btn_y, 24, C_ACCENT);
        draw_icon_next(buf, stride, W, H, next_cx, btn_y, 24, C_ACCENT);

        if (st->state == PLAYER_PLAYING)
            draw_icon_pause(buf, stride, W, H, cx, btn_y, 28, C_BG);
        else
            draw_icon_play (buf, stride, W, H, cx, btn_y, 28, C_BG);
    }

    /* ── state overlay ── */
    if (st->state == PLAYER_LOADING) {
        ttf_centred(buf, stride, W, H, 0, W, 1340, "Loading...", C_TEXT_SEC, 34.0f);
    } else if (st->state == PLAYER_ERROR) {
        ttf_centred(buf, stride, W, H, 0, W, 1340, "! Playback Error", C_HIGHLIGHT, 34.0f);
    } else if (st->state == PLAYER_IDLE) {
        ttf_centred(buf, stride, W, H, 0, W, 1340,
                    "Tap play to start", C_TEXT_DIM, 30.0f);
    }
}
