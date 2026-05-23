#pragma once
#include <stdint.h>
#include "../common/ipc.h"
#include "../common/track.h"

typedef struct Player Player;

/* Create a player instance. index_root is the fully-built music index. */
Player *player_create(IndexNode *index_root);
void    player_destroy(Player *p);

/* Dispatch a command from an IPC client.
 * Returns an IpcMsg to send back to the client (type EVT_*), or a msg with
 * type 0 if no reply is needed. */
IpcMsg  player_handle_cmd(Player *p, const IpcMsg *cmd);

/* Optional state-change callback for audiod main (timerfd arm/disarm).
 * Must be called before the epoll event loop starts. */
void player_set_state_callback(Player *p,
                                void (*cb)(PlayerState s, void *ud), void *ud);

/* Must be called periodically (~1 Hz) to push EVT_POSITION to subscribers.
 * Returns position_ms, or UINT32_MAX if not playing/paused. */
uint32_t    player_get_position(Player *p);

/* Send EVT_POSITION to all subscribers under p->lock.  Call from audiod main's
 * tfd tick instead of a bare send() loop to prevent concurrent-send races with
 * the playback thread's publish_state calls (both funnel through p->lock). */
void player_broadcast_position(Player *p, uint32_t pos_ms);
size_t      player_get_track_idx(Player *p);
PlayerState player_get_state(Player *p);

/* Subscribe a client fd to receive unsolicited events (track change, position).
 * Returns 0 on success. */
int player_subscribe(Player *p, int client_fd);
void player_unsubscribe(Player *p, int client_fd);
