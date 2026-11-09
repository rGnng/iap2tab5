#include "media_index.h"
#include "store_util.h"

#pragma pack(push, 1)
typedef struct
{
    char magic[8];
    uint32_t version, albums, tracks;
    char library_id[128];
    uint32_t crc;
} index_header_t;
#pragma pack(pop)

void media_index_clear(media_index_t *index)
{
    if(!index)
        return;
    free(index->albums);
    free(index->tracks);
    memset(index, 0, sizeof(*index));
}
static int album_compare(const void *a, const void *b)
{
    uint64_t x = ((const media_index_album_t *)a)->pid, y = ((const media_index_album_t *)b)->pid;
    return (x > y) - (x < y);
}
static int track_compare(const void *a, const void *b)
{
    uint64_t x = ((const media_index_track_t *)a)->pid, y = ((const media_index_track_t *)b)->pid;
    return (x > y) - (x < y);
}
void media_index_sort(media_index_t *index)
{
    if(index->album_count > 1)
        qsort(index->albums, index->album_count, sizeof(*index->albums), album_compare);
    if(index->track_count > 1)
        qsort(index->tracks, index->track_count, sizeof(*index->tracks), track_compare);
}
const media_index_album_t *media_index_album(const media_index_t *index, uint64_t pid)
{
    uint32_t lo = 0, hi = index->album_count;
    while(lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if(index->albums[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < index->album_count && index->albums[lo].pid == pid ? &index->albums[lo] : NULL;
}
const media_index_track_t *media_index_track(const media_index_t *index, uint64_t pid)
{
    uint32_t lo = 0, hi = index->track_count;
    while(lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        if(index->tracks[mid].pid < pid)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < index->track_count && index->tracks[lo].pid == pid ? &index->tracks[lo] : NULL;
}
uint64_t media_index_artwork_version(const char *path)
{
    if(!path || !*path)
        return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    while(*path)
    {
        hash ^= (uint8_t)*path++;
        hash *= UINT64_C(1099511628211);
    }
    return hash ? hash : 1;
}
static bool load_one(media_index_t *out, const char *path, const char *library_id)
{
    FILE *file = fopen(path, "rb");
    if(!file)
        return false;
    index_header_t header;
    media_index_t next = { 0 };
    bool ok = false;
    if(fread(&header, 1, sizeof(header), file) != sizeof(header)
        || !((!memcmp(header.magic, "T5AFC001", 8) && header.version == 1)
            || (!memcmp(header.magic, "T5AFC002", 8) && header.version == 2))
        || header.albums > MEDIA_INDEX_MAX_ALBUMS || header.tracks > MEDIA_INDEX_MAX_TRACKS
        || !memchr(header.library_id, 0, sizeof(header.library_id))
        || strcmp(header.library_id, library_id))
        goto done;
    next.album_count = header.albums;
    next.track_count = header.tracks;
    snprintf(next.library_id, sizeof(next.library_id), "%s", library_id);
    next.albums = calloc(header.albums ? header.albums : 1, sizeof(*next.albums));
    next.tracks = calloc(header.tracks ? header.tracks : 1, sizeof(*next.tracks));
    if(!next.albums || !next.tracks)
        goto done;
    uint32_t crc = 0;
    size_t album_size = sizeof(*next.albums) - (header.version == 1 ? sizeof(uint16_t) : 0);
    for(uint32_t i = 0; i < header.albums; i++)
    {
        if(fread(&next.albums[i], 1, album_size, file) != album_size)
            goto done;
        crc = store_crc32(crc, &next.albums[i], album_size);
    }
    size_t track_size=sizeof(*next.tracks)-(header.version==1?2*sizeof(uint16_t):0);
    for(uint32_t i=0;i<header.tracks;i++)
    {
        if(fread(&next.tracks[i],1,track_size,file)!=track_size) goto done;
        crc=store_crc32(crc,&next.tracks[i],track_size);
    }
    if(fgetc(file)!=EOF) goto done;
    if(crc != header.crc)
        goto done;
    for(uint32_t i = 0; i < next.album_count; i++)
    {
        media_index_album_t *a = &next.albums[i];
        if(!a->pid || !memchr(a->title, 0, sizeof(a->title))
            || !memchr(a->artist, 0, sizeof(a->artist)) || !memchr(a->remote, 0, sizeof(a->remote)))
            goto done;
    }
    media_index_sort(&next);
    for(uint32_t i = 1; i < next.album_count; i++)
        if(next.albums[i - 1].pid == next.albums[i].pid)
            goto done;
    for(uint32_t i = 0; i < next.track_count; i++)
        if(!next.tracks[i].pid || (i && next.tracks[i - 1].pid == next.tracks[i].pid))
            goto done;
    media_index_clear(out);
    *out = next;
    memset(&next, 0, sizeof(next));
    ok = true;
done:
    fclose(file);
    media_index_clear(&next);
    return ok;
}
bool media_index_load(media_index_t *index, const char *path, const char *library_id)
{
    if(load_one(index, path, library_id))
        return true;
    char backup[320];
    snprintf(backup, sizeof(backup), "%s.bak", path);
    return load_one(index, backup, library_id);
}
bool media_index_save(const media_index_t *index, const char *path)
{
    size_t ab = (size_t)index->album_count * sizeof(*index->albums);
    size_t tb = (size_t)index->track_count * sizeof(*index->tracks);
    if(index->album_count > MEDIA_INDEX_MAX_ALBUMS || index->track_count > MEDIA_INDEX_MAX_TRACKS)
        return false;
    char temporary[320];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    if(!file)
        return false;
    index_header_t header = { .magic = "T5AFC002",
        .version = 2,
        .albums = index->album_count,
        .tracks = index->track_count };
    snprintf(header.library_id, sizeof(header.library_id), "%s", index->library_id);
    header.crc = store_crc32(store_crc32(0, index->albums, ab), index->tracks, tb);
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header)
        && fwrite(index->albums, 1, ab, file) == ab && fwrite(index->tracks, 1, tb, file) == tb;
    if(fflush(file) != 0 || fsync(fileno(file)) != 0)
        ok = false;
    if(fclose(file) != 0)
        ok = false;
    if(ok)
        ok = store_replace(path, temporary);
    if(!ok)
        remove(temporary);
    return ok;
}
