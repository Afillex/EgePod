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
#include <stdatomic.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#ifndef SIMULATE
#include <sys/reboot.h>
#include <linux/reboot.h>
#endif

#define BATTERY_INTERVAL_SEC    30  /* battery % changes ~1% per 6–10 min at 30mA */
#define THERMAL_WARN_CELSIUS   45   /* notify UI above this */
#define BRIGHTNESS_PATH        "/sys/class/leds/lcd-backlight/brightness"
#define BRIGHTNESS_ON          "200"
#define BRIGHTNESS_OFF         "0"
#define BATTERY_CAPACITY_PATH  "/sys/class/power_supply/battery/capacity"
#define THERMAL_PATH           "/sys/class/thermal/thermal_zone0/temp"

static _Atomic int  g_quit = 0;
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

    {
        struct sigaction sa = {0};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;   /* no SA_RESTART — epoll_wait must return EINTR on SIGTERM */
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT,  &sa, NULL);
        sa.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa, NULL);
    }

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
    uint32_t last_batt    = 0xFF;
    int      thermal_warn = 0;   /* 1 while above threshold (hysteresis gate) */
    uint32_t g_volume     = 70;  /* current volume 0-100 */

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
                if (client_fd >= 0) {
                    /* Remove the old fd from epoll BEFORE closing it — otherwise
                     * the closed fd stays in the epoll set and triggers a busy loop. */
                    epoll_ctl(ep, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                }
                client_fd = cfd;
                ev.events = EPOLLIN | EPOLLHUP | EPOLLERR;
                ev.data.fd = cfd;
                epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev);
                LOGI("pwrd: uid connected");
                /* Push current volume so UI is in sync immediately */
                {
                    IpcMsg vm = { .type = EVT_VOLUME };
                    vm.param.volume = g_volume;
                    send(cfd, &vm, sizeof(vm), MSG_DONTWAIT);
                }
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

                /* Thermal — send event only on rising edge (hysteresis: re-arm
                 * only after temperature drops 3°C below the threshold). */
                uint32_t temp_mc = 0;
                if (sysfs_read_u32(THERMAL_PATH, &temp_mc) == 0) {
                    uint32_t temp_c = temp_mc / 1000;
                    if (!thermal_warn && temp_c >= THERMAL_WARN_CELSIUS) {
                        thermal_warn = 1;
                        if (client_fd >= 0) {
                            IpcMsg m = { .type = EVT_THERMAL };
                            m.param.temp_celsius_x10 = temp_mc / 100;
                            send(client_fd, &m, sizeof(m), MSG_DONTWAIT);
                        }
                        LOGW("pwrd: thermal warning: %u °C", temp_c);
                    } else if (thermal_warn && temp_c < THERMAL_WARN_CELSIUS - 3) {
                        thermal_warn = 0;   /* re-arm after cooling */
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
                        case CMD_SET_VOLUME: {
                            uint32_t v = cmd.param.volume;
                            if (v > 100) v = 100;
                            g_volume = v;
                            LOGI("pwrd: volume → %u%%", v);
                            /* Broadcast EVT_VOLUME back to UI */
                            if (client_fd >= 0) {
                                IpcMsg m = { .type = EVT_VOLUME };
                                m.param.volume = v;
                                send(client_fd, &m, sizeof(m), MSG_DONTWAIT);
                            }
                            break;
                        }
                        case CMD_REBOOT:
                        case CMD_SHUTDOWN: {
                            const int is_reboot = (cmd.type == CMD_REBOOT);
                            LOGI("pwrd: %s requested", is_reboot ? "reboot" : "shutdown");

                            /* 1. Notify uid so it renders the shutdown splash. */
                            if (client_fd >= 0) {
                                IpcMsg m = { .type = EVT_SHUTDOWN_PENDING };
                                send(client_fd, &m, sizeof(m), MSG_DONTWAIT);
                            }

                            /* 2. Notify audiod directly via its server socket so it
                             *    can persist state before we reboot.  One-shot connect:
                             *    if audiod crashed the connect fails silently, which is
                             *    fine — no state to save if audiod is already dead. */
                            {
                                int afd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
                                if (afd >= 0) {
                                    struct sockaddr_un aa = { .sun_family = AF_UNIX };
                                    snprintf(aa.sun_path, sizeof(aa.sun_path), "%s",
                                             AUDIOD_SOCK_PATH);
                                    if (connect(afd, (struct sockaddr *)&aa, sizeof(aa)) == 0) {
                                        IpcMsg sa = { .type = EVT_SHUTDOWN_PENDING };
                                        send(afd, &sa, sizeof(sa), 0);
                                        LOGI("pwrd: audiod notified of shutdown");
                                    }
                                    close(afd);
                                }
                            }

                            /* 3. Give audiod 800 ms to persist + uid to render splash. */
                            {
                                struct timespec delay = { .tv_nsec = 800000000L };
                                nanosleep(&delay, NULL);
                            }

#ifdef SIMULATE
                            LOGI("pwrd: sim %s — killing egepod daemons",
                                 is_reboot ? "reboot" : "shutdown");
                            if (is_reboot) {
                                /* Write a .reboot sidecar alongside the FB file so
                                 * simulate.sh detects it on viewer exit and re-execs
                                 * the script, restarting all three daemons. */
                                const char *fbp = getenv("EGEPOD_FB_FILE");
                                if (!fbp) fbp = "/tmp/egepod_fb.raw";
                                char rpath[600];
                                snprintf(rpath, sizeof(rpath), "%s.reboot", fbp);
                                FILE *rf = fopen(rpath, "w");
                                if (rf) { fputs("1\n", rf); fclose(rf); }
                                LOGI("pwrd: wrote reboot sidecar %s", rpath);
                            }
                            system("pkill -TERM -f 'egepod_audiod|egepod_uid' 2>/dev/null");
                            g_quit = 1;
#else
                            /* 3a. Primary path: hand off to Android init via sys.powerctl.
                             *     init's DoReboot() stops services in reverse order, syncs,
                             *     unmounts /sdcard and /data, then calls reboot() itself
                             *     with the PMIC poweroff handler properly registered. */
                            sync();
                            {
                                const char *prop = is_reboot
                                    ? "/system/bin/setprop sys.powerctl reboot,userrequested"
                                    : "/system/bin/setprop sys.powerctl shutdown,userrequested";
                                int sp_rc = system(prop);
                                LOGI("pwrd: setprop sys.powerctl → %d", sp_rc);
                            }

                            /* 3b. Fallback: if init doesn't begin killing us within 6 s,
                             *     it is broken — fall back to direct reboot(2).  If init
                             *     does its job we are SIGKILLed before this loop completes. */
                            for (int fb = 0; fb < 60; fb++) {
                                struct timespec t = { .tv_nsec = 100000000L };
                                nanosleep(&t, NULL);
                            }
                            LOGW("pwrd: init did not honor sys.powerctl after 6s — direct reboot(2)");
                            sync();
                            if (is_reboot)
                                reboot(LINUX_REBOOT_CMD_RESTART);
                            else
                                reboot(LINUX_REBOOT_CMD_POWER_OFF);

                            /* reboot(2) returned — pm_power_off is unbound in the kernel.
                             * Spin on pause() so init's respawn policy cannot relaunch us
                             * into a "device still on" illusion. */
                            LOGE("pwrd: reboot(2) returned — pm_power_off likely unbound");
                            while (1) pause();
#endif
                            break;
                        }
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
