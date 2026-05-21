#include "index.h"
#include "../common/log.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* ── helpers ───────────────────────────────────────────────────────────── */

static const char *file_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static int is_audio(const char *name)
{
    const char *e = file_ext(name);
    return !strcasecmp(e, "flac") ||
           !strcasecmp(e, "wav")  ||
           !strcasecmp(e, "mp3");
}

static AudioFormat ext_to_fmt(const char *name)
{
    const char *e = file_ext(name);
    if (!strcasecmp(e, "flac")) return FMT_FLAC;
    if (!strcasecmp(e, "wav"))  return FMT_WAV;
    if (!strcasecmp(e, "mp3"))  return FMT_MP3;
    return FMT_UNKNOWN;
}

static IndexNode *node_new(const char *name, int is_dir, IndexNode *parent)
{
    IndexNode *n = calloc(1, sizeof(IndexNode));
    if (!n) return NULL;
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->is_dir = is_dir;
    n->parent = parent;
    return n;
}

static int node_add_child(IndexNode *parent, IndexNode *child)
{
    if (parent->child_count >= parent->child_cap) {
        size_t newcap = parent->child_cap ? parent->child_cap * 2 : 8;
        IndexNode **tmp = realloc(parent->children,
                                  newcap * sizeof(IndexNode *));
        if (!tmp) return -1;
        parent->children = tmp;
        parent->child_cap = newcap;
    }
    parent->children[parent->child_count++] = child;
    return 0;
}

/* ── recursive scan ────────────────────────────────────────────────────── */

static void scan_dir(IndexNode *dir_node, const char *path)
{
    DIR *dp = opendir(path);
    if (!dp) {
        LOGW("index: cannot open %s", path);
        return;
    }

    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;   /* skip hidden / . / .. */

        char child_path[TRACK_MAX_PATH];
        snprintf(child_path, sizeof(child_path), "%s/%s", path, de->d_name);

        struct stat st;
        if (stat(child_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            IndexNode *sub = node_new(de->d_name, 1, dir_node);
            if (!sub) continue;
            scan_dir(sub, child_path);
            /* Only attach non-empty dirs */
            if (sub->child_count > 0)
                node_add_child(dir_node, sub);
            else
                index_free(sub);
        } else if (S_ISREG(st.st_mode) && is_audio(de->d_name)) {
            IndexNode *leaf = node_new(de->d_name, 0, dir_node);
            if (!leaf) continue;
            TrackInfo *ti = &leaf->track;
            snprintf(ti->path, sizeof(ti->path), "%s", child_path);
            /* Default title = filename without extension */
            snprintf(ti->title, sizeof(ti->title), "%s", de->d_name);
            char *dot = strrchr(ti->title, '.');
            if (dot) *dot = '\0';
            /* Default artist/album = parent directory basename */
            const char *dir_base = strrchr(dir_node->name, '/');
            dir_base = dir_base ? dir_base + 1 : dir_node->name;
            if (dir_base[0] == '\0') dir_base = "Unknown";
            snprintf(ti->artist, sizeof(ti->artist), "%s", dir_base);
            snprintf(ti->album,  sizeof(ti->album),  "%s", dir_base);
            ti->format = ext_to_fmt(de->d_name);
            /* Metadata (tags) are populated lazily by the decoder on first load. */
            node_add_child(dir_node, leaf);
        }
    }
    closedir(dp);
}

/* ── public API ────────────────────────────────────────────────────────── */

IndexNode *index_build(const char *music_dir)
{
    IndexNode *root = node_new(music_dir, 1, NULL);
    if (!root) return NULL;
    scan_dir(root, music_dir);
    LOGI("index: scanned '%s', %zu tracks", music_dir, index_track_count(root));
    return root;
}

size_t index_track_count(const IndexNode *node)
{
    if (!node) return 0;
    if (!node->is_dir) return 1;
    size_t count = 0;
    for (size_t i = 0; i < node->child_count; i++)
        count += index_track_count(node->children[i]);
    return count;
}

const TrackInfo *index_get_track(const IndexNode *node, size_t n)
{
    if (!node) return NULL;
    if (!node->is_dir) return (n == 0) ? &node->track : NULL;
    for (size_t i = 0; i < node->child_count; i++) {
        size_t sub = index_track_count(node->children[i]);
        if (n < sub) return index_get_track(node->children[i], n);
        n -= sub;
    }
    return NULL;
}

void index_free(IndexNode *node)
{
    if (!node) return;
    if (node->is_dir) {
        for (size_t i = 0; i < node->child_count; i++)
            index_free(node->children[i]);
        free(node->children);
    }
    free(node);
}
