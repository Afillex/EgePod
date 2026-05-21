/* 🖥️ Kiosk UI Engineer — EgePod framebuffer renderer.
 *
 * Minimal brand-grade UI, 720×1280, pure dark theme.
 * Primary font: Nimbus Sans Bold (Helvetica-compatible, fonts-urw-base35).
 * Falls back to Liberation Sans Bold → Ubuntu Bold → DejaVu Sans Bold.
 *
 * Layout (y-coords):
 *   0 – 50    Status bar    (clock L, battery R)
 *  50 – 110   Header        ("EGEPOD" wordmark, state label R)
 * 110 – 111   1 px separator
 * 130 – 650   Album art     (520×520, centred — vinyl-record placeholder)
 * 680 – 720   Track title   (centred, bold, 28 px, ALL CAPS)
 * 724 – 750   Artist / Alb  (centred, 17 px, muted)
 * 768 – 772   Progress bar  (4 px, with scrub handle dot)
 * 782 – 800   Timestamps    (elapsed L, duration R)
 * 824 – 898   Transport     (shuffle · prev · play · next · repeat)
 * 920 – 921   1 px separator
 * 960+        Library info  (track count, format / sample-rate)
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
#include <wchar.h>

/* ── Palette ─────────────────────────────────────────────────────────────── */
#define C_BG        0xFF0B0B0B   /* page background               */
#define C_CARD      0xFF0F0F0F   /* album art card background     */
#define C_SEP       0xFF1E1E1E   /* separator lines               */
#define C_TEXT      0xFFF0F0F0   /* primary text (off-white)      */
#define C_TEXT_2    0xFF787878   /* secondary — artist, battery   */
#define C_TEXT_3    0xFF383838   /* muted — timestamps, labels    */
#define C_PROG_BG   0xFF232323   /* progress track                */
#define C_PROG_FG   0xFFE0E0E0   /* progress fill                 */
#define C_VINYL_A   0xFF101010   /* groove trough                 */
#define C_VINYL_B   0xFF191919   /* groove ridge                  */
#define C_VINYL_LBL 0xFF1C1C1C   /* center label disc             */
#define C_VINYL_HUB 0xFF080808   /* spindle hole                  */

/* ── Layout constants ───────────────────────────────────────────────────── */
#define W_FB    720
#define H_FB    1280

#define STATUS_H   50

#define HDR_Y     (STATUS_H)
#define HDR_H      60
#define SEP_Y     (HDR_Y + HDR_H)      /* 110 */

#define ART_Y     (SEP_Y + 20)         /* 130 */
#define ART_SZ     520
#define ART_X     ((W_FB - ART_SZ) / 2)     /* 100 */
#define ART_CX    (ART_X + ART_SZ / 2)      /* 360 */
#define ART_CY    (ART_Y + ART_SZ / 2)      /* 390 */
#define ART_CARD_R 14   /* card corner radius */

/* Vinyl geometry (relative to ART_CX, ART_CY) */
#define VINYL_R      (ART_SZ / 2 - 8)    /* 252 */
#define VINYL_LBL_R  (VINYL_R / 4)       /* 63  */
#define VINYL_HUB_R    7

/* Text rows */
#define TITLE_Y   (ART_Y + ART_SZ + 30)  /* 680 */
#define ARTIST_Y  (TITLE_Y + 44)          /* 724 */

/* Progress */
#define PROG_Y    (ARTIST_Y + 30 + 18)   /* 772 */
#define PROG_H     4
#define PROG_X     36
#define PROG_W    (W_FB - PROG_X * 2)    /* 648 */
#define TIME_Y    (PROG_Y + PROG_H + 10) /* 786 */

/* Transport — must match render.h hit-test constants exactly */
#define CTRL_Y    824
#define CTRL_H     74
#define CTRL_CY   (CTRL_Y + CTRL_H / 2)   /* 861 */

#define BTN_SZ    50
#define PLAY_SZ   72
#define BTN_GAP   22
/* Row: 50+22+50+22+72+22+50+22+50 = 360; margin = (720-360)/2 = 180 */
#define BTN_M     180

#define BTN_SHUFFLE_X  BTN_M
#define BTN_PREV_X    (BTN_M + BTN_SZ + BTN_GAP)
#define BTN_PLAY_X    (BTN_M + BTN_SZ*2 + BTN_GAP*2)
#define BTN_NEXT_X    (BTN_M + BTN_SZ*2 + BTN_GAP*2 + PLAY_SZ + BTN_GAP)
#define BTN_REPEAT_X  (BTN_M + BTN_SZ*3 + BTN_GAP*2 + PLAY_SZ + BTN_GAP*2)

#define DIV2_Y    (CTRL_Y + CTRL_H + 22) /* 920 */
#define LIB_Y     (DIV2_Y + 44)          /* 964 */

/* ── TrueType font ──────────────────────────────────────────────────────── */
static stbtt_fontinfo  g_font;
static int             g_font_ok  = 0;
static unsigned char  *g_font_buf = NULL;

/* Priority list — Nimbus Sans Bold is metrically identical to Helvetica.
 * Android paths come first so the production binary finds a font quickly. */
static const char *FONT_PATHS[] = {
    /* Android system fonts (Redmi Note 10S / MIUI / AOSP) */
    "/system/fonts/Roboto-Bold.ttf",
    "/system/fonts/NotoSans-Bold.ttf",
    "/system/fonts/DroidSans-Bold.ttf",
    "/system/fonts/Helvetica.ttf",
    /* Linux simulation (OrbStack Ubuntu VM) */
    "/usr/share/fonts/opentype/urw-base35/NimbusSans-Bold.otf",
    "/usr/share/fonts/opentype/urw-base35/NimbusSansCond-Bold.otf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    /* macOS fallback */
    "/System/Library/Fonts/Helvetica.ttc",
    NULL
};

static void font_init(void)
{
    if (g_font_ok) return;
    const char *paths[24]; int n = 0;
    const char *env = getenv("EGEPOD_FONT_PATH");
    if (env && env[0]) paths[n++] = env;
    for (int i = 0; FONT_PATHS[i]; i++) paths[n++] = FONT_PATHS[i];
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
        g_font_buf = malloc(sz);
        if (!g_font_buf) { fclose(f); continue; }
        if (fread(g_font_buf, 1, sz, f) != (size_t)sz) {
            free(g_font_buf); g_font_buf = NULL; fclose(f); continue;
        }
        fclose(f);
        if (stbtt_InitFont(&g_font, g_font_buf, 0)) {
            g_font_ok = 1;
            LOGI("render: font %s", paths[i]);
            return;
        }
        free(g_font_buf); g_font_buf = NULL;
    }
    LOGW("render: no TTF — 8×8 bitmap fallback");
}

/* ── Pixel helpers ──────────────────────────────────────────────────────── */
static inline void blend_pixel(uint32_t *buf, uint32_t stride,
                                int px, int py, uint32_t fg, uint8_t alpha,
                                int fw, int fh)
{
    if (px < 0 || py < 0 || px >= fw || py >= fh) return;
    uint32_t bg = buf[(size_t)py * stride + px];
    uint32_t a  = alpha;
    uint32_t r  = ((fg>>16&0xFF)*a + (bg>>16&0xFF)*(255-a)) / 255;
    uint32_t g  = ((fg>>8 &0xFF)*a + (bg>>8 &0xFF)*(255-a)) / 255;
    uint32_t b  = ((fg    &0xFF)*a + (bg    &0xFF)*(255-a)) / 255;
    buf[(size_t)py * stride + px] = 0xFF000000 | (r<<16) | (g<<8) | b;
}

/* ── TTF rendering ──────────────────────────────────────────────────────── */
static int ttf_str_width(const char *s, float px)
{
    if (!g_font_ok) return (int)strlen(s) * 8;
    float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int w = 0;
    for (const char *c = s; *c; c++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*c, &adv, &lsb);
        w += (int)(adv * scale);
        if (*(c+1))
            w += (int)(stbtt_GetCodepointKernAdvance(&g_font,
                        (unsigned char)*c, (unsigned char)*(c+1)) * scale);
    }
    return w;
}

static void ttf_draw(uint32_t *buf, uint32_t stride, int fw, int fh,
                     int x, int y, const char *s, uint32_t color, float px)
{
    if (!g_font_ok) {
        int sc = (int)(px / 8); if (sc < 1) sc = 1;
        font_draw_str(buf, stride, x, y - (int)(px * 0.8f), s, color, sc);
        return;
    }
    float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int cx = x;
    for (const char *c = s; *c; c++) {
        int bw, bh, bx, by;
        uint8_t *bm = stbtt_GetCodepointBitmap(&g_font, scale, scale,
                                                (unsigned char)*c, &bw, &bh, &bx, &by);
        for (int row = 0; row < bh; row++)
            for (int col = 0; col < bw; col++)
                blend_pixel(buf, stride, cx+bx+col, y+by+row,
                            color, bm[row*bw+col], fw, fh);
        stbtt_FreeBitmap(bm, NULL);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (unsigned char)*c, &adv, &lsb);
        cx += (int)(adv * scale);
        if (*(c+1))
            cx += (int)(stbtt_GetCodepointKernAdvance(&g_font,
                         (unsigned char)*c, (unsigned char)*(c+1)) * scale);
    }
}

static void ttf_centred(uint32_t *buf, uint32_t stride, int fw, int fh,
                        int x0, int x1, int y,
                        const char *s, uint32_t color, float px)
{
    int w = ttf_str_width(s, px);
    int x = x0 + (x1 - x0 - w) / 2;
    if (x < x0) x = x0;
    ttf_draw(buf, stride, fw, fh, x, y, s, color, px);
}

static void ttf_right(uint32_t *buf, uint32_t stride, int fw, int fh,
                      int xr, int y, const char *s, uint32_t color, float px)
{
    ttf_draw(buf, stride, fw, fh, xr - ttf_str_width(s, px), y, s, color, px);
}

/* ── Primitives ─────────────────────────────────────────────────────────── */
static void buf_hline(uint32_t *buf, uint32_t stride, int x0, int x1, int y,
                      uint32_t c, int fw, int fh)
{
    if (y < 0 || y >= fh) return;
    for (int x = x0; x <= x1 && x < fw; x++)
        if (x >= 0) buf[(size_t)y * stride + x] = c;
}

static void buf_fill_rrect(uint32_t *buf, uint32_t stride, int fw, int fh,
                           int x, int y, int w, int h, int r, uint32_t c)
{
    if (r < 1) {
        for (int row = y; row < y+h && row < fh; row++) {
            if (row < 0) continue;
            uint32_t *line = buf + (size_t)row * stride;
            for (int col = x; col < x+w && col < fw; col++)
                if (col >= 0) line[col] = c;
        }
        return;
    }
    int r2 = r * r;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= fh) continue;
        int x0 = x, x1 = x + w - 1;
        if (row < r) {
            int dy = r - row, dx = (int)sqrtf((float)(r2 - dy*dy));
            x0 = x + (r - dx); x1 = x + w - 1 - (r - dx);
        } else if (row >= h - r) {
            int dy = r - (h-1-row), dx = (int)sqrtf((float)(r2 - dy*dy));
            x0 = x + (r - dx); x1 = x + w - 1 - (r - dx);
        }
        uint32_t *line = buf + (size_t)py * stride;
        for (int col = x0; col <= x1 && col < fw; col++)
            if (col >= 0) line[col] = c;
    }
}

static void buf_fill_circle(uint32_t *buf, uint32_t stride, int fw, int fh,
                             int cx, int cy, int r, uint32_t c)
{
    int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrtf((float)(r2 - dy*dy));
        int py = cy + dy;
        if (py < 0 || py >= fh) continue;
        int px0 = cx-dx < 0 ? 0 : cx-dx;
        int px1 = cx+dx >= fw ? fw-1 : cx+dx;
        uint32_t *row = buf + (size_t)py * stride;
        for (int px = px0; px <= px1; px++) row[px] = c;
    }
}

/* ── Vinyl record art ───────────────────────────────────────────────────── */
/* Renders a vinyl record texture into a circular region centred at (cx,cy).
 * When playing: a small white dot orbits the centre label — subtle spin cue. */
static void draw_vinyl(uint32_t *buf, uint32_t stride,
                       int cx, int cy, PlayerState state)
{
    const int outer_r  = VINYL_R;
    const int label_r  = VINYL_LBL_R;
    const int hub_r    = VINYL_HUB_R;
    const int gp       = 9;   /* groove pitch in pixels */
    /* Precompute squared thresholds — avoids sqrtf for hub and label pixels
     * (~6% of the disc area) and keeps the hot path branch-free. */
    const int hub_r2   = hub_r   * hub_r;
    const int label_r2 = label_r * label_r;

    for (int dy = -outer_r; dy <= outer_r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= H_FB) continue;
        int span = (int)sqrtf((float)(outer_r*outer_r - dy*dy));
        uint32_t *row = buf + (size_t)py * stride;
        for (int dx = -span; dx <= span; dx++) {
            int px = cx + dx;
            if (px < 0 || px >= W_FB) continue;
            int dist2 = dx*dx + dy*dy;
            uint32_t c;
            if (dist2 <= hub_r2) {
                c = C_VINYL_HUB;
            } else if (dist2 <= label_r2) {
                c = C_VINYL_LBL;
            } else {
                /* sqrtf only needed for groove-band index; unavoidable here. */
                int r = (int)sqrtf((float)dist2);
                c = ((r / gp) & 1) ? C_VINYL_B : C_VINYL_A;
            }
            row[px] = c;
        }
    }

}

/* ── Transport icons ────────────────────────────────────────────────────── */
static void draw_triangle_right(uint32_t *buf, uint32_t stride,
                                int cx, int cy, int sz, uint32_t c)
{
    for (int dy = -sz; dy <= sz; dy++) {
        int w = sz - abs(dy);
        for (int dx = 0; dx <= w; dx++) {
            int px = cx - sz/3 + dx, py = cy + dy;
            if (px >= 0 && px < W_FB && py >= 0 && py < H_FB)
                buf[(size_t)py * stride + px] = c;
        }
    }
}

static void draw_pause_bars(uint32_t *buf, uint32_t stride,
                            int cx, int cy, int sz, uint32_t c)
{
    int bw = sz/3, bh = sz*2, gap = sz/2;
    int x0 = cx - gap/2 - bw, x1 = cx + gap/2, y0 = cy - sz;
    for (int row = 0; row < bh; row++) {
        int y = y0 + row;
        if (y < 0 || y >= H_FB) continue;
        for (int col = 0; col < bw; col++) {
            int p;
            p = x0+col; if (p>=0 && p<W_FB) buf[(size_t)y*stride+p] = c;
            p = x1+col; if (p>=0 && p<W_FB) buf[(size_t)y*stride+p] = c;
        }
    }
}

/* |◄ — vertical bar + left-pointing triangle */
static void draw_icon_prev(uint32_t *buf, uint32_t stride,
                           int cx, int cy, int sz, uint32_t c)
{
    int bw = sz/4 + 1, bh = sz*2, bar_x = cx - sz/2 - bw, y0 = cy - sz;
    for (int row = 0; row < bh; row++) {
        int y = y0+row; if (y < 0 || y >= H_FB) continue;
        for (int col = 0; col < bw; col++) {
            int px = bar_x+col; if (px>=0 && px<W_FB) buf[(size_t)y*stride+px] = c;
        }
    }
    int tc = cx + bw/2;
    for (int dy = -sz; dy <= sz; dy++) {
        int w = sz - abs(dy), tx = tc - w;
        for (int dx = 0; dx <= w; dx++) {
            int px = tx+dx, py = cy+dy;
            if (px>=0 && px<W_FB && py>=0 && py<H_FB) buf[(size_t)py*stride+px] = c;
        }
    }
}

/* ►| — right-pointing triangle + vertical bar */
static void draw_icon_next(uint32_t *buf, uint32_t stride,
                           int cx, int cy, int sz, uint32_t c)
{
    int bw = sz/4 + 1, bh = sz*2, bar_x = cx + sz/2, y0 = cy - sz;
    for (int row = 0; row < bh; row++) {
        int y = y0+row; if (y < 0 || y >= H_FB) continue;
        for (int col = 0; col < bw; col++) {
            int px = bar_x+col; if (px>=0 && px<W_FB) buf[(size_t)y*stride+px] = c;
        }
    }
    int tc = cx - bw/2 - sz/3;
    for (int dy = -sz; dy <= sz; dy++) {
        int w = sz - abs(dy);
        for (int dx = 0; dx <= w; dx++) {
            int px = tc+dx, py = cy+dy;
            if (px>=0 && px<W_FB && py>=0 && py<H_FB) buf[(size_t)py*stride+px] = c;
        }
    }
}

/* ── String helpers ─────────────────────────────────────────────────────── */
static void trunc_str(const char *src, char *dst, size_t max_chars)
{
    size_t n = strlen(src);
    if (n <= max_chars) {
        memcpy(dst, src, n + 1);
    } else {
        memcpy(dst, src, max_chars - 2);
        dst[max_chars-2] = '.'; dst[max_chars-1] = '.'; dst[max_chars] = '\0';
    }
}

static void str_upper(const char *src, char *dst, size_t max)
{
    size_t i;
    for (i = 0; i+1 < max && src[i]; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? (char)(src[i]-32) : src[i];
    dst[i] = '\0';
}

static void format_time(char *buf, size_t n, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, n, "%d:%02d", (int)(s/60), (int)(s%60));
}

/* ── Library view ───────────────────────────────────────────────────────── */
static void render_library(uint32_t *buf, uint32_t stride, const UiState *st)
{
    /* Background */
    buf_fill_rrect(buf, stride, W_FB, H_FB, 0, 0, W_FB, H_FB, 0, C_BG);

    /* Status bar */
    {
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
        char clk[8]; snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
        int sy = STATUS_H - 10;
        ttf_draw(buf, stride, W_FB, H_FB, 28, sy, clk, C_TEXT_2, 16.0f);
        char bat[8]; snprintf(bat, sizeof(bat), "%u%%", st->battery_pct);
        ttf_right(buf, stride, W_FB, H_FB, W_FB-28, sy, bat, C_TEXT_2, 16.0f);
    }

    /* Header: wordmark + track count, "BACK" hint */
    {
        int hy = HDR_Y + HDR_H - 12;
        ttf_draw(buf, stride, W_FB, H_FB, 28, hy, "LIBRARY", C_TEXT, 30.0f);
        char cnt[24];
        if (st->library_loaded > 0)
            snprintf(cnt, sizeof(cnt), "%u / %u", st->library_loaded, st->track_count);
        else
            snprintf(cnt, sizeof(cnt), "LOADING");
        ttf_right(buf, stride, W_FB, H_FB, W_FB-28, hy, cnt, C_TEXT_3, 14.0f);
    }
    buf_hline(buf, stride, 0, W_FB-1, SEP_Y, C_SEP, W_FB, H_FB);

    /* Track rows — show all known tracks; use placeholders for unloaded metadata */
    int row_y0 = SEP_Y + 4;
    uint32_t loaded = st->library_loaded;
    uint32_t total  = st->track_count;
    int      scroll = st->lib_scroll;

    for (int i = 0; i < UI_LIB_VISIBLE; i++) {
        int idx = scroll + i;
        if (idx < 0 || (uint32_t)idx >= total || idx >= MAX_LIB_TRACKS) break;

        int ry = row_y0 + i * UI_LIB_ROW_H;

        /* Accent bar for currently playing track */
        if ((uint32_t)idx == st->cur_track_idx)
            buf_fill_rrect(buf, stride, W_FB, H_FB, 0, ry, 3, UI_LIB_ROW_H - 2, 0, C_PROG_FG);

        if ((uint32_t)idx < loaded) {
            const LibEntry *e = &st->library[idx];

            /* Title */
            char title[48]; trunc_str(e->title[0] ? e->title : "Unknown", title, 28);
            ttf_draw(buf, stride, W_FB, H_FB, 24, ry + 28, title, C_TEXT, 18.0f);

            /* Artist */
            char artist[40]; trunc_str(e->artist[0] ? e->artist : "Unknown Artist", artist, 24);
            ttf_draw(buf, stride, W_FB, H_FB, 24, ry + 54, artist, C_TEXT_2, 13.0f);

            /* Duration (right-aligned) */
            if (e->duration_ms > 0) {
                char dur[10]; format_time(dur, sizeof(dur), e->duration_ms);
                ttf_right(buf, stride, W_FB, H_FB, W_FB-20, ry + 28, dur, C_TEXT_3, 13.0f);
            }
        } else {
            /* Metadata not yet fetched — show a numbered placeholder */
            char ph[24]; snprintf(ph, sizeof(ph), "Track %d", idx + 1);
            ttf_draw(buf, stride, W_FB, H_FB, 24, ry + 28, ph, C_TEXT_3, 18.0f);
        }

        /* Row separator */
        buf_hline(buf, stride, 20, W_FB-20, ry + UI_LIB_ROW_H - 1, C_SEP, W_FB, H_FB);
    }

    /* Scrollbar (3 px right edge) */
    if (loaded > (uint32_t)UI_LIB_VISIBLE) {
        int bar_h = H_FB - SEP_Y;
        int thumb_h = bar_h * UI_LIB_VISIBLE / (int)loaded;
        if (thumb_h < 24) thumb_h = 24;
        int thumb_y = SEP_Y + (int)((int64_t)scroll * (bar_h - thumb_h) /
                                    ((int)loaded - UI_LIB_VISIBLE));
        buf_fill_rrect(buf, stride, W_FB, H_FB, W_FB-4, thumb_y, 3, thumb_h, 1, C_TEXT_3);
    }
}

/* ── render_frame ───────────────────────────────────────────────────────── */
void render_frame(FbCtx *fb, const UiState *st)
{
    font_init();

    /* All drawing targets a fixed 720×1280 virtual canvas.  At the end of
     * this function the canvas is scale-blitted to the actual framebuffer
     * (1080×2400 on Redmi Note 10S) so no layout constant needs to know
     * the physical display size. */
    static uint32_t vbuf[W_FB * H_FB];
    const uint32_t  stride = W_FB;
    uint32_t       *buf    = vbuf;
    /* wmemset writes 32-bit values — ~20× faster than a 921 600-iter loop on
     * AArch64 (compiler lowers it to a tight neon store-pair loop). Guard the
     * width assumption: if wchar_t is not 32-bit, fall back to the loop. */
    _Static_assert(sizeof(wchar_t) == sizeof(uint32_t),
                   "wmemset assumes 32-bit wchar_t");
    wmemset((wchar_t *)vbuf, (wchar_t)C_BG, W_FB * H_FB);

    if (st->lib_mode) {
        render_library(buf, stride, st);
    } else {

    /* ── Status bar ── */
    {
        time_t t = time(NULL);
        struct tm tm; localtime_r(&t, &tm);
        char clk[8]; snprintf(clk, sizeof(clk), "%02d:%02d", tm.tm_hour, tm.tm_min);
        int sy = STATUS_H - 10;
        ttf_draw(buf, stride, W_FB, H_FB, 28, sy, clk, C_TEXT_2, 16.0f);
        char bat[8]; snprintf(bat, sizeof(bat), "%u%%", st->battery_pct);
        ttf_right(buf, stride, W_FB, H_FB, W_FB-28, sy, bat, C_TEXT_2, 16.0f);
    }

    /* ── Header — "EGEPOD" wordmark + state label ── */
    {
        int hy = HDR_Y + HDR_H - 12;
        ttf_draw(buf, stride, W_FB, H_FB, 28, hy, "EGEPOD", C_TEXT, 30.0f);

        const char *state_str;
        uint32_t    sc;
        switch (st->state) {
        case PLAYER_PLAYING: state_str = "PLAYING"; sc = C_TEXT_2; break;
        case PLAYER_PAUSED:  state_str = "PAUSED";  sc = C_TEXT_3; break;
        case PLAYER_LOADING: state_str = "LOADING"; sc = C_TEXT_3; break;
        default:             state_str = "IDLE";    sc = C_TEXT_3; break;
        }
        ttf_right(buf, stride, W_FB, H_FB, W_FB-28, hy, state_str, sc, 14.0f);
    }

    /* ── Separator ── */
    buf_hline(buf, stride, 0, W_FB-1, SEP_Y, C_SEP, W_FB, H_FB);

    /* ── Album art card + vinyl ── */
    buf_fill_rrect(buf, stride, W_FB, H_FB, ART_X, ART_Y, ART_SZ, ART_SZ,
                   ART_CARD_R, C_CARD);
    draw_vinyl(buf, stride, ART_CX, ART_CY, st->state);

    /* ── Track title (ALL CAPS) ── */
    {
        const char *raw = st->track.title[0] ? st->track.title : "NO TRACK";
        char tmp[36], title[36];
        trunc_str(raw, tmp, 28);
        str_upper(tmp, title, sizeof(title));
        ttf_centred(buf, stride, W_FB, H_FB, 24, W_FB-24, TITLE_Y,
                    title, C_TEXT, 28.0f);
    }

    /* ── Artist / Album subtitle ── */
    {
        const char *a = st->track.artist[0] ? st->track.artist : "Unknown Artist";
        const char *l = st->track.album[0]  ? st->track.album  : "";
        char sub[72];
        if (l[0] && strcmp(a, l) != 0)
            snprintf(sub, sizeof(sub), "%s / %s", a, l);
        else
            snprintf(sub, sizeof(sub), "%s", a);
        char disp[48];
        trunc_str(sub, disp, 40);
        ttf_centred(buf, stride, W_FB, H_FB, 24, W_FB-24, ARTIST_Y,
                    disp, C_TEXT_2, 16.0f);
    }

    /* ── Progress bar + scrub dot ── */
    {
        buf_fill_rrect(buf, stride, W_FB, H_FB, PROG_X, PROG_Y,
                       PROG_W, PROG_H, PROG_H/2, C_PROG_BG);

        if (st->track.duration_ms > 0) {
            /* Interpolate position forward from last audiod update using
             * wall-clock time, so the bar moves smoothly between 5 Hz events. */
            uint32_t disp_ms = st->position_ms;
            if (st->state == PLAYER_PLAYING && st->pos_ref_mono_ms > 0) {
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                int64_t now_ms = (int64_t)ts.tv_sec * 1000
                               + ts.tv_nsec / 1000000;
                int64_t delta = now_ms - st->pos_ref_mono_ms;
                if (delta > 0 && delta < 1000) {   /* cap at 1 s of extrapolation */
                    disp_ms += (uint32_t)delta;
                    if (disp_ms > st->track.duration_ms)
                        disp_ms = st->track.duration_ms;
                }
            }

            /* Float fill width for sub-pixel antialiasing of the leading edge. */
            float filled_f = (float)disp_ms * (float)PROG_W
                             / (float)st->track.duration_ms;
            if (filled_f < 0.0f)         filled_f = 0.0f;
            if (filled_f > (float)PROG_W) filled_f = (float)PROG_W;
            int   filled_i = (int)filled_f;
            float frac     = filled_f - (float)filled_i;

            if (filled_i > 0)
                buf_fill_rrect(buf, stride, W_FB, H_FB, PROG_X, PROG_Y,
                               filled_i, PROG_H, PROG_H/2, C_PROG_FG);

            /* Antialias leading edge: blend one extra column proportional to frac
             * so the fill bar expands continuously rather than in 1-px jumps. */
            if (frac > 0.01f && PROG_X + filled_i < PROG_X + PROG_W) {
                uint8_t ea = (uint8_t)(frac * 255.0f);
                for (int ly = PROG_Y; ly < PROG_Y + PROG_H; ly++)
                    blend_pixel(buf, stride, PROG_X + filled_i, ly,
                                C_PROG_FG, ea, W_FB, H_FB);
            }

            buf_fill_circle(buf, stride, W_FB, H_FB,
                            PROG_X + filled_i, PROG_Y + PROG_H/2, 5, C_TEXT);
        }
    }

    /* ── Timestamps ── */
    {
        char elapsed[10], total[10];
        format_time(elapsed, sizeof(elapsed), st->position_ms);
        if (st->track.duration_ms > 0)
            format_time(total, sizeof(total), st->track.duration_ms);
        else
            snprintf(total, sizeof(total), "--:--");
        ttf_draw(buf, stride, W_FB, H_FB,
                 PROG_X, TIME_Y, elapsed, C_TEXT_3, 13.0f);
        ttf_right(buf, stride, W_FB, H_FB,
                  PROG_X + PROG_W, TIME_Y, total, C_TEXT_3, 13.0f);
    }

    /* ── Transport controls ── */
    {
        int cy = CTRL_CY;

        /* Shuffle — text label */
        ttf_centred(buf, stride, W_FB, H_FB,
                    BTN_SHUFFLE_X, BTN_SHUFFLE_X + BTN_SZ,
                    cy + 6, "SH", C_TEXT_3, 13.0f);

        /* Prev ◄| */
        draw_icon_prev(buf, stride, BTN_PREV_X + BTN_SZ/2, cy, 12, C_TEXT_2);

        /* Play / Pause — white disc + dark icon */
        {
            int pcx = BTN_PLAY_X + PLAY_SZ/2;
            buf_fill_circle(buf, stride, W_FB, H_FB, pcx, cy, PLAY_SZ/2, C_TEXT);
            if (st->state == PLAYER_PLAYING)
                draw_pause_bars(buf, stride, pcx, cy, 10, C_BG);
            else
                draw_triangle_right(buf, stride, pcx+2, cy, 12, C_BG);
        }

        /* Next |► */
        draw_icon_next(buf, stride, BTN_NEXT_X + BTN_SZ/2, cy, 12, C_TEXT_2);

        /* Repeat — text label */
        ttf_centred(buf, stride, W_FB, H_FB,
                    BTN_REPEAT_X, BTN_REPEAT_X + BTN_SZ,
                    cy + 6, "RP", C_TEXT_3, 13.0f);
    }

    /* ── Second separator ── */
    buf_hline(buf, stride, 0, W_FB-1, DIV2_Y, C_SEP, W_FB, H_FB);

    /* ── Library info ── */
    {
        char lib[40];
        if (st->track_count > 0)
            snprintf(lib, sizeof(lib), "%u TRACKS IN LIBRARY", st->track_count);
        else
            snprintf(lib, sizeof(lib), "SCANNING LIBRARY");
        ttf_centred(buf, stride, W_FB, H_FB, 0, W_FB, LIB_Y, lib, C_TEXT_3, 14.0f);

        /* Format / quality line */
        if (st->track.path[0]) {
            const char *fmt = "";
            switch (st->track.format) {
            case FMT_FLAC: fmt = "FLAC"; break;
            case FMT_WAV:  fmt = "WAV";  break;
            case FMT_MP3:  fmt = "MP3";  break;
            default: break;
            }
            char qual[48] = {0};
            if (fmt[0] && st->track.sample_rate > 0) {
                snprintf(qual, sizeof(qual), "%s  \xB7  %u kHz",
                         fmt, st->track.sample_rate / 1000);
                if (st->track.bits_per_sample > 0) {
                    char bps[16];
                    snprintf(bps, sizeof(bps), "  \xB7  %u-BIT",
                             st->track.bits_per_sample);
                    strncat(qual, bps, sizeof(qual)-strlen(qual)-1);
                }
            } else if (fmt[0]) {
                snprintf(qual, sizeof(qual), "%s", fmt);
            }
            if (qual[0])
                ttf_centred(buf, stride, W_FB, H_FB, 0, W_FB,
                            LIB_Y + 34, qual, C_TEXT_3, 12.0f);
        }
    }

    /* ── Brightness bar ── */
    {
        buf_hline(buf, stride, 0, W_FB-1, UI_BRIGHT_Y - 12, C_SEP, W_FB, H_FB);
        int bar_cy = UI_BRIGHT_Y + 25;  /* fixed visual center, independent of hit-zone H */
        /* Sun symbol */
        buf_fill_circle(buf, stride, W_FB, H_FB, UI_BRIGHT_X - 18, bar_cy, 5, C_TEXT_3);
        /* Track */
        buf_fill_rrect(buf, stride, W_FB, H_FB, UI_BRIGHT_X, bar_cy - 2,
                       UI_BRIGHT_W, 4, 2, C_PROG_BG);
        /* Fill */
        int bfill = (int)((uint64_t)st->brightness * (uint64_t)(UI_BRIGHT_W - 2) / 100);
        if (bfill > 0)
            buf_fill_rrect(buf, stride, W_FB, H_FB, UI_BRIGHT_X + 1, bar_cy - 2,
                           bfill, 4, 2, C_TEXT_3);
        /* Handle dot */
        int bdot_x = UI_BRIGHT_X +
                     (int)((uint64_t)st->brightness * (uint64_t)UI_BRIGHT_W / 100);
        buf_fill_circle(buf, stride, W_FB, H_FB, bdot_x, bar_cy, 5, C_TEXT_2);
    }
    } /* end else (player view) */

    /* Scale-blit: nearest-neighbour upscale from W_FB×H_FB to actual display.
     * On the Redmi Note 10S (1080×2400) this expands the 720×1280 canvas 1.5×
     * horizontally and 1.875× vertically, filling the full screen. */
    {
        uint32_t *dst     = fb_back(fb);
        uint32_t  dstride = fb->stride / sizeof(uint32_t);
        uint32_t  dw = fb->width, dh = fb->height;
        if (dw == W_FB && dh == H_FB) {
            /* Simulation (1:1) — memcpy is much faster than the scaling loop */
            memcpy(dst, vbuf, sizeof(vbuf));
        } else {
            for (uint32_t dy = 0; dy < dh; dy++) {
                uint32_t sy = dy * H_FB / dh;
                uint32_t *drow = dst + (size_t)dy * dstride;
                const uint32_t *srow = vbuf + (size_t)sy * W_FB;
                for (uint32_t dx = 0; dx < dw; dx++)
                    drow[dx] = srow[dx * W_FB / dw];
            }
        }
    }
}
