#ifndef IAP2TAB5_PLAYER_H
#define IAP2TAB5_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "artwork_quality.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_STOPPED = 0,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
} player_status_t;

typedef enum {
    PLAYER_REPEAT_OFF = 0,
    PLAYER_REPEAT_ALL,
    PLAYER_REPEAT_ONE,
} player_repeat_t;

typedef struct {
    uint64_t id;
    uint64_t album_id;
    uint64_t artist_id;
    char title[96];
    char artist[64];
    char album[64];
    uint32_t duration_sec;
    uint16_t track_no;
    uint16_t year;
    uint16_t disc_no;
} player_track_t;

#define PLAYER_REAL_COVER_BASE 1000

typedef struct {
    uint64_t id;
    uint64_t artist_id;
    uint64_t album_pid;   /* original SQLite/iAP2 album persistent ID */
    char title[80];
    char artist[64];
    uint16_t year;
    uint16_t track_count;
    uint32_t color;      /* 0xRRGGBB fallback shade */
    uint32_t date_added; /* AFC Unix timestamp, 0 when unknown */
    char cover[192];     /* LVGL path, e.g. A:/.../covers/000.png */
    uint16_t cover_index; /* index into the packed raw thumbnail files */
    bool afc_synced;     /* metadata membership; independent of cached artwork */
} player_album_t;

typedef struct {
    uint64_t id;
    char name[64];
    uint16_t song_count;
    uint16_t album_count;
} player_artist_t;

typedef struct {
    uint64_t id;
    char name[64];
    uint16_t track_count;
    bool local; /* Starred is the local playlist */
} player_playlist_t;

typedef struct {
    player_status_t status;
    player_track_t track;
    uint32_t elapsed_sec;
    uint32_t duration_sec;
    int volume;
    bool muted;
    bool starred;
    bool shuffle;
    player_repeat_t repeat;
    bool connected;
    int battery; /* -1 = unknown */
    bool charging;
    bool real_device; /* true when the snapshot comes from the iAP2 backend */
    char device_name[64];
    uint32_t revision;
} player_snapshot_t;

/* Lifecycle ------------------------------------------------------------- */
void player_init(void);
void player_tick(void);
void player_get_snapshot(player_snapshot_t *out);

/* Called whenever the backend revision changes. Runs on the UI thread for
 * the mock backend; a real backend should marshal it to the UI thread. */
typedef void (*player_change_cb_t)(void);
void player_set_change_callback(player_change_cb_t callback);

/* Library --------------------------------------------------------------- */
/* True when a device is connected/identified and its library may be shown. */
bool player_device_available(void);

int player_album_count(void);

/* Stored library counts, available without a connection for cache setup
 * and the AFC library summary. */
int player_library_album_count(void);
int player_library_track_count(void);

/* Latest iAP2 Now Playing artwork as 360x360 RGB565. revision changes when a
 * new image arrives or the track change invalidates the previous image. */
uint32_t player_artwork_revision(void);
/* Copy only if the original image meets minimum quality. Report the received
 * quality even when rejecting a smaller image; leave dst untouched on failure. */
bool player_artwork_copy(uint8_t * dst, size_t len, artwork_quality_t minimum,
                         artwork_quality_t * actual);

/* Remote AFC artwork path for an album ID (real imported library only). */
bool player_artwork_remote(uint64_t album_id, char * out, size_t out_len);
bool player_album_get(int index, player_album_t *out);

/* Album list order: 0 = added date, 1 = name, 2 = year.  Sorting is applied
 * in place; album IDs and track associations stay stable. On hardware,
 * albums with tracks absent from AFC metadata come first, with the selected
 * sort applied within both groups, independently of thumbnail availability. */
void player_sort_albums(int field, bool desc);
/* Independent Songs view order: 0 = date added, 1 = name, 2 = year.
 * Sort offsets rather than track records so playback IDs and memberships stay stable. */
void player_sort_songs(int field, bool desc);
int player_song_index(uint64_t track_pid);

/* iAP2 media library update: remember when an album PID was first seen.
 * Used as the local fallback order for albums without an AFC date. */
void player_note_iap2_album(uint64_t album_pid);
uint64_t player_legacy_album_pid(uint32_t slot);
/* Media item property numbers follow the iAP2 MediaItem property group. */
typedef struct {
    uint64_t pid, album_pid, artist_pid, album_artist_pid;
    uint32_t fields, duration_ms;
    uint16_t track_no, year, disc_no;
    char title[96], album[80], artist[64], album_artist[64];
} player_media_item_t;
void player_library_select(const char *library_id);
void player_library_update(const player_media_item_t *item);
void player_library_delete(uint64_t track_pid);
void player_library_reset(void);
void player_library_complete(void);
void player_library_complete_revision(const char *revision);
bool player_library_cached_revision(const char *library_id, char *out, size_t size);
void player_library_refresh_afc(void);
/* Drain bounded updates on the UI thread; return true when rows changed. */
bool player_library_poll(void);
uint32_t player_library_revision(void);
bool player_library_album_pid(uint64_t track_pid, uint64_t *album_pid);
typedef struct { uint32_t albums, tracks, pending_albums, revision; } player_library_status_t;
void player_library_status(player_library_status_t *out);
/* A verified legacy upload supersedes the live snapshot until its reboot. */
void player_library_discard_snapshot(void);

int player_artist_count(void);
bool player_artist_get(int index, player_artist_t *out);
int player_song_count(void);
bool player_song_get(int index, player_track_t *out);

int player_album_track_count(uint64_t album_id);
bool player_album_track_get(uint64_t album_id, int index, player_track_t *out);
int player_artist_song_count(uint64_t artist_id);
bool player_artist_song_get(uint64_t artist_id, int index, player_track_t *out);

int player_playlist_count(void);
bool player_playlist_get(int index, player_playlist_t *out);
int player_playlist_track_count(int index);
bool player_playlist_track_get(int index, int track_index, player_track_t *out);

int player_queue_count(void);
/* Changes whenever the queue contents change (new list, iPod queue update). */
uint32_t player_queue_revision(void);
int player_queue_index(void);
bool player_queue_at(int index, player_track_t *out);

/* Commands -------------------------------------------------------------- */
void player_play_pause(void);
void player_next(void);
void player_previous(void);
void player_seek_sec(uint32_t sec);
/* One native volume step: negative = down, positive = up, zero = no-op.
 * The iPod's absolute volume is not reported by the current iAP2 session. */
void player_step_volume(int direction);
void player_mute_toggle(void);
void player_toggle_star(void);
/* Clear all local stars and remove stars.csv (debug/reset). */
void player_clear_stars(void);
void player_set_shuffle(bool on);
void player_cycle_repeat(void);

/* Each "from" call starts the chosen item and queues what follows it in the
 * list the UI shows; see mock_player.c for the ordering rules. */
void player_play_album(uint64_t album_id);
void player_play_album_from(uint64_t album_id, int track_index);
void player_play_album_shuffled(uint64_t album_id);
void player_play_artist(uint64_t artist_id);
void player_play_artist_from(uint64_t artist_id, uint64_t track_id);
void player_play_artist_shuffled(uint64_t artist_id);
void player_play_song(uint64_t track_id);
void player_play_playlist(int index);
void player_play_playlist_from(int index, int track_index);
void player_play_playlist_shuffled(int index);
/* Whole library in random order (Albums "Shuffle All"). */
void player_play_all_shuffled(void);
void player_queue_jump(int index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IAP2TAB5_PLAYER_H */
