#include "h2w.h"
#include "../common/ipc.h"
#include "../common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <sys/select.h>

#ifdef SIMULATE
# define DEFAULT_H2W_FILE "/tmp/egepod_h2w"
#else
# define DEFAULT_H2W_FILE "/sys/class/switch/h2w/state"
#endif

struct H2wWatcher {
    Player          *player;
    pthread_t        tid;
    volatile int     quit;
};

static const char *h2w_path(void)
{
    const char *p = getenv("EGEPOD_H2W_FILE");
    return (p && p[0]) ? p : DEFAULT_H2W_FILE;
}

static int read_state(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    fscanf(f, "%d", &v);
    fclose(f);
    return v;
}

static void *h2w_thread(void *arg)
{
    H2wWatcher *w = arg;
    const char *path = h2w_path();

    int in_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (in_fd < 0) {
        LOGW("h2w: inotify_init1: %s", strerror(errno));
        return NULL;
    }

    int wd = inotify_add_watch(in_fd, path, IN_MODIFY | IN_CREATE | IN_CLOSE_WRITE);
    if (wd < 0) {
        /* File may not exist yet in sim — retry once after 2 s */
        sleep(2);
        wd = inotify_add_watch(in_fd, path, IN_MODIFY | IN_CREATE | IN_CLOSE_WRITE);
        if (wd < 0) {
            LOGW("h2w: watch %s: %s (headphone auto-pause disabled)", path, strerror(errno));
            close(in_fd);
            return NULL;
        }
    }

    int last_state = read_state(path);
    if (last_state < 0) last_state = 1;   /* assume plugged at startup */
    LOGI("h2w: watching %s (state=%d)", path, last_state);

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (!w->quit) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in_fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(in_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) continue;

        /* Drain inotify events */
        ssize_t r = read(in_fd, buf, sizeof(buf));
        (void)r;

        int cur = read_state(path);
        if (cur < 0) continue;
        if (cur == last_state) continue;

        if (cur == 0) {
            LOGI("h2w: headphone unplugged — pausing");
            IpcMsg cmd = { .type = CMD_PAUSE, .seq = 0 };
            player_handle_cmd(w->player, &cmd);
        } else {
            LOGI("h2w: headphone plugged");
        }
        last_state = cur;
    }

    inotify_rm_watch(in_fd, wd);
    close(in_fd);
    return NULL;
}

H2wWatcher *h2w_start(Player *player)
{
    H2wWatcher *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->player = player;
    pthread_create(&w->tid, NULL, h2w_thread, w);
    return w;
}

void h2w_stop(H2wWatcher *w)
{
    if (!w) return;
    w->quit = 1;
    pthread_join(w->tid, NULL);
    free(w);
}
