/* egepod_uid — kiosk UI daemon
 *
 * 🖥️ Kiosk UI Engineer
 *
 * The ENTIRE main thread is a single epoll_wait(-1) call.  There are no
 * polling loops and no sleep() calls anywhere in this daemon.
 *
 * Registered epoll fds:
 *   - audiod IPC socket  (EVT_TRACK, EVT_POSITION, EVT_STATE)
 *   - pwrd IPC socket    (EVT_BATTERY, EVT_THERMAL)
 *   - all /dev/input/event* devices  (touch, power, volume keys)
 *
 * Screen-off contract:
 *   - g_screen_on = 0 gates all request_redraw() calls
 *   - The render thread stays blocked at sem_wait with zero CPU overhead
 *   - The main thread continues blocking in epoll for the power button
 *   - On screen-on: g_screen_on = 1, request_redraw() posts the semaphore */

#include "fb.h"
#include "input.h"
#include "render.h"
#include "../common/ipc.h"
#include "../common/log.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

/* ── forward declarations from ipc_client.c ─────────────────────────────── */
int ipc_connect(const char *path);
int ipc_send_cmd(int fd, IpcMsgType type, uint32_t param);
int ipc_recv(int fd, IpcMsg *out);

/* ── globals ─────────────────────────────────────────────────────────────── */

static volatile int   g_quit          = 0;
static int            g_screen_on     = 1;
static int            g_progress_tfd  = -1;  /* 20 Hz timerfd for progress bar redraws */

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void progress_timer_arm(void)
{
    if (g_progress_tfd < 0) return;
    struct itimerspec its = {
        .it_value    = { .tv_nsec = 50000000 },   /* 50 ms first fire */
        .it_interval = { .tv_nsec = 50000000 },   /* 20 Hz */
    };
    timerfd_settime(g_progress_tfd, 0, &its, NULL);
}

static void progress_timer_disarm(void)
{
    if (g_progress_tfd < 0) return;
    struct itimerspec its = { {0,0}, {0,0} };
    timerfd_settime(g_progress_tfd, 0, &its, NULL);
}
static int            g_lib_fetch_idx = -1;  /* next track idx to fetch; -1 = done */
static pthread_t      g_render_tid;
static sem_t          g_render_sem;     /* posted when a new frame is needed */
static atomic_flag    g_redraw_pending = ATOMIC_FLAG_INIT;
static FbCtx          g_fb;
static UiState        g_ui;            /* written by main thread, read by render */
static pthread_mutex_t g_ui_lock       = PTHREAD_MUTEX_INITIALIZER;

static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── render thread ───────────────────────────────────────────────────────── */
/* This thread has ONLY ONE job: wait on g_render_sem, draw, flip.
 * It can be SIGSTOP'd safely — the main thread never holds g_ui_lock
 * across the sem_post, so there's no lock inversion risk. */
static void *render_thread(void *arg)
{
    (void)arg;
    while (!g_quit) {
        sem_wait(&g_render_sem);
        if (g_quit) break;

        /* Clear the coalescing flag BEFORE snapshotting state so any new
         * request_redraw() arriving during this frame queues a follow-up. */
        atomic_flag_clear(&g_redraw_pending);

        pthread_mutex_lock(&g_ui_lock);
        UiState snap = g_ui;
        pthread_mutex_unlock(&g_ui_lock);

        render_frame(&g_fb, &snap);
        fb_flip(&g_fb);
    }
    return NULL;
}

static void request_redraw(void)
{
    /* atomic test-and-set: only the first caller to flip 0→1 posts the
     * semaphore. Subsequent callers (e.g., position event + timer firing
     * in the same epoll wakeup) are coalesced into a single frame. */
    if (!atomic_flag_test_and_set(&g_redraw_pending))
        sem_post(&g_render_sem);
}

static void screen_off(void)
{
    if (!g_screen_on) return;
    g_screen_on = 0;
    fb_set_brightness(0);
    /* Stop 20 Hz redraw wakeups so the SoC can stay in deep sleep. */
    progress_timer_disarm();
    /* render thread stays blocked at sem_wait — no SIGSTOP needed */
    LOGI("uid: screen off");
}

static void screen_on(void)
{
    if (g_screen_on) return;
    g_screen_on = 1;
    fb_set_brightness(200);
    /* Re-arm progress redraws only if audio is currently playing. */
    pthread_mutex_lock(&g_ui_lock);
    PlayerState s = g_ui.state;
    pthread_mutex_unlock(&g_ui_lock);
    if (s == PLAYER_PLAYING) progress_timer_arm();
    request_redraw();
    LOGI("uid: screen on");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_OPEN("egepod_uid");
    LOGI("uid: starting");

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (fb_open(&g_fb) != 0) {
        LOGE("uid: cannot open framebuffer");
        return 1;
    }

    sem_init(&g_render_sem, 0, 0);
    pthread_create(&g_render_tid, NULL, render_thread, NULL);

    /* Connect to daemons */
    int audiod_fd = ipc_connect(AUDIOD_SOCK_PATH);
    int pwrd_fd   = ipc_connect(PWRD_SOCK_PATH);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;

    if (audiod_fd >= 0) {
        ev.events = EPOLLIN | EPOLLHUP;
        ev.data.fd = audiod_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, audiod_fd, &ev);
    }
    if (pwrd_fd >= 0) {
        ev.events = EPOLLIN | EPOLLHUP;
        ev.data.fd = pwrd_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, pwrd_fd, &ev);
    }

    /* 20 Hz progress-bar redraw timer — armed only while PLAYING */
    g_progress_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_progress_tfd >= 0) {
        ev.events = EPOLLIN; ev.data.fd = g_progress_tfd;
        epoll_ctl(ep, EPOLL_CTL_ADD, g_progress_tfd, &ev);
    }

    InputCtx *input = input_open(ep);

    /* Initial draw */
    g_ui.battery_pct = 100;
    g_ui.brightness  = 80;
    fb_set_brightness(204);  /* 80 × 255 / 100 */
    request_redraw();

    struct epoll_event events[16];
    LOGI("uid: event loop started");

    while (!g_quit) {
        int n = epoll_wait(ep, events, 16, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            /* ── audiod events ── */
            if (fd == audiod_fd) {
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    LOGE("uid: audiod connection lost — attempting reconnect");
                    epoll_ctl(ep, EPOLL_CTL_DEL, audiod_fd, NULL);
                    close(audiod_fd);
                    audiod_fd = ipc_connect(AUDIOD_SOCK_PATH);
                    if (audiod_fd >= 0) {
                        ev.events = EPOLLIN | EPOLLHUP;
                        ev.data.fd = audiod_fd;
                        epoll_ctl(ep, EPOLL_CTL_ADD, audiod_fd, &ev);
                        LOGI("uid: audiod reconnected");
                    } else {
                        LOGE("uid: audiod reconnect failed — UI will be static");
                    }
                    break;
                }
                IpcMsg msg;
                if (ipc_recv(audiod_fd, &msg) == 0) {
                    pthread_mutex_lock(&g_ui_lock);
                    switch ((IpcMsgType)msg.type) {
                    case EVT_INDEX_READY:
                        g_ui.track_count    = msg.param.track_count;
                        g_ui.library_loaded = 0;
                        g_lib_fetch_idx     = 0;
                        /* Auto-play first track on startup */
                        if (audiod_fd >= 0 && g_ui.state == PLAYER_IDLE) {
                            ipc_send_cmd(audiod_fd, CMD_LOAD_TRACK, 0);
                            ipc_send_cmd(audiod_fd, CMD_PLAY, 0);
                        }
                        /* Kick off background library metadata fetch */
                        if (msg.param.track_count > 0 && audiod_fd >= 0)
                            ipc_send_cmd(audiod_fd, CMD_GET_TRACK_INFO, 0);
                        break;
                    case EVT_TRACK_INFO: {
                        uint32_t tidx = msg.seq;
                        if (tidx < MAX_LIB_TRACKS) {
                            LibEntry *e = &g_ui.library[tidx];
                            snprintf(e->title,  sizeof(e->title),
                                     "%s", msg.param.track.title);
                            snprintf(e->artist, sizeof(e->artist),
                                     "%s", msg.param.track.artist);
                            e->duration_ms = msg.param.track.duration_ms;
                            e->track_idx   = tidx;
                            if (tidx + 1 > g_ui.library_loaded)
                                g_ui.library_loaded = tidx + 1;
                        }
                        /* Chain: fetch next track's metadata */
                        if (g_lib_fetch_idx >= 0 && audiod_fd >= 0) {
                            g_lib_fetch_idx++;
                            if ((uint32_t)g_lib_fetch_idx < g_ui.track_count
                                    && g_lib_fetch_idx < MAX_LIB_TRACKS)
                                ipc_send_cmd(audiod_fd, CMD_GET_TRACK_INFO,
                                             (uint32_t)g_lib_fetch_idx);
                            else
                                g_lib_fetch_idx = -1;
                        }
                        break;
                    }
                    case EVT_TRACK:
                        g_ui.track           = msg.param.track;
                        g_ui.position_ms     = 0;
                        /* Reset anchor — otherwise interpolation would
                         * extrapolate from the previous track's last update. */
                        g_ui.pos_ref_mono_ms = mono_ms();
                        g_ui.cur_track_idx   = msg.seq;
                        break;
                    case EVT_STATE:
                        g_ui.state = msg.param.player_state;
                        if (msg.param.player_state == PLAYER_PLAYING) {
                            /* Re-anchor on transition into PLAYING so the bar
                             * doesn't jump forward using a stale delta from
                             * before the pause. audiod's next EVT_POSITION
                             * (≤200 ms away) refines this. */
                            g_ui.pos_ref_mono_ms = mono_ms();
                            progress_timer_arm();
                        } else {
                            progress_timer_disarm();
                        }
                        break;
                    case EVT_POSITION:
                        g_ui.position_ms      = msg.param.position_ms;
                        g_ui.pos_ref_mono_ms  = mono_ms();
                        break;
                    default: break;
                    }
                    pthread_mutex_unlock(&g_ui_lock);
                    if (g_screen_on) request_redraw();
                }
                continue;
            }

            /* ── pwrd events ── */
            if (fd == pwrd_fd) {
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    LOGE("uid: pwrd connection lost — exiting");
                    g_quit = 1;
                    break;
                }
                IpcMsg msg;
                if (ipc_recv(pwrd_fd, &msg) == 0) {
                    pthread_mutex_lock(&g_ui_lock);
                    if (msg.type == EVT_BATTERY)
                        g_ui.battery_pct = msg.param.battery_pct;
                    pthread_mutex_unlock(&g_ui_lock);
                    if (g_screen_on) request_redraw();
                }
                continue;
            }

            /* ── progress bar redraw timer ── */
            if (fd == g_progress_tfd) {
                uint64_t expirations;
                (void)read(g_progress_tfd, &expirations, sizeof(expirations));
                if (g_screen_on) request_redraw();
                continue;
            }

            /* ── input events ── */
            if (input) {
                InputEvt ie = input_process_fd(input, fd);
                switch (ie.type) {
                case INPUT_POWER_BUTTON:
                    if (g_screen_on) screen_off();
                    else             screen_on();
                    break;
                case INPUT_TAP: {
                    /* Scale from physical device pixels to 720×1280 design space */
                    int tap_x = (g_fb.width  > 0)
                                ? (int)((int64_t)ie.x * UI_DESIGN_W / (int)g_fb.width)
                                : ie.x;
                    int tap_y = (g_fb.height > 0)
                                ? (int)((int64_t)ie.y * UI_DESIGN_H / (int)g_fb.height)
                                : ie.y;
                    if (!g_screen_on) { screen_on(); break; }
                    if (g_ui.lib_mode) {
                        /* Library view: y < back threshold → exit; else select row */
                        if (tap_y < UI_LIB_BACK_H) {
                            pthread_mutex_lock(&g_ui_lock);
                            g_ui.lib_mode = 0;
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                        } else {
                            pthread_mutex_lock(&g_ui_lock);
                            int scroll     = g_ui.lib_scroll;
                            uint32_t total = g_ui.track_count;
                            pthread_mutex_unlock(&g_ui_lock);
                            /* rows start at y = UI_LIB_BACK_H + 4 */
                            int row  = (tap_y - (UI_LIB_BACK_H + 4)) / UI_LIB_ROW_H;
                            int tidx = scroll + row;
                            if (row >= 0 && (uint32_t)tidx < total) {
                                ipc_send_cmd(audiod_fd, CMD_LOAD_TRACK, (uint32_t)tidx);
                                ipc_send_cmd(audiod_fd, CMD_PLAY, 0);
                                pthread_mutex_lock(&g_ui_lock);
                                g_ui.lib_mode = 0;
                                pthread_mutex_unlock(&g_ui_lock);
                                request_redraw();
                            }
                        }
                    } else {
                        /* Player view: brightness bar tap first, then library enter */
                        if (tap_y >= UI_BRIGHT_Y &&
                                tap_y < UI_BRIGHT_Y + UI_BRIGHT_H) {
                            int bx = tap_x - UI_BRIGHT_X;
                            if (bx < 0) bx = 0;
                            if (bx > UI_BRIGHT_W) bx = UI_BRIGHT_W;
                            uint32_t brt = (uint32_t)
                                ((uint64_t)bx * 100 / UI_BRIGHT_W);
                            pthread_mutex_lock(&g_ui_lock);
                            g_ui.brightness = brt;
                            pthread_mutex_unlock(&g_ui_lock);
                            fb_set_brightness((int)(brt * 255 / 100));
                            request_redraw();
                        } else
                        if (tap_y >= UI_LIB_ENTER_Y &&
                                tap_y < UI_LIB_ENTER_Y + UI_LIB_ENTER_H) {
                            pthread_mutex_lock(&g_ui_lock);
                            g_ui.lib_mode = 1;
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                        } else if (tap_y >= UI_CTRL_Y && tap_y <= UI_CTRL_Y + UI_CTRL_H) {
                            int x = tap_x;
                            if (x >= UI_BTN_PREV_X && x < UI_BTN_PREV_X + UI_BTN_PREV_W)
                                ipc_send_cmd(audiod_fd, CMD_PREV, 0);
                            else if (x >= UI_BTN_PLAY_X && x < UI_BTN_PLAY_X + UI_BTN_PLAY_W) {
                                /* Race-safe read of player state: render thread
                                 * writes the same field under g_ui_lock. */
                                pthread_mutex_lock(&g_ui_lock);
                                PlayerState ps = g_ui.state;
                                pthread_mutex_unlock(&g_ui_lock);
                                ipc_send_cmd(audiod_fd,
                                    ps == PLAYER_PLAYING ? CMD_PAUSE : CMD_PLAY, 0);
                            }
                            else if (x >= UI_BTN_NEXT_X && x < UI_BTN_NEXT_X + UI_BTN_NEXT_W)
                                ipc_send_cmd(audiod_fd, CMD_NEXT, 0);
                        }
                    }
                    break;
                }
                case INPUT_SWIPE_LEFT:
                    if (g_screen_on && !g_ui.lib_mode)
                        ipc_send_cmd(audiod_fd, CMD_NEXT, 0);
                    break;
                case INPUT_SWIPE_RIGHT:
                    if (g_screen_on && !g_ui.lib_mode)
                        ipc_send_cmd(audiod_fd, CMD_PREV, 0);
                    break;
                case INPUT_SWIPE_UP:
                    if (g_screen_on) {
                        if (g_ui.lib_mode) {
                            pthread_mutex_lock(&g_ui_lock);
                            int max_s = (int)g_ui.track_count - UI_LIB_VISIBLE;
                            if (max_s > 0 && g_ui.lib_scroll < max_s)
                                g_ui.lib_scroll++;
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                        } else {
                            pthread_mutex_lock(&g_ui_lock);
                            if (g_ui.brightness < 100) {
                                g_ui.brightness += 10;
                                if (g_ui.brightness > 100) g_ui.brightness = 100;
                            }
                            fb_set_brightness((int)(g_ui.brightness * 255 / 100));
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                        }
                    }
                    break;
                case INPUT_SWIPE_DOWN:
                    if (g_screen_on) {
                        if (g_ui.lib_mode) {
                            pthread_mutex_lock(&g_ui_lock);
                            if (g_ui.lib_scroll > 0) g_ui.lib_scroll--;
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                        } else {
                            pthread_mutex_lock(&g_ui_lock);
                            if (g_ui.brightness >= 10) {
                                g_ui.brightness -= 10;
                            } else {
                                g_ui.brightness = 0;
                            }
                            fb_set_brightness((int)(g_ui.brightness * 255 / 100));
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                        }
                    }
                    break;
                case INPUT_VOLUME_UP:
                    ipc_send_cmd(pwrd_fd, CMD_SET_VOLUME, 0);   /* handled by pwrd */
                    break;
                case INPUT_VOLUME_DOWN:
                    ipc_send_cmd(pwrd_fd, CMD_SET_VOLUME, 0);
                    break;
                default: break;
                }
            }
        }
    }

    LOGI("uid: shutting down");
    g_quit = 1;
    sem_post(&g_render_sem);   /* unblock render thread so it can exit */
    pthread_join(g_render_tid, NULL);
    if (input)            input_close(input);
    if (audiod_fd >= 0)   close(audiod_fd);
    if (pwrd_fd   >= 0)   close(pwrd_fd);
    if (g_progress_tfd >= 0) close(g_progress_tfd);
    close(ep);
    fb_close(&g_fb);
    sem_destroy(&g_render_sem);
    LOG_CLOSE();
    return 0;
}
