#pragma once
#include <stdint.h>

typedef enum {
    INPUT_NONE = 0,
    INPUT_TAP,               /* single tap: x, y */
    INPUT_SWIPE_LEFT,
    INPUT_SWIPE_RIGHT,
    INPUT_SWIPE_UP,
    INPUT_SWIPE_DOWN,
    INPUT_POWER_BUTTON,      /* value=1 press, value=0 release */
    INPUT_VOLUME_UP,
    INPUT_VOLUME_DOWN,
    INPUT_POWER_BUTTON_LONG, /* ≥400 ms hold — sim: Shift+P in viewer */
} InputEvent;

typedef struct {
    InputEvent type;
    int        x;
    int        y;
    int        value;   /* INPUT_POWER_BUTTON: 1=press 0=release; others: unused */
} InputEvt;

typedef struct InputCtx InputCtx;

/* Open all /dev/input/event* devices and register them on epoll_fd.
 * epoll_fd should already be open.
 * Returns opaque context, or NULL on failure. */
InputCtx *input_open(int epoll_fd);
void      input_close(InputCtx *ctx);

/* Process a ready input fd. Returns decoded high-level event, or INPUT_NONE.
 * Call this when epoll reports activity on one of the input fds. */
InputEvt input_process_fd(InputCtx *ctx, int ready_fd);

/* Arm (on=1) or disarm (on=0) the sim tap-polling timerfd.
 * No-op on production builds and when ctx is NULL. */
void input_set_polling(InputCtx *ctx, int on);
