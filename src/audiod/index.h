#pragma once
#include "../common/track.h"

/* Build an in-RAM directory tree rooted at `music_dir`.
 * Returns the root IndexNode on success, NULL on failure.
 * Caller owns the tree; free with index_free(). */
IndexNode *index_build(const char *music_dir);

/* Total number of leaf (track) nodes in the tree. */
size_t index_track_count(const IndexNode *root);

/* Return the Nth track (depth-first order), or NULL if out of range. */
const TrackInfo *index_get_track(const IndexNode *root, size_t n);

/* Free the entire tree. */
void index_free(IndexNode *node);
