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

static _Atomic int    g_quit          = 0;
static int            g_screen_on     = 1;
static int            g_progress_tfd  = -1;  /* 20 Hz timerfd for progress bar redraws */
static int            g_power_long_tfd = -1;  /* 400 ms timerfd for long-press detection */
static uint32_t       g_volume        = 70;   /* current volume 0-100 */
static InputCtx      *g_input         = NULL; /* set after input_open() */

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

static void power_long_arm(void)
{
    if (g_power_long_tfd < 0) return;
    struct itimerspec its = {
        .it_value    = { .tv_nsec = 400000000 },   /* 400 ms one-shot — snappier than Android 750 ms */
        .it_interval = { 0, 0 },
    };
    timerfd_settime(g_power_long_tfd, 0, &its, NULL);
}

static void power_long_disarm(void)
{
    if (g_power_long_tfd < 0) return;
    struct itimerspec its = { {0,0}, {0,0} };
    timerfd_settime(g_power_long_tfd, 0, &its, NULL);
}

static void screen_off(void)
{
    if (!g_screen_on) return;
    g_screen_on = 0;
    fb_set_brightness(0);
    pthread_mutex_lock(&g_ui_lock);
    g_ui.locked = 1;            /* turning screen off always locks */
    g_ui.power_menu_open = 0;   /* dismiss menu if it was open */
    pthread_mutex_unlock(&g_ui_lock);
    /* Stop 20 Hz redraw wakeups so the SoC can stay in deep sleep. */
    progress_timer_disarm();
    /* Stop sim tap polling — no taps are valid with screen off. */
    input_set_polling(g_input, 0);
    /* render thread stays blocked at sem_wait — no SIGSTOP needed */
    LOGI("uid: screen off (locked)");
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
    /* Resume sim tap polling now that the screen is active again. */
    input_set_polling(g_input, 1);
    request_redraw();
    LOGI("uid: screen on");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_OPEN("egepod_uid");
    LOGI("uid: starting");

    /* sigaction without SA_RESTART so epoll_wait returns EINTR on SIGTERM,
     * allowing the event loop to re-check g_quit.  Any new blocking syscall
     * added to this daemon must handle EINTR — see existing epoll_wait path. */
    {
        struct sigaction sa = {0};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;   /* no SA_RESTART */
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT,  &sa, NULL);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa, NULL);
    }

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

    /* 400 ms one-shot timerfd for power-button long-press detection */
    g_power_long_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (g_power_long_tfd >= 0) {
        ev.events = EPOLLIN; ev.data.fd = g_power_long_tfd;
        epoll_ctl(ep, EPOLL_CTL_ADD, g_power_long_tfd, &ev);
    }

    InputCtx *input = input_open(ep);
    g_input = input;

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
                        /* Kick off background library metadata fetch — no auto-play.
                         * A DAP must never blast audio on unintended power-on. */
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
                            /* Only arm the progress timer if the screen is on —
                             * with the screen off there's nothing to redraw and
                             * the 20 Hz wakeup wastes CPU. screen_on() re-arms
                             * when the user wakes the device. */
                            if (g_screen_on) progress_timer_arm();
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
                    switch ((IpcMsgType)msg.type) {
                    case EVT_BATTERY:
                        g_ui.battery_pct = msg.param.battery_pct;
                        break;
                    case EVT_VOLUME:
                        g_volume = msg.param.volume;
                        g_ui.volume = msg.param.volume;
                        break;
                    case EVT_SHUTDOWN_PENDING:
                        g_ui.shutting_down = 1;
                        /* Force a redraw regardless of screen state so the
                         * splash is committed to the FB before pwrd kills us. */
                        pthread_mutex_unlock(&g_ui_lock);
                        request_redraw();
#ifdef SIMULATE
                        /* Give the render thread ~150 ms to commit the splash,
                         * then signal the macOS fb_viewer to close. */
                        {
                            struct timespec _ts = { .tv_nsec = 150000000L };
                            nanosleep(&_ts, NULL);
                            const char *_fb = getenv("EGEPOD_FB_FILE");
                            if (_fb && _fb[0]) {
                                char _shut[520];
                                snprintf(_shut, sizeof(_shut), "%s.shutdown", _fb);
                                FILE *_f = fopen(_shut, "w");
                                if (_f) { fputs("1\n", _f); fclose(_f); }
                            }
                        }
#endif
                        /* Exit the event loop — on real hardware init/pwrd will
                         * SIGKILL us, but self-terminating is cleaner (runs
                         * cleanup) and fixes the sim path where SA_RESTART on
                         * signal() causes epoll_wait to restart after SIGTERM
                         * rather than returning EINTR. */
                        g_quit = 1;
                        pthread_mutex_lock(&g_ui_lock);
                        break;
                    default: break;
                    }
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

            /* ── power long-press timer ── */
            if (fd == g_power_long_tfd) {
                uint64_t exp;
                (void)read(g_power_long_tfd, &exp, sizeof(exp));
                /* 400 ms expired — open power menu */
                if (!g_screen_on) screen_on();
                pthread_mutex_lock(&g_ui_lock);
                g_ui.power_menu_open = 1;
                pthread_mutex_unlock(&g_ui_lock);
                request_redraw();
                LOGI("uid: power long-press → power menu");
                continue;
            }

            /* ── input events ── */
            if (input) {
                InputEvt ie = input_process_fd(input, fd);
                /* Ignore all input while shutting down */
                pthread_mutex_lock(&g_ui_lock);
                int shutting = g_ui.shutting_down;
                pthread_mutex_unlock(&g_ui_lock);
                if (shutting) break;   /* drop all input events */

                switch (ie.type) {
                case INPUT_POWER_BUTTON:
                    if (ie.value == 1) {
                        /* Key down — arm 750 ms long-press timer */
                        power_long_arm();
                    } else {
                        /* Key up — if timer didn't fire yet → short press */
                        power_long_disarm();
                        pthread_mutex_lock(&g_ui_lock);
                        int menu_open = g_ui.power_menu_open;
                        pthread_mutex_unlock(&g_ui_lock);
                        if (!menu_open) {
                            if (g_screen_on) screen_off();
                            else             screen_on();
                        }
                    }
                    break;
                case INPUT_POWER_BUTTON_LONG:
                    /* Synthetic event from fb_viewer (Shift+P shortcut) */
                    if (!g_screen_on) screen_on();
                    pthread_mutex_lock(&g_ui_lock);
                    g_ui.power_menu_open = 1;
                    pthread_mutex_unlock(&g_ui_lock);
                    request_redraw();
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

                    /* Power menu takes priority over all other tap routing */
                    pthread_mutex_lock(&g_ui_lock);
                    int pmenu    = g_ui.power_menu_open;
                    int locked   = g_ui.locked;
                    int lib_mode = g_ui.lib_mode;   /* snapshot — avoid bare race below */
                    pthread_mutex_unlock(&g_ui_lock);

                    if (pmenu) {
                        /* Tap inside a button */
                        if (tap_x >= UI_PMENU_BTN_X &&
                            tap_x <  UI_PMENU_BTN_X + UI_PMENU_BTN_W) {
                            if (tap_y >= UI_PMENU_BTN_LOCK_Y &&
                                tap_y <  UI_PMENU_BTN_LOCK_Y + UI_PMENU_BTN_H) {
                                screen_off();   /* sets locked=1, power_menu_open=0 */
                            } else if (tap_y >= UI_PMENU_BTN_REBT_Y &&
                                       tap_y <  UI_PMENU_BTN_REBT_Y + UI_PMENU_BTN_H) {
                                ipc_send_cmd(pwrd_fd, CMD_REBOOT, 0);
                            } else if (tap_y >= UI_PMENU_BTN_SHDN_Y &&
                                       tap_y <  UI_PMENU_BTN_SHDN_Y + UI_PMENU_BTN_H) {
                                ipc_send_cmd(pwrd_fd, CMD_SHUTDOWN, 0);
                            } else {
                                /* tap inside card but not a button — dismiss */
                                pthread_mutex_lock(&g_ui_lock);
                                g_ui.power_menu_open = 0;
                                pthread_mutex_unlock(&g_ui_lock);
                                request_redraw();
                            }
                        } else {
                            /* tap outside card — dismiss */
                            pthread_mutex_lock(&g_ui_lock);
                            g_ui.power_menu_open = 0;
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                        }
                        break;
                    }

                    /* Lock screen: taps are ignored (only swipe-up unlocks) */
                    if (locked) break;

                    if (lib_mode) {
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
                    if (g_screen_on) {
                        pthread_mutex_lock(&g_ui_lock);
                        int sl_lm = g_ui.lib_mode, sl_lk = g_ui.locked;
                        pthread_mutex_unlock(&g_ui_lock);
                        if (!sl_lm && !sl_lk)
                            ipc_send_cmd(audiod_fd, CMD_NEXT, 0);
                    }
                    break;
                case INPUT_SWIPE_RIGHT:
                    if (g_screen_on) {
                        pthread_mutex_lock(&g_ui_lock);
                        int sr_lm = g_ui.lib_mode, sr_lk = g_ui.locked;
                        pthread_mutex_unlock(&g_ui_lock);
                        if (!sr_lm && !sr_lk)
                            ipc_send_cmd(audiod_fd, CMD_PREV, 0);
                    }
                    break;
                case INPUT_SWIPE_UP:
                    if (g_screen_on) {
                        pthread_mutex_lock(&g_ui_lock);
                        int is_locked = g_ui.locked;
                        int su_lm    = g_ui.lib_mode;   /* snapshot together */
                        pthread_mutex_unlock(&g_ui_lock);
                        if (is_locked) {
                            /* Swipe up unlocks */
                            pthread_mutex_lock(&g_ui_lock);
                            g_ui.locked = 0;
                            pthread_mutex_unlock(&g_ui_lock);
                            request_redraw();
                            LOGI("uid: unlocked");
                            break;
                        }
                        if (su_lm) {
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
                        pthread_mutex_lock(&g_ui_lock);
                        int sd_lm = g_ui.lib_mode;
                        pthread_mutex_unlock(&g_ui_lock);
                        if (sd_lm) {
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
                    if (g_volume < 100) {
                        g_volume += 10;
                        if (g_volume > 100) g_volume = 100;
                    }
                    ipc_send_cmd(pwrd_fd, CMD_SET_VOLUME, g_volume);
                    break;
                case INPUT_VOLUME_DOWN:
                    if (g_volume >= 10) g_volume -= 10;
                    else g_volume = 0;
                    ipc_send_cmd(pwrd_fd, CMD_SET_VOLUME, g_volume);
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
    if (input)               input_close(input);
    if (audiod_fd >= 0)      close(audiod_fd);
    if (pwrd_fd   >= 0)      close(pwrd_fd);
    if (g_progress_tfd  >= 0) close(g_progress_tfd);
    if (g_power_long_tfd >= 0) close(g_power_long_tfd);
    close(ep);
    fb_close(&g_fb);
    sem_destroy(&g_render_sem);
    LOG_CLOSE();
    return 0;
}
