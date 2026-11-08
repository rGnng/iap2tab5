#include "player.h"

#include "cJSON.h"


#if defined(ESP_PLATFORM)
#include "afc_client.h"
#include "app_config.h"
#include "star_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "iap2_host.h"
static const char * TAG = "player";
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

/* iAP2 album first-seen order.  The AFC date is authoritative when present;
 * this map only provides the local fallback for albums missing that date. */
#define ALBUM_SEEN_SLOTS 8192u
typedef struct {
    uint64_t pid;
    uint32_t seq;
} album_seen_t;

static MOCK_LARGE_BSS album_seen_t album_seen[ALBUM_SEEN_SLOTS];
static uint32_t album_seen_next_seq = 1;
static uint32_t album_seen_batch_seq;
static int64_t album_seen_batch_last_us;
static bool album_seen_dirty;
static uint32_t album_seen_revision;
static int64_t album_seen_dirty_since_us;

static int album_sort_field;
static bool album_sort_desc = true;

#if defined(ESP_PLATFORM)
static SemaphoreHandle_t album_seen_lock;

static void album_seen_lock_take(void)
{
    if(album_seen_lock != NULL) xSemaphoreTake(album_seen_lock, portMAX_DELAY);
}

static void album_seen_lock_give(void)
{
    if(album_seen_lock != NULL) xSemaphoreGive(album_seen_lock);
}
#else
static void album_seen_lock_take(void) { }
static void album_seen_lock_give(void) { }
#endif

static void copy_str(char *dst, size_t size, const char *src)
{
    if(size == 0) return;
    size_t n = src ? strlen(src) : 0;
    if(n >= size) n = size - 1;
    if(n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t album_seen_hash(uint64_t pid)
{
    pid ^= pid >> 33;
    pid *= 0xff51afd7ed558ccdULL;
    pid ^= pid >> 33;
    return (uint32_t)pid & (ALBUM_SEEN_SLOTS - 1u);
}

/* Caller holds album_seen_lock. */
static album_seen_t * album_seen_find_unlocked(uint64_t pid)
{
    if(pid == 0) return NULL;
    uint32_t slot = album_seen_hash(pid);
    for(uint32_t i = 0; i < ALBUM_SEEN_SLOTS; i++) {
        album_seen_t * entry = &album_seen[slot];
        if(entry->pid == pid) return entry;
        if(entry->pid == 0) return NULL;
        slot = (slot + 1u) & (ALBUM_SEEN_SLOTS - 1u);
    }
    return NULL;
}

static uint32_t album_seen_seq_unlocked(uint64_t pid)
{
    album_seen_t * entry = album_seen_find_unlocked(pid);
    return entry != NULL ? entry->seq : 0;
}

/* Caller holds album_seen_lock. */
static bool album_seen_insert_unlocked(uint64_t pid, uint32_t seq)
{
    if(pid == 0) return false;
    uint32_t slot = album_seen_hash(pid);
    for(uint32_t i = 0; i < ALBUM_SEEN_SLOTS; i++) {
        album_seen_t * entry = &album_seen[slot];
        if(entry->pid == pid) return false;
        if(entry->pid == 0) {
            entry->pid = pid;
            entry->seq = seq;
            return true;
        }
        slot = (slot + 1u) & (ALBUM_SEEN_SLOTS - 1u);
    }
    return false;
}

static int album_compare(const void * va, const void * vb)
{
    const player_album_t * a = va;
    const player_album_t * b = vb;
    int cmp = 0;

    if(album_sort_field == 0) {
        uint32_t ad = a->date_added;
        uint32_t bd = b->date_added;
        if(ad == 0 || bd == 0) {
            if(ad == 0 && bd == 0) {
                uint32_t as = album_seen_seq_unlocked(a->album_pid);
                uint32_t bs = album_seen_seq_unlocked(b->album_pid);
                cmp = (as > bs) - (as < bs);
            }
            else {
                /* An album without an AFC timestamp was found after the
                 * last AFC sync; treat it as newer. */
                cmp = ad != 0 ? -1 : 1;
            }
        }
        else {
            cmp = (ad > bd) - (ad < bd);
            if(cmp == 0) {
                uint32_t as = album_seen_seq_unlocked(a->album_pid);
                uint32_t bs = album_seen_seq_unlocked(b->album_pid);
                cmp = (as > bs) - (as < bs);
            }
        }
    }
    else if(album_sort_field == 1) {
        cmp = strcasecmp(a->title, b->title);
        if(cmp == 0) cmp = strcasecmp(a->artist, b->artist);
    }
    else {
        cmp = (a->year > b->year) - (a->year < b->year);
    }

    if(cmp == 0) {
        cmp = strcasecmp(a->title, b->title);
        if(cmp == 0) cmp = (a->id > b->id) - (a->id < b->id);
        return cmp;
    }
    return album_sort_desc ? -cmp : cmp;
}

void player_sort_albums(int field, bool desc)
{
    if(album_count <= 1) return;
    album_seen_lock_take();
    album_sort_field = field;
    album_sort_desc = desc;
    qsort(albums, (size_t)album_count, sizeof(albums[0]), album_compare);
    album_seen_lock_give();
}

void player_note_iap2_album(uint64_t album_pid)
{
#if defined(ESP_PLATFORM)
    if(album_pid == 0) return;
    album_seen_lock_take();
    if(album_seen_find_unlocked(album_pid) != NULL) {
        album_seen_lock_give();
        return;
    }
    int64_t now = esp_timer_get_time();
    if(album_seen_batch_seq == 0 ||
       now - album_seen_batch_last_us > 5000000) {
        album_seen_batch_seq = album_seen_next_seq++;
    }
    album_seen_batch_last_us = now;
    if(album_seen_insert_unlocked(album_pid, album_seen_batch_seq)) {
        album_seen_dirty = true;
        album_seen_revision++;
        album_seen_dirty_since_us = now;
    }
    album_seen_lock_give();
#else
    (void)album_pid;
#endif
}

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t version;
    uint32_t count;
} album_seen_header_t;

typedef struct __attribute__((packed)) {
    uint64_t pid;
    uint32_t seq;
} album_seen_record_t;

#if defined(ESP_PLATFORM)
static void album_seen_path(char * path, size_t size)
{
    snprintf(path, size, "%s/album_seen.bin", app_config_library_dir());
}

static void album_seen_load(void)
{
    if(!app_config_sd_ready()) return;
    char path[160];
    album_seen_path(path, sizeof(path));
    FILE * f = fopen(path, "rb");
    if(f == NULL) return;

    album_seen_header_t header;
    if(fread(&header, 1, sizeof(header), f) != sizeof(header) ||
       memcmp(header.magic, "T5SEE001", 8) != 0 ||
       header.version != 1 || header.count > ALBUM_SEEN_SLOTS) {
        fclose(f);
        return;
    }

    uint32_t max_seq = 0;
    for(uint32_t i = 0; i < header.count; i++) {
        album_seen_record_t rec;
        if(fread(&rec, 1, sizeof(rec), f) != sizeof(rec)) break;
        if(rec.pid == 0 || rec.seq == 0) continue;
        album_seen_insert_unlocked(rec.pid, rec.seq);
        if(rec.seq > max_seq) max_seq = rec.seq;
    }
    fclose(f);
    album_seen_next_seq = max_seq + 1;
    album_seen_batch_seq = 0;
    album_seen_dirty = false;
    ESP_LOGI(TAG, "album seen order loaded (%u entries)",
             (unsigned)header.count);
}

static void album_seen_save(bool force)
{
    if(!app_config_sd_ready()) return;
    album_seen_lock_take();
    int64_t now = esp_timer_get_time();
    if(!album_seen_dirty || (!force && now - album_seen_dirty_since_us < 5000000)) {
        album_seen_lock_give();
        return;
    }
    uint32_t count = 0;
    for(uint32_t i = 0; i < ALBUM_SEEN_SLOTS; i++) {
        if(album_seen[i].pid != 0 && album_seen[i].seq != 0) count++;
    }
    album_seen_record_t * records = malloc((size_t)count * sizeof(*records));
    if(records == NULL) { album_seen_lock_give(); return; }
    uint32_t index = 0;
    for(uint32_t i = 0; i < ALBUM_SEEN_SLOTS; i++) {
        if(album_seen[i].pid == 0 || album_seen[i].seq == 0) continue;
        records[index++] = (album_seen_record_t){album_seen[i].pid, album_seen[i].seq};
    }
    uint32_t saved_revision = album_seen_revision;
    char path[160], tmp[168];
    album_seen_path(path, sizeof(path));
    album_seen_lock_give();
    /* Snapshot under the lock, perform all SD I/O without it. Incoming
     * iAP2 albums must never wait for an SD flush or rename. */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "wb");
    if(f == NULL) { free(records); return; }
    album_seen_header_t header = {
        .magic = "T5SEE001", .version = 1, .count = count,
    };
    bool ok = fwrite(&header, 1, sizeof(header), f) == sizeof(header) &&
              fwrite(records, sizeof(*records), count, f) == count;
    free(records);
    if(fclose(f) != 0) ok = false;
    if(ok) {
        /* FATFS rename does not replace an existing file. */
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if(ok) {
        album_seen_lock_take();
        if(album_seen_revision == saved_revision) album_seen_dirty = false;
        album_seen_lock_give();
        ESP_LOGI(TAG, "album seen order saved (%u entries)", (unsigned)count);
    }
    else {
        remove(tmp);
        ESP_LOGW(TAG, "album seen order save failed");
    }
}

#else
static void album_seen_load(void) { }
static void album_seen_save(bool force) { (void)force; }
#endif

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

/* Compact PID -> track offset index: 128 KiB in PSRAM instead of scanning
 * 20k large metadata records once for every queue row and UI snapshot.
 * The library is immutable after player_init(); album sorting keeps offsets. */
#define TRACK_ID_SLOTS 65536u
_Static_assert(MAX_TRACKS < UINT16_MAX && MAX_TRACKS < TRACK_ID_SLOTS / 2,
               "track ID index must fit all track offsets with room for probing");
static MOCK_LARGE_BSS uint16_t track_id_slots[TRACK_ID_SLOTS];
static bool track_id_index_ready;

static uint32_t track_id_hash(uint64_t id)
{
    id ^= id >> 30;
    id *= UINT64_C(0xbf58476d1ce4e5b9);
    id ^= id >> 27;
    id *= UINT64_C(0x94d049bb133111eb);
    id ^= id >> 31;
    return (uint32_t)id & (TRACK_ID_SLOTS - 1);
}

static void track_id_index_build(void)
{
    memset(track_id_slots, 0, sizeof(track_id_slots));
    for(int i = 0; i < track_count; i++) {
        uint32_t slot = track_id_hash(tracks[i].id);
        while(track_id_slots[slot] != 0) {
            /* Preserve the linear lookup's first-match behavior. */
            if(tracks[track_id_slots[slot] - 1].id == tracks[i].id) break;
            slot = (slot + 1) & (TRACK_ID_SLOTS - 1);
        }
        if(track_id_slots[slot] == 0) track_id_slots[slot] = (uint16_t)(i + 1);
    }
    track_id_index_ready = true;
}

static int track_index_by_id(uint64_t id)
{
    if(!track_id_index_ready) {
        for(int i = 0; i < track_count; i++) {
            if(tracks[i].id == id) return i;
        }
        return -1;
    }
    uint32_t slot = track_id_hash(id);
    for(uint32_t n = 0; n < TRACK_ID_SLOTS; n++) {
        uint16_t entry = track_id_slots[slot];
        if(entry == 0) return -1;
        if(tracks[entry - 1].id == id) return entry - 1;
        slot = (slot + 1) & (TRACK_ID_SLOTS - 1);
    }
    return -1;
}

static const player_track_t *track_by_id(uint64_t id)
{
    int index = track_index_by_id(id);
    return index >= 0 ? &tracks[index] : NULL;
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

/* Album/artist membership: 96 KB in PSRAM, built once after loading the
 * immutable library. Sort offsets, never metadata or catalog rows: album
 * sorting must not affect membership, and songs retain their file order.
 * Binary searches use local bounds so getters remain safe across tasks. */
static MOCK_LARGE_BSS uint16_t album_track_order[MAX_TRACKS];
static MOCK_LARGE_BSS uint16_t artist_song_order[MAX_TRACKS];
static bool membership_index_ready;

static uint64_t membership_id(uint16_t offset, bool by_artist)
{
    return by_artist ? tracks[offset].artist_id : tracks[offset].album_id;
}

static int membership_compare(const void * a, const void * b, bool by_artist)
{
    uint16_t ia = *(const uint16_t *)a, ib = *(const uint16_t *)b;
    uint64_t pa = membership_id(ia, by_artist), pb = membership_id(ib, by_artist);
    if(pa != pb) return pa < pb ? -1 : 1;
    return (ia > ib) - (ia < ib);
}

static int album_track_compare(const void * a, const void * b)
{
    return membership_compare(a, b, false);
}

static int artist_song_compare(const void * a, const void * b)
{
    return membership_compare(a, b, true);
}

static void membership_index_build(void)
{
    membership_index_ready = false;
    for(int i = 0; i < track_count; i++) {
        album_track_order[i] = artist_song_order[i] = (uint16_t)i;
    }
    qsort(album_track_order, track_count, sizeof(album_track_order[0]), album_track_compare);
    qsort(artist_song_order, track_count, sizeof(artist_song_order[0]), artist_song_compare);
    membership_index_ready = true;
}

/* First entry >= id (lower bound) or > id (upper bound). All 64-bit IDs,
 * including zero and UINT64_MAX, work without sentinel IDs or id + 1. */
static int membership_bound(const uint16_t * order, uint64_t id,
                            bool by_artist, bool upper)
{
    int lo = 0, hi = track_count;
    while(lo < hi) {
        int mid = lo + (hi - lo) / 2;
        uint64_t parent = membership_id(order[mid], by_artist);
        if(parent < id || (upper && parent == id)) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool membership_get(const uint16_t * order, uint64_t id, int index,
                           bool by_artist, player_track_t * out)
{
    int first = membership_bound(order, id, by_artist, false);
    if(index >= track_count - first) return false;
    uint16_t offset = order[first + index];
    if(membership_id(offset, by_artist) != id) return false;
    *out = tracks[offset];
    return true;
}

/* "Play from here" lists.  Each action builds the ordered list the UI
 * shows, then starts it from the chosen item:
 *   album    : chosen track to the end of the album (no wrap);
 *   artist   : artist list in display order, chosen track to the end, then
 *              the head of the list up to the item before it;
 *   songs    : whole library in list order with the same wrap rule (the
 *              device uses PlayMediaLibrarySpecial so the 20k-track library
 *              is never sent as a PID list);
 *   playlist : chosen track to the end of the playlist (no wrap).
 * Shuffle permutes the list in the simulator; on the device the iPod's own
 * shuffle is switched on instead. */
#define ARTIST_ALBUMS_MAX 256

/* The iAP2 path sends at most ~500 keys per command, so the device keeps
 * these lists small; the simulator needs room for the whole library. */
#if defined(ESP_PLATFORM)
#define PLAY_IDS_MAX 512
#else
#define PLAY_IDS_MAX MAX_QUEUE
#endif

static MOCK_LARGE_BSS uint64_t play_ids[PLAY_IDS_MAX];
#if defined(ESP_PLATFORM)
/* The last ordered list sent to the iPod.  Up Next shows it on the device;
 * the iPod's own queue is not read back yet. */
static uint64_t device_queue[PLAY_IDS_MAX];
static int device_queue_count;
#endif

static int ids_album_from(uint64_t album_id, int from, uint64_t * out, int max)
{
    int n = 0;
    int seen = 0;
    for(int i = 0; i < track_count && n < max; i++) {
        if(tracks[i].album_id != album_id) continue;
        if(seen++ < from) continue;
        out[n++] = tracks[i].id;
    }
    return n;
}

static int ids_artist_display(uint64_t artist_id, uint64_t * out, int max)
{
    uint64_t albums[ARTIST_ALBUMS_MAX];
    int album_count = 0;
    for(int i = 0; i < track_count; i++) {
        if(tracks[i].artist_id != artist_id) continue;
        bool known = false;
        for(int a = 0; a < album_count && !known; a++) {
            known = albums[a] == tracks[i].album_id;
        }
        if(!known && album_count < ARTIST_ALBUMS_MAX) {
            albums[album_count++] = tracks[i].album_id;
        }
    }
    int n = 0;
    for(int a = 0; a < album_count; a++) {
        for(int i = 0; i < track_count && n < max; i++) {
            if(tracks[i].artist_id == artist_id &&
               tracks[i].album_id == albums[a]) {
                out[n++] = tracks[i].id;
            }
        }
    }
    return n;
}

static int ids_playlist(int index, uint64_t * out, int max)
{
    int n = 0;
    if(index == 0) {
        for(int i = 0; i < track_count && n < max; i++) {
            if(track_starred[i]) out[n++] = tracks[i].id;
        }
        return n;
    }
    int d = index - 1;
    if(d < 0 || d >= playlist_count) return 0;
    for(int i = 0; i < playlists[d].track_count && n < max; i++) {
        int track_index = track_index_by_id(playlists[d].track_ids[i]);
        if(track_index >= 0) out[n++] = tracks[track_index].id;
    }
    return n;
}

/* Turns play_ids[0..count) into play_ids[start..count) followed by
 * play_ids[0..start). */
static void rotate_play_ids(int count, int start)
{
    static MOCK_LARGE_BSS uint64_t rotated[PLAY_IDS_MAX];
    for(int k = 0; k < count; k++) {
        rotated[k] = play_ids[(start + k) % count];
    }
    memcpy(play_ids, rotated, (size_t)count * sizeof(uint64_t));
}

static uint32_t queue_rev;

uint32_t player_queue_revision(void)
{
#if defined(ESP_PLATFORM)
    uint32_t rev = queue_rev;
    if(iap2_host_is_enabled()) rev += iap2_host_queue_list_revision();
    return rev;
#else
    return queue_rev;
#endif
}

static void play_ids_start(int count, bool shuffle_on)
{
    if(count <= 0) return;
    if(count > PLAY_IDS_MAX) count = PLAY_IDS_MAX;
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        iap2_host_play_track_keys(play_ids, count, 0);
        memcpy(device_queue, play_ids, (size_t)count * sizeof(uint64_t));
        device_queue_count = count;
        queue_rev++;
        if(shuffle_on) player_set_shuffle(true);
        return;
    }
    return;
#endif
    if(shuffle_on) {
        for(int i = count - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            uint64_t tmp = play_ids[i];
            play_ids[i] = play_ids[j];
            play_ids[j] = tmp;
        }
        shuffle = true;
    }
    memcpy(queue, play_ids, (size_t)count * sizeof(uint64_t));
    queue_count = count;
    queue_rev++;
    queue_index = 0;
    elapsed_sec = 0;
    status = PLAYER_PLAYING;
    bump_revision();
}

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

/* Album persistent IDs are emitted as 16-hex strings to preserve all 64
 * bits through cJSON's double-backed numbers. */
static uint64_t json_u64_hex(const cJSON * object, const char * key)
{
    const cJSON * item = cJSON_GetObjectItemCaseSensitive(object, key);
    if(item == NULL) return 0;
    if(cJSON_IsString(item) && item->valuestring != NULL) {
        return strtoull(item->valuestring, NULL, 16);
    }
    if(cJSON_IsNumber(item)) {
        return (uint64_t)item->valuedouble;
    }
    return 0;
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
    uint64_t album_pid;
    uint32_t year;
    uint32_t track_count;
    uint32_t color;
    uint32_t cover_index;
    uint32_t date_added;
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
    if(fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return false;
    }
    if(memcmp(header, "T5LIB003", 8) != 0 ||
       rd_le32(header + 8) != 3) {
        fclose(f);
        return false;
    }

    uint32_t wa = rd_le32(header + 12);
    uint32_t wl = rd_le32(header + 16);
    uint32_t wt = rd_le32(header + 20);
    uint32_t wp = rd_le32(header + 24);
    uint32_t wc = rd_le32(header + 28);

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
        a->album_pid = w.album_pid;
        a->year = (uint16_t)w.year;
        a->track_count = (uint16_t)w.track_count;
        a->color = w.color;
        a->cover_index = (uint16_t)w.cover_index;
        a->date_added = w.date_added;
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
            album->album_pid = json_u64_hex(item, "album_pid");
            album->date_added = (uint32_t)json_u64(item, "date_added");
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
    track_id_index_ready = false;
    membership_index_ready = false;
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

#if defined(ESP_PLATFORM)
    if(album_seen_lock == NULL) {
        album_seen_lock = xSemaphoreCreateMutex();
    }
#endif
    memset(album_seen, 0, sizeof(album_seen));
    album_seen_next_seq = 1;
    album_seen_batch_seq = 0;
    album_seen_batch_last_us = 0;
    album_seen_dirty = false;
    album_seen_dirty_since_us = 0;

    load_library();
#if defined(ESP_PLATFORM)
    int64_t index_start = esp_timer_get_time();
#endif
    track_id_index_build();
    membership_index_build();

#if defined(ESP_PLATFORM)
    ESP_LOGI(TAG, "library indexes: %d tracks in %lld us", track_count,
             (long long)(esp_timer_get_time() - index_start));
    album_seen_load();
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
    /* iAP2 pushes state from the host task; AFC has no playback at all.
     * The UI tick is still a good place to flush the compact iAP2 album
     * seen-order sidecar without blocking the USB task. */
    album_seen_save(false);
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
int player_library_track_count(void) { return track_count; }

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

bool player_artwork_copy(uint8_t * dst, size_t len, artwork_quality_t minimum,
                         artwork_quality_t * actual)
{
    if(actual) *actual = (artwork_quality_t){0};
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) return iap2_host_artwork_copy(dst, len, minimum, actual);
#else
    (void)dst; (void)len; (void)minimum;
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
    if(membership_index_ready) {
        return membership_bound(album_track_order, album_id, false, true) -
               membership_bound(album_track_order, album_id, false, false);
    }
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
    if(membership_index_ready) {
        return membership_get(album_track_order, album_id, index, false, out);
    }
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
    if(membership_index_ready) {
        return membership_bound(artist_song_order, artist_id, true, true) -
               membership_bound(artist_song_order, artist_id, true, false);
    }
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
    if(membership_index_ready) {
        return membership_get(artist_song_order, artist_id, index, true, out);
    }
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
    int n = ids_playlist(index, play_ids, PLAY_IDS_MAX);
    play_ids_start(n, false);
}

void player_play_playlist_from(int index, int track_index)
{
    int n = ids_playlist(index, play_ids, PLAY_IDS_MAX);
    if(track_index < 0 || track_index >= n) return;
    n -= track_index;
    memmove(play_ids, play_ids + track_index, (size_t)n * sizeof(uint64_t));
    play_ids_start(n, false);
}

void player_play_playlist_shuffled(int index)
{
    int n = ids_playlist(index, play_ids, PLAY_IDS_MAX);
    play_ids_start(n, true);
}

int player_queue_count(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled() && iap2_host_queue_list_count() > 0) {
        return iap2_host_queue_list_count();
    }
    if(device_queue_count > 0) return device_queue_count;
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
    if(iap2_host_is_enabled() && iap2_host_queue_list_count() > 0) {
        iap2_state_t state;
        if(!iap2_host_get_state(&state) || state.queue_index < 0) return -1;
        int index = state.queue_index - iap2_host_queue_list_base();
        int count = iap2_host_queue_list_count();
        return (index >= 0 && index < count) ? index : -1;
    }
    if(device_queue_count > 0) {
        player_snapshot_t snapshot;
        player_get_snapshot(&snapshot);
        for(int i = 0; i < device_queue_count; i++) {
            if(device_queue[i] == snapshot.track.id) return i;
        }
        return -1;
    }
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
    if(iap2_host_is_enabled() && iap2_host_queue_list_count() > 0) {
        uint64_t key = 0;
        if(out == NULL || !iap2_host_queue_list_get(index, &key)) return false;
        int track_index = track_index_by_id(key);
        if(track_index < 0) return false;
        *out = tracks[track_index];
        return true;
    }
    if(device_queue_count > 0) {
        if(index < 0 || index >= device_queue_count || out == NULL) return false;
        int track_index = track_index_by_id(device_queue[index]);
        if(track_index < 0) return false;
        *out = tracks[track_index];
        return true;
    }
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

void player_step_volume(int direction)
{
    if(direction == 0) return;
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        /* Send a relative tap for every detent. A local percentage estimate
         * drifts from the iPod and can incorrectly prevent further turns. */
        iap2_host_hid_command(direction > 0 ? "volume_up" : "volume_down");
        return;
    }
#endif
    int new_volume = volume + (direction > 0 ? 5 : -5);
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

void player_play_album(uint64_t album_id)
{
    int n = ids_album_from(album_id, 0, play_ids, PLAY_IDS_MAX);
    play_ids_start(n, false);
}

void player_play_album_from(uint64_t album_id, int track_index)
{
    int n = ids_album_from(album_id, track_index, play_ids, PLAY_IDS_MAX);
    play_ids_start(n, false);
}

void player_play_album_shuffled(uint64_t album_id)
{
    int n = ids_album_from(album_id, 0, play_ids, PLAY_IDS_MAX);
    play_ids_start(n, true);
}

void player_play_artist(uint64_t artist_id)
{
    int n = ids_artist_display(artist_id, play_ids, PLAY_IDS_MAX);
    play_ids_start(n, false);
}

void player_play_artist_from(uint64_t artist_id, uint64_t track_id)
{
    int n = ids_artist_display(artist_id, play_ids, PLAY_IDS_MAX);
    int pos = -1;
    for(int i = 0; i < n && pos < 0; i++) {
        if(play_ids[i] == track_id) pos = i;
    }
    if(pos < 0) return;
    rotate_play_ids(n, pos);
    play_ids_start(n, false);
}

void player_play_artist_shuffled(uint64_t artist_id)
{
    int n = ids_artist_display(artist_id, play_ids, PLAY_IDS_MAX);
    play_ids_start(n, true);
}

void player_play_song(uint64_t track_id)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        if(track_index_by_id(track_id) >= 0) {
            iap2_host_play_all_songs(track_id);
            device_queue_count = 0;
        }
        return;
    }
    return;
#endif
    int pos = track_index_by_id(track_id);
    if(pos < 0) return;
    int n = track_count < PLAY_IDS_MAX ? track_count : PLAY_IDS_MAX;
    for(int k = 0; k < n; k++) {
        play_ids[k] = tracks[(pos + k) % track_count].id;
    }
    play_ids_start(n, false);
}

void player_play_all_shuffled(void)
{
#if defined(ESP_PLATFORM)
    if(iap2_host_is_enabled()) {
        if(track_count <= 0) return;
        /* PlayMediaLibrarySpecial from a random track, then iPod shuffle. */
        uint64_t start = tracks[rand() % track_count].id;
        iap2_host_play_all_songs(start);
        device_queue_count = 0;
        player_set_shuffle(true);
    }
    return;
#endif
    int n = track_count < PLAY_IDS_MAX ? track_count : PLAY_IDS_MAX;
    for(int k = 0; k < n; k++) play_ids[k] = tracks[k].id;
    play_ids_start(n, true);
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
