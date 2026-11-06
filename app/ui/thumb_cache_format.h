#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packed real-album thumbnail cache.
 *
 * thumb_cache.bin layout:
 *   [64-byte header][slot 0][slot 1]...[slot count-1]
 * Each slot is a fixed 54x54 RGB565 image. Slot N belongs to album ID N+1.
 * thumb_cache.idx is a bitmap: bit N set means slot N is valid.
 */
#define THUMB_CACHE_MAGIC       "T5THM001"
#define THUMB_CACHE_VERSION     1
#define THUMB_CACHE_HEADER_SIZE 64
#define THUMB_CACHE_THUMB_SIZE  (54 * 54 * 2)

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t count;
    uint32_t thumb_size;
    uint32_t reserved;
} thumb_cache_header_t;

static inline uint64_t thumb_cache_slot_offset(uint32_t album_id)
{
    return (uint64_t)THUMB_CACHE_HEADER_SIZE +
           (uint64_t)(album_id - 1) * THUMB_CACHE_THUMB_SIZE;
}

#ifdef __cplusplus
}
#endif
