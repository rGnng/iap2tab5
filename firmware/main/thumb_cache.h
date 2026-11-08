#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens (or creates) the per-library packed thumbnail cache and loads the
 * validity bitmap. album_count is the number of album slots. */
esp_err_t thumb_cache_open(uint32_t album_count);

/* Boot-only reset, before either the UI or writer opens the cache. Removes
 * packed thumbnails and numeric cover files for the current library only. */
esp_err_t thumb_cache_reset(void);

/* One directory pass: imports legacy <id>.thm files into the pack, deletes
 * the imported files, and records which originals are still on SD. */
esp_err_t thumb_cache_scan(const char * artwork_dir, uint32_t album_count);

bool thumb_cache_has(uint32_t album_id);
uint32_t thumb_cache_count(void);
uint64_t thumb_cache_bytes(void);
bool thumb_cache_original(uint32_t album_id);
void thumb_cache_set_original(uint32_t album_id);

esp_err_t thumb_cache_write(uint32_t album_id, const uint8_t * thumb);

/* Flushes both the pack and its bitmap. */
void thumb_cache_flush(void);

/* Flush and close the files but keep the in-memory count for the UI. */
void thumb_cache_sync(void);

/* Flush and close; safe to call repeatedly. */
void thumb_cache_close(void);

#ifdef __cplusplus
}
#endif
