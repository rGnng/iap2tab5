#ifndef IAP2TAB5_COVER_GRADIENT_MATH_H
#define IAP2TAB5_COVER_GRADIENT_MATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COVER_GRADIENT_SAMPLE_SIDE 32
#define COVER_GRADIENT_SAMPLES (COVER_GRADIENT_SAMPLE_SIDE * COVER_GRADIENT_SAMPLE_SIDE)
#define COVER_GRADIENT_COLORS 3
#define COVER_GRADIENT_FINISH_LUT_ENTRIES (3u * 16u * 256u)

typedef struct { uint8_t r, g, b, a; } cover_gradient_sample_t;
typedef struct { uint8_t r, g, b; } cover_gradient_color_t;
typedef enum { COVER_GRADIENT_RGB565, COVER_GRADIENT_ARGB8888 } cover_gradient_format_t;

/* Inputs use LVGL/ESP byte order: little-endian RGB565 or B,G,R,A. */
bool cover_gradient_sample(const void * pixels, size_t bytes, unsigned width,
                           unsigned height, size_t stride, cover_gradient_format_t format,
                           cover_gradient_sample_t samples[COVER_GRADIENT_SAMPLES]);
/* Dominant hue plus distinct accents; neutral art stays neutral. */
bool cover_gradient_palette(const cover_gradient_sample_t samples[COVER_GRADIENT_SAMPLES],
                            cover_gradient_color_t palette[COVER_GRADIENT_COLORS]);
/* RGB888 output uses B,G,R byte order, matching the PPA input format. */
void cover_gradient_render(const cover_gradient_color_t palette[COVER_GRADIENT_COLORS],
                           uint8_t * pixels, unsigned width, unsigned height, size_t stride,
                           uint32_t theme_tint);
void cover_gradient_scale_cpu(const uint8_t * input, unsigned width, unsigned height,
                              size_t stride, uint8_t * output,
                              unsigned out_width, unsigned out_height);
/* Convert B,G,R RGB888 to RGB565 in place, tinting and dithering only once.
 * Conversion proceeds forwards, safely packing 3-byte pixels into 2 bytes.
 * The caller supplies a reusable LUT in PSRAM to keep the worker stack small. */
void cover_gradient_finish(uint8_t * pixels, unsigned width, unsigned height,
                           uint32_t tint_rgb, uint8_t tint_opacity, uint16_t * lut);

#endif
