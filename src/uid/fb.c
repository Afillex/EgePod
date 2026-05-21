/* 🖥️ Kiosk UI Engineer — framebuffer driver.
 *
 * Double-buffering via FBIOPAN_DISPLAY:
 *   Page 0 maps to yoffset = 0
 *   Page 1 maps to yoffset = height
 * We mmap the entire 2× virtual resolution at open time. */

#include "fb.h"
#include "../common/log.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define FB_DEV "/dev/fb0"

/* ── simulation: file-backed framebuffer ────────────────────────────────── */
/* When /dev/fb0 is unavailable (OrbStack custom kernel has no vfb module),
 * we mmap a plain file.  EGEPOD_FB_FILE controls the path; default below.
 * fb_viewer.c on macOS reads the same file for display. */
#ifdef SIMULATE
#define SIM_FB_DEFAULT "/tmp/egepod_fb.raw"
#define SIM_FB_W 720
#define SIM_FB_H 1280

static int fb_open_sim(FbCtx *ctx)
{
    const char *path = getenv("EGEPOD_FB_FILE");
    if (!path) path = SIM_FB_DEFAULT;

    ctx->width     = SIM_FB_W;
    ctx->height    = SIM_FB_H;
    ctx->bpp       = 32;
    ctx->stride    = SIM_FB_W * 4;
    ctx->page_size = (size_t)ctx->stride * ctx->height;
    size_t total   = ctx->page_size * 2;

    /* Create / truncate the backing file */
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { LOGE("fb_sim: open(%s): %s", path, strerror(errno)); return -1; }
    if (ftruncate(fd, (off_t)total) < 0) {
        LOGE("fb_sim: ftruncate: %s", strerror(errno)); close(fd); return -1;
    }

    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        LOGE("fb_sim: mmap: %s", strerror(errno)); close(fd); return -1;
    }

    /* Zero-init so no stale pixels from a previous session bleed through
     * before the first render completes. */
    memset(map, 0, total);
    msync(map, total, MS_SYNC);

    ctx->fd     = fd;
    ctx->buf[0] = map;
    ctx->buf[1] = (uint8_t *)map + ctx->page_size;
    ctx->active_buf = 0;

    /* Write current page index to sidecar file so fb_viewer knows which page */
    char side[256]; snprintf(side, sizeof(side), "%s.page", path);
    FILE *f = fopen(side, "w"); if (f) { fputs("0\n", f); fclose(f); }

    LOGI("fb_sim: file-backed FB at %s (%ux%u RGBX8888, %.1f MB)",
         path, ctx->width, ctx->height, total / 1048576.0);
    return 0;
}
#endif /* SIMULATE */

int fb_open(FbCtx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));

#ifdef SIMULATE
    /* Try real fb0 first; fall back to file-backed simulation */
    ctx->fd = open(FB_DEV, O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) {
        LOGW("fb: %s unavailable (%s) — using file-backed simulation", FB_DEV, strerror(errno));
        return fb_open_sim(ctx);
    }
#else
    ctx->fd = open(FB_DEV, O_RDWR | O_CLOEXEC);
    if (ctx->fd < 0) {
        LOGE("fb: open(%s): %s", FB_DEV, strerror(errno));
        return -1;
    }
#endif

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(ctx->fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(ctx->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        LOGE("fb: ioctl FBIOGET_*SCREENINFO: %s", strerror(errno));
        close(ctx->fd); return -1;
    }

    ctx->width  = vinfo.xres;
    ctx->height = vinfo.yres;
    ctx->bpp    = vinfo.bits_per_pixel;
    ctx->stride = finfo.line_length;

    /* Request double-buffer height */
    vinfo.yres_virtual = vinfo.yres * 2;
    ioctl(ctx->fd, FBIOPUT_VSCREENINFO, &vinfo);

    ctx->page_size = ctx->stride * ctx->height;
    size_t total   = ctx->page_size * 2;

    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, 0);
    if (map == MAP_FAILED) {
        LOGE("fb: mmap: %s", strerror(errno));
        close(ctx->fd); return -1;
    }

    ctx->buf[0] = map;
    ctx->buf[1] = (uint8_t *)map + ctx->page_size;
    ctx->active_buf = 0;

    LOGI("fb: %ux%u %u bpp, stride=%u, double-buffered",
         ctx->width, ctx->height, ctx->bpp, ctx->stride);
    return 0;
}

void fb_close(FbCtx *ctx)
{
    if (ctx->buf[0])
        munmap(ctx->buf[0], ctx->page_size * 2);
    if (ctx->fd >= 0)
        close(ctx->fd);
    memset(ctx, 0, sizeof(*ctx));
}

uint32_t *fb_back(FbCtx *ctx)
{
    return (uint32_t *)ctx->buf[1 - ctx->active_buf];
}

void fb_flip(FbCtx *ctx)
{
    int back = 1 - ctx->active_buf;

#ifdef SIMULATE
    /* Flush the back-buffer pages to the host filesystem before signalling
     * the viewer — VirtioFS mmap writes stay in the VM page cache otherwise. */
    msync(ctx->buf[back], ctx->page_size, MS_SYNC);

    /* File-backed sim: write the page index so fb_viewer picks the right page */
    const char *path = getenv("EGEPOD_FB_FILE");
    if (!path) path = SIM_FB_DEFAULT;
    ctx->flip_gen++;
    char side[256]; snprintf(side, sizeof(side), "%s.page", path);
    FILE *f = fopen(side, "w");
    if (f) { fprintf(f, "%d %u\n", back, ctx->flip_gen); fclose(f); }
#else
    struct fb_var_screeninfo vinfo;
    if (ioctl(ctx->fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        vinfo.yoffset = (uint32_t)(back * ctx->height);
        ioctl(ctx->fd, FBIOPAN_DISPLAY, &vinfo);
    }
#endif
    ctx->active_buf = back;
}

void fb_set_brightness(int level)
{
    FILE *f = fopen("/sys/class/leds/lcd-backlight/brightness", "w");
    if (!f) return;
    fprintf(f, "%d\n", level);
    fclose(f);
}
