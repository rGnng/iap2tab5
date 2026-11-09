#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MEDIA_INDEX_MAX_ALBUMS 4096u
#define MEDIA_INDEX_MAX_TRACKS 24000u

#pragma pack(push, 1)
typedef struct
{
    uint64_t pid;
    uint32_t date_added;
    uint64_t artwork_version;
    char title[80], artist[64], remote[160];
    uint16_t year;
} media_index_album_t;
typedef struct
{
    uint64_t pid, album_pid;
    uint32_t date_added;
    uint16_t disc_no, track_no;
} media_index_track_t;
#pragma pack(pop)

typedef struct
{
    char library_id[128];
    uint32_t album_count, track_count;
    media_index_album_t *albums;
    media_index_track_t *tracks;
} media_index_t;

void media_index_clear(media_index_t *index);
void media_index_sort(media_index_t *index);
const media_index_album_t *media_index_album(const media_index_t *index, uint64_t pid);
const media_index_track_t *media_index_track(const media_index_t *index, uint64_t pid);
bool media_index_load(media_index_t *index, const char *path, const char *library_id);
bool media_index_save(const media_index_t *index, const char *path);
uint64_t media_index_artwork_version(const char *path);

/* Read a verified AFC database snapshot, including its committed WAL frames.
 * The imported index replaces the old one only after the entire query succeeds. */
bool media_index_import(media_index_t *index, const char *database, const char *wal,
    const char *library_id, char *error, size_t error_size);
