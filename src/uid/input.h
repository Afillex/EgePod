#pragma once
#include <stdint.h>

typedef enum {
    INPUT_NONE = 0,
    INPUT_TAP,          /* single tap: x, y */
    INPUT_SWIPE_LEFT,
    INPUT_SWIPE_RIGHT,
    INPUT_SWIPE_UP,
    INPUT_SWIPE_DOWN,
    INPUT_POWER_BUTTON,
    INPUT_VOLUME_UP,
    INPUT_VOLUME_DOWN,
} InputEvent;

typedef struct {
    InputEvent type;
    int        x;
    int        y;
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
