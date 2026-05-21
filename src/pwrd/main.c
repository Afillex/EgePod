/* egepod_pwrd — power management daemon
 *
 * Runs before audiod and uid.  Applies the DAP power policy once, then
 * serves the IPC socket for:
 *   - Screen on/off requests from uid
 *   - Battery/thermal events pushed to uid
 *
 * Battery polling uses a timerfd (5-second interval) to avoid any busy loop.
 * Thermal events only fire when temperature exceeds THERMAL_WARN_CELSIUS. */

#include "cpu.h"
#include "rfkill.h"
#include "../common/ipc.h"
#include "../common/log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#define BATTERY_INTERVAL_SEC    30  /* battery % changes ~1% per 6–10 min at 30mA */
#define THERMAL_WARN_CELSIUS   45   /* notify UI above this */
#define BRIGHTNESS_PATH        "/sys/class/leds/lcd-backlight/brightness"
#define BRIGHTNESS_ON          "200"
#define BRIGHTNESS_OFF         "0"
#define BATTERY_CAPACITY_PATH  "/sys/class/power_supply/battery/capacity"
#define BATTERY_CURRENT_PATH   "/sys/class/power_supply/battery/current_now"
#define THERMAL_PATH           "/sys/class/thermal/thermal_zone0/temp"

static volatile int g_quit = 0;
static void on_signal(int sig) { (void)sig; g_quit = 1; }

static int sysfs_read_u32(const char *path, uint32_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int rc = fscanf(f, "%u", out);
    fclose(f);
    return (rc == 1) ? 0 : -1;
}

static int create_server_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    unlink(path);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 2) < 0) {
        close(fd); return -1;
    }
    return fd;
}

static int create_battery_timer(void)
{
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) return -1;
    struct itimerspec its = {
        .it_value    = { .tv_sec = BATTERY_INTERVAL_SEC },
        .it_interval = { .tv_sec = BATTERY_INTERVAL_SEC },
    };
    timerfd_settime(tfd, 0, &its, NULL);
    return tfd;
}

int main(void)
{
    LOG_OPEN("egepod_pwrd");
    LOGI("pwrd: starting");

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Apply power policy — must be done before audiod starts */
    cpu_apply_dap_policy();
    rfkill_block_all();

    /* Signal readiness to init (path matches init.rc wait command). */
#ifndef EGEPOD_READY_DIR
# define EGEPOD_READY_DIR "/tmp"
#endif
    {
        FILE *f = fopen(EGEPOD_READY_DIR "/egepod_pwrd_ready", "w");
        if (f) { fputs("1\n", f); fclose(f); }
    }

    int srv_fd = create_server_socket(PWRD_SOCK_PATH);
    if (srv_fd < 0) {
        LOGE("pwrd: server socket: %s", strerror(errno));
        return 1;
    }

    int timer_fd = create_battery_timer();
    if (timer_fd < 0) {
        LOGW("pwrd: timerfd unavailable — no battery polling");
    }

    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;

    ev.events = EPOLLIN; ev.data.fd = srv_fd;
    epoll_ctl(ep, EPOLL_CTL_ADD, srv_fd, &ev);
    if (timer_fd >= 0) {
        ev.events = EPOLLIN; ev.data.fd = timer_fd;
        epoll_ctl(ep, EPOLL_CTL_ADD, timer_fd, &ev);
    }

    int    client_fd = -1;
    uint32_t last_batt = 0xFF;

    struct epoll_event events[4];

    LOGI("pwrd: event loop started");

    while (!g_quit) {
        int n = epoll_wait(ep, events, 4, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            if (fd == srv_fd) {
                int cfd = accept4(srv_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0) continue;
                if (client_fd >= 0) close(client_fd); /* only one UI client */
                client_fd = cfd;
                ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
                ev.data.fd = cfd;
                epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev);
                LOGI("pwrd: uid connected");
                continue;
            }

            if (fd == timer_fd) {
                uint64_t expirations;
                (void)read(timer_fd, &expirations, sizeof(expirations));

                /* Battery */
                uint32_t pct = 0;
                if (sysfs_read_u32(BATTERY_CAPACITY_PATH, &pct) == 0 &&
                    pct != last_batt && client_fd >= 0) {
                    IpcMsg m = { .type = EVT_BATTERY };
                    m.param.battery_pct = pct;
                    send(client_fd, &m, sizeof(m), MSG_DONTWAIT);
                    last_batt = pct;
                }

                /* Thermal */
                uint32_t temp_mc = 0;
                if (sysfs_read_u32(THERMAL_PATH, &temp_mc) == 0) {
                    uint32_t temp_c = temp_mc / 1000;
                    if (temp_c >= THERMAL_WARN_CELSIUS && client_fd >= 0) {
                        IpcMsg m = { .type = EVT_THERMAL };
                        m.param.temp_celsius_x10 = temp_mc / 100;
                        send(client_fd, &m, sizeof(m), MSG_DONTWAIT);
                    }
                }
                continue;
            }

            /* ── client message ── */
            if (fd == client_fd) {
                if (events[i].events & EPOLLIN) {
                    IpcMsg cmd;
                    ssize_t r = recv(fd, &cmd, sizeof(cmd), 0);
                    if (r == sizeof(cmd)) {
                        switch ((IpcMsgType)cmd.type) {
                        case CMD_SCREEN_OFF:
                            /* Turn off backlight; audiod continues unaffected */
                            {
                                FILE *f = fopen(BRIGHTNESS_PATH, "w");
                                if (f) { fputs(BRIGHTNESS_OFF, f); fclose(f); }
                            }
                            LOGI("pwrd: screen off");
                            break;
                        case CMD_SCREEN_ON:
                            {
                                FILE *f = fopen(BRIGHTNESS_PATH, "w");
                                if (f) { fputs(BRIGHTNESS_ON, f); fclose(f); }
                            }
                            LOGI("pwrd: screen on");
                            break;
                        default:
                            LOGW("pwrd: unknown cmd type 0x%02x", cmd.type);
                            break;
                        }
                    } else if (r <= 0) {
                        goto drop;
                    }
                }
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
drop:
                    epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
                    close(fd); client_fd = -1;
                    LOGI("pwrd: uid disconnected");
                }
            }
        }
    }

    LOGI("pwrd: shutting down");
    cpu_restore_defaults();
    rfkill_unblock_all();
    if (client_fd >= 0) close(client_fd);
    if (timer_fd  >= 0) close(timer_fd);
    close(srv_fd);
    close(ep);
    LOG_CLOSE();
    return 0;
}
