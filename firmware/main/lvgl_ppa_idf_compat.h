#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * LVGL 9.6's PPA draw unit uses esp_cache_get_line_size_by_addr(), which was
 * only added to esp_mm in IDF v5.5. We build with IDF v5.4.2, so this header is
 * force-included into the lvgl__lvgl component and the function is implemented
 * in lvgl_ppa_idf_compat.c.
 */
size_t esp_cache_get_line_size_by_addr(const void *addr);

#ifdef __cplusplus
}
#endif
