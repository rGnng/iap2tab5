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

static const char * TAG = "thumbs";

static FILE * s_pack;
static FILE * s_idx;
static uint8_t * s_valid;
static uint8_t * s_original;
static uint32_t s_count;
static size_t s_valid_bytes;
static uint32_t s_write_count;
static char s_pack_path[128];
static char s_idx_path[128];

#define THUMB_FLUSH_EVERY 32

static bool bit_get(const uint8_t * bits, uint32_t id)
{
    if(bits == NULL || id < 1 || id > s_count) return false;
    uint32_t n = id - 1;
    return (bits[n >> 3] & (1u << (n & 7))) != 0;
}

static void bit_set(uint8_t * bits, uint32_t id)
{
    if(bits == NULL || id < 1 || id > s_count) return;
    uint32_t n = id - 1;
    bits[n >> 3] |= (uint8_t)(1u << (n & 7));
}

static void make_paths(void)
{
    snprintf(s_pack_path, sizeof(s_pack_path), "%s/thumbs.bin",
             app_config_library_dir());
    snprintf(s_idx_path, sizeof(s_idx_path), "%s/thumbs.idx",
             app_config_library_dir());
}

static bool is_cached_cover(const char * name)
{
    const char * ext = name;
    while(*ext >= '0' && *ext <= '9') ext++;
    if(ext == name) return false;
    const char * suffixes[] = {".thm", ".cov", ".jpg", ".jpeg", ".png",
                               ".wbp", ".webp", ".gif", ".art"};
    for(size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if(strcasecmp(ext, suffixes[i]) == 0) return true;
    }
    return false;
}

esp_err_t thumb_cache_reset(void)
{
    if(s_pack != NULL || s_idx != NULL || s_valid != NULL ||
       !app_config_sd_ready()) return ESP_ERR_INVALID_STATE;
    make_paths();
    /* Invalidate before deleting data; an interrupted reset cannot advertise
     * a missing pack as valid. No recursive directory deletion. */
    if(remove(s_idx_path) != 0 && errno != ENOENT) return ESP_FAIL;
    if(remove(s_pack_path) != 0 && errno != ENOENT) return ESP_FAIL;
    DIR * dir = opendir(app_config_artwork_dir());
    if(dir == NULL) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    esp_err_t result = ESP_OK;
    unsigned removed = 0;
    struct dirent * ent;
    while((ent = readdir(dir)) != NULL) {
        if(!is_cached_cover(ent->d_name)) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", app_config_artwork_dir(), ent->d_name);
        struct stat st;
        if(stat(path, &st) != 0) { result = ESP_FAIL; continue; }
        if(!S_ISREG(st.st_mode)) continue;
        if(remove(path) != 0) result = ESP_FAIL;
        else removed++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "artwork cache reset: pack/index and %u cover files removed", removed);
    return result;
}

static esp_err_t create_pack(void)
{
    s_pack = fopen(s_pack_path, "w+b");
    if(s_pack == NULL) {
        ESP_LOGE(TAG, "cannot create %s", s_pack_path);
        return ESP_FAIL;
    }
    setvbuf(s_pack, NULL, _IONBF, 0);

    thumb_cache_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, THUMB_CACHE_MAGIC, 8);
    hdr.version = THUMB_CACHE_VERSION;
    hdr.count = s_count;
    hdr.thumb_size = THUMB_CACHE_THUMB_SIZE;

    uint8_t zero[THUMB_CACHE_HEADER_SIZE];
    memset(zero, 0, sizeof(zero));
    if(fwrite(&hdr, 1, sizeof(hdr), s_pack) != sizeof(hdr)) return ESP_FAIL;
    if(fwrite(zero, 1, sizeof(zero) - sizeof(hdr), s_pack) !=
       sizeof(zero) - sizeof(hdr)) {
        return ESP_FAIL;
    }

    /* Preallocate the whole slot array so later writes are sequential and
     * the FAT does not have to grow the file one cluster at a time. */
    size_t remaining = (size_t)s_count * THUMB_CACHE_THUMB_SIZE;
    uint8_t * buf = heap_caps_aligned_alloc(128, 64 * 1024,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(buf == NULL) buf = heap_caps_aligned_alloc(128, 64 * 1024, MALLOC_CAP_8BIT);
    if(buf != NULL) {
        memset(buf, 0, 64 * 1024);
        while(remaining > 0) {
            size_t n = remaining > 64 * 1024 ? 64 * 1024 : remaining;
            if(fwrite(buf, 1, n, s_pack) != n) {
                heap_caps_free(buf);
                return ESP_FAIL;
            }
            remaining -= n;
        }
        heap_caps_free(buf);
    }
    else {
        if(fseek(s_pack, (long)(THUMB_CACHE_HEADER_SIZE +
                                (size_t)s_count * THUMB_CACHE_THUMB_SIZE - 1),
                 SEEK_SET) != 0) {
            return ESP_FAIL;
        }
        fputc(0, s_pack);
    }
    fflush(s_pack);
    return ESP_OK;
}

static void recover_bitmap_from_pack(void)
{
    uint8_t * slot = heap_caps_malloc(THUMB_CACHE_THUMB_SIZE,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(slot == NULL) return;

    uint32_t recovered = 0;
    for(uint32_t id = 1; id <= s_count; id++) {
        if(fseek(s_pack, (long)thumb_cache_slot_offset(id), SEEK_SET) != 0) {
            break;
        }
        size_t n = fread(slot, 1, THUMB_CACHE_THUMB_SIZE, s_pack);
        if(n != THUMB_CACHE_THUMB_SIZE) break;
        bool nonzero = false;
        for(size_t i = 0; i < THUMB_CACHE_THUMB_SIZE; i++) {
            if(slot[i] != 0) {
                nonzero = true;
                break;
            }
        }
        if(nonzero) {
            bit_set(s_valid, id);
            recovered++;
        }
    }
    heap_caps_free(slot);
    if(recovered > 0) {
        ESP_LOGW(TAG, "recovered %u slots from pack (bitmap was empty)",
                 (unsigned)recovered);
    }
}

esp_err_t thumb_cache_open(uint32_t album_count)
{
    if(album_count == 0) return ESP_ERR_INVALID_ARG;
    if(s_pack != NULL && s_count == album_count) return ESP_OK;

    thumb_cache_close();
    s_count = album_count;
    s_valid_bytes = (album_count + 7) / 8;
    s_write_count = 0;
    make_paths();

    s_valid = calloc(s_valid_bytes, 1);
    s_original = calloc(s_valid_bytes, 1);
    if(s_valid == NULL || s_original == NULL) {
        thumb_cache_close();
        return ESP_ERR_NO_MEM;
    }

    bool created = false;
    s_pack = fopen(s_pack_path, "r+b");
    if(s_pack != NULL) {
        thumb_cache_header_t hdr;
        if(fread(&hdr, 1, sizeof(hdr), s_pack) != sizeof(hdr) ||
           memcmp(hdr.magic, THUMB_CACHE_MAGIC, 8) != 0 ||
           hdr.version != THUMB_CACHE_VERSION ||
           hdr.count != album_count ||
           hdr.thumb_size != THUMB_CACHE_THUMB_SIZE) {
            ESP_LOGW(TAG, "pack header mismatch; rebuilding %s",
                     s_pack_path);
            fclose(s_pack);
            s_pack = NULL;
            remove(s_pack_path);
            remove(s_idx_path);
        }
    }
    if(s_pack == NULL) {
        if(create_pack() != ESP_OK) {
            thumb_cache_close();
            return ESP_FAIL;
        }
        created = true;
    }

    s_idx = fopen(s_idx_path, "r+b");
    if(s_idx != NULL && !created) {
        size_t n = fread(s_valid, 1, s_valid_bytes, s_idx);
        if(n < s_valid_bytes) {
            memset(s_valid + n, 0, s_valid_bytes - n);
        }
    }
    else {
        if(s_idx != NULL) fclose(s_idx);
        s_idx = fopen(s_idx_path, "w+b");
        if(s_idx == NULL) {
            ESP_LOGE(TAG, "cannot create %s", s_idx_path);
            thumb_cache_close();
            return ESP_FAIL;
        }
        memset(s_valid, 0, s_valid_bytes);
        fwrite(s_valid, 1, s_valid_bytes, s_idx);
        fflush(s_idx);
    }

    if(!created && thumb_cache_count() == 0) {
        recover_bitmap_from_pack();
        thumb_cache_flush();
    }

    ESP_LOGI(TAG, "pack ready: %s (%u slots, %u bytes), %s",
             s_pack_path, (unsigned)album_count,
             (unsigned)(album_count * THUMB_CACHE_THUMB_SIZE), s_idx_path);
    return ESP_OK;
}

bool thumb_cache_has(uint32_t album_id)
{
    return bit_get(s_valid, album_id);
}

uint32_t thumb_cache_count(void)
{
    if(s_valid == NULL) return 0;
    uint32_t n = 0;
    for(size_t i = 0; i < s_valid_bytes; i++) {
        n += (uint32_t)__builtin_popcount((unsigned)s_valid[i]);
    }
    return n;
}

uint64_t thumb_cache_bytes(void)
{
    return (uint64_t)thumb_cache_count() * THUMB_CACHE_THUMB_SIZE;
}

bool thumb_cache_original(uint32_t album_id)
{
    return bit_get(s_original, album_id);
}

void thumb_cache_set_original(uint32_t album_id)
{
    bit_set(s_original, album_id);
}

esp_err_t thumb_cache_write(uint32_t album_id, const uint8_t * thumb)
{
    if(s_pack == NULL || s_idx == NULL || thumb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if(album_id < 1 || album_id > s_count) return ESP_ERR_INVALID_ARG;

    if(fseek(s_pack, (long)thumb_cache_slot_offset(album_id), SEEK_SET) != 0) {
        return ESP_FAIL;
    }
    if(fwrite(thumb, 1, THUMB_CACHE_THUMB_SIZE, s_pack) !=
       THUMB_CACHE_THUMB_SIZE) {
        return ESP_FAIL;
    }

    uint32_t n = album_id - 1;
    size_t byte = n >> 3;
    s_valid[byte] |= (uint8_t)(1u << (n & 7));
    if(fseek(s_idx, (long)byte, SEEK_SET) != 0) return ESP_FAIL;
    if(fputc(s_valid[byte], s_idx) == EOF) return ESP_FAIL;

    if((++s_write_count % THUMB_FLUSH_EVERY) == 0) {
        fflush(s_pack);
        fsync(fileno(s_pack));
        fflush(s_idx);
        fsync(fileno(s_idx));
    }
    return ESP_OK;
}

void thumb_cache_flush(void)
{
    if(s_idx != NULL && s_valid != NULL) {
        fseek(s_idx, 0, SEEK_SET);
        fwrite(s_valid, 1, s_valid_bytes, s_idx);
        fflush(s_idx);
        fsync(fileno(s_idx));
    }
    if(s_pack != NULL) {
        fflush(s_pack);
        fsync(fileno(s_pack));
    }
}

void thumb_cache_sync(void)
{
    thumb_cache_flush();
    if(s_pack != NULL) {
        fclose(s_pack);
        s_pack = NULL;
    }
    if(s_idx != NULL) {
        fclose(s_idx);
        s_idx = NULL;
    }
}

void thumb_cache_close(void)
{
    thumb_cache_flush();
    if(s_pack != NULL) fclose(s_pack);
    if(s_idx != NULL) fclose(s_idx);
    free(s_valid);
    free(s_original);
    s_pack = NULL;
    s_idx = NULL;
    s_valid = NULL;
    s_original = NULL;
    s_count = 0;
    s_valid_bytes = 0;
    s_write_count = 0;
}

static bool ext_is_original(const char * ext)
{
    return strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
           strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".wbp") == 0 ||
           strcasecmp(ext, ".webp") == 0 || strcasecmp(ext, ".gif") == 0;
}

esp_err_t thumb_cache_scan(const char * artwork_dir, uint32_t album_count)
{
    esp_err_t err = thumb_cache_open(album_count);
    if(err != ESP_OK) return err;

    memset(s_original, 0, s_valid_bytes);

    DIR * dir = opendir(artwork_dir);
    if(dir == NULL) {
        ESP_LOGW(TAG, "artwork dir missing: %s", artwork_dir);
        return ESP_OK;
    }

    uint8_t * tmp = heap_caps_malloc(THUMB_CACHE_THUMB_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(tmp == NULL) tmp = malloc(THUMB_CACHE_THUMB_SIZE);

    int packed = 0;
    int originals = 0;
    int removed = 0;
    struct dirent * ent;
    while((ent = readdir(dir)) != NULL) {
        const char * name = ent->d_name;
        char * end = NULL;
        unsigned long id = strtoul(name, &end, 10);
        if(end == name || *end != '.' || id < 1 || id > album_count) {
            continue;
        }
        const char * ext = end;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", artwork_dir, name);

        if(strcasecmp(ext, ".thm") == 0) {
            if(thumb_cache_has((uint32_t)id)) {
                if(remove(path) == 0) removed++;
                continue;
            }
            if(tmp == NULL) continue;
            FILE * f = fopen(path, "rb");
            if(f == NULL) continue;
            size_t n = fread(tmp, 1, THUMB_CACHE_THUMB_SIZE, f);
            fclose(f);
            if(n == THUMB_CACHE_THUMB_SIZE &&
               thumb_cache_write((uint32_t)id, tmp) == ESP_OK) {
                packed++;
                if(remove(path) == 0) removed++;
                if((packed % 100) == 0) {
                    ESP_LOGI(TAG, "packed %d legacy thumbs...", packed);
                }
            }
        }
        else if(ext_is_original(ext)) {
            thumb_cache_set_original((uint32_t)id);
            originals++;
        }
    }
    closedir(dir);
    if(tmp != NULL) free(tmp);

    thumb_cache_flush();
    ESP_LOGI(TAG, "index: %d legacy thumbs packed, %d originals, %d .thm removed",
             packed, originals, removed);
    return ESP_OK;
}
