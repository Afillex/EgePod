#pragma once
#include "player.h"

/* Headphone-jack watcher.
 * Monitors EGEPOD_H2W_FILE (default: /sys/class/switch/h2w/state on hardware,
 * /tmp/egepod_h2w in simulation) via inotify.  On unplug (state→0) the player
 * is paused directly (in-process, no IPC round-trip). */

typedef struct H2wWatcher H2wWatcher;

H2wWatcher *h2w_start(Player *player);
void        h2w_stop(H2wWatcher *w);
