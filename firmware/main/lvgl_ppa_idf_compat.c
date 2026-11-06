#include <stddef.h>
#include <stdint.h>

#include "hal/cache_hal.h"

#include "lvgl_ppa_idf_compat.h"

/*
 * Backport of the IDF v5.5 helper used by LVGL's PPA draw unit.
 *
 * ESP32-P4 caches internal SRAM through L1 and external PSRAM through L2, so
 * the cache line size must be looked up per address. Non-cacheable addresses
 * report 0, matching the upstream behaviour.
 */
size_t esp_cache_get_line_size_by_addr(const void *addr)
{
    if (addr == NULL) {
        return 0;
    }

    uint32_t level = 0;
    uint32_t id = 0;
    if (!cache_hal_vaddr_to_cache_level_id((uint32_t)(uintptr_t)addr, 1, &level, &id)) {
        return 0;
    }

    return cache_hal_get_cache_line_size(level, CACHE_TYPE_DATA);
}
