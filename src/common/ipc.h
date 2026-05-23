#pragma once
#include <stdint.h>
#include <assert.h>
#include "track.h"

#ifdef SIMULATE
#define AUDIOD_SOCK_PATH  "/tmp/egepod_audiod.sock"
#define PWRD_SOCK_PATH    "/tmp/egepod_pwrd.sock"
#else
#define AUDIOD_SOCK_PATH  "/dev/socket/egepod_audiod"
#define PWRD_SOCK_PATH    "/dev/socket/egepod_pwrd"
#endif
#define MAX_IPC_CLIENTS   4

/* ── message types ─────────────────────────────────────────────────────── */
typedef enum {
    /* Commands: UI → audiod */
    CMD_PLAY        = 0x01,
    CMD_PAUSE       = 0x02,
    CMD_STOP        = 0x03,
    CMD_NEXT        = 0x04,
    CMD_PREV        = 0x05,
    CMD_SEEK        = 0x06,   /* param.seek_ms */
    CMD_LOAD_TRACK  = 0x07,   /* param.track_idx */
    CMD_GET_STATE   = 0x08,
    CMD_GET_INDEX   = 0x09,
    CMD_GET_TRACK_INFO = 0x0A,   /* param.track_idx → EVT_TRACK_INFO (no decode) */

    /* Events: audiod → UI */
    EVT_STATE       = 0x81,   /* param.player_state */
    EVT_TRACK       = 0x82,   /* param.track */
    EVT_POSITION    = 0x83,   /* param.position_ms */
    EVT_INDEX_READY = 0x84,   /* param.track_count */
    EVT_ERROR       = 0x85,   /* param.error_code */
    EVT_TRACK_INFO  = 0x86,   /* param.track — metadata only, no decode */

    /* Commands: UI → pwrd */
    CMD_SCREEN_OFF  = 0x41,
    CMD_SCREEN_ON   = 0x42,
    CMD_SET_VOLUME  = 0x43,   /* param.volume (0–100) */
    CMD_REBOOT      = 0x44,   /* pwrd broadcasts EVT_SHUTDOWN_PENDING then reboots */
    CMD_SHUTDOWN    = 0x45,   /* pwrd broadcasts EVT_SHUTDOWN_PENDING then powers off */

    /* Events: pwrd → UI */
    EVT_BATTERY          = 0xC1,   /* param.battery_pct */
    EVT_THERMAL          = 0xC2,   /* param.temp_celsius (x10, e.g. 352 = 35.2°C) */
    EVT_VOLUME           = 0xC3,   /* param.volume (0–100) — broadcast on volume change */
    EVT_SHUTDOWN_PENDING = 0xC5,   /* ~500 ms before reboot/poweroff; save state, show splash */
} IpcMsgType;

typedef enum {
    PLAYER_IDLE    = 0,
    PLAYER_LOADING = 1,
    PLAYER_PLAYING = 2,
    PLAYER_PAUSED  = 3,
    PLAYER_ERROR   = 4,
} PlayerState;

/* Fixed-size message. SOCK_SEQPACKET guarantees atomic delivery — no framing
 * header required. Pad to 8-byte alignment. */
typedef struct __attribute__((packed)) {
    uint8_t    type;      /* IpcMsgType */
    uint8_t    _pad[3];
    uint32_t   seq;
    union {
        uint32_t   seek_ms;
        uint32_t   track_idx;
        uint32_t   position_ms;
        uint32_t   track_count;
        uint32_t   battery_pct;
        uint32_t   volume;
        int32_t    error_code;
        uint32_t   temp_celsius_x10;
        PlayerState player_state;
        TrackInfo   track;
        uint8_t     raw[sizeof(TrackInfo)];
    } param;
} IpcMsg;

/* Compile-time wire-format guard: if TrackInfo changes size or __packed__
 * is removed, all daemons would silently misparse every message.  Fail the
 * build immediately instead. */
_Static_assert(sizeof(((IpcMsg *)0)->type)
             + sizeof(((IpcMsg *)0)->_pad)
             + sizeof(((IpcMsg *)0)->seq) == 8,
               "IpcMsg header must be exactly 8 bytes (type[1]+pad[3]+seq[4])");
_Static_assert(sizeof(PlayerState) == sizeof(uint32_t),
               "PlayerState enum must be uint32-sized for IPC param union");
