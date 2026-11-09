/* Immutable SQLite snapshot reader. Overlay only checksum-verified, committed
 * WAL frames; the copied Apple database is never opened for writing. */
#include "media_index.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

typedef struct
{
    FILE *db, *wal;
    uint32_t page_size, pages;
    uint64_t size, *offsets;
    const char *error;
} snapshot_t;
typedef struct
{
    sqlite3_file base;
    snapshot_t *snapshot;
} snapshot_file_t;
static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t word(const uint8_t *p, bool big)
{
    return big ? be32(p) : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
static void checksum(const uint8_t *p, size_t n, bool big, uint32_t *a, uint32_t *b)
{
    for(size_t i = 0; i < n; i += 8)
    {
        *a += word(p + i, big) + *b;
        *b += word(p + i + 4, big) + *a;
    }
}
static void snapshot_close(snapshot_t *s)
{
    if(s->db)
        fclose(s->db);
    if(s->wal)
        fclose(s->wal);
    free(s->offsets);
    memset(s, 0, sizeof(*s));
}
static bool snapshot_open(snapshot_t *s, const char *db, const char *wal)
{
    uint8_t header[100];
    s->error = "Cannot open AFC database";
    s->db = fopen(db, "rb");
    if(!s->db)
        return false;
    s->error = "Invalid AFC database header";
    if(fread(header, 1, 100, s->db) != 100 || memcmp(header, "SQLite format 3\0", 16))
        return false;
    s->page_size = (uint32_t)header[16] << 8 | header[17];
    if(s->page_size == 1)
        s->page_size = 65536;
    s->error = "Invalid AFC database page size";
    if(s->page_size < 512 || s->page_size > 65536 || (s->page_size & (s->page_size - 1)))
        return false;
    s->error = "Invalid AFC database size";
    if(fseek(s->db, 0, SEEK_END) || ftell(s->db) < 0)
        return false;
    s->size = (uint64_t)ftell(s->db);
    s->pages = s->size / s->page_size;
    if(s->size > 512u * 1024 * 1024 || s->size % s->page_size)
        return false;
    s->wal = wal ? fopen(wal, "rb") : NULL;
    if(!s->wal)
        return true;
    size_t got = fread(header, 1, 32, s->wal);
    if(got == 0)
        return true;
    s->error = "Truncated AFC WAL header";
    if(got != 32)
        return false;
    uint32_t magic = be32(header), a = 0, b = 0;
    bool big = magic & 1;
    s->error = "Invalid AFC WAL format/page size";
    if((magic != 0x377f0682 && magic != 0x377f0683) || be32(header + 4) != 3007000
        || be32(header + 8) != s->page_size)
        return false;
    checksum(header, 24, big, &a, &b);
    s->error = "Invalid AFC WAL header checksum";
    if(a != be32(header + 24) || b != be32(header + 28))
        return false;
    s->error = "Not enough memory for AFC WAL";
    uint8_t salts[8];
    memcpy(salts, header + 16, 8);
    uint8_t *page = malloc(s->page_size);
    if(!page)
        return false;
    uint32_t frame = 0, committed = 0, db_pages = s->pages;
    while(fread(header, 1, 24, s->wal) == 24)
    {
        if(!be32(header) || memcmp(header + 8, salts, 8)
            || fread(page, 1, s->page_size, s->wal) != s->page_size)
            break;
        checksum(header, 8, big, &a, &b);
        checksum(page, s->page_size, big, &a, &b);
        if(a != be32(header + 16) || b != be32(header + 20))
            break;
        frame++;
        if(be32(header + 4))
        {
            committed = frame;
            db_pages = be32(header + 4);
        }
#ifdef ESP_PLATFORM
        if((frame & 127) == 0)
            vTaskDelay(1);
#endif
    }
    free(page);
    if(!committed)
        return true;
    s->error = "Invalid AFC WAL commit size";
    if(!db_pages || (uint64_t)db_pages * s->page_size > 512u * 1024 * 1024)
        return false;
    s->error = "Not enough memory for AFC WAL page map";
    s->offsets = calloc((size_t)db_pages + 1, sizeof(*s->offsets));
    if(!s->offsets)
        return false;
    s->pages = db_pages;
    s->size = (uint64_t)db_pages * s->page_size;
    for(uint32_t i = 0; i < committed; i++)
    {
        uint64_t pos = 32 + (uint64_t)i * (24 + s->page_size);
        s->error = "Cannot reread committed AFC WAL";
        if(fseek(s->wal, (long)pos, SEEK_SET) || fread(header, 1, 24, s->wal) != 24)
            return false;
        uint32_t number = be32(header);
        if(number <= db_pages)
            s->offsets[number] = pos + 24;
    }
    return true;
}
static int file_close(sqlite3_file *f)
{
    (void)f;
    return SQLITE_OK;
}
static int file_read(sqlite3_file *f, void *dst, int n, sqlite3_int64 offset)
{
    snapshot_t *s = ((snapshot_file_t *)f)->snapshot;
    uint8_t *out = dst;
    if(offset < 0 || n < 0)
        return SQLITE_IOERR_READ;
    memset(dst, 0, n);
    int left = n;
    while(left && (uint64_t)offset < s->size)
    {
        uint32_t page = (uint64_t)offset / s->page_size + 1, inside = offset % s->page_size;
        size_t count = s->page_size - inside;
        if(count > (unsigned)left)
            count = left;
        uint64_t source = s->offsets ? s->offsets[page] : 0;
        FILE *file = source ? s->wal : s->db;
        uint64_t pos = source ? source + inside : (uint64_t)offset;
        /* IDF's size-optimized stdio can turn fread into byte-sized VFS
         * traffic. SQLite page reads must reach FATFS as bounded blocks. */
        int fd = fileno(file);
        if(lseek(fd, (off_t)pos, SEEK_SET) < 0)
            return SQLITE_IOERR_READ;
        size_t copied = 0;
        while(copied < count)
        {
            ssize_t got = read(fd, out + copied, count - copied);
            if(got < 0 && errno == EINTR)
                continue;
            if(got <= 0)
                return SQLITE_IOERR_READ;
            copied += (size_t)got;
        }
        /* WAL has already been overlaid: expose rollback-journal header. */
        if(offset <= 18 && offset + (sqlite3_int64)count > 18)
            out[18 - offset] = 1;
        if(offset <= 19 && offset + (sqlite3_int64)count > 19)
            out[19 - offset] = 1;
        left -= count;
        out += count;
        offset += count;
    }
    return left ? SQLITE_IOERR_SHORT_READ : SQLITE_OK;
}
static int file_write(sqlite3_file *f, const void *p, int n, sqlite3_int64 o)
{
    (void)f;
    (void)p;
    (void)n;
    (void)o;
    return SQLITE_READONLY;
}
static int file_truncate(sqlite3_file *f, sqlite3_int64 n)
{
    (void)f;
    (void)n;
    return SQLITE_READONLY;
}
static int file_sync(sqlite3_file *f, int flags)
{
    (void)f;
    (void)flags;
    return SQLITE_OK;
}
static int file_size(sqlite3_file *f, sqlite3_int64 *size)
{
    *size = ((snapshot_file_t *)f)->snapshot->size;
    return SQLITE_OK;
}
static int file_lock(sqlite3_file *f, int lock)
{
    (void)f;
    (void)lock;
    return SQLITE_OK;
}
static int file_reserved(sqlite3_file *f, int *result)
{
    (void)f;
    *result = 0;
    return SQLITE_OK;
}
static int file_control(sqlite3_file *f, int op, void *arg)
{
    (void)f;
    (void)op;
    (void)arg;
    return SQLITE_NOTFOUND;
}
static int file_sector(sqlite3_file *f)
{
    (void)f;
    return 512;
}
static int file_device(sqlite3_file *f)
{
    (void)f;
    return SQLITE_IOCAP_IMMUTABLE;
}
static const sqlite3_io_methods methods = { .iVersion = 1,
    .xClose = file_close,
    .xRead = file_read,
    .xWrite = file_write,
    .xTruncate = file_truncate,
    .xSync = file_sync,
    .xFileSize = file_size,
    .xLock = file_lock,
    .xUnlock = file_lock,
    .xCheckReservedLock = file_reserved,
    .xFileControl = file_control,
    .xSectorSize = file_sector,
    .xDeviceCharacteristics = file_device };
static int vfs_open(sqlite3_vfs *v, const char *name, sqlite3_file *file, int flags, int *out)
{
    (void)name;
    if(!(flags & SQLITE_OPEN_MAIN_DB) || !(flags & SQLITE_OPEN_READONLY))
        return SQLITE_CANTOPEN;
    snapshot_file_t *f = (snapshot_file_t *)file;
    f->base.pMethods = &methods;
    f->snapshot = v->pAppData;
    if(out)
        *out = SQLITE_OPEN_READONLY;
    return SQLITE_OK;
}
static int vfs_delete(sqlite3_vfs *v, const char *p, int sync)
{
    (void)v;
    (void)p;
    (void)sync;
    return SQLITE_READONLY;
}
static int vfs_access(sqlite3_vfs *v, const char *p, int flags, int *out)
{
    (void)v;
    (void)p;
    (void)flags;
    *out = 0;
    return SQLITE_OK;
}
static int vfs_path(sqlite3_vfs *v, const char *p, int n, char *out)
{
    (void)v;
    return snprintf(out, n, "%s", p) < n ? SQLITE_OK : SQLITE_CANTOPEN;
}
static int vfs_random(sqlite3_vfs *v, int n, char *out)
{
    (void)v;
    for(int i = 0; i < n; i++)
        out[i] = (char)(i * 37 + 11);
    return n;
}
static int vfs_sleep(sqlite3_vfs *v, int us)
{
    (void)v;
    return us;
}
static int vfs_time(sqlite3_vfs *v, double *value)
{
    (void)v;
    *value = 2440587.5 + (double)time(NULL) / 86400;
    return SQLITE_OK;
}
static int query(sqlite3 *db, sqlite3_stmt **stmt, const char *sql)
{
    return sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
}
static void copy_column(char *out, size_t size, sqlite3_stmt *stmt, int column)
{
    const unsigned char *p = sqlite3_column_text(stmt, column);
    snprintf(out, size, "%s", p ? (const char *)p : "");
}
static uint32_t date_column(sqlite3_stmt *stmt, int column)
{
    sqlite3_int64 t = sqlite3_column_int64(stmt, column);
    return t > 0 && t < UINT32_MAX - 978307200u ? (uint32_t)t + 978307200u : 0;
}
static int track_album_compare(const void *left, const void *right)
{
    uint64_t a = ((const media_index_track_t *)left)->album_pid;
    uint64_t b = ((const media_index_track_t *)right)->album_pid;
    return (a > b) - (a < b);
}

bool media_index_import(media_index_t *out, const char *database, const char *wal,
    const char *library_id, char *error, size_t error_size)
{
    snapshot_t snapshot = { 0 };
    media_index_t next = { 0 };
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool ok = false;
    const char *failure = "Invalid AFC snapshot/WAL";
    sqlite3_vfs vfs = { .iVersion = 1,
        .szOsFile = sizeof(snapshot_file_t),
        .mxPathname = 320,
        .zName = "tab5-afc",
        .pAppData = &snapshot,
        .xOpen = vfs_open,
        .xDelete = vfs_delete,
        .xAccess = vfs_access,
        .xFullPathname = vfs_path,
        .xRandomness = vfs_random,
        .xSleep = vfs_sleep,
        .xCurrentTime = vfs_time };
    if(!snapshot_open(&snapshot, database, wal))
    {
        failure = snapshot.error;
        goto done;
    }
#ifdef ESP_PLATFORM
    ESP_LOGI("afc_sqlite", "snapshot: page=%u pages=%u bytes=%llu WAL=%s",
        (unsigned)snapshot.page_size, (unsigned)snapshot.pages, (unsigned long long)snapshot.size,
        snapshot.offsets ? "committed overlay" : "base only");
#endif
    sqlite3_vfs_register(&vfs, 0);
    char sqlite_error[160];
    int opened = sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY, "tab5-afc");
    if(opened != SQLITE_OK)
    {
        snprintf(sqlite_error, sizeof(sqlite_error), "SQLite open %d: %s", opened,
            db ? sqlite3_errmsg(db) : "no memory");
        failure = sqlite_error;
        goto done;
    }
    failure = "Unsupported or incomplete AFC database";
    if(query(db, &stmt, "SELECT value FROM _MLDatabaseProperties WHERE key='_UUID'") != SQLITE_OK
        || sqlite3_step(stmt) != SQLITE_ROW)
        goto done;
    copy_column(next.library_id, sizeof(next.library_id), stmt, 0);
    if(strcasecmp(next.library_id, library_id))
    {
        failure = "AFC library UUID differs from iAP2 library";
        goto done;
    }
    snprintf(next.library_id, sizeof(next.library_id), "%s", library_id);
    sqlite3_finalize(stmt);
    stmt = NULL;
    next.albums = calloc(MEDIA_INDEX_MAX_ALBUMS, sizeof(*next.albums));
    next.tracks = calloc(MEDIA_INDEX_MAX_TRACKS, sizeof(*next.tracks));
    if(!next.albums || !next.tracks)
    {
        failure = "Not enough memory for AFC metadata";
        goto done;
    }
    if(query(db, &stmt,
           "SELECT i.item_pid,i.album_pid,i.date_added,i.disc_number,i.track_number FROM item i "
           "JOIN item_extra e ON e.item_pid=i.item_pid WHERE i.in_my_library!=0 AND i.media_type=8 "
           "AND e.title IS NOT NULL AND e.title!=''")
        != SQLITE_OK)
    {
        if(stmt) sqlite3_finalize(stmt);
        stmt=NULL;
        if(query(db,&stmt,"SELECT i.item_pid,i.album_pid,i.date_added,0,0 FROM item i "
            "JOIN item_extra e ON e.item_pid=i.item_pid WHERE i.in_my_library!=0 AND i.media_type=8 "
            "AND e.title IS NOT NULL AND e.title!=''")!=SQLITE_OK) goto done;
    }
    int rc;
    while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        if(next.track_count == MEDIA_INDEX_MAX_TRACKS)
        {
            failure = "Track capacity exceeded";
            goto done;
        }
        media_index_track_t *t = &next.tracks[next.track_count++];
        t->pid = (uint64_t)sqlite3_column_int64(stmt, 0);
        t->album_pid = (uint64_t)sqlite3_column_int64(stmt, 1);
        t->date_added = date_column(stmt, 2);
        int disc=sqlite3_column_int(stmt,3),number=sqlite3_column_int(stmt,4);
        t->disc_no=disc>0 && disc<=65535?(uint16_t)disc:0;
        t->track_no=number>0 && number<=65535?(uint16_t)number:0;
        if(!t->pid)
            goto done;
#ifdef ESP_PLATFORM
        if((next.track_count & 127) == 0)
            vTaskDelay(1);
        if((next.track_count & 4095) == 0)
            ESP_LOGI("afc_sqlite", "indexed %u track dates", (unsigned)next.track_count);
#endif
    }
    if(rc != SQLITE_DONE)
        goto done;
    sqlite3_finalize(stmt);
    stmt = NULL;
    /* Aggregate dates in RAM from the track rows already read. A SQL join
     * and GROUP BY caused many random SD reads on the Apple schema. */
    qsort(next.tracks, next.track_count, sizeof(*next.tracks), track_album_compare);
    for(uint32_t i = 0; i < next.track_count; i++)
    {
        const media_index_track_t *track = &next.tracks[i];
        if(!track->album_pid)
            continue;
        if(!next.album_count || next.albums[next.album_count - 1].pid != track->album_pid)
        {
            if(next.album_count == MEDIA_INDEX_MAX_ALBUMS)
            {
                failure = "Album capacity exceeded";
                goto done;
            }
            next.albums[next.album_count++].pid = track->album_pid;
        }
        media_index_album_t *album = &next.albums[next.album_count - 1];
        if(track->date_added > album->date_added)
            album->date_added = track->date_added;
    }
    media_index_sort(&next);
    if(query(db, &stmt,
           "SELECT a.album_pid,a.album,aa.album_artist,a.album_year "
           "FROM album a LEFT JOIN album_artist aa ON aa.album_artist_pid=a.album_artist_pid")
        != SQLITE_OK)
    {
        if(stmt)
            sqlite3_finalize(stmt);
        stmt = NULL;
        if(query(db, &stmt,
               "SELECT a.album_pid,a.album,aa.album_artist,0 "
               "FROM album a LEFT JOIN album_artist aa ON aa.album_artist_pid=a.album_artist_pid")
            != SQLITE_OK)
            goto done;
    }
    while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        media_index_album_t *a = (media_index_album_t *)media_index_album(
            &next, (uint64_t)sqlite3_column_int64(stmt, 0));
        if(!a)
            continue;
        copy_column(a->title, sizeof(a->title), stmt, 1);
        copy_column(a->artist, sizeof(a->artist), stmt, 2);
        int year = sqlite3_column_int(stmt, 3);
        a->year = year > 0 && year <= 65535 ? (uint16_t)year : 0;
    }
    if(rc != SQLITE_DONE)
        goto done;
    sqlite3_finalize(stmt);
    stmt = NULL;
    media_index_sort(&next);
#ifdef ESP_PLATFORM
    ESP_LOGI("afc_sqlite", "dates ready: %u albums, %u tracks", (unsigned)next.album_count,
        (unsigned)next.track_count);
#endif
    /* Missing artwork tables are valid on devices without any cover art. */
    if(query(db, &stmt,
           "SELECT i.album_pid,a.relative_path FROM item i "
           "JOIN best_artwork_token b ON b.entity_pid=i.item_pid "
           "JOIN artwork a ON a.artwork_token=b.available_artwork_token "
           "LEFT JOIN album al ON al.album_pid=i.album_pid "
           "WHERE b.entity_type=0 AND b.artwork_type=1 AND a.relative_path!='' "
           "ORDER BY CASE WHEN i.item_pid=al.representative_item_pid THEN 0 ELSE 1 END,i.item_pid")
        == SQLITE_OK)
    {
        while((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            media_index_album_t *a = (media_index_album_t *)media_index_album(
                &next, (uint64_t)sqlite3_column_int64(stmt, 0));
            const char *path = (const char *)sqlite3_column_text(stmt, 1);
            if(!a || a->remote[0] || !path || !*path)
                continue;
            if(strstr(path, ".."))
                continue;
            int n = snprintf(a->remote, sizeof(a->remote), "%s%s",
                path[0] == '/' ? "" : "/iTunes_Control/iTunes/Artwork/Originals/", path);
            if(n < 0 || (size_t)n >= sizeof(a->remote))
            {
                a->remote[0] = 0;
                continue;
            }
            a->artwork_version = media_index_artwork_version(path);
        }
        if(rc != SQLITE_DONE)
            goto done;
    }
    /* Keep only actual rows while artwork decoding needs the remaining PSRAM. */
    media_index_album_t *small_albums
        = realloc(next.albums, (next.album_count ? next.album_count : 1) * sizeof(*next.albums));
    if(small_albums)
        next.albums = small_albums;
    media_index_track_t *small_tracks
        = realloc(next.tracks, (next.track_count ? next.track_count : 1) * sizeof(*next.tracks));
    if(small_tracks)
        next.tracks = small_tracks;
    media_index_clear(out);
    *out = next;
    memset(&next, 0, sizeof(next));
    ok = true;
done:
#ifdef ESP_PLATFORM
    if(!ok && db)
        ESP_LOGE("afc_sqlite", "%s: %s", failure, sqlite3_errmsg(db));
#endif
    if(error && error_size)
        snprintf(error, error_size, "%s", ok ? "" : failure);
    if(stmt)
        sqlite3_finalize(stmt);
    if(db)
        sqlite3_close(db);
    sqlite3_vfs_unregister(&vfs);
    snapshot_close(&snapshot);
    media_index_clear(&next);
    return ok;
}
