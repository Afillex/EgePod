/* EgePod framebuffer viewer — macOS native SDL2, reads file-backed FB.
 *
 * The VM's egepod_uid mmaps /tmp/egepod_fb.raw (two RGBX8888 pages).
 * A sidecar file /tmp/egepod_fb.raw.page contains the active page index (0/1).
 * This viewer polls both files at 60 fps and blits the active page to a window.
 *
 * Build on macOS:
 *   clang sim/fb_viewer.c $(sdl2-config --cflags --libs) -O2 -o out/sim/fb_viewer_macos
 *
 * Run:
 *   ./out/sim/fb_viewer_macos [/path/to/egepod_fb.raw]
 */

#if __has_include(<SDL2/SDL.h>)
#  include <SDL2/SDL.h>
#else
#  include <SDL.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define FB_W         1080
#define FB_H         2400
#define SCALE        0.38f
#define TARGET_FPS   60

static int read_page_index(const char *page_file)
{
    FILE *f = fopen(page_file, "r");
    if (!f) return 0;
    int p = 0; fscanf(f, "%d", &p); fclose(f);
    return (p == 1) ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *fb_path = (argc > 1) ? argv[1] : "/tmp/egepod_fb.raw";
    char page_path[512];
    snprintf(page_path, sizeof(page_path), "%s.page", fb_path);

    size_t page_bytes = (size_t)FB_W * FB_H * 4;
    size_t total      = page_bytes * 2;

    printf("fb_viewer: waiting for %s ...\n", fb_path);
    for (int i = 0; i < 60; i++) {
        struct stat st;
        if (stat(fb_path, &st) == 0 && (size_t)st.st_size == total) break;
        SDL_Delay(500);
        if (i == 59) {
            fprintf(stderr, "fb_viewer: timed out waiting for %s\n", fb_path);
            return 1;
        }
    }

    int fd = open(fb_path, O_RDONLY);
    if (fd < 0) { perror("fb_viewer: open"); return 1; }

    uint8_t *fb = mmap(NULL, total, PROT_READ, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("fb_viewer: mmap"); return 1; }

    printf("fb_viewer: mapped %s (%.1f MB)\n", fb_path, total / 1048576.0);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    int win_w = (int)(FB_W * SCALE);
    int win_h = (int)(FB_H * SCALE);

    SDL_Window   *win = SDL_CreateWindow("EgePod",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture  *tex = SDL_CreateTexture(ren,
        SDL_PIXELFORMAT_ARGB8888,   /* matches 0xAARRGGBB uint32 on little-endian */
        SDL_TEXTUREACCESS_STREAMING,
        FB_W, FB_H);

    SDL_SetWindowTitle(win, "EgePod  (Q = quit)");
    printf("fb_viewer: window %dx%d (%.0f%% scale). Q = quit.\n",
           win_w, win_h, SCALE * 100.0f);

    /* Row-copy buffer for vertical flip */
    uint8_t *row_buf = malloc(FB_W * 4);
    if (!row_buf) { fprintf(stderr, "fb_viewer: OOM\n"); return 1; }

    uint32_t ms_per_frame = 1000 / TARGET_FPS;
    int last_page = -1;
    int running = 1;

    while (running) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN &&
               (e.key.keysym.sym == SDLK_q || e.key.keysym.sym == SDLK_ESCAPE))
                running = 0;
        }

        int page = read_page_index(page_path);

        /* Only re-upload texture when the page flips (avoids tearing mid-render) */
        if (page != last_page) {
            const uint8_t *src = fb + (size_t)page * page_bytes;
            void *pixels; int pitch;
            SDL_LockTexture(tex, NULL, &pixels, &pitch);

            for (int row = 0; row < FB_H; row++)
                memcpy((uint8_t *)pixels + row * pitch,
                       src + row * FB_W * 4, FB_W * 4);

            SDL_UnlockTexture(tex);
            last_page = page;
        }

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        uint32_t elapsed = SDL_GetTicks() - t0;
        if (elapsed < ms_per_frame) SDL_Delay(ms_per_frame - elapsed);
    }

    free(row_buf);
    munmap(fb, total);
    close(fd);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
