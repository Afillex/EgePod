/* 🖥️ Kiosk UI Engineer — input event handler.
 *
 * Opens every /dev/input/event* device and registers it on the caller's
 * epoll fd.  Uses hardware interrupts exclusively — no polling loops.
 *
 * Touch gesture detection:
 *   - ABS_MT_TRACKING_ID -1 = finger lifted; classify as tap or swipe
 *   - Swipe threshold: 80 px minimum displacement */

#include "input.h"
#include "../common/log.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <linux/input.h>
#ifdef SIMULATE
#include <sys/timerfd.h>
#include <stdint.h>
#endif

#define MAX_INPUT_DEVS  16
#define SWIPE_THRESHOLD 80

typedef struct {
    int      fd;
    char     path[64];
    /* Touch state for gesture detection */
    int      touch_down;
    int      start_x, start_y;
    int      cur_x,   cur_y;
} InputDev;

struct InputCtx {
    int       epoll_fd;
    InputDev  devs[MAX_INPUT_DEVS];
    int       n_devs;
#ifdef SIMULATE
    int       sim_tap_tfd;          /* timerfd polling the tap sidecar file */
    char      sim_tap_path[512];    /* path to the .tap sidecar file */
    uint32_t  sim_last_seq;         /* last processed tap sequence number */
#endif
};

static int open_input_dev(InputCtx *ctx, const char *path, int epoll_fd)
{
    if (ctx->n_devs >= MAX_INPUT_DEVS) return -1;

    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    struct epoll_event ev = {
        .events  = EPOLLIN,
        .data.fd = fd,
    };
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        close(fd); return -1;
    }

    InputDev *d = &ctx->devs[ctx->n_devs++];
    d->fd = fd;
    snprintf(d->path, sizeof(d->path), "%s", path);
    LOGD("input: opened %s", path);
    return 0;
}

InputCtx *input_open(int epoll_fd)
{
    InputCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->epoll_fd = epoll_fd;

#ifdef SIMULATE
    ctx->sim_tap_tfd = -1;
    /* Derive .tap path from EGEPOD_FB_FILE */
    const char *fb = getenv("EGEPOD_FB_FILE");
    if (!fb) fb = "/tmp/egepod_fb.raw";
    snprintf(ctx->sim_tap_path, sizeof(ctx->sim_tap_path), "%s.tap", fb);

    /* Create a timerfd that fires at 60 Hz to poll the tap file */
    ctx->sim_tap_tfd = timerfd_create(CLOCK_MONOTONIC,
                                      TFD_NONBLOCK | TFD_CLOEXEC);
    if (ctx->sim_tap_tfd >= 0) {
        struct itimerspec ts = {
            .it_interval = {0, 16666667},   /* ~60 Hz */
            .it_value    = {0, 16666667},
        };
        timerfd_settime(ctx->sim_tap_tfd, 0, &ts, NULL);
        struct epoll_event ev = {
            .events  = EPOLLIN,
            .data.fd = ctx->sim_tap_tfd,
        };
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ctx->sim_tap_tfd, &ev);

        /* Sync seq to existing tap file so stale taps from a previous run
         * are not re-processed on startup. */
        {
            int tapfd = open(ctx->sim_tap_path, O_RDONLY | O_CLOEXEC);
            if (tapfd >= 0) {
                struct { int32_t x, y; uint32_t seq; } ev2;
                if (pread(tapfd, &ev2, sizeof(ev2), 0) == (ssize_t)sizeof(ev2))
                    ctx->sim_last_seq = ev2.seq;
                close(tapfd);
            }
        }

        LOGI("input: sim tap polling active (%s)", ctx->sim_tap_path);
    }
#endif

    DIR *dp = opendir("/dev/input");
    if (!dp) {
        LOGW("input: no /dev/input — hardware input unavailable in simulation");
        LOGI("input: %d device(s) registered", ctx->n_devs);
        return ctx;
    }
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
        open_input_dev(ctx, path, epoll_fd);
    }
    closedir(dp);

    LOGI("input: %d device(s) registered", ctx->n_devs);
    return ctx;
}

void input_close(InputCtx *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < ctx->n_devs; i++) {
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ctx->devs[i].fd, NULL);
        close(ctx->devs[i].fd);
    }
#ifdef SIMULATE
    if (ctx->sim_tap_tfd >= 0) {
        epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ctx->sim_tap_tfd, NULL);
        close(ctx->sim_tap_tfd);
    }
#endif
    free(ctx);
}

InputEvt input_process_fd(InputCtx *ctx, int ready_fd)
{
    InputEvt result = { .type = INPUT_NONE };

#ifdef SIMULATE
    /* Simulation: tap file polling via timerfd */
    if (ready_fd == ctx->sim_tap_tfd && ctx->sim_tap_tfd >= 0) {
        uint64_t exp; read(ctx->sim_tap_tfd, &exp, sizeof(exp));  /* drain */
        struct { int32_t x, y; uint32_t seq; } ev;
        int tapfd = open(ctx->sim_tap_path, O_RDONLY | O_CLOEXEC);
        if (tapfd >= 0) {
            if (pread(tapfd, &ev, sizeof(ev), 0) == (ssize_t)sizeof(ev) &&
                ev.seq != ctx->sim_last_seq) {
                ctx->sim_last_seq = ev.seq;
                result.type = INPUT_TAP;
                result.x    = ev.x;
                result.y    = ev.y;
            }
            close(tapfd);
        }
        return result;
    }
#endif

    /* Find the device */
    InputDev *dev = NULL;
    for (int i = 0; i < ctx->n_devs; i++) {
        if (ctx->devs[i].fd == ready_fd) { dev = &ctx->devs[i]; break; }
    }
    if (!dev) return result;

    /* Drain all available events in one pass */
    struct input_event ev[32];
    ssize_t rd;
    while ((rd = read(ready_fd, ev, sizeof(ev))) > 0) {
        int n = (int)(rd / sizeof(struct input_event));
        for (int i = 0; i < n; i++) {
            /* KEY events */
            if (ev[i].type == EV_KEY && ev[i].value == 1 /* pressed */) {
                switch (ev[i].code) {
                case KEY_POWER:      result.type = INPUT_POWER_BUTTON; break;
                case KEY_VOLUMEUP:   result.type = INPUT_VOLUME_UP;    break;
                case KEY_VOLUMEDOWN: result.type = INPUT_VOLUME_DOWN;  break;
                default: break;
                }
            }
            /* Multi-touch */
            if (ev[i].type == EV_ABS) {
                switch (ev[i].code) {
                case ABS_MT_TRACKING_ID:
                    if (ev[i].value == -1 && dev->touch_down) {
                        /* Finger lifted — classify gesture */
                        int dx = dev->cur_x - dev->start_x;
                        int dy = dev->cur_y - dev->start_y;
                        int adx = dx < 0 ? -dx : dx;
                        int ady = dy < 0 ? -dy : dy;
                        if (adx < SWIPE_THRESHOLD && ady < SWIPE_THRESHOLD) {
                            result.type = INPUT_TAP;
                            result.x    = dev->cur_x;
                            result.y    = dev->cur_y;
                        } else if (adx > ady) {
                            result.type = dx > 0 ? INPUT_SWIPE_RIGHT : INPUT_SWIPE_LEFT;
                        } else {
                            result.type = dy > 0 ? INPUT_SWIPE_DOWN : INPUT_SWIPE_UP;
                        }
                        dev->touch_down = 0;
                    } else if (ev[i].value >= 0) {
                        dev->touch_down = 1;
                        dev->start_x = dev->cur_x;
                        dev->start_y = dev->cur_y;
                    }
                    break;
                case ABS_MT_POSITION_X: dev->cur_x = ev[i].value; break;
                case ABS_MT_POSITION_Y: dev->cur_y = ev[i].value; break;
                default: break;
                }
            }
        }
    }
    return result;
}
