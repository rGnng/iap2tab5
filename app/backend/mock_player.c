#include "player.h"

#include "cJSON.h"


#if defined(ESP_PLATFORM)
#include "afc_client.h"
#include "app_config.h"
#include "star_store.h"
#include "esp_log.h"
#include "iap2_host.h"
static const char * TAG = "player";
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MOCK_ASSET_DIR
#define MOCK_ASSET_DIR "."
#endif

/* The mock library is large (~300 KB of static data). On the Tab5 it must
 * live in PSRAM so the internal RAM stays available to LVGL and the drivers. */
#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define MOCK_LARGE_BSS EXT_RAM_BSS_ATTR
#else
#define MOCK_LARGE_BSS
#endif

#define MAX_TRACKS 24000
#define MAX_ALBUMS 4096
#define MAX_ARTISTS 2048
#define MAX_PLAYLISTS 32
#define MAX_PLAYLIST_TRACKS 1024
#define MAX_QUEUE 24000

static MOCK_LARGE_BSS player_track_t tracks[MAX_TRACKS];
static int track_count;

static MOCK_LARGE_BSS player_album_t albums[MAX_ALBUMS];
static int album_count;

static MOCK_LARGE_BSS player_artist_t artists[MAX_ARTISTS];
static int artist_count;

typedef struct {
    uint64_t id;
    char name[64];
    uint64_t track_ids[MAX_PLAYLIST_TRACKS];
    int track_count;
} device_playlist_t;

static MOCK_LARGE_BSS device_playlist_t playlists[MAX_PLAYLISTS];
static int playlist_count;

static MOCK_LARGE_BSS uint64_t queue[MAX_QUEUE];
static int queue_count;
static int queue_index;

static player_status_t status;
static uint32_t elapsed_sec;
static int volume = 72;
static bool muted;
static MOCK_LARGE_BSS bool track_starred[MAX_TRACKS];
static bool shuffle;
static player_repeat_t repeat_mode = PLAYER_REPEAT_ALL;
static bool connected = true;
static int battery = 86;
static bool charging;
static char device_name[64] = "iPod touch";
static uint32_t revision = 1;
static player_change_cb_t change_callback;
static bool real_library;
static MOCK_LARGE_BSS char artwork_remote[MAX_ALBUMS][128];
static int artwork_count;

static void copy_str(char *dst, size_t size, const char *src)
{
    if(size == 0) return;
    size_t n = src ? strlen(src) : 0;
    if(n >= size) n = size - 1;
    if(n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

#if defined(ESP_PLATFORM)
static bool iap2_active(iap2_state_t * state)
{
    return iap2_host_is_enabled() && iap2_host_get_state(state) &&
           state->identified;
}

static uint64_t iap2_track_id(const iap2_state_t * state)
{
    if(state->track_key != 0) return state->track_key;
    if(state->title[0] == '\0') return 0;
    uint64_t hash = 1469598103934665603ULL;
    const char * parts[3] = { state->title, state->artist, state->album };
    for(int p = 0; p < 3; p++) {
        for(const char * c = parts[p]; *c != '\0'; c++) {
            hash ^= (uint8_t)*c;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xFF;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

static uint64_t iap2_album_id(const iap2_state_t * state)
{
    if(state->album[0] == '\0') return 0;

    uint64_t title_match = 0;
    int title_matches = 0;
    for(int i = 0; i < album_count; i++) {
        if(strcmp(albums[i].title, state->album) != 0) continue;
        if(state->artist[0] != '\0' &&
           strcmp(albums[i].artist, state->artist) == 0) {
            return albums[i].id;
        }
        title_match = albums[i].id;
        title_matches++;
    }
    /* iAP2 tags and the SQLite tags sometimes differ by punctuation or
     * spaces in the artist name (e.g. "Bubble Records" vs "BubbleRecords").
     * A unique album title is still a safe match. */
    return title_matches == 1 ? title_match : 0;
}

static int track_index_by_id(uint64_t id);
static int track_index_by_meta(const char * title, const char * album);

static void iap2_snapshot(player_snapshot_t * out, const iap2_state_t * state)
{
    memset(out, 0, sizeof(*out));
    out->real_device = true;
    out->status = state->play_status == 1 ? PLAYER_PLAYING :
                  (state->play_status == 2 ? PLAYER_PAUSED : PLAYER_STOPPED);
    copy_str(out->track.title, sizeof(out->track.title), state->title);
    copy_str(out->track.artist, sizeof(out->track.artist), state->artist);
    copy_str(out->track.album, sizeof(out->track.album), state->album);
    out->track.id = iap2_track_id(state);
    out->track.album_id = iap2_album_id(state);
    out->duration_sec = state->duration_ms / 1000;
    out->elapsed_sec = state->elapsed_ms / 1000;
    out->track.duration_sec = out->duration_sec;
    /* The UI polls the snapshot several times per tick.  Looking up the
     * current track in the full library each time is O(N) and showed up as
     * 14 ms per call on a large library.  Cache the index until the Now
     * Playing identity changes; the starred flag itself is read live. */
    static uint64_t star_key;
    static char star_title[128];
    static char star_album[96];
    static int star_idx = -1;
    if(state->track_key != star_key ||
       strcmp(star_title, state->title) != 0 ||
       strcmp(star_album, state->album) != 0) {
        star_key = state->track_key;
        snprintf(star_title, sizeof(star_title), "%s", state->title);
        snprintf(star_album, sizeof(star_album), "%s", state->album);
        star_idx = state->track_key != 0
                       ? track_index_by_id(state->track_key) : -1;
        if(star_idx < 0) {
            star_idx = track_index_by_meta(state->title, state->album);
        }
    }
    if(star_idx >= 0) out->starred = track_starred[star_idx];
    out->volume = volume;
    out->muted = muted;
    out->shuffle = state->shuffle;
    out->repeat = state->repeat == 1 ? PLAYER_REPEAT_ONE :
                  (state->repeat == 2 ? PLAYER_REPEAT_ALL :
                   PLAYER_REPEAT_OFF);
    out->connected = state->connected;
    out->battery = state->battery;
    out->charging = state->charging;
    out->revision = state->revision;
    copy_str(out->device_name, sizeof(out->device_name), state->device_name);
}
#endif

static void bump_revision(void)
{
    revision++;
    if(change_callback) change_callback();
}

void player_set_change_callback(player_change_cb_t callback)
{
    change_callback = callback;
}

static const player_track_t *track_by_id(uint64_t id)
{
    for(int i = 0; i < track_count; i++) {
        if(tracks[i].id == id) return &tracks[i];
    }
    return NULL;
}

static int track_index_by_id(uint64_t id)
{
    for(int i = 0; i < track_count; i++) {
        if(tracks[i].id == id) return i;
    }
    return -1;
}

/* iAP2 tags and the SQLite tags occasionally differ by punctuation. Prefer
 * an exact title+album match, then a unique title match. */
#if defined(ESP_PLATFORM)
static int track_index_by_meta(const char * title, const char * album)
{
    if(title == NULL || title[0] == '\0') return -1;

    int title_match = -1;
    int title_matches = 0;
    for(int i = 0; i < track_count; i++) {
        if(strcmp(tracks[i].title, title) != 0) continue;
        if(album != NULL && album[0] != '\0' &&
           strcmp(tracks[i].album, album) == 0) {
            return i;
        }
        title_match = i;
        title_matches++;
    }
    return title_matches == 1 ? title_match : -1;
}
#endif

static void set_queue_from_tracks(const int *indices, int count, int start)
{
    if(count <= 0 || count > MAX_QUEUE) return;
    queue_count = count;
    for(int i = 0; i < count; i++) {
        queue[i] = tracks[indices[i]].id;
    }
    if(start < 0) start = 0;
    if(start >= count) start = count - 1;
    queue_index = start;
    elapsed_sec = 0;
    status = PLAYER_PLAYING;
    bump_revision();
}

static void queue_all_tracks(void)
{
    queue_count = 0;
    for(int i = 0; i < track_count && queue_count < MAX_QUEUE; i++) {
        queue[queue_count++] = tracks[i].id;
    }
    if(queue_count > 0) {
        queue_index = 0;
        elapsed_sec = 0;
        status = PLAYER_PLAYING;
        bump_revision();
    }
}

static char * read_file(const char * path, size_t * out_size)
{
    FILE * file = fopen(path, "rb");
    if(file == NULL) {
        fprintf(stderr, "[library] cannot open %s\n", path);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if(size <= 0) {
        fclose(file);
        return NULL;
    }
    char * data = malloc((size_t)size + 1);
    if(data == NULL) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[read] = '\0';
    if(out_size) *out_size = read;
    return data;
}

static uint64_t json_u64(const cJSON * object, const char * key)
{
    const cJSON * item = cJSON_GetObjectItemCaseSensitive(object, key);
    return item ? (uint64_t)item->valuedouble : 0;
}

static const char * json_str(const cJSON * object, const char * key)
{
    const cJSON * item = cJSON_GetObjectItemCaseSensitive(object, key);
    return (item && cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
}


#pragma pack(push, 1)
typedef struct {
    uint64_t id;
    char name[64];
} lib_artist_t;

typedef struct {
    uint64_t id;
    uint64_t artist_id;
    uint32_t year;
    uint32_t track_count;
    uint32_t color;
    uint32_t cover_index;
    char title[80];
    char artist[64];
} lib_album_t;

typedef struct {
    uint64_t id;
    uint64_t album_id;
    uint64_t artist_id;
    uint32_t duration_sec;
    uint16_t track_no;
    uint16_t year;
    char title[96];
    char artist[64];
    char album[64];
} lib_track_t;

typedef struct {
    uint64_t id;
    char name[64];
    uint32_t track_count;
} lib_playlist_t;

typedef struct {
    uint32_t album_id;
    char remote[128];
} lib_artwork_t;
#pragma pack(pop)

static uint32_t rd_le32(const uint8_t * p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool load_library_bin(const char * path)
{
    FILE * f = fopen(path, "rb");
    if(f == NULL) return false;

    uint8_t header[36];
    if(fread(header, 1, 32, f) != 32) {
        fclose(f);
        return false;
    }
    bool v2 = memcmp(header, "T5LIB002", 8) == 0 &&
              rd_le32(header + 8) == 2;
    if(!v2 && !(memcmp(header, "T5LIB001", 8) == 0 &&
                rd_le32(header + 8) == 1)) {
        fclose(f);
        return false;
    }

    uint32_t wa = rd_le32(header + 12);
    uint32_t wl = rd_le32(header + 16);
    uint32_t wt = rd_le32(header + 20);
    uint32_t wp = rd_le32(header + 24);
    uint32_t wc = 0;
    if(v2) {
        uint8_t reserved[4];
        if(fread(reserved, 1, sizeof(reserved), f) != sizeof(reserved)) {
            fclose(f);
            return false;
        }
        wc = rd_le32(header + 28);
    }

    uint32_t ca = wa < MAX_ARTISTS ? wa : MAX_ARTISTS;
    uint32_t cl = wl < MAX_ALBUMS ? wl : MAX_ALBUMS;
    uint32_t ct = wt < MAX_TRACKS ? wt : MAX_TRACKS;
    uint32_t cp = wp < MAX_PLAYLISTS ? wp : MAX_PLAYLISTS;

    artist_count = 0;
    album_count = 0;
    track_count = 0;
    playlist_count = 0;
    artwork_count = 0;
    memset(artwork_remote, 0, sizeof(artwork_remote));

    for(uint32_t i = 0; i < ca; i++) {
        lib_artist_t w;
        if(fread(&w, sizeof(w), 1, f) != 1) goto fail;
        player_artist_t * a = &artists[artist_count++];
        memset(a, 0, sizeof(*a));
        a->id = w.id;
        copy_str(a->name, sizeof(a->name), w.name);
    }
    if(wa > ca) fseek(f, (long)(wa - ca) * sizeof(lib_artist_t), SEEK_CUR);

    for(uint32_t i = 0; i < cl; i++) {
        lib_album_t w;
        if(fread(&w, sizeof(w), 1, f) != 1) goto fail;
        player_album_t * a = &albums[album_count++];
        memset(a, 0, sizeof(*a));
        a->id = w.id;
        a->artist_id = w.artist_id;
        a->year = (uint16_t)w.year;
        a->track_count = (uint16_t)w.track_count;
        a->color = w.color;
        a->cover_index = (uint16_t)w.cover_index;
        copy_str(a->title, sizeof(a->title), w.title);
        copy_str(a->artist, sizeof(a->artist), w.artist);
    }
    if(wl > cl) fseek(f, (long)(wl - cl) * sizeof(lib_album_t), SEEK_CUR);

    for(uint32_t i = 0; i < ct; i++) {
        lib_track_t w;
        if(fread(&w, sizeof(w), 1, f) != 1) goto fail;
        player_track_t * t = &tracks[track_count++];
        memset(t, 0, sizeof(*t));
        t->id = w.id;
        t->album_id = w.album_id;
        t->artist_id = w.artist_id;
        t->duration_sec = w.duration_sec;
        t->track_no = w.track_no;
        t->year = w.year;
        copy_str(t->title, sizeof(t->title), w.title);
        copy_str(t->artist, sizeof(t->artist), w.artist);
        copy_str(t->album, sizeof(t->album), w.album);
    }
    if(wt > ct) fseek(f, (long)(wt - ct) * sizeof(lib_track_t), SEEK_CUR);

    for(uint32_t i = 0; i < cp; i++) {
        lib_playlist_t w;
        if(fread(&w, sizeof(w), 1, f) != 1) goto fail;
        device_playlist_t * p = &playlists[playlist_count++];
        memset(p, 0, sizeof(*p));
        p->id = w.id;
        copy_str(p->name, sizeof(p->name), w.name);
        uint32_t n = w.track_count;
        uint32_t keep = n < MAX_PLAYLIST_TRACKS ? n : MAX_PLAYLIST_TRACKS;
        for(uint32_t j = 0; j < keep; j++) {
            uint8_t idbuf[8];
            if(fread(idbuf, 1, sizeof(idbuf), f) != sizeof(idbuf)) goto fail;
            p->track_ids[p->track_count++] =
                (uint64_t)idbuf[0] | ((uint64_t)idbuf[1] << 8) |
                ((uint64_t)idbuf[2] << 16) | ((uint64_t)idbuf[3] << 24) |
                ((uint64_t)idbuf[4] << 32) | ((uint64_t)idbuf[5] << 40) |
                ((uint64_t)idbuf[6] << 48) | ((uint64_t)idbuf[7] << 56);
        }
        if(n > keep) fseek(f, (long)(n - keep) * 8, SEEK_CUR);
    }

    for(uint32_t i = 0; i < wc; i++) {
        lib_artwork_t w;
        if(fread(&w, sizeof(w), 1, f) != 1) break;
        if(w.album_id >= 1 && w.album_id <= MAX_ALBUMS) {
            copy_str(artwork_remote[w.album_id - 1],
                     sizeof(artwork_remote[0]), w.remote);
            artwork_count++;
        }
    }

    fclose(f);

    for(int i = 0; i < artist_count; i++) {
        artists[i].song_count = 0;
        artists[i].album_count = 0;
    }
    for(int i = 0; i < album_count; i++) {
        uint64_t id = albums[i].artist_id;
        if(id >= 1 && id <= (uint64_t)artist_count) {
            artists[id - 1].album_count++;
        }
    }
    for(int i = 0; i < track_count; i++) {
        uint64_t id = tracks[i].artist_id;
        if(id >= 1 && id <= (uint64_t)artist_count) {
            artists[id - 1].song_count++;
        }
    }

    real_library = true;
#if defined(ESP_PLATFORM)
    ESP_LOGI(TAG, "binary library: %d albums, %d tracks, %d artists, "
             "%d playlists, %d artwork entries from %s", album_count,
             track_count, artist_count, playlist_count, artwork_count, path);
#endif
    return true;

fail:
    fclose(f);
    artist_count = album_count = track_count = playlist_count = 0;
    return false;
}

static void load_library(void)
{
    char path[512];
    const char * base = "/sdcard";
    char * data = NULL;
    size_t size = 0;

#if defined(ESP_PLATFORM)
    if(app_config_sd_ensure()) {
        const char * lib_path = app_config_library_bin_path();
        if(load_library_bin(lib_path)) return;
        /* Legacy layout: /sdcard/library.bin. */
        if(strcmp(lib_path, "/sdcard/library.bin") != 0 &&
           load_library_bin("/sdcard/library.bin")) {
            return;
        }

        snprintf(path, sizeof(path), "/sdcard/library.json");
        data = read_file(path, &size);
        if(data != NULL) real_library = true;
    }
    /* Hardware has no demo library: no media means no media. */
    if(data == NULL) return;
#else
    base = MOCK_ASSET_DIR;
    snprintf(path, sizeof(path), "%s/library.json", MOCK_ASSET_DIR);
    data = read_file(path, &size);
    real_library = false;
    if(data == NULL) return;
#endif

    cJSON * root = cJSON_ParseWithLength(data, size);
    free(data);
    if(root == NULL) {
        fprintf(stderr, "[library] failed to parse %s\n", path);
        return;
    }

    const cJSON * artist_array = cJSON_GetObjectItemCaseSensitive(root, "artists");
    const cJSON * album_array = cJSON_GetObjectItemCaseSensitive(root, "albums");
    const cJSON * track_array = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    const cJSON * playlist_array = cJSON_GetObjectItemCaseSensitive(root, "playlists");

    if(cJSON_IsArray(artist_array)) {
        const cJSON * item = NULL;
        cJSON_ArrayForEach(item, artist_array) {
            if(artist_count >= MAX_ARTISTS) break;
            player_artist_t * artist = &artists[artist_count++];
            memset(artist, 0, sizeof(*artist));
            artist->id = json_u64(item, "id");
            copy_str(artist->name, sizeof(artist->name), json_str(item, "name"));
        }
    }

    if(cJSON_IsArray(album_array)) {
        const cJSON * item = NULL;
        cJSON_ArrayForEach(item, album_array) {
            if(album_count >= MAX_ALBUMS) break;
            player_album_t * album = &albums[album_count++];
            memset(album, 0, sizeof(*album));
            album->id = json_u64(item, "id");
            album->artist_id = json_u64(item, "artist_id");
            copy_str(album->title, sizeof(album->title), json_str(item, "title"));
            copy_str(album->artist, sizeof(album->artist), json_str(item, "artist"));
            album->year = (uint16_t)json_u64(item, "year");
            album->track_count = (uint16_t)json_u64(item, "track_count");
            album->color = (uint32_t)json_u64(item, "color");
            const char * cover = json_str(item, "cover");
            if(cover[0] != '\0') {
                snprintf(album->cover, sizeof(album->cover), "A:%s/%s",
                         base, cover);
                const char * slash = strrchr(cover, '/');
                const char * file = slash ? slash + 1 : cover;
                album->cover_index = (uint16_t)strtoul(file, NULL, 10);
            }
        }
    }

    if(cJSON_IsArray(track_array)) {
        const cJSON * item = NULL;
        cJSON_ArrayForEach(item, track_array) {
            if(track_count >= MAX_TRACKS) break;
            player_track_t * track = &tracks[track_count++];
            memset(track, 0, sizeof(*track));
            track->id = json_u64(item, "id");
            track->album_id = json_u64(item, "album_id");
            track->artist_id = json_u64(item, "artist_id");
            copy_str(track->title, sizeof(track->title), json_str(item, "title"));
            copy_str(track->artist, sizeof(track->artist), json_str(item, "artist"));
            copy_str(track->album, sizeof(track->album), json_str(item, "album"));
            track->duration_sec = (uint32_t)json_u64(item, "duration_sec");
            track->track_no = (uint16_t)json_u64(item, "track_no");
            track->year = (uint16_t)json_u64(item, "year");
        }
    }

    if(cJSON_IsArray(playlist_array)) {
        const cJSON * item = NULL;
        cJSON_ArrayForEach(item, playlist_array) {
            if(playlist_count >= MAX_PLAYLISTS) break;
            device_playlist_t * playlist = &playlists[playlist_count++];
            memset(playlist, 0, sizeof(*playlist));
            playlist->id = json_u64(item, "id");
            copy_str(playlist->name, sizeof(playlist->name),
                     json_str(item, "name"));
            const cJSON * ids = cJSON_GetObjectItemCaseSensitive(item, "track_ids");
            if(cJSON_IsArray(ids)) {
                const cJSON * id = NULL;
                cJSON_ArrayForEach(id, ids) {
                    if(playlist->track_count >= MAX_PLAYLIST_TRACKS) break;
                    if(cJSON_IsNumber(id)) {
                        playlist->track_ids[playlist->track_count++] =
                            (uint64_t)id->valuedouble;
                    }
                }
            }
        }
    }

    cJSON_Delete(root);

    /* Derive artist counters from the imported albums/tracks. */
    for(int i = 0; i < artist_count; i++) {
        artists[i].song_count = 0;
        artists[i].album_count = 0;
    }
    for(int i = 0; i < album_count; i++) {
        for(int j = 0; j < artist_count; j++) {
            if(artists[j].name[0] != '\0' &&
               strcmp(artists[j].name, albums[i].artist) == 0) {
                artists[j].album_count++;
                break;
            }
        }
    }
    for(int i = 0; i < track_count; i++) {
        for(int j = 0; j < artist_count; j++) {
            if(artists[j].id == tracks[i].artist_id) {
                artists[j].song_count++;
                break;
            }
        }
    }

    fprintf(stderr, "[library] loaded %d albums, %d tracks, %d artists, %d playlists from %s\n",
            album_count, track_count, artist_count, playlist_count, path);
#if defined(ESP_PLATFORM)
    ESP_LOGI(TAG, "library: %d albums, %d tracks, %d artists, %d playlists "
             "from %s", album_count, track_count, artist_count, playlist_count,
             path);
#endif
}

void player_init(void)
{
    track_count = 0;
    album_count = 0;
    artist_count = 0;
    playlist_count = 0;
    queue_count = 0;
    queue_index = 0;
    status = PLAYER_PAUSED;
    elapsed_sec = 0;
    volume = 72;
    muted = false;
    memset(track_starred, 0, sizeof(track_starred));
    shuffle = false;
    repeat_mode = PLAYER_REPEAT_ALL;
    connected = true;
    battery = 86;
    charging = false;
    revision = 1;

    load_library();

#if defined(ESP_PLATFORM)
    /* stars.csv is the source of truth across reboots. It is per-library, so
     * switching iPods cannot leak stars into another media library. */
    if(app_config_sd_ready()) {
        star_store_load(app_config_library_stars_path(), tracks, track_count,
                        track_starred);
    }
    /* Hardware has no demo playback: the iPod is the only source. */
    status = PLAYER_STOPPED;
    queue_count = 0;
    queue_index = 0;
#else
    /* Seed a few starred tracks only for the mock library. A real imported
     * library starts with an empty local Starred playlist. */
    if(!real_library) {
        for(int i = 0; i < track_count && i < 8; i++) {
            track_starred[i] = true;
        }
    }

    if(album_count > 0) {
        player_play_album(albums[0].id);
        status = PLAYER_PLAYING;
        elapsed_sec = 72;
    }
#endif
}

void player_tick(void)
{
#if defined(ESP_PLATFORM)
    /* iAP2 pushes state from the host task; AFC has no playback at all. */
    return;
#else
    if(status != PLAYER_PLAYING || queue_count <= 0) return;
    const player_track_t * tr = track_by_id(queue[queue_index]);
    if(tr == NULL) return;

    elapsed_sec++;
    if(elapsed_sec < tr->duration_sec) return;

    if(repeat_mode == PLAYER_REPEAT_ONE) {
        elapsed_sec = 0;
        bump_revision();
        return;
    }
    if(shuffle) {
        int next = (queue_index + 3) % queue_count;
        if(next == queue_index) next = (next + 1) % queue_count;
        queue_index = next;
        elapsed_sec = 0;
        bump_revision();
        return;
    }
    if(queue_index + 1 < queue_count) {
        queue_index++;
        elapsed_sec = 0;
        bump_revision();
    }
    else if(repeat_mode == PLAYER_REPEAT_ALL) {
        queue_index = 0;
        elapsed_sec = 0;
        bump_revision();
    }
    else {
        status = PLAYER_STOPPED;
        elapsed_sec = tr->duration_sec;
        bump_revision();
    }
#endif
}

void player_get_snapshot(player_snapshot_t * out)
{
#if defined(ESP_PLATFORM)
    memset(out, 0, sizeof(*out));
    out->volume = volume;
    out->muted = muted;
    out->battery = -1;
    out->revision = revision;
    out->real_device = iap2_host_is_enabled();
    if(iap2_host_is_enabled()) {
        iap2_state_t state;
        if(iap2_host_get_state(&state)) {
            iap2_snapshot(out, &state);
        }
        return;
    }
    out->connected = afc_client_is_ready();
    return;
#else
    memset(out, 0, sizeof(*out));
    out->status = status;
    out->elapsed_sec = elapsed_sec;
    out->volume = volume;
    out->muted = muted;
    if(queue_count > 0 && queue_index >= 0 && queue_index < queue_count) {
        int idx = track_index_by_id(queue[queue_index]);
        if(idx >= 0) out->starred = track_starred[idx];
    }
    out->shuffle = shuffle;
    out->repeat = repeat_mode;
    out->connected = connected;
    out->battery = battery;
    out->charging = charging;
    out->revision = revision;
    copy_str(out->device_name, sizeof(out->device_name), device_name);
    if(queue_count > 0 && queue_index >= 0 && queue_index < queue_count) {
        const player_track_t * tr = track_by_id(queue[queue_index]);
        if(tr) {
            out->track = *tr;
            out->duration_sec = tr->duration_sec;
        }
    }
#endif
}

bool player_device_available(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) return iap2_host_is_identified();
    return afc_client_is_ready();
#else
    return true;
#endif
}

/* Raw count for the boot-time thumbnail pack setup. The visible library is
 * still gated by player_device_available(). */
int player_library_album_count(void) { return album_count; }

int player_album_count(void)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return 0;
#endif
    return album_count;
}

bool player_album_get(int index, player_album_t * out)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return false;
#endif
    if(index < 0 || index >= album_count || out == NULL) return false;
    *out = albums[index];
    return true;
}

bool player_artwork_remote(uint64_t album_id, char * out, size_t out_len)
{
    if(out == NULL || out_len == 0) return false;
    out[0] = '\0';
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return false;
#endif
    if(album_id < 1 || album_id > (uint64_t)album_count) return false;
    const char * remote = artwork_remote[album_id - 1];
    if(remote[0] == '\0') return false;
    copy_str(out, out_len, remote);
    return true;
}

uint32_t player_artwork_revision(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) return iap2_host_artwork_revision();
#endif
    return 0;
}

bool player_artwork_copy(uint8_t * dst, size_t len)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) return iap2_host_artwork_copy(dst, len);
#endif
    return false;
}

int player_artist_count(void)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return 0;
#endif
    return artist_count;
}

bool player_artist_get(int index, player_artist_t * out)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return false;
#endif
    if(index < 0 || index >= artist_count || out == NULL) return false;
    *out = artists[index];
    return true;
}

int player_song_count(void)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return 0;
#endif
    return track_count;
}

bool player_song_get(int index, player_track_t * out)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return false;
#endif
    if(index < 0 || index >= track_count || out == NULL) return false;
    *out = tracks[index];
    return true;
}

int player_album_track_count(uint64_t album_id)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return 0;
#endif
    int count = 0;
    for(int i = 0; i < track_count; i++) {
        if(tracks[i].album_id == album_id) count++;
    }
    return count;
}

bool player_album_track_get(uint64_t album_id, int index, player_track_t * out)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return false;
#endif
    if(index < 0 || out == NULL) return false;
    int seen = 0;
    for(int i = 0; i < track_count; i++) {
        if(tracks[i].album_id != album_id) continue;
        if(seen++ == index) {
            *out = tracks[i];
            return true;
        }
    }
    return false;
}

int player_artist_song_count(uint64_t artist_id)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return 0;
#endif
    int count = 0;
    for(int i = 0; i < track_count; i++) {
        if(tracks[i].artist_id == artist_id) count++;
    }
    return count;
}

bool player_artist_song_get(uint64_t artist_id, int index, player_track_t * out)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return false;
#endif
    if(index < 0 || out == NULL) return false;
    int seen = 0;
    for(int i = 0; i < track_count; i++) {
        if(tracks[i].artist_id != artist_id) continue;
        if(seen++ == index) {
            *out = tracks[i];
            return true;
        }
    }
    return false;
}

int player_playlist_count(void)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return 0;
#endif
    return 1 + playlist_count; /* Starred is always first */
}

bool player_playlist_get(int index, player_playlist_t * out)
{
#if defined(ESP_PLATFORM)
    if(!player_device_available()) return false;
#endif
    if(out == NULL) return false;
    memset(out, 0, sizeof(*out));

    if(index == 0) {
        out->id = 0;
        out->local = true;
        copy_str(out->name, sizeof(out->name), "Starred");
        for(int i = 0; i < track_count; i++) {
            if(track_starred[i]) out->track_count++;
        }
        return true;
    }

    int d = index - 1;
    if(d < 0 || d >= playlist_count) return false;
    out->id = playlists[d].id;
    out->local = false;
    copy_str(out->name, sizeof(out->name), playlists[d].name);
    out->track_count = (uint16_t)playlists[d].track_count;
    return true;
}

int player_playlist_track_count(int index)
{
    player_playlist_t playlist;
    if(!player_playlist_get(index, &playlist)) return 0;
    return playlist.track_count;
}

bool player_playlist_track_get(int index, int track_index, player_track_t * out)
{
    if(track_index < 0 || out == NULL) return false;

    if(index == 0) {
        int seen = 0;
        for(int i = 0; i < track_count; i++) {
            if(!track_starred[i]) continue;
            if(seen++ == track_index) {
                *out = tracks[i];
                return true;
            }
        }
        return false;
    }

    int d = index - 1;
    if(d < 0 || d >= playlist_count) return false;
    int seen = 0;
    for(int i = 0; i < playlists[d].track_count; i++) {
        const player_track_t * track = track_by_id(playlists[d].track_ids[i]);
        if(track == NULL) continue;
        if(seen++ == track_index) {
            *out = *track;
            return true;
        }
    }
    return false;
}

void player_play_playlist(int index)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        uint64_t keys[240];
        int count = 0;
        if(index == 0) {
            for(int i = 0; i < track_count && count < 240; i++) {
                if(track_starred[i]) keys[count++] = tracks[i].id;
            }
        }
        else {
            int d = index - 1;
            if(d < 0 || d >= playlist_count) return;
            for(int i = 0; i < playlists[d].track_count && count < 240; i++) {
                int ti = track_index_by_id(playlists[d].track_ids[i]);
                if(ti >= 0) keys[count++] = tracks[ti].id;
            }
        }
        if(count > 0) iap2_host_play_track_keys(keys, count, 0);
        return;
    }
    return;
#endif
    queue_count = 0;
    if(index == 0) {
        for(int i = 0; i < track_count && queue_count < MAX_QUEUE; i++) {
            if(track_starred[i]) queue[queue_count++] = tracks[i].id;
        }
    }
    else {
        int d = index - 1;
        if(d < 0 || d >= playlist_count) return;
        for(int i = 0; i < playlists[d].track_count &&
                    queue_count < MAX_QUEUE; i++) {
            int track_index = track_index_by_id(playlists[d].track_ids[i]);
            if(track_index >= 0) queue[queue_count++] = tracks[track_index].id;
        }
    }
    if(queue_count > 0) {
        queue_index = 0;
        elapsed_sec = 0;
        status = PLAYER_PLAYING;
        bump_revision();
    }
}

int player_queue_count(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_state_t state;
        if(iap2_host_get_state(&state)) {
            return (state.identified && state.title[0]) ? 1 : 0;
        }
    }
    return 0;
#else
    return queue_count;
#endif
}

int player_queue_index(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_state_t state;
        if(iap2_host_get_state(&state) &&
           state.identified && state.title[0]) {
            return 0;
        }
    }
    return -1;
#else
    return queue_index;
#endif
}

bool player_queue_at(int index, player_track_t * out)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        if(index != 0 || out == NULL) return false;
        player_snapshot_t snapshot;
        player_get_snapshot(&snapshot);
        if(snapshot.track.id == 0) return false;
        *out = snapshot.track;
        return true;
    }
    return false;
#else
    if(index < 0 || index >= queue_count || out == NULL) return false;
    const player_track_t * tr = track_by_id(queue[index]);
    if(tr == NULL) return false;
    *out = *tr;
    return true;
#endif
}

void player_play_pause(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_state_t state;
        if(iap2_active(&state)) {
            iap2_host_hid_command(state.play_status == 1 ? "pause" : "play");
        }
        return;
    }
    return;
#endif
    if(queue_count == 0) {
        queue_all_tracks();
        return;
    }
    if(status == PLAYER_PLAYING) status = PLAYER_PAUSED;
    else status = PLAYER_PLAYING;
    bump_revision();
}

void player_next(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_host_hid_command("next");
        return;
    }
    return;
#endif
    if(queue_count == 0) return;
    if(shuffle && queue_count > 1) {
        queue_index = (queue_index + 3) % queue_count;
    }
    else {
        queue_index++;
        if(queue_index >= queue_count) {
            queue_index = (repeat_mode == PLAYER_REPEAT_OFF) ? queue_count - 1 : 0;
        }
    }
    elapsed_sec = 0;
    status = PLAYER_PLAYING;
    bump_revision();
}

void player_previous(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_host_hid_command("previous");
        return;
    }
    return;
#endif
    if(queue_count == 0) return;
    if(elapsed_sec > 3) {
        elapsed_sec = 0;
        bump_revision();
        return;
    }
    queue_index--;
    if(queue_index < 0) queue_index = queue_count - 1;
    elapsed_sec = 0;
    status = PLAYER_PLAYING;
    bump_revision();
}

void player_seek_sec(uint32_t sec)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_host_seek_ms(sec * 1000);
        return;
    }
    return;
#endif
    if(queue_count == 0) return;
    const player_track_t * tr = track_by_id(queue[queue_index]);
    if(tr == NULL) return;
    if(sec > tr->duration_sec) sec = tr->duration_sec;
    elapsed_sec = sec;
    bump_revision();
}

void player_set_volume(int new_volume)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        if(new_volume < 0) new_volume = 0;
        if(new_volume > 100) new_volume = 100;
        int delta = new_volume - volume;
        while(delta >= 5) {
            iap2_host_hid_command("volume_up");
            delta -= 5;
        }
        while(delta <= -5) {
            iap2_host_hid_command("volume_down");
            delta += 5;
        }
        volume = new_volume;
        if(volume > 0) muted = false;
        return;
    }
#endif
    if(new_volume < 0) new_volume = 0;
    if(new_volume > 100) new_volume = 100;
    volume = new_volume;
    if(new_volume > 0) muted = false;
    bump_revision();
}

void player_mute_toggle(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_host_hid_command("mute");
        muted = !muted;
        return;
    }
#endif
    muted = !muted;
    bump_revision();
}

void player_toggle_star(void)
{
#if defined(ESP_PLATFORM)
    /* In iAP2 mode the displayed track comes from the iPod, not from the
     * local queue. Match its persistent key against library.bin so the star
     * belongs to the song the user is actually looking at. */
    if(iap2_host_is_enabled()) {
        iap2_state_t state;
        if(iap2_host_get_state(&state) && state.title[0] != '\0') {
            int idx = state.track_key != 0
                          ? track_index_by_id(state.track_key) : -1;
            if(idx < 0) {
                idx = track_index_by_meta(state.title, state.album);
            }
            if(idx < 0) {
                ESP_LOGW(TAG, "star: track %016llX '%s' is not in library.bin",
                         (unsigned long long)state.track_key, state.title);
                return;
            }
            track_starred[idx] = !track_starred[idx];
            if(app_config_sd_ready()) {
                star_store_update(app_config_library_stars_path(),
                                  app_config_library_id(), state.device_name,
                                  &tracks[idx], track_starred[idx]);
            }
            bump_revision();
            return;
        }
    }
#endif
    if(queue_count <= 0 || queue_index < 0 || queue_index >= queue_count) return;
    int idx = track_index_by_id(queue[queue_index]);
    if(idx < 0) return;
    track_starred[idx] = !track_starred[idx];
#if defined(ESP_PLATFORM)
    if(app_config_sd_ready()) {
        star_store_update(app_config_library_stars_path(),
                          app_config_library_id(), device_name, &tracks[idx],
                          track_starred[idx]);
    }
#endif
    bump_revision();
}

void player_clear_stars(void)
{
    memset(track_starred, 0, sizeof(track_starred));
#if defined(ESP_PLATFORM)
    if(app_config_sd_ready()) {
        star_store_clear(app_config_library_stars_path());
    }
#endif
    bump_revision();
}

void player_set_shuffle(bool on)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_state_t state;
        if(iap2_active(&state) && state.shuffle != on) {
            iap2_host_hid_command("shuffle");
        }
        return;
    }
    return;
#endif
    shuffle = on;
    bump_revision();
}

void player_cycle_repeat(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_host_hid_command("repeat");
        return;
    }
    return;
#endif
    repeat_mode = (player_repeat_t)((repeat_mode + 1) % 3);
    bump_revision();
}

void player_play_track(uint64_t track_id)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        if(track_index_by_id(track_id) >= 0) {
            iap2_host_play_track_keys(&track_id, 1, 0);
        }
        return;
    }
    return;
#endif
    int idx = track_index_by_id(track_id);
    if(idx < 0) return;
    uint64_t album_id = tracks[idx].album_id;
    player_play_album(album_id);
    int count = player_album_track_count(album_id);
    for(int i = 0; i < count; i++) {
        player_track_t tr;
        if(player_album_track_get(album_id, i, &tr) && tr.id == track_id) {
            player_queue_jump(i);
            break;
        }
    }
}

void player_play_album(uint64_t album_id)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        uint64_t keys[240];
        int count = 0;
        for(int i = 0; i < track_count && count < 240; i++) {
            if(tracks[i].album_id == album_id) keys[count++] = tracks[i].id;
        }
        if(count > 0) iap2_host_play_track_keys(keys, count, 0);
        return;
    }
    return;
#endif
    queue_count = 0;
    for(int i = 0; i < track_count && queue_count < MAX_QUEUE; i++) {
        if(tracks[i].album_id == album_id) {
            queue[queue_count++] = tracks[i].id;
        }
    }
    if(queue_count > 0) {
        queue_index = 0;
        elapsed_sec = 0;
        status = PLAYER_PLAYING;
        bump_revision();
    }
}

void player_play_artist(uint64_t artist_id)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        uint64_t keys[240];
        int count = 0;
        for(int i = 0; i < track_count && count < 240; i++) {
            if(tracks[i].artist_id == artist_id) keys[count++] = tracks[i].id;
        }
        if(count > 0) iap2_host_play_track_keys(keys, count, 0);
        return;
    }
    return;
#endif
    queue_count = 0;
    for(int i = 0; i < track_count && queue_count < MAX_QUEUE; i++) {
        if(tracks[i].artist_id == artist_id) {
            queue[queue_count++] = tracks[i].id;
        }
    }
    if(queue_count > 0) {
        queue_index = 0;
        elapsed_sec = 0;
        status = PLAYER_PLAYING;
        bump_revision();
    }
}

void player_play_all(void)
{
#if defined(ESP_PLATFORM)
    return;
#endif
    queue_all_tracks();
}

void player_queue_jump(int index)
{
#if defined(ESP_PLATFORM)
    return;
#endif
    if(index < 0 || index >= queue_count) return;
    queue_index = index;
    elapsed_sec = 0;
    status = PLAYER_PLAYING;
    bump_revision();
}
