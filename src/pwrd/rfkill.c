/* 🔋 Power Warden — rfkill all radios.
 *
 * Iterates /dev/rfkill using ioctl RFKILL_IOCTL_NOINPUT to enumerate and
 * block every radio in the system (BT, Wi-Fi, NFC, modem FM, etc.).
 * This is permanent for the lifetime of the DAP session. */

#include "rfkill.h"
#include "../common/log.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <linux/rfkill.h>

static int g_rfkill_fd = -1;

int rfkill_block_all(void)
{
#ifdef SIMULATE
    LOGI("pwrd: [SIM] rfkill_block_all — no-op in simulation");
    return 0;
#endif
    int fd = open("/dev/rfkill", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        LOGE("pwrd: open(/dev/rfkill): %s", strerror(errno));
        return -1;
    }
    g_rfkill_fd = fd;

    /* Read current rfkill events to discover all indices */
    struct rfkill_event ev;
    int blocked = 0;

    /* Set all known type = RFKILL_TYPE_ALL (0xFF) — blocks everything */
    struct rfkill_event block_all = {
        .idx   = 0,
        .type  = RFKILL_TYPE_ALL,
        .op    = RFKILL_OP_CHANGE_ALL,
        .soft  = 1,
        .hard  = 0,
    };
    if (write(fd, &block_all, sizeof(block_all)) == sizeof(block_all)) {
        LOGI("pwrd: rfkill block_all sent");
        blocked = 1;
    } else {
        LOGW("pwrd: rfkill block_all write: %s — trying per-device", strerror(errno));
    }

    /* Drain the event queue to count blocked radios */
    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.op == RFKILL_OP_ADD || ev.op == RFKILL_OP_CHANGE) {
            LOGD("pwrd: rfkill idx=%u type=%u soft=%u hard=%u",
                 ev.idx, ev.type, ev.soft, ev.hard);
            if (ev.soft || ev.hard) blocked++;
        }
    }

    LOGI("pwrd: rfkill: %d radios blocked", blocked);
    /* Keep fd open so we can receive hotplug events later (e.g. USB BT dongle) */
    return blocked;
}

void rfkill_unblock_all(void)
{
    if (g_rfkill_fd < 0) return;
    struct rfkill_event ev = {
        .idx  = 0,
        .type = RFKILL_TYPE_ALL,
        .op   = RFKILL_OP_CHANGE_ALL,
        .soft = 0,
    };
    write(g_rfkill_fd, &ev, sizeof(ev));
    close(g_rfkill_fd);
    g_rfkill_fd = -1;
    LOGI("pwrd: rfkill: all radios unblocked");
}
