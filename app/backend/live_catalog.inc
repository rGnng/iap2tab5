/* Private live catalog implementation, included after the legacy importer so
 * both backends use the same arrays and membership indexes. Only the UI thread
 * applies events. The USB worker queues bounded, copied property groups. */
typedef struct
{
    uint32_t discovered, last_seen;
    bool afc, present;
} live_track_t;
static MOCK_LARGE_BSS live_track_t live_tracks[MAX_TRACKS];
static media_index_t live_afc;
static uint32_t live_revision = 1, live_batch;
static bool live_in_batch, live_dirty;
static bool live_publish;
static bool live_suspended;
static char live_library[128], live_directory[192];
static char live_remote_revision[128], live_committed_revision[128];
typedef struct
{
    char magic[8], library[128], revision[128];
    uint32_t catalog_crc, crc;
} live_remote_t;
static void live_load_remote(uint32_t catalog_crc)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/catalog.rev", live_directory);
    FILE *file = fopen(path, "rb");
    if(!file)
        return;
    live_remote_t remote;
    bool ok = fread(&remote, 1, sizeof(remote), file) == sizeof(remote) && fgetc(file) == EOF;
    fclose(file);
    if(ok && !memcmp(remote.magic, "T5REV001", 8)
        && memchr(remote.library, 0, sizeof(remote.library))
        && !strcmp(remote.library, live_library)
        && memchr(remote.revision, 0, sizeof(remote.revision)) && remote.catalog_crc == catalog_crc
        && remote.crc == store_crc32(0, &remote, offsetof(live_remote_t, crc)))
    {
        snprintf(live_remote_revision, sizeof(live_remote_revision), "%s", remote.revision);
        snprintf(live_committed_revision, sizeof(live_committed_revision), "%s", remote.revision);
    }
}
#ifdef ESP_PLATFORM
static QueueHandle_t live_events;
static SemaphoreHandle_t live_lock;
static bool live_saving;
static void live_take(void)
{
    if(live_lock)
        xSemaphoreTakeRecursive(live_lock, portMAX_DELAY);
}
static void live_give(void)
{
    if(live_lock)
        xSemaphoreGiveRecursive(live_lock);
}
#else
static void live_take(void) { }
static void live_give(void) { }
#endif
typedef enum
{
    LIVE_SELECT,
    LIVE_ITEM,
    LIVE_DELETE,
    LIVE_RESET,
    LIVE_COMPLETE,
    LIVE_AFC
} live_kind_t;
typedef struct
{
    live_kind_t kind;
    union
    {
        player_media_item_t item;
        uint64_t pid;
        char library[128];
    };
} live_event_t;
static MOCK_LARGE_BSS uint16_t live_album_slots[8192];
static int live_album(uint64_t pid)
{
    if(!live_album_ready)
    {
        memset(live_album_slots, 0, sizeof(live_album_slots));
        for(int i = 0; i < album_count; i++)
        {
            uint32_t hash = album_seen_hash(albums[i].album_pid);
            while(live_album_slots[hash])
                hash = (hash + 1) & 8191;
            live_album_slots[hash] = i + 1;
        }
        live_album_ready = true;
    }
    uint32_t hash = album_seen_hash(pid);
    for(unsigned n = 0; n < 8192; n++)
    {
        uint16_t slot = live_album_slots[hash];
        if(!slot)
            return -1;
        if(albums[slot - 1].album_pid == pid)
            return slot - 1;
        hash = (hash + 1) & 8191;
    }
    return -1;
}
static int song_compare(const void *left,const void *right)
{
    uint16_t ia=*(const uint16_t*)left,ib=*(const uint16_t*)right;
    const player_track_t *a=&tracks[ia],*b=&tracks[ib];
    int comparison=0;
    if(song_sort_field==0)
    {
        uint32_t ad=song_sort_dates[ia],bd=song_sort_dates[ib];
        if(!ad || !bd)
        {
            if(!ad && !bd)
            {
                uint32_t as=live_tracks[ia].discovered,bs=live_tracks[ib].discovered;
                comparison=(as>bs)-(as<bs);
            }
            else comparison=ad ? -1 : 1;
        }
        else comparison=(ad>bd)-(ad<bd);
    }
    else if(song_sort_field==1) comparison=strcasecmp(a->title,b->title);
    else comparison=(song_sort_years[ia]>song_sort_years[ib])-(song_sort_years[ia]<song_sort_years[ib]);
    if(comparison) return song_sort_desc ? -comparison : comparison;
    comparison=strcasecmp(a->title,b->title);
    if(!comparison) comparison=strcasecmp(a->artist,b->artist);
    return comparison ? comparison : (a->id>b->id)-(a->id<b->id);
}
void player_sort_songs(int field,bool desc)
{
    live_take();
    if(field<0 || field>2) field=0;
    if(song_order_ready && song_order_count==track_count &&
       song_sort_field==field && song_sort_desc==desc)
    {
        live_give();
        return;
    }
    song_sort_field=field;song_sort_desc=desc;
    for(int i=0;i<track_count;i++)
    {
        song_order[i]=(uint16_t)i;
        const media_index_track_t *dated=media_index_track(&live_afc,tracks[i].id);
        bool matched=dated && dated->album_pid==tracks[i].album_id;
        int album=live_album(tracks[i].album_id);
        song_sort_dates[i]=matched ? dated->date_added : 0;
        if(!live_afc.library_id[0] && live_tracks[i].afc && album>=0)
            song_sort_dates[i]=albums[album].date_added;
        song_sort_years[i]=tracks[i].year;
        if(!song_sort_years[i] && album>=0) song_sort_years[i]=albums[album].year;
    }
    qsort(song_order,(size_t)track_count,sizeof(*song_order),song_compare);
    song_order_count=track_count;song_order_ready=true;
    live_give();
}
int player_song_index(uint64_t pid)
{
    int index=track_index_by_id(pid);
    if(index<0 || !song_order_ready || song_order_count!=track_count) return index;
    for(int i=0;i<song_order_count;i++) if(song_order[i]==index) return i;
    return -1;
}
static void live_load_afc(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/afc_index.bin", live_directory);
    media_index_clear(&live_afc);
    media_index_load(&live_afc, path, live_library);
}
static void live_dates(void)
{
    song_order_ready=false;
    for(int a = 0; a < album_count; a++)
    {
        if(live_afc.library_id[0])
        {
            const media_index_album_t *indexed = media_index_album(&live_afc, albums[a].album_pid);
            albums[a].afc_synced = indexed != NULL;
            if(indexed && !albums[a].title[0] && indexed->title[0])
                copy_str(albums[a].title,sizeof(albums[a].title),indexed->title);
            if(indexed && !albums[a].artist[0] && indexed->artist[0])
                copy_str(albums[a].artist,sizeof(albums[a].artist),indexed->artist);
            if(indexed && indexed->year)
                albums[a].year = indexed->year;
            albums[a].date_added = 0;
        }
        albums[a].track_count = 0;
    }
    for(int i = 0; i < artist_count; i++)
        artists[i].song_count = artists[i].album_count = 0;
    for(int i = 0; i < track_count; i++)
    {
        int a = live_album(tracks[i].album_id);
        if(a < 0)
            continue;
        player_album_t *album = &albums[a];
        album->track_count++;
        const media_index_track_t *dated = media_index_track(&live_afc, tracks[i].id);
        if(live_afc.library_id[0])
            live_tracks[i].afc = dated && dated->album_pid == album->album_pid;
        if(!live_tracks[i].afc)
            album->afc_synced = false;
        if(dated && dated->date_added > album->date_added)
            album->date_added = dated->date_added;
        if(dated && dated->album_pid==album->album_pid)
        {
            if(dated->disc_no) tracks[i].disc_no=dated->disc_no;
            if(dated->track_no) tracks[i].track_no=dated->track_no;
        }
        album_seen_insert_unlocked(album->album_pid, live_tracks[i].discovered);
        album_seen_t *seen = album_seen_find_unlocked(album->album_pid);
        if(seen && seen->seq < live_tracks[i].discovered)
            seen->seq = live_tracks[i].discovered;
        if(tracks[i].artist_id && tracks[i].artist_id <= (uint64_t)artist_count)
            artists[tracks[i].artist_id - 1].song_count++;
    }
    for(int a = 0; a < album_count; a++)
        if(albums[a].artist_id && albums[a].artist_id <= (uint64_t)artist_count)
            artists[albums[a].artist_id - 1].album_count++;
}
static void live_covers(void)
{
#ifdef ESP_PLATFORM
    /* Tabs stay unavailable without SD. Failed cache opens must not advance
     * the revision and trigger another whole-catalog retry on every tick. */
    if(!app_config_sd_ready())
        return;
    for(int i = 0; i < album_count; i++)
    {
        uint32_t slot = thumb_cache_slot(albums[i].album_pid, false);
        albums[i].cover_index = slot ? PLAYER_REAL_COVER_BASE + slot : 0;
    }
#endif
}
#pragma pack(push, 1)
typedef struct
{
    char magic[8], library[128];
    uint32_t tracks, albums, artists, batch, crc;
    uint32_t track_size, album_size, artist_size, seen_size;
} live_header_t;
#pragma pack(pop)
/* Validate in a streaming first pass, then load in place. A corrupt or foreign
 * snapshot cannot replace a working imported catalog. */
static bool live_load_one(const char *path)
{
    FILE *f = fopen(path, "rb");
    if(!f)
        return false;
    live_header_t h;
    bool ok = false;
    if(fread(&h, 1, sizeof(h), f) != sizeof(h) || memcmp(h.magic, "T5LIV001", 8)
        || !memchr(h.library, 0, sizeof(h.library)) || strcmp(h.library, live_library)
        || h.tracks > MAX_TRACKS || h.albums > MAX_ALBUMS || h.artists > MAX_ARTISTS
        || h.track_size != sizeof(*tracks) || h.album_size != sizeof(*albums)
        || h.artist_size != sizeof(*artists) || h.seen_size != sizeof(*live_tracks))
        goto done;
    size_t size = (size_t)h.tracks * (sizeof(*tracks) + sizeof(*live_tracks))
        + (size_t)h.albums * sizeof(*albums) + (size_t)h.artists * sizeof(*artists);
    uint8_t buffer[4096];
    uint32_t crc = 0;
    for(size_t left = size; left;)
    {
        size_t n = left > sizeof(buffer) ? sizeof(buffer) : left;
        if(fread(buffer, 1, n, f) != n)
            goto done;
        crc = store_crc32(crc, buffer, n);
        left -= n;
    }
    if(crc != h.crc || fgetc(f) != EOF || fseek(f, sizeof(h), SEEK_SET))
        goto done;
    if(fread(tracks, sizeof(*tracks), h.tracks, f) != h.tracks
        || fread(albums, sizeof(*albums), h.albums, f) != h.albums
        || fread(artists, sizeof(*artists), h.artists, f) != h.artists
        || fread(live_tracks, sizeof(*live_tracks), h.tracks, f) != h.tracks)
        goto done;
    track_count = h.tracks;
    album_count = h.albums;
    artist_count = h.artists;
    album_seen_next_seq = h.batch ? h.batch : 1;
    real_library = true;
    ok = true;
    live_load_remote(h.crc);
done:
    fclose(f);
    return ok;
}
static void live_load(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/catalog.bin", live_directory);
    if(!live_load_one(path))
    {
        snprintf(path, sizeof(path), "%s/catalog.bin.bak", live_directory);
        live_load_one(path);
    }
}
#ifdef ESP_PLATFORM
typedef struct
{
    char path[256], library[128], remote_revision[128];
    live_header_t header;
    uint32_t revision;
    player_album_t *album_snapshot;
} live_save_t;
static void live_save_task(void *argument)
{
    live_save_t *job = argument;
    char temporary[272];
    snprintf(temporary, sizeof(temporary), "%s.tmp", job->path);
    FILE *file = fopen(temporary, "wb");
    bool ok = file != NULL;
    if(ok)
        ok = fwrite(&job->header, 1, sizeof(job->header), file) == sizeof(job->header);
    const void *arrays[] = { tracks, job->album_snapshot, artists, live_tracks };
    size_t sizes[] = { (size_t)job->header.tracks * sizeof(*tracks),
        (size_t)job->header.albums * sizeof(*albums),
        (size_t)job->header.artists * sizeof(*artists),
        (size_t)job->header.tracks * sizeof(*live_tracks) };
    uint8_t buffer[4096];
    uint32_t crc = 0;
    for(unsigned a = 0; a < 4 && ok; a++)
        for(size_t off = 0; off < sizes[a] && ok;)
        {
            size_t n = sizes[a] - off;
            if(n > sizeof(buffer))
                n = sizeof(buffer);
            live_take();
            ok = job->revision == live_revision && !strcmp(job->library, live_library);
            if(ok)
                memcpy(buffer, (const uint8_t *)arrays[a] + off, n);
            live_give();
            if(ok)
            {
                crc = store_crc32(crc, buffer, n);
                ok = fwrite(buffer, 1, n, file) == n;
            }
            off += n;
            vTaskDelay(1);
        }
    job->header.crc = crc;
    if(ok)
        ok = !fseek(file, 0, SEEK_SET)
            && fwrite(&job->header, 1, sizeof(job->header), file) == sizeof(job->header)
            && !fflush(file) && !fsync(fileno(file));
    if(file && fclose(file))
        ok = false;
    live_take();
    if(ok && job->revision == live_revision && !strcmp(job->library, live_library))
    {
        ok = store_replace(job->path, temporary);
        if(ok)
        {
            live_remote_t remote = { .catalog_crc = crc };
            memcpy(remote.magic, "T5REV001", 8);
            snprintf(remote.library, sizeof(remote.library), "%s", job->library);
            snprintf(remote.revision, sizeof(remote.revision), "%s", job->remote_revision);
            remote.crc = store_crc32(0, &remote, offsetof(live_remote_t, crc));
            char remote_path[256];
            snprintf(remote_path, sizeof(remote_path), "%s/catalog.rev", live_directory);
            if(store_write_atomic(remote_path, &remote, sizeof(remote)))
                snprintf(live_committed_revision, sizeof(live_committed_revision), "%s",
                    job->remote_revision);
            live_dirty = false;
        }
    }
    else
        ok = false;
    live_saving = false;
    live_give();
    if(!ok)
        remove(temporary);
    ESP_LOGI(TAG, "live catalog save %s (%u tracks)", ok ? "complete" : "deferred",
        (unsigned)job->header.tracks);
    free(job->album_snapshot);
    free(job);
    vTaskDelete(NULL);
}
static void live_save(void)
{
    if(live_saving || !live_dirty || live_in_batch || !app_config_sd_ready())
        return;
    live_save_t *job = calloc(1, sizeof(*job));
    if(!job)
        return;
    snprintf(job->path, sizeof(job->path), "%s/catalog.bin", live_directory);
    snprintf(job->library, sizeof(job->library), "%s", live_library);
    job->revision = live_revision;
    snprintf(job->remote_revision, sizeof(job->remote_revision), "%s", live_remote_revision);
    live_header_t *h = &job->header;
    memcpy(h->magic, "T5LIV001", 8);
    snprintf(h->library, sizeof(h->library), "%s", live_library);
    h->tracks = track_count;
    h->albums = album_count;
    h->artists = artist_count;
    h->batch = album_seen_next_seq;
    h->track_size = sizeof(*tracks);
    h->album_size = sizeof(*albums);
    h->artist_size = sizeof(*artists);
    h->seen_size = sizeof(*live_tracks);
    /* Album sorting can occur while the worker streams tracks. Snapshot only
     * these small reorderable rows; metadata mutations still abort by revision. */
    job->album_snapshot = malloc((album_count ? (size_t)album_count : 1) * sizeof(*albums));
    if(!job->album_snapshot)
    {
        free(job);
        return;
    }
    memcpy(job->album_snapshot, albums, (size_t)album_count * sizeof(*albums));
    live_saving = true;
    if(xTaskCreateWithCaps(
           live_save_task, "library_save", 8192, job, 1, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        != pdPASS)
    {
        live_saving = false;
        free(job->album_snapshot);
        free(job);
    }
}
#else
static void live_save(void) { }
#endif
static void live_init(void)
{
    song_order_ready=false;
    memset(live_tracks, 0, sizeof(live_tracks));
    media_index_clear(&live_afc);
    for(int i = 0; i < track_count; i++)
        live_tracks[i] = (live_track_t) { .present = true, .afc = real_library };
    /* Convert the legacy importer once; cover slots retain their old numbers. */
    if(real_library)
    {
        for(int i = 0; i < track_count; i++)
        {
            uint64_t old = tracks[i].album_id;
            if(old && old <= MAX_ALBUMS && legacy_album_pids[old - 1])
                tracks[i].album_id = legacy_album_pids[old - 1];
        }
        for(int i = 0; i < album_count; i++)
            if(albums[i].album_pid)
                albums[i].id = albums[i].album_pid;
    }
    live_suspended = false;
    live_in_batch = false;
    live_dirty = false;
    live_publish = false;
    live_batch = 0;
    live_remote_revision[0] = live_committed_revision[0] = 0;
    live_album_ready = false;
#ifdef ESP_PLATFORM
    if(!live_lock)
        live_lock = xSemaphoreCreateRecursiveMutex();
    if(!live_events)
        live_events
            = xQueueCreateWithCaps(96, sizeof(live_event_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snprintf(live_library, sizeof(live_library), "%s", app_config_library_id());
    snprintf(live_directory, sizeof(live_directory), "%s", app_config_library_dir());
    live_load();
    live_load_afc();
    live_dates();
    if(!g_app_config.iap2_mode)
        media_index_clear(&live_afc);
#else
    (void)live_load;
    (void)live_load_afc;
    (void)live_covers;
#endif
    live_revision++;
}
static void live_begin(void)
{
    if(live_in_batch)
        return;
    live_in_batch = true;
    live_batch = album_seen_next_seq++;
}
static void live_upsert(const player_media_item_t *item)
{
    if(!item->pid)
        return;
    int index = track_index_by_id(item->pid);
    bool added = index < 0;
    if(added && (!item->title[0] || !item->album_pid || track_count == MAX_TRACKS))
        return;
    live_begin();
    live_revision++;
    if(added)
    {
        index = track_count++;
        memset(&tracks[index], 0, sizeof(tracks[index]));
        live_tracks[index] = (live_track_t) { .discovered = live_batch };
        tracks[index].id = item->pid;
        uint32_t hash = track_id_hash(item->pid);
        while(track_id_slots[hash])
            hash = (hash + 1) & (TRACK_ID_SLOTS - 1);
        track_id_slots[hash] = index + 1;
        track_id_index_ready = true;
    }
    player_track_t *track = &tracks[index];
    player_track_t before = *track;
    uint64_t album_pid = (item->fields & (1u << 5)) ? item->album_pid : track->album_id;
    int a = live_album(album_pid);
    if(a < 0 && album_pid && album_count < MAX_ALBUMS)
    {
        a = album_count++;
        memset(&albums[a], 0, sizeof(albums[a]));
        albums[a].id = albums[a].album_pid = album_pid;
        albums[a].color = 0x303030;
        live_album_ready = false;
    }
    if(a < 0)
        return;
    player_album_t *album = &albums[a];
    const char *name;
    if(item->fields & (1u << 14))
        name = item->album_artist[0] ? item->album_artist : item->artist;
    else
        name = album->artist[0] ? album->artist : item->artist;
    if((item->fields & (1u << 6)) && item->album[0])
        copy_str(album->title, sizeof(album->title), item->album);
    if(name[0])
    {
        copy_str(album->artist, sizeof(album->artist), name);
        int artist = -1;
        for(int i = 0; i < artist_count; i++)
            if(!strcmp(artists[i].name, name))
            {
                artist = i;
                break;
            }
        if(artist < 0 && artist_count < MAX_ARTISTS)
        {
            artist = artist_count++;
            memset(&artists[artist], 0, sizeof(artists[artist]));
            artists[artist].id = artist + 1;
            copy_str(artists[artist].name, sizeof(artists[artist].name), name);
        }
        if(artist >= 0)
            album->artist_id = artists[artist].id;
    }
    track->album_id = album_pid;
    track->artist_id = album->artist_id;
    if(before.album_id != album_pid)
    {
        int previous = live_album(before.album_id);
        if(previous >= 0 && albums[previous].track_count)
            albums[previous].track_count--;
        album->track_count++;
    }
    if((item->fields & (1u << 1)) && item->title[0])
        copy_str(track->title, sizeof(track->title), item->title);
    if((item->fields & (1u << 12)) && item->artist[0])
        copy_str(track->artist, sizeof(track->artist), item->artist);
    if((item->fields & (1u << 6)) && item->album[0])
        copy_str(track->album, sizeof(track->album), item->album);
    if(item->fields & (1u << 4))
        track->duration_sec = (item->duration_ms + 500) / 1000;
    if(item->fields & (1u << 7))
        track->track_no = item->track_no;
    if(item->fields & (1u << 9))
        track->disc_no = item->disc_no;
    if(item->year)
        track->year = album->year = item->year;
    live_tracks[index].present = true;
    const media_index_track_t *dated = media_index_track(&live_afc, item->pid);
    if(live_afc.library_id[0])
        live_tracks[index].afc = dated && dated->album_pid == album_pid;
    if(!live_tracks[index].afc)
        album->afc_synced = false;
    if(dated && dated->date_added > album->date_added)
        album->date_added = dated->date_added;
    album_seen_insert_unlocked(album_pid, live_tracks[index].discovered);
    time_t now = time(NULL);
    live_tracks[index].last_seen = now > 1000000000 ? (uint32_t)now : 0;
    if(added || memcmp(track, &before, sizeof(*track)))
    {
        song_order_ready=false;
        membership_index_ready = false;
        live_dirty = true;
        live_publish = true;
    }
}
static void live_finish(void)
{
    int kept = 0;
    for(int i = 0; i < track_count; i++)
        if(live_tracks[i].present)
        {
            if(i != kept)
            {
                tracks[kept] = tracks[i];
                live_tracks[kept] = live_tracks[i];
                track_starred[kept] = track_starred[i];
            }
            kept++;
        }
    if(kept != track_count)
    {
        track_count = kept;
        live_dirty = true;
    }
    live_dates();
    kept = 0;
    for(int a = 0; a < album_count; a++)
        if(albums[a].track_count)
            albums[kept++] = albums[a];
    album_count = kept;
    live_album_ready = false;
    track_id_index_build();
    membership_index_build();
    live_covers();
    player_sort_albums(album_sort_field, album_sort_desc);
    live_in_batch = false;
    live_revision++;
    live_dirty = true;
    live_publish = true;
    revision++;
}
static void live_apply(const live_event_t *event)
{
    if(live_suspended)
        return;
    switch(event->kind)
    {
    case LIVE_ITEM:
        live_upsert(&event->item);
        break;
    case LIVE_DELETE:
    {
        live_revision++;
        live_begin();
        int i = track_index_by_id(event->pid);
        if(i >= 0)
        {
            live_tracks[i].present = false;
            live_dirty = true;
        }
        break;
    }
    case LIVE_RESET:
        live_revision++;
        live_begin();
        for(int i = 0; i < track_count; i++)
            live_tracks[i].present = false;
        break;
    case LIVE_COMPLETE:
        if(event->library[0])
            snprintf(live_remote_revision, sizeof(live_remote_revision), "%s", event->library);
        live_finish();
        break;
    case LIVE_AFC:
        live_load_afc();
        live_dates();
        membership_index_build();
        live_covers();
        live_revision++;
        live_dirty = true;
        live_publish = true;
#ifdef ESP_PLATFORM
        if(!g_app_config.iap2_mode)
            media_index_clear(&live_afc);
#endif
        break;
    case LIVE_SELECT:
#ifdef ESP_PLATFORM
    {
        char canonical[128];
        snprintf(canonical, sizeof(canonical), "%s", event->library);
        char *suffix = strstr(canonical, "-MPB-");
        if(!suffix)
            suffix = strstr(canonical, "-mpb-");
        if(suffix)
            *suffix = 0;
        if(!strcasecmp(canonical, live_library))
            break;
        thumb_cache_close();
        app_config_set_library_id(canonical);
        track_count = album_count = artist_count = playlist_count = queue_count = 0;
        real_library = true;
        memset(track_starred, 0, sizeof(track_starred));
        memset(album_seen, 0, sizeof(album_seen));
        memset(legacy_album_pids, 0, sizeof(legacy_album_pids));
        album_seen_next_seq = 1;
        load_library_bin(app_config_library_bin_path());
        live_init();
        track_id_index_build();
        membership_index_build();
        thumb_cache_open(album_count);
        live_covers();
        star_store_load(app_config_library_stars_path(), tracks, track_count, track_starred);
        if(g_app_config.iap2_mode)
            thumb_cache_preload();
        live_publish = true;
        break;
    }
#else
        break;
#endif
    }
}
static void live_post(const live_event_t *event)
{
#ifdef ESP_PLATFORM
    if(!live_events)
        return;
    /* This runs on the ordered control worker, never the USB RX callback. */
    if(xQueueSend(live_events, event, pdMS_TO_TICKS(3000)) != pdTRUE)
        ESP_LOGE(TAG, "live library queue full; update not applied");
#else
    live_apply(event);
#endif
}
void player_library_select(const char *id)
{
    live_event_t e = { .kind = LIVE_SELECT };
    snprintf(e.library, sizeof(e.library), "%s", id);
    live_post(&e);
}
void player_library_update(const player_media_item_t *item)
{
    live_event_t e = { .kind = LIVE_ITEM, .item = *item };
    live_post(&e);
}
void player_library_delete(uint64_t pid)
{
    live_event_t e = { .kind = LIVE_DELETE, .pid = pid };
    live_post(&e);
}
void player_library_reset(void)
{
    live_event_t e = { .kind = LIVE_RESET };
    live_post(&e);
}
void player_library_complete_revision(const char *remote)
{
    live_event_t e = { .kind = LIVE_COMPLETE };
    if(remote)
        snprintf(e.library, sizeof(e.library), "%s", remote);
    live_post(&e);
}
void player_library_complete(void)
{
    player_library_complete_revision(NULL);
}
bool player_library_cached_revision(const char *library, char *out, size_t size)
{
    if(!library || !out || !size)
        return false;
    char base[128];
    snprintf(base, sizeof(base), "%s", library);
    char *suffix = strstr(base, "-MPB-");
    if(!suffix)
        suffix = strstr(base, "-mpb-");
    if(suffix)
        *suffix = 0;
    live_take();
    bool ok = !strcasecmp(base, live_library) && live_committed_revision[0];
    if(ok)
        snprintf(out, size, "%s", live_committed_revision);
    live_give();
    return ok;
}
void player_library_refresh_afc(void)
{
    live_event_t e = { .kind = LIVE_AFC };
    live_post(&e);
}
void player_library_discard_snapshot(void)
{
    live_take();
    live_suspended = true;
    live_dirty = false;
    live_revision++;
    live_remote_revision[0] = live_committed_revision[0] = 0;
    const char *files[] = { "catalog.bin", "catalog.bin.bak", "catalog.rev", "catalog.rev.bak" };
    for(unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", live_directory, files[i]);
        remove(path);
    }
    live_give();
}
uint32_t player_library_revision(void)
{
    return live_revision;
}
void player_library_status(player_library_status_t *out)
{
    if(!out)
        return;
    live_take();
    *out = (player_library_status_t) {
        .albums = album_count, .tracks = track_count, .revision = live_revision
    };
    for(int i = 0; i < album_count; i++)
        out->pending_albums += !albums[i].afc_synced;
    live_give();
}
bool player_library_album_pid(uint64_t track_pid, uint64_t *pid)
{
    int i = track_index_by_id(track_pid);
    if(i < 0 || !pid)
        return false;
    *pid = tracks[i].album_id;
    return *pid != 0;
}
bool player_library_poll(void)
{
    live_take();
    if(live_suspended)
    {
        live_give();
        return false;
    }
#ifdef ESP_PLATFORM
    live_event_t event;
    int64_t start = esp_timer_get_time();
    for(unsigned n = 0; n < 64 && esp_timer_get_time() - start < 5000; n++)
    {
        if(xQueueReceive(live_events, &event, 0) != pdTRUE)
            break;
        live_apply(&event);
    }
    static uint32_t cache_revision;
    uint32_t current = thumb_cache_revision();
    if(cache_revision != current)
    {
        cache_revision = current;
        live_covers();
        live_revision++;
        live_publish = true;
    }
#endif
    live_save();
    bool changed = live_publish;
    live_publish = false;
    live_give();
    return changed;
}
