#include "cover_gradient_math.h"

#include <stdlib.h>

static unsigned luma(const cover_gradient_sample_t * color)
{
    return (77u * color->r + 150u * color->g + 29u * color->b) >> 8;
}

bool cover_gradient_sample(const void * pixels, size_t bytes, unsigned width,
                           unsigned height, size_t stride, cover_gradient_format_t format,
                           cover_gradient_sample_t samples[COVER_GRADIENT_SAMPLES])
{
    if(format != COVER_GRADIENT_RGB565 && format != COVER_GRADIENT_ARGB8888) return false;
    size_t bpp = format == COVER_GRADIENT_RGB565 ? 2 : 4;
    if(!pixels || !samples || !width || !height || width > SIZE_MAX / bpp) return false;
    size_t row_bytes = (size_t)width * bpp;
    if(stride < row_bytes || (height - 1u) > (SIZE_MAX - row_bytes) / stride ||
       bytes < (height - 1u) * stride + row_bytes) return false;
    const uint8_t * source = pixels;
    for(unsigned y = 0; y < COVER_GRADIENT_SAMPLE_SIDE; y++) {
        unsigned sy = (unsigned)(((uint64_t)(2 * y + 1) * height) /
                                  (2 * COVER_GRADIENT_SAMPLE_SIDE));
        for(unsigned x = 0; x < COVER_GRADIENT_SAMPLE_SIDE; x++) {
            unsigned sx = (unsigned)(((uint64_t)(2 * x + 1) * width) /
                                      (2 * COVER_GRADIENT_SAMPLE_SIDE));
            const uint8_t * pixel = source + sy * stride + sx * bpp;
            cover_gradient_sample_t * out = &samples[y * COVER_GRADIENT_SAMPLE_SIDE + x];
            if(format == COVER_GRADIENT_RGB565) {
                unsigned value = pixel[0] | (unsigned)pixel[1] << 8;
                unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
                *out = (cover_gradient_sample_t){(r << 3) | (r >> 2),
                    (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255};
            }
            else *out = (cover_gradient_sample_t){pixel[2], pixel[1], pixel[0], pixel[3]};
        }
    }
    return true;
}

static int sample_compare(const void * a, const void * b)
{
    const cover_gradient_sample_t * ca = a, * cb = b;
    int delta = (int)luma(ca) - (int)luma(cb);
    if(delta) return delta;
    if(ca->r != cb->r) return (int)ca->r - cb->r;
    if(ca->g != cb->g) return (int)ca->g - cb->g;
    return (int)ca->b - cb->b;
}

bool cover_gradient_palette(cover_gradient_sample_t samples[COVER_GRADIENT_SAMPLES],
                            cover_gradient_color_t palette[COVER_GRADIENT_COLORS])
{
    unsigned opaque = 0, useful = 0;
    for(unsigned i = 0; i < COVER_GRADIENT_SAMPLES; i++) {
        if(samples[i].a < 200) continue;
        samples[opaque++] = samples[i];
        unsigned value = luma(&samples[i]);
        if(value >= 20 && value <= 235) useful++;
    }
    if(!opaque) return false;
    unsigned count = opaque;
    /* Black/white or very dark covers must not silently disable the effect. */
    if(useful >= COVER_GRADIENT_COLORS * 3) {
        count = 0;
        for(unsigned i = 0; i < opaque; i++) {
            unsigned value = luma(&samples[i]);
            if(value >= 20 && value <= 235) samples[count++] = samples[i];
        }
    }
    qsort(samples, count, sizeof(samples[0]), sample_compare);
    int centers[COVER_GRADIENT_COLORS][3];
    for(unsigned k = 0; k < COVER_GRADIENT_COLORS; k++) {
        const cover_gradient_sample_t * sample = &samples[(count - 1) * k / (COVER_GRADIENT_COLORS - 1)];
        centers[k][0] = sample->r; centers[k][1] = sample->g; centers[k][2] = sample->b;
    }
    for(unsigned iteration = 0; iteration < 10; iteration++) {
        uint32_t sums[COVER_GRADIENT_COLORS][3] = {{0}};
        unsigned used[COVER_GRADIENT_COLORS] = {0};
        for(unsigned i = 0; i < count; i++) {
            unsigned best = 0;
            int best_distance = INT32_MAX;
            for(unsigned k = 0; k < COVER_GRADIENT_COLORS; k++) {
                int r = (int)samples[i].r - centers[k][0];
                int g = (int)samples[i].g - centers[k][1];
                int b = (int)samples[i].b - centers[k][2];
                int distance = r*r + g*g + b*b;
                if(distance < best_distance) { best_distance = distance; best = k; }
            }
            sums[best][0] += samples[i].r;
            sums[best][1] += samples[i].g;
            sums[best][2] += samples[i].b;
            used[best]++;
        }
        bool changed = false;
        for(unsigned k = 0; k < COVER_GRADIENT_COLORS; k++) {
            if(!used[k]) continue;
            for(unsigned c = 0; c < 3; c++) {
                int value = (int)(sums[k][c] / used[k]);
                if(value != centers[k][c]) changed = true;
                centers[k][c] = value;
            }
        }
        if(!changed) break;
    }
    for(unsigned k = 0; k < COVER_GRADIENT_COLORS; k++) {
        samples[k] = (cover_gradient_sample_t){centers[k][0], centers[k][1], centers[k][2], 255};
    }
    qsort(samples, COVER_GRADIENT_COLORS, sizeof(samples[0]), sample_compare);
    for(unsigned k = 0; k < COVER_GRADIENT_COLORS; k++) {
        palette[k] = (cover_gradient_color_t){samples[k].r, samples[k].g, samples[k].b};
    }
    return true;
}

void cover_gradient_render(const cover_gradient_color_t palette[COVER_GRADIENT_COLORS],
                           uint8_t * pixels, unsigned width, unsigned height, size_t stride)
{
    static const uint8_t grid[3][3] = {{0,3,1}, {2,4,3}, {1,2,0}};
    float x_step = width > 1 ? 1.0f / (width - 1) : 0;
    float y_step = height > 1 ? 1.0f / (height - 1) : 0;
    for(unsigned y = 0; y < height; y++) {
        float t = y * y_step;
        float weights[3] = {(1-t)*(1-t), 2*t*(1-t), t*t};
        float row[3][3] = {{0}};
        /* Separable Bezier evaluation: calculate vertical contributions once
         * per row instead of evaluating all nine control points per pixel. */
        for(unsigned j = 0; j < 3; j++) {
            for(unsigned i = 0; i < 3; i++) {
                const cover_gradient_color_t * c = &palette[grid[i][j]];
                row[j][0] += weights[i] * c->b;
                row[j][1] += weights[i] * c->g;
                row[j][2] += weights[i] * c->r;
            }
        }
        for(unsigned x = 0; x < width; x++) {
            float tx = x * x_step;
            float wx[3] = {(1-tx)*(1-tx), 2*tx*(1-tx), tx*tx};
            for(unsigned c = 0; c < 3; c++) {
                float value = row[0][c]*wx[0] + row[1][c]*wx[1] + row[2][c]*wx[2];
                if(value < 0) value = 0;
                if(value > 255) value = 255;
                pixels[y * stride + x*3 + c] = (uint8_t)(value + 0.5f);
            }
        }
    }
}

void cover_gradient_scale_cpu(const uint8_t * input, unsigned width, unsigned height,
                              size_t stride, uint16_t * output,
                              unsigned out_width, unsigned out_height)
{
    for(unsigned y = 0; y < out_height; y++) {
        uint32_t sy = out_height > 1 ? (uint32_t)((uint64_t)y * (height-1) * 256 / (out_height-1)) : 0;
        unsigned y0 = sy >> 8, y1 = y0 + 1 < height ? y0 + 1 : y0;
        for(unsigned x = 0; x < out_width; x++) {
            uint32_t sx = out_width > 1 ? (uint32_t)((uint64_t)x * (width-1) * 256 / (out_width-1)) : 0;
            unsigned x0 = sx >> 8, x1 = x0 + 1 < width ? x0 + 1 : x0;
            unsigned rgb[3];
            for(unsigned c = 0; c < 3; c++) {
                unsigned top = input[y0*stride+x0*3+c]*(256-(sx&255)) + input[y0*stride+x1*3+c]*(sx&255);
                unsigned bottom = input[y1*stride+x0*3+c]*(256-(sx&255)) + input[y1*stride+x1*3+c]*(sx&255);
                rgb[c] = (top*(256-(sy&255)) + bottom*(sy&255) + 32768) >> 16;
            }
            output[y*out_width+x] = (uint16_t)((rgb[2]>>3)<<11 | (rgb[1]>>2)<<5 | (rgb[0]>>3));
        }
    }
}

static uint16_t finish_level(unsigned blend, unsigned maximum, unsigned phase,
                              unsigned span)
{
    const int denominator = 255 * 255;
    int numerator = (int)blend, ceiling = (int)maximum * denominator;
    int headroom = numerator < ceiling-numerator ? numerator : ceiling-numerator;
    int amplitude = (int)(span * denominator * 15u / 32u);
    if(amplitude > headroom) amplitude = headroom;
    /* Symmetric ordered texture, faded near black/white instead of clipping
     * the pattern and changing the average brightness of dark regions. */
    int offset = ((int)phase*2-15) * amplitude / 15;
    return (uint16_t)((numerator + offset + denominator/2) / denominator);
}

void cover_gradient_finish(uint16_t * pixels, unsigned width, unsigned height,
                           uint32_t tint_rgb, uint8_t tint_opacity)
{
    if(!pixels || !tint_opacity) return;
    /* A stronger, stationary 4x4 Bayer texture breaks up the remaining source
     * quantization contours. Red/blue span four 5-bit steps; green spans eight
     * 6-bit steps, giving comparable perceived brightness variation. */
    static const uint8_t bayer[4][4] = {
        {0,8,2,10}, {12,4,14,6}, {3,11,1,9}, {15,7,13,5}
    };
    unsigned keep = 255u * (255u - tint_opacity);
    unsigned red = ((tint_rgb >> 16) & 255u) * 31u * tint_opacity;
    unsigned green = ((tint_rgb >> 8) & 255u) * 63u * tint_opacity;
    unsigned blue = (tint_rgb & 255u) * 31u * tint_opacity;
    /* Only 32/64/32 input levels and 16 thresholds exist. A 4 KB lookup
     * table replaces millions of repeated blend/division calculations. */
    uint16_t reds[16][32], greens[16][64], blues[16][32];
    for(unsigned phase = 0; phase < 16; phase++) {
        for(unsigned value = 0; value < 32; value++) {
            reds[phase][value] = (uint16_t)(finish_level(value*keep + red,31,phase,4) << 11);
            blues[phase][value] = finish_level(value*keep + blue,31,phase,4);
        }
        for(unsigned value = 0; value < 64; value++) {
            greens[phase][value] = (uint16_t)(finish_level(value*keep + green,63,phase,8) << 5);
        }
    }
    for(unsigned y = 0; y < height; y++) {
        const uint8_t * phases = bayer[y&3];
        uint16_t * row = pixels + (size_t)y * width;
        for(unsigned x = 0; x < width; x += 4) {
            for(unsigned cell = 0; cell < 4 && x+cell < width; cell++) {
                unsigned value = row[x+cell], phase = phases[cell];
                row[x+cell] = reds[phase][value>>11] | greens[phase][(value>>5)&63u] | blues[phase][value&31u];
            }
        }
    }
}
