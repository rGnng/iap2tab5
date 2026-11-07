#ifndef IAP2TAB5_PLAYER_H
#define IAP2TAB5_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/* Raw album count regardless of connection; used at boot to load the
 * packed thumbnail cache. */
int player_library_album_count(void);

/* Latest iAP2 Now Playing artwork as 360x360 RGB565. revision changes when a
 * new image arrives or the track change invalidates the previous image. */
uint32_t player_artwork_revision(void);
bool player_artwork_copy(uint8_t * dst, size_t len);

/* Remote AFC artwork path for an album ID (real imported library only). */
bool player_artwork_remote(uint64_t album_id, char * out, size_t out_len);
bool player_album_get(int index, player_album_t *out);

/* Album list order: 0 = added date, 1 = name, 2 = year.  Sorting is applied
 * in place; album IDs and track associations stay stable. */
void player_sort_albums(int field, bool desc);

/* iAP2 media library update: remember when an album PID was first seen.
 * Used as the local fallback order for albums without an AFC date. */
void player_note_iap2_album(uint64_t album_pid);

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
int player_queue_index(void);
bool player_queue_at(int index, player_track_t *out);

/* Commands -------------------------------------------------------------- */
void player_play_pause(void);
void player_next(void);
void player_previous(void);
void player_seek_sec(uint32_t sec);
void player_set_volume(int volume);
void player_mute_toggle(void);
void player_toggle_star(void);
/* Clear all local stars and remove stars.csv (debug/reset). */
void player_clear_stars(void);
void player_set_shuffle(bool on);
void player_cycle_repeat(void);

void player_play_track(uint64_t track_id);
void player_play_album(uint64_t album_id);
void player_play_artist(uint64_t artist_id);
void player_play_playlist(int index);
void player_play_all(void);
void player_queue_jump(int index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IAP2TAB5_PLAYER_H */
