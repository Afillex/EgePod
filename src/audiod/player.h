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

/* Must be called periodically (~1 Hz) to push EVT_POSITION to subscribers.
 * Returns position_ms, or UINT32_MAX if not playing. */
uint32_t player_get_position(const Player *p);

/* Subscribe a client fd to receive unsolicited events (track change, position).
 * Returns 0 on success. */
int player_subscribe(Player *p, int client_fd);
void player_unsubscribe(Player *p, int client_fd);
