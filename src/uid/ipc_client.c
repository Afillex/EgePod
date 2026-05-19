/* IPC client helpers shared by egepod_uid.
 * Connects to audiod and pwrd sockets with non-blocking connect + retry. */

#include "../common/ipc.h"
#include "../common/log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

int ipc_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    /* Retry up to 10× with 500 ms delay (daemons may not be ready yet) */
    for (int i = 0; i < 10; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            LOGI("uid: connected to %s", path);
            return fd;
        }
        if (errno != ENOENT && errno != ECONNREFUSED) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 500000000 };
        nanosleep(&ts, NULL);
    }

    LOGE("uid: connect(%s): %s", path, strerror(errno));
    close(fd);
    return -1;
}

int ipc_send_cmd(int fd, IpcMsgType type, uint32_t param)
{
    IpcMsg m = { .type = (uint8_t)type };
    m.param.raw[0] = (uint8_t)(param);
    m.param.raw[1] = (uint8_t)(param >> 8);
    m.param.raw[2] = (uint8_t)(param >> 16);
    m.param.raw[3] = (uint8_t)(param >> 24);
    ssize_t r = send(fd, &m, sizeof(m), MSG_DONTWAIT);
    if (r != (ssize_t)sizeof(m))
        LOGE("uid: ipc_send_cmd(0x%02x) failed: %s", (unsigned)type, strerror(errno));
    return (r == (ssize_t)sizeof(m)) ? 0 : -1;
}

int ipc_recv(int fd, IpcMsg *out)
{
    ssize_t r = recv(fd, out, sizeof(*out), 0);
    return (r == (ssize_t)sizeof(*out)) ? 0 : -1;
}
