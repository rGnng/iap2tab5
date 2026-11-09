#include "thumb_cache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "thumb_cache_format.h"

static const char *TAG = "thumbs";

static FILE *s_pack;
static FILE *s_idx;
static uint8_t *s_valid;
static uint8_t *s_original;
static uint32_t s_count;
static size_t s_valid_bytes;
static uint32_t s_write_count;
static char s_pack_path[128];
static char s_idx_path[128];

#define THUMB_FLUSH_EVERY 32

static bool bit_get(const uint8_t *bits, uint32_t id)
{
    if(bits == NULL || id < 1 || id > s_count)
        return false;
    uint32_t n = id - 1;
    return (bits[n >> 3] & (1u << (n & 7))) != 0;
}

static void bit_set(uint8_t *bits, uint32_t id)
{
    if(bits == NULL || id < 1 || id > s_count)
        return;
    uint32_t n = id - 1;
    bits[n >> 3] |= (uint8_t)(1u << (n & 7));
}

static void make_paths(void)
{
    snprintf(s_pack_path, sizeof(s_pack_path), "%s/thumbs.bin", app_config_library_dir());
    snprintf(s_idx_path, sizeof(s_idx_path), "%s/thumbs.idx", app_config_library_dir());
}

static bool is_cached_cover(const char *name)
{
    const char *ext = name;
    while(*ext >= '0' && *ext <= '9')
        ext++;
    if(ext == name)
        return false;
    const char *suffixes[]
        = { ".thm", ".cov", ".jpg", ".jpeg", ".png", ".wbp", ".webp", ".gif", ".art" };
    for(size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
    {
        if(strcasecmp(ext, suffixes[i]) == 0)
            return true;
    }
    return false;
}

esp_err_t thumb_cache_reset(void)
{
    if(s_pack != NULL || s_idx != NULL || s_valid != NULL || !app_config_sd_ready())
        return ESP_ERR_INVALID_STATE;
    make_paths();
    /* Invalidate before deleting data; an interrupted reset cannot advertise
     * a missing pack as valid. No recursive directory deletion. */
    if(remove(s_idx_path) != 0 && errno != ENOENT)
        return ESP_FAIL;
    if(remove(s_pack_path) != 0 && errno != ENOENT)
        return ESP_FAIL;
    char mapping[192];
    snprintf(mapping, sizeof(mapping), "%s/thumbs.map", app_config_library_dir());
    remove(mapping);
    snprintf(mapping, sizeof(mapping), "%s/thumbs.map.bak", app_config_library_dir());
    remove(mapping);
    DIR *dir = opendir(app_config_artwork_dir());
    if(dir == NULL)
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t result = ESP_OK;
    unsigned removed = 0;
    struct dirent *ent;
    while((ent = readdir(dir)) != NULL)
    {
        if(!is_cached_cover(ent->d_name))
            continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", app_config_artwork_dir(), ent->d_name);
        struct stat st;
        if(stat(path, &st) != 0)
        {
            result = ESP_FAIL;
            continue;
        }
        if(!S_ISREG(st.st_mode))
            continue;
        if(remove(path) != 0)
            result = ESP_FAIL;
        else
            removed++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "artwork cache reset: pack/index and %u cover files removed", removed);
    return result;
}

/* All readers and writers share this lock, including library switches. */
#define CACHE_SLOTS 8192u
#include "player.h"
#include "store_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
static SemaphoreHandle_t s_mutex;
static uint64_t *s_pid, *s_version;
static uint8_t *s_ram;
static size_t s_ram_bytes;
static uint32_t s_revision, s_generation;
static char s_namespace[128];
static bool s_map_dirty;
static void lock(void)
{
    if(!s_mutex)
        s_mutex = xSemaphoreCreateRecursiveMutex();
    if(s_mutex)
        xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}
static void unlock(void)
{
    if(s_mutex)
        xSemaphoreGiveRecursive(s_mutex);
}
#pragma pack(push, 1)
typedef struct
{
    char magic[8], library[128];
    uint32_t count, crc;
} map_header_t;
typedef struct
{
    uint64_t pid, version;
} map_record_t;
#pragma pack(pop)
static bool map_save(void)
{
    if(!s_map_dirty)
        return true;
    size_t size = sizeof(map_header_t) + (size_t)s_count * sizeof(map_record_t);
    uint8_t *data = calloc(1, size);
    if(!data)
        return false;
    map_header_t *h = (map_header_t *)data;
    memcpy(h->magic, "T5MAP001", 8);
    snprintf(h->library, sizeof(h->library), "%s", s_namespace);
    h->count = s_count;
    map_record_t *r = (map_record_t *)(data + sizeof(*h));
    for(uint32_t i = 0; i < s_count; i++)
    {
        r[i].pid = s_pid[i];
        r[i].version = s_version[i];
    }
    h->crc = store_crc32(0, r, (size_t)s_count * sizeof(*r));
    char path[192];
    snprintf(path, sizeof(path), "%s/thumbs.map", app_config_library_dir());
    bool ok = store_write_atomic(path, data, size);
    free(data);
    if(ok)
        s_map_dirty = false;
    return ok;
}
static bool map_load_one(const char *path)
{
    FILE *f = fopen(path, "rb");
    if(!f)
        return false;
    map_header_t h;
    bool ok = false;
    if(fread(&h, 1, sizeof(h), f) != sizeof(h) || memcmp(h.magic, "T5MAP001", 8)
        || !memchr(h.library, 0, sizeof(h.library)) || strcmp(h.library, s_namespace)
        || h.count > s_count)
        goto done;
    map_record_t *r = calloc(h.count ? h.count : 1, sizeof(*r));
    if(!r)
        goto done;
    if(fread(r, sizeof(*r), h.count, f) == h.count && fgetc(f) == EOF
        && store_crc32(0, r, (size_t)h.count * sizeof(*r)) == h.crc)
    {
        for(uint32_t i = 0; i < h.count; i++)
        {
            s_pid[i] = r[i].pid;
            s_version[i] = r[i].version;
        }
        ok = true;
    }
    free(r);
done:
    fclose(f);
    return ok;
}
static bool ensure_files(void)
{
    if(!s_pack)
        s_pack = fopen(s_pack_path, "r+b");
    if(!s_idx)
        s_idx = fopen(s_idx_path, "r+b");
    return s_pack && s_idx;
}
static bool grow(uint32_t count)
{
    if(count <= s_count)
        return true;
    if(count > CACHE_SLOTS || !s_pack)
        return false;
    uint8_t *zero = calloc(1, 16384);
    if(!zero)
        return false;
    bool ok = fseek(s_pack, 0, SEEK_END) == 0;
    size_t left = (size_t)(count - s_count) * THUMB_CACHE_THUMB_SIZE;
    while(ok && left)
    {
        size_t n = left > 16384 ? 16384 : left;
        ok = fwrite(zero, 1, n, s_pack) == n;
        left -= n;
    }
    free(zero);
    if(!ok)
        return false;
    thumb_cache_header_t h
        = { .version = THUMB_CACHE_VERSION, .count = count, .thumb_size = THUMB_CACHE_THUMB_SIZE };
    memcpy(h.magic, THUMB_CACHE_MAGIC, 8);
    ok = fseek(s_pack, 0, SEEK_SET) == 0 && fwrite(&h, 1, sizeof(h), s_pack) == sizeof(h)
        && fflush(s_pack) == 0;
    if(ok)
    {
        s_count = count;
        s_map_dirty = true;
    }
    return ok;
}
esp_err_t thumb_cache_open(uint32_t album_count)
{
    (void)album_count;
    lock();
    if(s_valid && !strcmp(s_namespace, app_config_library_id()))
    {
        bool ok = ensure_files();
        unlock();
        return ok ? ESP_OK : ESP_FAIL;
    }
    thumb_cache_close();
    make_paths();
    s_generation++;
    snprintf(s_namespace, sizeof(s_namespace), "%s", app_config_library_id());
    s_valid_bytes = (CACHE_SLOTS + 7) / 8;
    s_valid = calloc(1, s_valid_bytes);
    s_original = calloc(1, s_valid_bytes);
    s_pid = calloc(CACHE_SLOTS, sizeof(*s_pid));
    s_version = calloc(CACHE_SLOTS, sizeof(*s_version));
    if(!s_valid || !s_original || !s_pid || !s_version)
        goto fail;
    s_pack = fopen(s_pack_path, "r+b");
    if(s_pack)
    {
        thumb_cache_header_t h;
        if(fread(&h, 1, sizeof(h), s_pack) != sizeof(h) || memcmp(h.magic, THUMB_CACHE_MAGIC, 8)
            || h.version != THUMB_CACHE_VERSION || h.thumb_size != THUMB_CACHE_THUMB_SIZE
            || h.count > CACHE_SLOTS)
        {
            ESP_LOGE(TAG, "invalid thumbnail pack; retaining file for recovery");
            goto fail;
        }
        s_count = h.count;
    }
    else
    {
        s_pack = fopen(s_pack_path, "w+b");
        if(!s_pack)
            goto fail;
        uint8_t h[THUMB_CACHE_HEADER_SIZE] = { 0 };
        thumb_cache_header_t *header = (void *)h;
        memcpy(header->magic, THUMB_CACHE_MAGIC, 8);
        header->version = THUMB_CACHE_VERSION;
        header->thumb_size = THUMB_CACHE_THUMB_SIZE;
        if(fwrite(h, 1, sizeof(h), s_pack) != sizeof(h) || fflush(s_pack))
            goto fail;
    }
    s_idx = fopen(s_idx_path, "r+b");
    if(s_idx)
        fread(s_valid, 1, s_valid_bytes, s_idx);
    else
    {
        s_idx = fopen(s_idx_path, "w+b");
        if(!s_idx)
            goto fail;
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/thumbs.map", app_config_library_dir());
    bool had_map = access(path, F_OK) == 0;
    bool mapped = map_load_one(path);
    if(!mapped)
    {
        snprintf(path, sizeof(path), "%s/thumbs.map.bak", app_config_library_dir());
        had_map = had_map || access(path, F_OK) == 0;
        mapped = map_load_one(path);
    }
    if(had_map && !mapped)
    {
        ESP_LOGE(TAG, "thumbnail map is corrupt or belongs to another library; retaining cache");
        goto fail;
    }
    if(!mapped)
    {
        /* Old numeric slots are translated once using the original library.bin.
         * Slot numbers and originals remain intact; new albums append slots. */
        for(uint32_t slot = 1; slot <= s_count; slot++)
            s_pid[slot - 1] = player_legacy_album_pid(slot);
        s_map_dirty = true;
        if(!map_save())
            goto fail;
    }
    s_revision++;
    ESP_LOGI(TAG, "PID cache ready: %u slots", (unsigned)s_count);
    unlock();
    return ESP_OK;
fail:
    thumb_cache_close();
    unlock();
    return ESP_FAIL;
}
uint32_t thumb_cache_slot(uint64_t pid, bool create)
{
    if(!pid)
        return 0;
    lock();
    if(!s_valid && thumb_cache_open(0) != ESP_OK)
    {
        unlock();
        return 0;
    }
    for(uint32_t i = 0; i < s_count; i++)
        if(s_pid[i] == pid)
        {
            unlock();
            return i + 1;
        }
    if(!create || !ensure_files())
    {
        unlock();
        return 0;
    }
    uint32_t slot = 0;
    for(uint32_t i = 0; i < s_count; i++)
        if(!s_pid[i] && !bit_get(s_valid, i + 1))
        {
            slot = i + 1;
            break;
        }
    if(!slot)
    {
        uint32_t old = s_count;
        if(!grow((old + 64) > CACHE_SLOTS ? CACHE_SLOTS : old + 64) || old == s_count)
        {
            unlock();
            return 0;
        }
        slot = old + 1;
    }
    s_pid[slot - 1] = pid;
    s_map_dirty = true;
    if(!map_save())
    {
        s_pid[slot - 1] = 0;
        slot = 0;
    }
    unlock();
    return slot;
}
uint64_t thumb_cache_pid(uint32_t slot)
{
    lock();
    uint64_t pid = slot && slot <= s_count ? s_pid[slot - 1] : 0;
    unlock();
    return pid;
}
bool thumb_cache_matches(uint32_t slot, uint64_t version)
{
    lock();
    bool ok = bit_get(s_valid, slot);
    if(ok && version && s_version[slot - 1] && version != s_version[slot - 1])
        ok = false;
    if(ok && version && !s_version[slot - 1])
    {
        s_version[slot - 1] = version;
        s_map_dirty = true;
    }
    unlock();
    return ok;
}
void thumb_cache_version(uint32_t slot, uint64_t version)
{
    lock();
    if(slot && slot <= s_count)
    {
        s_version[slot - 1] = version;
        s_map_dirty = true;
    }
    unlock();
}
bool thumb_cache_has(uint32_t slot)
{
    lock();
    bool ok = bit_get(s_valid, slot);
    unlock();
    return ok;
}
uint32_t thumb_cache_count(void)
{
    lock();
    uint32_t n = 0;
    for(uint32_t i = 1; i <= s_count; i++)
        n += s_pid[i - 1] != 0 && bit_get(s_valid, i);
    unlock();
    return n;
}
uint64_t thumb_cache_bytes(void)
{
    return (uint64_t)thumb_cache_count() * THUMB_CACHE_THUMB_SIZE;
}
bool thumb_cache_original(uint32_t slot)
{
    lock();
    bool ok = bit_get(s_original, slot);
    unlock();
    return ok;
}
void thumb_cache_set_original(uint32_t slot)
{
    lock();
    bit_set(s_original, slot);
    unlock();
}
esp_err_t thumb_cache_write(uint32_t slot, const uint8_t *thumb)
{
    lock();
    esp_err_t result = ESP_FAIL;
    if(!thumb || !slot || slot > s_count || !ensure_files())
        goto done;
    if(fseek(s_pack, (long)thumb_cache_slot_offset(slot), SEEK_SET)
        || fwrite(thumb, 1, THUMB_CACHE_THUMB_SIZE, s_pack) != THUMB_CACHE_THUMB_SIZE
        || fflush(s_pack))
        goto done;
    /* Publish validity only after the thumbnail data is durable. */
    if(fsync(fileno(s_pack)))
        goto done;
    bit_set(s_valid, slot);
    if(fseek(s_idx, (long)((slot - 1) / 8), SEEK_SET)
        || fputc(s_valid[(slot - 1) / 8], s_idx) == EOF || fflush(s_idx))
        goto done;
    if(fsync(fileno(s_idx)))
        goto done;
    size_t off = (size_t)thumb_cache_slot_offset(slot);
    if(s_ram && off + THUMB_CACHE_THUMB_SIZE <= s_ram_bytes)
        memcpy(s_ram + off, thumb, THUMB_CACHE_THUMB_SIZE);
    s_write_count++;
    s_revision++;
    result = ESP_OK;
done:
    unlock();
    return result;
}
esp_err_t thumb_cache_replace(uint64_t pid, uint64_t version, const uint8_t *thumb)
{
    lock();
    esp_err_t result = ESP_FAIL;
    uint32_t old = thumb_cache_slot(pid, false);
    if(!old || !thumb || !ensure_files())
        goto done;
    if(!bit_get(s_valid, old))
    {
        result = thumb_cache_write(old, thumb);
        if(result == ESP_OK)
        {
            s_version[old - 1] = version;
            s_map_dirty = true;
        }
        goto done;
    }
    uint32_t next = 0;
    for(uint32_t i = 0; i < s_count; i++)
        if(!s_pid[i] && !bit_get(s_valid, i + 1))
        {
            next = i + 1;
            break;
        }
    if(!next)
    {
        uint32_t previous = s_count;
        if(!grow(previous + 64 > CACHE_SLOTS ? CACHE_SLOTS : previous + 64) || previous == s_count)
            goto done;
        next = previous + 1;
    }
    if(thumb_cache_write(next, thumb) != ESP_OK)
        goto done;
    uint64_t old_version = s_version[old - 1];
    s_pid[next - 1] = pid;
    s_version[next - 1] = version;
    s_pid[old - 1] = 0;
    s_version[old - 1] = 0;
    s_map_dirty = true;
    if(!map_save())
    {
        s_pid[old - 1] = pid;
        s_version[old - 1] = old_version;
        s_pid[next - 1] = 0;
        s_version[next - 1] = 0;
        goto done;
    }
    /* Keep the retired slot's valid bit: it cannot be reused after a power
     * loss with an old original still on disk, and old UI readers stay valid
     * until they observe the new mapping revision. Counts exclude orphans. */
    s_revision++;
    result = ESP_OK;
done:
    unlock();
    return result;
}
bool thumb_cache_read(uint32_t slot, uint8_t *dst)
{
    lock();
    bool ok = false;
    if(dst && bit_get(s_valid, slot))
    {
        size_t off = (size_t)thumb_cache_slot_offset(slot);
        if(s_ram && off + THUMB_CACHE_THUMB_SIZE <= s_ram_bytes)
        {
            memcpy(dst, s_ram + off, THUMB_CACHE_THUMB_SIZE);
            ok = true;
        }
        else if(ensure_files())
            ok = !fseek(s_pack, (long)off, SEEK_SET)
                && fread(dst, 1, THUMB_CACHE_THUMB_SIZE, s_pack) == THUMB_CACHE_THUMB_SIZE;
    }
    unlock();
    return ok;
}
uint32_t thumb_cache_revision(void)
{
    lock();
    uint32_t r = s_revision;
    unlock();
    return r;
}
void thumb_cache_flush(void)
{
    lock();
    if(s_pack)
    {
        fflush(s_pack);
        fsync(fileno(s_pack));
    }
    if(s_idx)
    {
        fflush(s_idx);
        fsync(fileno(s_idx));
    }
    if(s_pid)
        map_save();
    unlock();
}
void thumb_cache_sync(void)
{
    thumb_cache_flush();
}
void thumb_cache_close(void)
{
    lock();
    thumb_cache_flush();
    if(s_pack)
        fclose(s_pack);
    if(s_idx)
        fclose(s_idx);
    s_pack = NULL;
    s_idx = NULL;
    free(s_valid);
    free(s_original);
    free(s_pid);
    free(s_version);
    free(s_ram);
    s_valid = NULL;
    s_original = NULL;
    s_pid = NULL;
    s_version = NULL;
    s_ram = NULL;
    s_ram_bytes = 0;
    s_count = 0;
    s_valid_bytes = 0;
    s_map_dirty = false;
    s_namespace[0] = 0;
    s_generation++;
    s_revision++;
    unlock();
}
static bool ext_is_original(const char *ext)
{
    return !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png")
        || !strcasecmp(ext, ".wbp") || !strcasecmp(ext, ".webp") || !strcasecmp(ext, ".gif");
}
esp_err_t thumb_cache_scan(const char *directory, uint32_t count)
{
    if(thumb_cache_open(count) != ESP_OK)
        return ESP_FAIL;
    lock();
    memset(s_original, 0, s_valid_bytes);
    DIR *dir = opendir(directory);
    if(!dir)
    {
        unlock();
        return ESP_OK;
    }
    uint8_t *tmp = malloc(THUMB_CACHE_THUMB_SIZE);
    struct dirent *entry;
    while((entry = readdir(dir)))
    {
        char *end;
        unsigned long slot = strtoul(entry->d_name, &end, 10);
        if(end == entry->d_name || *end != '.' || !slot || slot > s_count)
            continue;
        if(ext_is_original(end))
            bit_set(s_original, slot);
        else if(!strcasecmp(end, ".thm") && tmp)
        {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
            if(!bit_get(s_valid, slot))
            {
                FILE *f = fopen(path, "rb");
                if(!f)
                    continue;
                bool ok = fread(tmp, 1, THUMB_CACHE_THUMB_SIZE, f) == THUMB_CACHE_THUMB_SIZE;
                fclose(f);
                if(!ok || thumb_cache_write(slot, tmp) != ESP_OK)
                    continue;
            }
            remove(path);
        }
    }
    closedir(dir);
    free(tmp);
    thumb_cache_flush();
    unlock();
    return ESP_OK;
}
typedef struct
{
    char path[128];
    uint32_t generation, revision, count;
} preload_t;
static void preload_task(void *arg)
{
    preload_t *p = arg;
    size_t bytes = THUMB_CACHE_HEADER_SIZE + (size_t)p->count * THUMB_CACHE_THUMB_SIZE;
    uint8_t *ram = NULL;
    FILE *f = NULL;
    if(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > bytes + 4 * 1024 * 1024)
    {
        ram = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        f = fopen(p->path, "rb");
        if(!f || !ram || fread(ram, 1, bytes, f) != bytes)
        {
            free(ram);
            ram = NULL;
        }
    }
    if(f)
        fclose(f);
    lock();
    if(ram && p->generation == s_generation && p->revision == s_revision)
    {
        free(s_ram);
        s_ram = ram;
        s_ram_bytes = bytes;
        ram = NULL;
        ESP_LOGI(TAG, "thumbnail preload: %u KB", (unsigned)(bytes / 1024));
    }
    unlock();
    free(ram);
    free(p);
    vTaskDelete(NULL);
}
void thumb_cache_preload(void)
{
    lock();
    preload_t *p = calloc(1, sizeof(*p));
    if(p && s_count && !s_ram)
    {
        snprintf(p->path, sizeof(p->path), "%s", s_pack_path);
        p->generation = s_generation;
        p->revision = s_revision;
        p->count = s_count;
        if(xTaskCreateWithCaps(
               preload_task, "thumb_load", 6144, p, 1, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
            == pdPASS)
            p = NULL;
    }
    free(p);
    unlock();
}
bool thumb_cache_capture(const char *library_id, uint64_t album_pid, const uint8_t *cover)
{
    if(!cover || !album_pid || !library_id)
        return false;
    uint8_t *thumb = malloc(THUMB_CACHE_THUMB_SIZE);
    if(!thumb)
        return false;
    for(unsigned y = 0; y < 54; y++)
        for(unsigned x = 0; x < 54; x++)
        {
            size_t src = (((y * 2 + 1) * 360 / (54 * 2)) * 360 + (x * 2 + 1) * 360 / (54 * 2)) * 2;
            size_t dst = (y * 54 + x) * 2;
            thumb[dst] = cover[src];
            thumb[dst + 1] = cover[src + 1];
        }
    lock();
    bool ok = false;
    if(!strcmp(library_id, s_namespace))
    {
        uint32_t slot = thumb_cache_slot(album_pid, true);
        ok = slot && (thumb_cache_has(slot) || thumb_cache_write(slot, thumb) == ESP_OK);
        thumb_cache_flush();
    }
    unlock();
    free(thumb);
    return ok;
}
