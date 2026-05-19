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
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* ── forward declarations from ipc_client.c ─────────────────────────────── */
int ipc_connect(const char *path);
int ipc_send_cmd(int fd, IpcMsgType type, uint32_t param);
int ipc_recv(int fd, IpcMsg *out);

/* ── globals ─────────────────────────────────────────────────────────────── */

static volatile int   g_quit          = 0;
static int            g_screen_on     = 1;
static pthread_t      g_render_tid;
static sem_t          g_render_sem;     /* posted when a new frame is needed */
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
    /* Drop duplicate redraws if the render thread is already queued */
    int v = 0;
    sem_getvalue(&g_render_sem, &v);
    if (v == 0) sem_post(&g_render_sem);
}

static void screen_off(void)
{
    if (!g_screen_on) return;
    g_screen_on = 0;
    fb_set_brightness(0);
    /* render thread stays blocked at sem_wait — no SIGSTOP needed */
    LOGI("uid: screen off");
}

static void screen_on(void)
{
    if (g_screen_on) return;
    g_screen_on = 1;
    fb_set_brightness(200);
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

    InputCtx *input = input_open(ep);

    /* Initial draw */
    g_ui.battery_pct = 100;
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
                IpcMsg msg;
                if (ipc_recv(audiod_fd, &msg) == 0) {
                    pthread_mutex_lock(&g_ui_lock);
                    switch ((IpcMsgType)msg.type) {
                    case EVT_INDEX_READY:
                        /* Auto-play first track on startup */
                        if (audiod_fd >= 0 && g_ui.state == PLAYER_IDLE) {
                            ipc_send_cmd(audiod_fd, CMD_LOAD_TRACK, 0);
                            ipc_send_cmd(audiod_fd, CMD_PLAY, 0);
                        }
                        break;
                    case EVT_TRACK:
                        g_ui.track = msg.param.track;
                        break;
                    case EVT_STATE:
                        g_ui.state = msg.param.player_state;
                        break;
                    case EVT_POSITION:
                        g_ui.position_ms = msg.param.position_ms;
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

            /* ── input events ── */
            if (input) {
                InputEvt ie = input_process_fd(input, fd);
                switch (ie.type) {
                case INPUT_POWER_BUTTON:
                    if (g_screen_on) screen_off();
                    else             screen_on();
                    break;
                case INPUT_TAP:
                    if (!g_screen_on) { screen_on(); break; }
                    /* Hit test transport controls (portrait 1080 wide) */
                    {
                        int cx = 1080 / 2;
                        int spacing = 200;
                        if (ie.y >= 890 && ie.y <= 1000) {
                            if (ie.x > cx - spacing - 60 && ie.x < cx - spacing + 60)
                                ipc_send_cmd(audiod_fd, CMD_PREV, 0);
                            else if (ie.x > cx - 80 && ie.x < cx + 80)
                                ipc_send_cmd(audiod_fd,
                                    g_ui.state == PLAYER_PLAYING ? CMD_PAUSE : CMD_PLAY, 0);
                            else if (ie.x > cx + spacing - 60 && ie.x < cx + spacing + 60)
                                ipc_send_cmd(audiod_fd, CMD_NEXT, 0);
                        }
                    }
                    break;
                case INPUT_SWIPE_LEFT:
                    if (g_screen_on) ipc_send_cmd(audiod_fd, CMD_NEXT, 0);
                    break;
                case INPUT_SWIPE_RIGHT:
                    if (g_screen_on) ipc_send_cmd(audiod_fd, CMD_PREV, 0);
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
    if (input)     input_close(input);
    if (audiod_fd >= 0) close(audiod_fd);
    if (pwrd_fd   >= 0) close(pwrd_fd);
    close(ep);
    fb_close(&g_fb);
    sem_destroy(&g_render_sem);
    LOG_CLOSE();
    return 0;
}
