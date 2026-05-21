/* EgePod framebuffer viewer — macOS native SDL2, reads file-backed FB.
 *
 * The VM's egepod_uid mmaps <fb_path> (two ARGB8888 pages, portrait 720×1280).
 * Sidecar <fb_path>.page contains "<page_index> <flip_gen>\n".
 * Sidecar <fb_path>.tap  is written on mouse click for click-forwarding to uid.
 *
 * Generation-gated rendering: texture is only re-uploaded and msync'd when
 * uid commits a new frame (flip_gen changes).  Between renders the viewer
 * presents the cached texture at 60 FPS with zero VirtioFS traffic.
 *
 * Build on macOS:
 *   clang sim/fb_viewer.c $(sdl2-config --cflags --libs) -O2 -o out/sim/fb_viewer_macos
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

#define FB_W         720
#define FB_H         1280
#define SCALE        0.55f   /* window = 396 × 704 */
#define TARGET_FPS   60

/* Read page index and generation counter from the sidecar file.
 * Returns 1 if successfully parsed both fields, 0 otherwise. */
static int read_page_info(const char *page_file, int *page_out, uint32_t *gen_out)
{
    FILE *f = fopen(page_file, "r");
    if (!f) return 0;
    int p = 0; uint32_t g = 0;
    int ok = (fscanf(f, "%d %u", &p, &g) == 2);
    fclose(f);
    if (!ok) return 0;
    *page_out = (p == 1) ? 1 : 0;
    *gen_out  = g;
    return 1;
}

static void write_tap(const char *tap_path, int fb_x, int fb_y)
{
    static uint32_t seq = 0;
    struct { int32_t x, y; uint32_t seq; } ev = { fb_x, fb_y, ++seq };
    int fd = open(tap_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    ftruncate(fd, sizeof(ev));
    pwrite(fd, &ev, sizeof(ev), 0);
    close(fd);
}

int main(int argc, char **argv)
{
    const char *fb_path = (argc > 1) ? argv[1] : "/tmp/egepod_fb.raw";
    char page_path[512], tap_path[512];
    snprintf(page_path, sizeof(page_path), "%s.page", fb_path);
    snprintf(tap_path,  sizeof(tap_path),  "%s.tap",  fb_path);

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
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        FB_W, FB_H);

    SDL_SetWindowTitle(win, "EgePod  (Q = quit, click = tap)");
    printf("fb_viewer: window %dx%d (%.0f%% scale). Q = quit, click = tap.\n",
           win_w, win_h, SCALE * 100.0f);

    uint32_t ms_per_frame = 1000 / TARGET_FPS;
    uint32_t last_gen     = UINT32_MAX;   /* force first upload */
    int      last_page    = -1;
    int      running      = 1;

    while (running) {
        uint32_t t0 = SDL_GetTicks();

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_KEYDOWN &&
               (e.key.keysym.sym == SDLK_q || e.key.keysym.sym == SDLK_ESCAPE))
                running = 0;

            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                int fb_x = (int)(e.button.x / SCALE);
                int fb_y = (int)(e.button.y / SCALE);
                if (fb_x < 0) fb_x = 0;
                if (fb_y < 0) fb_y = 0;
                if (fb_x >= FB_W) fb_x = FB_W - 1;
                if (fb_y >= FB_H) fb_y = FB_H - 1;
                write_tap(tap_path, fb_x, fb_y);
                printf("fb_viewer: tap at FB (%d, %d)\n", fb_x, fb_y);
            }
        }

        int      page = last_page < 0 ? 0 : last_page;
        uint32_t gen  = last_gen;
        read_page_info(page_path, &page, &gen);

        /* Only re-fetch + re-upload when uid has committed a new frame.
         * This limits VirtioFS msync traffic to the actual render rate (~5 Hz)
         * instead of the display rate (60 Hz). */
        if (gen != last_gen) {
            /* Invalidate only the active front page — the back page is being
             * drawn to and its intermediate state is irrelevant to display. */
            msync((void *)(fb + (size_t)page * page_bytes), page_bytes, MS_INVALIDATE);

            const uint8_t *src = fb + (size_t)page * page_bytes;
            void *pixels; int pitch;
            SDL_LockTexture(tex, NULL, &pixels, &pitch);
            for (int row = 0; row < FB_H; row++)
                memcpy((uint8_t *)pixels + row * pitch,
                       src + row * FB_W * 4, FB_W * 4);
            SDL_UnlockTexture(tex);

            last_gen  = gen;
            last_page = page;
        }

        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);

        uint32_t elapsed = SDL_GetTicks() - t0;
        if (elapsed < ms_per_frame) SDL_Delay(ms_per_frame - elapsed);
    }

    munmap(fb, total);
    close(fd);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
