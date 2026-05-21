#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int      fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;      /* bytes per row */
    uint32_t bpp;         /* bits per pixel */
    void    *buf[2];      /* two framebuffer pages for double-buffering */
    int      active_buf;  /* index of the currently displayed page */
    size_t   page_size;   /* bytes per page */
    uint32_t flip_gen;    /* incremented on every fb_flip; viewer polls this */
} FbCtx;

/* Open /dev/fb0, map both framebuffer pages.
 * Returns 0 on success. */
int  fb_open(FbCtx *ctx);
void fb_close(FbCtx *ctx);

/* Get a pointer to the back buffer for drawing. */
uint32_t *fb_back(FbCtx *ctx);

/* Flip the back buffer to the display. */
void fb_flip(FbCtx *ctx);

/* Set/clear display brightness (0 = off, 255 = max). */
void fb_set_brightness(int level);
