#ifndef IAP2TAB5_COVER_GRADIENT_H
#define IAP2TAB5_COVER_GRADIENT_H

#include "cover_gradient_math.h"

#define COVER_GRADIENT_WIDTH 1280
#define COVER_GRADIENT_HEIGHT 720
#define COVER_GRADIENT_SMALL_WIDTH 320
#define COVER_GRADIENT_SMALL_HEIGHT 180

typedef enum {
    COVER_GRADIENT_PENDING,
    COVER_GRADIENT_READY,
    COVER_GRADIENT_FAILED,
    COVER_GRADIENT_SUPERSEDED,
} cover_gradient_state_t;

/* UI-thread submission copies just 32x32 samples, never retains artwork.
 * Zero indicates an invalid source or unavailable worker. Equal artwork and
 * theme tint reuse the cache even across songs sharing the same album cover. */
uint64_t cover_gradient_request(const void * pixels, size_t bytes, unsigned width,
                                unsigned height, size_t stride, cover_gradient_format_t format,
                                uint32_t tint_rgb, uint8_t tint_opacity);
/* Poll/install between LVGL frames. The returned pixels stay immutable until
 * the UI polls a different ready result; two buffers keep worker writes away
 * from the currently displayed image. No LVGL calls occur in the worker. */
cover_gradient_state_t cover_gradient_poll(uint64_t key, const uint16_t ** pixels);
/* Drop pending work on page exit. Keep the last completed image cached. */
void cover_gradient_cancel(void);

#endif
