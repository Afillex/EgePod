#pragma once
#include <stdint.h>
#include <stddef.h>

#define TRACK_MAX_PATH   512
#define TRACK_MAX_STR    256

typedef enum {
    FMT_UNKNOWN = 0,
    FMT_FLAC,
    FMT_WAV,
    FMT_MP3,
} AudioFormat;

typedef struct {
    char        path[TRACK_MAX_PATH];
    char        title[TRACK_MAX_STR];
    char        artist[TRACK_MAX_STR];
    char        album[TRACK_MAX_STR];
    uint32_t    duration_ms;
    uint32_t    sample_rate;
    uint32_t    channels;
    uint32_t    bits_per_sample;
    AudioFormat format;
} TrackInfo;

/* Tree node for the music index.
 * Directories own a heap-allocated children array.
 * Leaf nodes store a TrackInfo inline. */
typedef struct IndexNode IndexNode;
struct IndexNode {
    char       name[TRACK_MAX_STR];
    int        is_dir;
    IndexNode *parent;
    union {
        struct {
            IndexNode **children;
            size_t      child_count;
            size_t      child_cap;
        };
        TrackInfo track;
    };
};
