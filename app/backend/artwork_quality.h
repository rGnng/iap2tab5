#ifndef IAP2TAB5_ARTWORK_QUALITY_H
#define IAP2TAB5_ARTWORK_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

/* Original dimensions, before decoding into the fixed 360x360 UI buffer.
 * Zero means an older cache whose source resolution was not recorded. */
typedef struct {
    uint16_t width, height;
} artwork_quality_t;

static inline unsigned artwork_quality_rank(artwork_quality_t quality)
{
    return quality.width < quality.height ? quality.width : quality.height;
}

static inline bool artwork_quality_at_least(artwork_quality_t candidate,
                                           artwork_quality_t minimum)
{
    return artwork_quality_rank(candidate) >= artwork_quality_rank(minimum);
}

#endif
