#include "persist.h"
#include "../common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#ifdef SIMULATE
# define DEFAULT_STATE_FILE "/tmp/egepod_state"
#else
# define DEFAULT_STATE_FILE "/data/egepod/state"
#endif

#define PERSIST_MAGIC  0x45474F50u   /* "EGOP" */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t track_idx;
    uint32_t position_ms;
    uint8_t  was_playing;
    uint8_t  _pad[3];
} PersistState;

static const char *state_path(void)
{
    const char *p = getenv("EGEPOD_STATE_FILE");
    return (p && p[0]) ? p : DEFAULT_STATE_FILE;
}

int persist_save(uint32_t track_idx, uint32_t position_ms, int was_playing)
{
    const char *path = state_path();
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;

    PersistState s = {
        .magic       = PERSIST_MAGIC,
        .track_idx   = track_idx,
        .position_ms = position_ms,
        .was_playing = (uint8_t)(was_playing ? 1 : 0),
    };
    int ok = (fwrite(&s, sizeof(s), 1, f) == 1);
    fclose(f);
    if (!ok) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

int persist_load(uint32_t *track_idx, uint32_t *position_ms, int *was_playing)
{
    const char *path = state_path();
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    PersistState s;
    int ok = (fread(&s, sizeof(s), 1, f) == 1);
    fclose(f);

    if (!ok || s.magic != PERSIST_MAGIC) {
        LOGW("persist: invalid state file at %s", path);
        return -1;
    }

    *track_idx   = s.track_idx;
    *position_ms = s.position_ms;
    *was_playing = s.was_playing;
    LOGI("persist: loaded track=%u pos=%ums playing=%d",
         s.track_idx, s.position_ms, s.was_playing);
    return 0;
}
