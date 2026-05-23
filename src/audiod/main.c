/* egepod_audiod — audio playback daemon
 *
 * Responsibilities:
 *   1. Build the music index from /sdcard/Music
 *   2. Serve IPC commands from egepod_uid over a Unix Domain Socket
 *   3. Forward unsolicited events back to connected clients
 *   4. Manage the Player (decoder + playback threads) lifecycle */

#include "player.h"
#include "index.h"
#include "persist.h"
#include "h2w.h"
#include "../common/ipc.h"
#include "../common/log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdatomic.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef MUSIC_DIR
#define MUSIC_DIR    "/sdcard/Music"
#endif
#define MAX_EVENTS   (MAX_IPC_CLIENTS + 2)

static _Atomic int  g_quit = 0;
static int          g_tfd  = -1;   /* position timerfd — armed only while PLAYING */

static void on_signal(int sig) { (void)sig; g_quit = 1; }

static void tfd_arm(void)
{
    if (g_tfd < 0) return;
    struct itimerspec its = {
        .it_value    = { .tv_nsec = 200000000 },
        .it_interval = { .tv_nsec = 200000000 },
    };
    timerfd_settime(g_tfd, 0, &its, NULL);
}

static void tfd_disarm(void)
{
    if (g_tfd < 0) return;
    struct itimerspec its = { {0,0}, {0,0} };
    timerfd_settime(g_tfd, 0, &its, NULL);
}

/* Called by player on every state transition (from playback thread or main).
 * Fires timerfd only when audio is actually moving. */
static void on_player_state(PlayerState s, void *ud)
{
    (void)ud;
    if (s == PLAYER_PLAYING) tfd_arm();
    else                     tfd_disarm();
}

/* ── IPC server setup ──────────────────────────────────────────────────── */

static int create_server_socket(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { LOGE("audiod: socket: %s", strerror(errno)); return -1; }

    unlink(path);   /* remove stale socket */

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOGE("audiod: bind(%s): %s", path, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, MAX_IPC_CLIENTS) < 0) {
        LOGE("audiod: listen: %s", strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void)
{
    LOG_OPEN("egepod_audiod");
    LOGI("audiod: starting");

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* Raise RLIMIT_MEMLOCK to unlimited so mlock() on large PCM buffers
     * succeeds.  Default limit (64 KB) is far below a 5-min FLAC (~31 MB).
     * Without this, PCM buffers can be swapped to zram causing audio dropouts. */
    {
        struct rlimit rl = { .rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY };
        if (setrlimit(RLIMIT_MEMLOCK, &rl) != 0)
            LOGW("audiod: setrlimit(MEMLOCK,INF): %s — mlock may fail", strerror(errno));
    }

    /* Build music index (blocking; runs on main thread before event loop) */
    IndexNode *index = index_build(MUSIC_DIR);
    if (!index) { LOGE("audiod: index_build failed"); return 1; }

    Player *player = player_create(index);
    if (!player) { LOGE("audiod: player_create failed"); return 1; }

    /* Resume from persisted state — loads track at saved position, paused.
     * Never auto-plays: a DAP must not blast audio on unintended power-on. */
    {
        uint32_t saved_track = 0, saved_pos = 0;
        int saved_playing = 0;
        if (persist_load(&saved_track, &saved_pos, &saved_playing) == 0 &&
            saved_track < (uint32_t)index_track_count(index)) {
            IpcMsg cmd = {0};
            cmd.type = CMD_LOAD_TRACK;
            cmd.param.track_idx = saved_track;
            player_handle_cmd(player, &cmd);
            if (saved_pos > 0) {
                cmd.type = CMD_SEEK;
                cmd.param.seek_ms = saved_pos;
                player_handle_cmd(player, &cmd);
            }
            cmd.type = CMD_PAUSE;
            player_handle_cmd(player, &cmd);
            LOGI("audiod: restored track=%u pos=%ums (paused)", saved_track, saved_pos);
        }
    }

    H2wWatcher *h2w = h2w_start(player);

    /* Notify init that the service is ready.
     * On Android production EGEPOD_READY_DIR=/dev (writeable by root).
     * On simulation it stays /tmp (default). */
#ifndef EGEPOD_READY_DIR
# define EGEPOD_READY_DIR "/tmp"
#endif
    {
        FILE *f = fopen(EGEPOD_READY_DIR "/egepod_audiod_ready", "w");
        if (f) { fputs("1\n", f); fclose(f); }
    }

    /* Publish index-ready event when first client connects */
    IpcMsg idx_ready = { .type = EVT_INDEX_READY };
    idx_ready.param.track_count = (uint32_t)index_track_count(index);

    /* IPC server socket */
    int srv_fd = create_server_socket(AUDIOD_SOCK_PATH);
    if (srv_fd < 0) { player_destroy(player); index_free(index); return 1; }

    /* Position ticker: timerfd at 5 Hz — armed only while PLAYING.
     * Starts disarmed; the player state callback arms it on the first PLAY. */
    g_tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    int tfd = g_tfd;
    if (tfd < 0) {
        LOGE("audiod: timerfd: %s", strerror(errno));
        player_destroy(player); index_free(index); close(srv_fd); return 1;
    }
    player_set_state_callback(player, on_player_state, NULL);

    /* epoll */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;

    ev.events  = EPOLLIN;
    ev.data.fd = srv_fd;
    epoll_ctl(ep, EPOLL_CTL_ADD, srv_fd, &ev);

    ev.events  = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);

    /* Track connected client fds */
    int clients[MAX_IPC_CLIENTS];
    for (int i = 0; i < MAX_IPC_CLIENTS; i++) clients[i] = -1;
    int n_clients = 0;

    struct epoll_event events[MAX_EVENTS];

    LOGI("audiod: event loop started, %zu tracks indexed", index_track_count(index));

    while (!g_quit) {
        int n = epoll_wait(ep, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("audiod: epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            /* ── new connection ── */
            if (fd == srv_fd) {
                int cfd = accept4(srv_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0) continue;
                if (n_clients >= MAX_IPC_CLIENTS) { close(cfd); continue; }
                clients[n_clients++] = cfd;
                player_subscribe(player, cfd);

                ev.events  = EPOLLIN | EPOLLHUP | EPOLLERR;
                ev.data.fd = cfd;
                epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev);

                /* Push current player state BEFORE index-ready so uid knows
                 * whether to auto-play (IDLE) or respect a persist-restored
                 * PAUSED state without issuing CMD_PLAY on connect. */
                {
                    IpcMsg st_msg = { .type = EVT_STATE };
                    st_msg.param.player_state = player_get_state(player);
                    send(cfd, &st_msg, sizeof(st_msg), MSG_DONTWAIT);
                }
                send(cfd, &idx_ready, sizeof(idx_ready), MSG_DONTWAIT);
                LOGI("audiod: client connected (fd=%d)", cfd);
                continue;
            }

            /* ── position tick ── */
            if (fd == tfd) {
                uint64_t exp; (void)read(tfd, &exp, sizeof(exp)); /* drain expirations */

                /* 5-second persist heartbeat: every 25 × 200ms ticks.
                 * Skip when not PLAYING — paused state is identical to the last
                 * persist, so the eMMC write is pure waste. */
                static int persist_tick = 0;
                if (++persist_tick >= 25) {
                    persist_tick = 0;
                    PlayerState pst = player_get_state(player);
                    if (pst == PLAYER_PLAYING) {
                        uint32_t ppos = player_get_position(player);
                        if (ppos != UINT32_MAX)
                            persist_save((uint32_t)player_get_track_idx(player),
                                         ppos, 1);
                    }
                }

                uint32_t pos = player_get_position(player);
                static uint32_t last_pos = UINT32_MAX;
                if (pos == UINT32_MAX) {
                    /* Not playing/paused: reset guard so the first tick of the
                     * next track is always sent regardless of direction. */
                    last_pos = UINT32_MAX;
                } else if (pos != last_pos) {
                    last_pos = pos;
                    /* Broadcast under p->lock to serialize with playback thread's
                     * publish_state sends — prevents concurrent send() on same fd. */
                    player_broadcast_position(player, pos);
                }
                continue;
            }

            /* ── client message ── */
            if (events[i].events & EPOLLIN) {
                IpcMsg cmd;
                ssize_t r = recv(fd, &cmd, sizeof(cmd), 0);
                if (r == sizeof(cmd)) {
                    /* pwrd pushes EVT_SHUTDOWN_PENDING directly to our server socket
                     * at reboot/shutdown time.  Intercept before player_handle_cmd
                     * so we can persist state and exit cleanly. */
                    if ((IpcMsgType)cmd.type == EVT_SHUTDOWN_PENDING) {
                        LOGI("audiod: EVT_SHUTDOWN_PENDING — persisting state");
                        uint32_t ppos = player_get_position(player);
                        if (ppos != UINT32_MAX)
                            persist_save((uint32_t)player_get_track_idx(player), ppos,
                                         player_get_state(player) == PLAYER_PLAYING ? 1 : 0);
                        g_quit = 1;
                    } else {
                        IpcMsg reply = player_handle_cmd(player, &cmd);
                        if (reply.type)
                            send(fd, &reply, sizeof(reply), MSG_DONTWAIT);
                        /* CMD_PLAY doesn't call publish_state, so arm/disarm the
                         * ticker here for the commands that go through this path. */
                        if ((IpcMsgType)reply.type == EVT_STATE) {
                            if (reply.param.player_state == PLAYER_PLAYING) tfd_arm();
                            else                                             tfd_disarm();
                        }
                    }
                } else if (r <= 0) {
                    goto drop_client;
                }
            }
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
drop_client:
                epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
                player_unsubscribe(player, fd);
                close(fd);
                for (int j = 0; j < n_clients; j++) {
                    if (clients[j] == fd) {
                        clients[j] = clients[--n_clients];
                        clients[n_clients] = -1;
                        break;
                    }
                }
                LOGI("audiod: client disconnected (fd=%d)", fd);
            }
        }
    }

    LOGI("audiod: shutting down");

    /* Save final position so resume-on-boot has the latest state. */
    {
        uint32_t fpos = player_get_position(player);
        if (fpos != UINT32_MAX) {
            PlayerState fst = player_get_state(player);
            persist_save((uint32_t)player_get_track_idx(player), fpos,
                         fst == PLAYER_PLAYING ? 1 : 0);
        }
    }

    h2w_stop(h2w);
    close(tfd);
    close(srv_fd);
    for (int i = 0; i < n_clients; i++) if (clients[i] >= 0) close(clients[i]);
    close(ep);
    player_destroy(player);
    index_free(index);
    LOG_CLOSE();
    return 0;
}
