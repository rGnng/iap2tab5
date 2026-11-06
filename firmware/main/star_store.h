#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "player.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-library star persistence.
 *
 * `stars.csv` keeps one row per track that has ever been toggled. The row
 * holds the current state plus enough metadata to locate the track in iTunes
 * manually (persistent ID, title, artist, album, track number, duration).
 *
 * Load applies every starred=1 row to track_starred[]. Update rewrites the
 * file atomically (tmp + rename) and keeps exactly one row per track ID. */

void star_store_load(const char * path,
                     const player_track_t * tracks, int track_count,
                     bool * track_starred);

void star_store_update(const char * path, const char * library_id,
                       const char * device_name,
                       const player_track_t * track, bool starred);

/* Remove the CSV and any temporary file. Used by the debug reset command. */
void star_store_clear(const char * path);

#ifdef __cplusplus
}
#endif
