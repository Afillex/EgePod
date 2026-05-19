/* Minimal IPC test client — connects to egepod_audiod and sends CMD_PLAY.
 * Prints all events received for 15 seconds then exits. */

#include "../src/common/ipc.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

static const char *state_str(PlayerState s) {
    switch(s) {
    case PLAYER_IDLE:    return "IDLE";
    case PLAYER_LOADING: return "LOADING";
    case PLAYER_PLAYING: return "PLAYING";
    case PLAYER_PAUSED:  return "PAUSED";
    case PLAYER_ERROR:   return "ERROR";
    default:             return "?";
    }
}

int main(void)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", AUDIOD_SOCK_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }
    printf("test_play: connected to audiod\n");

    /* Receive the initial EVT_INDEX_READY */
    IpcMsg msg;
    ssize_t r = recv(fd, &msg, sizeof(msg), 0);
    if (r > 0 && msg.type == EVT_INDEX_READY)
        printf("test_play: index ready — %u tracks\n", msg.param.track_count);

    /* Send CMD_PLAY */
    IpcMsg cmd = { .type = CMD_PLAY, .seq = 1 };
    send(fd, &cmd, sizeof(cmd), 0);
    printf("test_play: sent CMD_PLAY\n");

    /* Listen for events for 15 seconds */
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    time_t end = time(NULL) + 15;
    while (time(NULL) < end) {
        r = recv(fd, &msg, sizeof(msg), 0);
        if (r <= 0) { continue; }
        switch ((IpcMsgType)msg.type) {
        case EVT_STATE:
            printf("  [EVENT] STATE → %s\n", state_str(msg.param.player_state));
            break;
        case EVT_TRACK:
            printf("  [EVENT] TRACK → '%s' by '%s'\n",
                   msg.param.track.title, msg.param.track.artist);
            break;
        case EVT_POSITION:
            printf("  [EVENT] POSITION → %u ms\n", msg.param.position_ms);
            break;
        case EVT_INDEX_READY:
            printf("  [EVENT] INDEX_READY → %u tracks\n", msg.param.track_count);
            break;
        case EVT_ERROR:
            printf("  [EVENT] ERROR → %d\n", msg.param.error_code);
            break;
        default:
            printf("  [EVENT] unknown 0x%02x\n", msg.type);
        }
        fflush(stdout);
    }

    close(fd);
    printf("test_play: done\n");
    return 0;
}
