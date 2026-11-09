#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Opens (or creates) the per-library packed thumbnail cache and loads the
     * validity bitmap. album_count is the number of album slots. */
    esp_err_t thumb_cache_open(uint32_t album_count);

    /* Boot-only reset, before either the UI or writer opens the cache. Removes
     * packed thumbnails and numeric cover files for the current library only. */
    esp_err_t thumb_cache_reset(void);

    /* One directory pass: imports legacy <id>.thm files into the pack, deletes
     * the imported files, and records which originals are still on SD. */
    esp_err_t thumb_cache_scan(const char *artwork_dir, uint32_t album_count);

    bool thumb_cache_has(uint32_t album_id);
    uint32_t thumb_cache_count(void);
    uint64_t thumb_cache_bytes(void);
    bool thumb_cache_original(uint32_t album_id);
    void thumb_cache_set_original(uint32_t album_id);

    esp_err_t thumb_cache_write(uint32_t album_id, const uint8_t *thumb);

    /* Flushes both the pack and its bitmap. */
    void thumb_cache_flush(void);

    /* Flush and close the files but keep the in-memory count for the UI. */
    void thumb_cache_sync(void);

    /* Flush and close; safe to call repeatedly. */
    void thumb_cache_close(void);
    /* PID identity is scoped to the active library UUID. Existing numeric slots
     * migrate once; appending albums never renumbers existing thumbnails. */
    uint32_t thumb_cache_slot(uint64_t album_pid, bool create);
    uint64_t thumb_cache_pid(uint32_t slot);
    bool thumb_cache_matches(uint32_t slot, uint64_t artwork_version);
    void thumb_cache_version(uint32_t slot, uint64_t artwork_version);
    bool thumb_cache_read(uint32_t slot, uint8_t *rgb565);
    uint32_t thumb_cache_revision(void);
    void thumb_cache_preload(void);
    /* Called by the artwork worker with identity captured from the same transfer. */
    bool thumb_cache_capture(const char *library_id, uint64_t album_pid, const uint8_t *rgb565_360);
    /* Replacement is copied to a new slot, then atomically published in the map. */
    esp_err_t thumb_cache_replace(uint64_t album_pid, uint64_t version, const uint8_t *rgb565);

#ifdef __cplusplus
}
#endif
