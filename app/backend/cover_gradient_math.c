#include "cover_gradient_math.h"

#include "cover_gradient_noise.h"

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

typedef struct {
    uint32_t r, g, b, weight, count;
} hue_bin_t;

static unsigned channel_max(unsigned r, unsigned g, unsigned b)
{
    unsigned value = r > g ? r : g;
    return value > b ? value : b;
}

static unsigned channel_min(unsigned r, unsigned g, unsigned b)
{
    unsigned value = r < g ? r : g;
    return value < b ? value : b;
}

bool cover_gradient_palette(const cover_gradient_sample_t samples[COVER_GRADIENT_SAMPLES],
                            cover_gradient_color_t palette[COVER_GRADIENT_COLORS])
{
    /* Group nearby hues rather than averaging unrelated colors in RGB space.
     * Vivid small accents can survive a large black/white/gray cover border. */
    hue_bin_t bins[24] = {{0}};
    uint32_t sum[3] = {0}, opaque = 0;
    for(unsigned i = 0; i < COVER_GRADIENT_SAMPLES; i++) {
        const cover_gradient_sample_t * sample = &samples[i];
        if(sample->a < 200) continue;
        unsigned r = sample->r, g = sample->g, b = sample->b;
        sum[0] += r; sum[1] += g; sum[2] += b; opaque++;
        unsigned maximum = channel_max(r,g,b), minimum = channel_min(r,g,b);
        unsigned chroma = maximum - minimum;
        if(chroma < 18 || chroma * 255u < maximum * 40u || luma(sample) < 8) continue;
        int hue;
        if(maximum == r) hue = ((int)g - (int)b) * 256 / (int)chroma;
        else if(maximum == g) hue = 512 + ((int)b - (int)r) * 256 / (int)chroma;
        else hue = 1024 + ((int)r - (int)g) * 256 / (int)chroma;
        if(hue < 0) hue += 1536;
        unsigned bin = ((unsigned)hue + 32u) / 64u % 24u;
        unsigned weight = chroma * (chroma + 32u) * (maximum + 32u) / 65536u;
        if(!weight) weight = 1;
        bins[bin].r += r * weight;
        bins[bin].g += g * weight;
        bins[bin].b += b * weight;
        bins[bin].weight += weight;
        bins[bin].count++;
    }
    if(!opaque) return false;
    unsigned chosen[COVER_GRADIENT_COLORS], selected = 0;
    unsigned minimum_count = opaque / 100u;
    if(minimum_count < 4) minimum_count = 4;
    for(unsigned k = 0; k < COVER_GRADIENT_COLORS; k++) {
        uint32_t best_score = 0;
        unsigned best_bin = 0;
        cover_gradient_color_t best = {0};
        for(unsigned i = 0; i < 24; i++) {
            unsigned distance = 12;
            for(unsigned j = 0; j < selected; j++) {
                unsigned delta = i > chosen[j] ? i-chosen[j] : chosen[j]-i;
                if(delta > 12) delta = 24-delta;
                if(delta < distance) distance = delta;
            }
            if(distance < 4) continue;
            hue_bin_t group = {0};
            for(unsigned j = 0; j < 3; j++) {
                const hue_bin_t * bin = &bins[(i+j+23)%24];
                group.r += bin->r; group.g += bin->g; group.b += bin->b;
                group.weight += bin->weight; group.count += bin->count;
            }
            if(group.count < minimum_count || !group.weight) continue;
            unsigned r = (group.r + group.weight/2) / group.weight;
            unsigned g = (group.g + group.weight/2) / group.weight;
            unsigned b = (group.b + group.weight/2) / group.weight;
            unsigned chroma = channel_max(r,g,b)-channel_min(r,g,b);
            uint32_t score = group.count * (chroma+48u) * distance;
            if(score > best_score) {
                best_score = score; best_bin = i;
                best = (cover_gradient_color_t){r,g,b};
            }
        }
        if(!best_score) break;
        palette[selected] = best;
        chosen[selected++] = best_bin;
    }
    if(!selected) {
        palette[selected++] = (cover_gradient_color_t){sum[0]/opaque,sum[1]/opaque,sum[2]/opaque};
    }
    for(unsigned i = selected; i < COVER_GRADIENT_COLORS; i++) palette[i] = palette[0];
    return true;
}

static float smooth(float low, float high, float value)
{
    float t = (value-low)/(high-low);
    if(t <= 0) return 0;
    if(t >= 1) return 1;
    return t*t*(3-2*t);
}

void cover_gradient_render(const cover_gradient_color_t palette[COVER_GRADIENT_COLORS],
                           uint8_t * pixels, unsigned width, unsigned height, size_t stride,
                           uint32_t theme_tint)
{
    /* Broad color lights surround the artwork, with a smaller accent in the
     * opposite top corner. Compact cubic falloffs have no hard pool edges. */
    static const float center[3][2] = {{0.25f,0.25f}, {0.02f,0.75f}, {0.99f,-0.12f}};
    static const float radius[3][2] = {{0.68f,0.85f}, {0.70f,0.80f}, {0.95f,0.90f}};
    static const float dark_strength[3] = {148,108,86};
    static const float light_strength[3] = {100,70,60};
    float colors[COVER_GRADIENT_COLORS][3], lights[3][3], base[3];
    for(unsigned i = 0; i < COVER_GRADIENT_COLORS; i++) {
        unsigned maximum = channel_max(palette[i].r,palette[i].g,palette[i].b);
        float scale = maximum ? 1.0f/maximum : 0;
        colors[i][0] = palette[i].b * scale;
        colors[i][1] = palette[i].g * scale;
        colors[i][2] = palette[i].r * scale;
        /* Enrich muted cover colors without inventing another hue.
         * Neutral artwork still has identical channels after this transform. */
        float floor = channel_min(palette[i].r,palette[i].g,palette[i].b)*scale*0.35f;
        for(unsigned c = 0; c < 3; c++) colors[i][c] = (colors[i][c]-floor)/(1-floor);
    }
    unsigned tint_luma = (77*((theme_tint>>16)&255u) + 150*((theme_tint>>8)&255u) + 29*(theme_tint&255u)) >> 8;
    bool light = tint_luma >= 128;
    for(unsigned c = 0; c < 3; c++) {
        base[c] = light ? (((theme_tint>>(c*8))&255u)+255)*0.5f
                         -(1-colors[1][c])*10-(1-colors[2][c])*4
                       : 4+colors[1][c]*28+colors[2][c]*12;
        for(unsigned i = 0; i < 3; i++) {
            lights[i][c] = light ? -(1-colors[i][c])*light_strength[i]
                                 : colors[i][c]*dark_strength[i];
        }
    }
    float x_step = width > 1 ? 1.0f/(width-1) : 0;
    float y_step = height > 1 ? 1.0f/(height-1) : 0;
    /* The device renders one 320px tile. Bounded 5 KB scratch also supports
     * larger simulator/oracle renders without growing the worker stack.
     * Horizontal distances and text falloffs are evaluated once per column. */
    float horizontal[3][320], text_x[320];
    for(unsigned start = 0; start < width; start += 320) {
        unsigned columns = width-start < 320 ? width-start : 320;
        for(unsigned x = 0; x < columns; x++) {
            float tx = (start+x)*x_step;
            text_x[x] = smooth(0.28f,0.68f,tx);
            for(unsigned i = 0; i < 3; i++) {
                float dx = (tx-center[i][0])/radius[i][0];
                horizontal[i][x] = dx*dx;
            }
        }
        for(unsigned y = 0; y < height; y++) {
            float ty = y*y_step, vertical[3];
            for(unsigned i = 0; i < 3; i++) {
                float dy = (ty-center[i][1])/radius[i][1];
                vertical[i] = dy*dy;
            }
            float calm = smooth(0.54f,0.82f,ty);
            for(unsigned x = 0; x < columns; x++) {
                float text = text_x[x], weights[3], rgb[3];
                float strength = (1-0.65f*text)*(1-0.62f*calm);
                for(unsigned i = 0; i < 3; i++) {
                    float weight = 1-horizontal[i][x]-vertical[i];
                    weights[i] = weight > 0 ? weight*weight*weight*strength : 0;
                }
                for(unsigned c = 0; c < 3; c++) {
                    rgb[c] = base[c]+lights[0][c]*weights[0]+lights[1][c]*weights[1]+lights[2][c]*weights[2];
                }
                float luminance = (29*rgb[0]+150*rgb[1]+77*rgb[2])/256;
                if(light) {
                    float floor = 175+text*39+calm*23;
                    if(luminance < floor) {
                        float lift = (floor-luminance)/(255-luminance);
                        for(unsigned c = 0; c < 3; c++) rgb[c] += (255-rgb[c])*lift;
                    }
                }
                else {
                    float ceiling = 96-text*52-calm*44;
                    if(ceiling < 40) ceiling = 40;
                    if(luminance > ceiling) for(unsigned c = 0; c < 3; c++) rgb[c] *= ceiling/luminance;
                }
                for(unsigned c = 0; c < 3; c++) {
                    float value = rgb[c] < 0 ? 0 : rgb[c] > 255 ? 255 : rgb[c];
                    pixels[y*stride+(start+x)*3+c] = (uint8_t)(value+0.5f);
                }
            }
        }
    }
}

uint32_t cover_gradient_album_hash(const char * album)
{
    uint32_t hash = UINT32_C(2166136261);
    if(album) for(const unsigned char * p = (const unsigned char *)album; *p; p++) {
        hash = (hash ^ *p) * UINT32_C(16777619);
    }
    return hash;
}

static float wash_random(uint32_t * seed)
{
    /* All randomness is local and deterministic; never touches global RNG or
     * changes with elapsed time, artwork resolution, or the current song. */
    *seed += UINT32_C(0x9e3779b9);
    uint32_t value = *seed;
    value = (value ^ (value >> 16)) * UINT32_C(0x85ebca6b);
    value = (value ^ (value >> 13)) * UINT32_C(0xc2b2ae35);
    value ^= value >> 16;
    return (float)(value >> 8) / 16777215.0f;
}

void cover_gradient_render_wash(const cover_gradient_color_t palette[COVER_GRADIENT_COLORS],
                                uint8_t * pixels, unsigned width, unsigned height, size_t stride,
                                uint32_t theme_tint, uint32_t album_seed)
{
    float colors[3][3], center[2][2], radius[2][2];
    for(unsigned i = 0; i < 3; i++) {
        unsigned maximum = channel_max(palette[i].r,palette[i].g,palette[i].b);
        float scale = maximum ? 1.0f/maximum : 0;
        colors[i][0] = palette[i].b*scale;
        colors[i][1] = palette[i].g*scale;
        colors[i][2] = palette[i].r*scale;
        float floor = channel_min(palette[i].r,palette[i].g,palette[i].b)*scale*0.35f;
        for(unsigned c = 0; c < 3; c++) colors[i][c] = (colors[i][c]-floor)/(1-floor);
    }
    center[0][0] = 0.18f+0.16f*wash_random(&album_seed);
    center[0][1] = 0.18f+0.64f*wash_random(&album_seed);
    if(wash_random(&album_seed) > 0.5f) center[0][0] = 1-center[0][0];
    center[1][0] = 1-center[0][0];
    center[1][1] = 1-center[0][1];
    for(unsigned i = 0; i < 2; i++) {
        radius[i][0] = 0.60f+0.18f*wash_random(&album_seed);
        radius[i][1] = 0.65f+0.18f*wash_random(&album_seed);
    }
    unsigned tint_luma = (77*((theme_tint>>16)&255u)+150*((theme_tint>>8)&255u)+29*(theme_tint&255u)) >> 8;
    bool light = tint_luma >= 128;
    float color_luma[3];
    for(unsigned i = 0; i < 3; i++) color_luma[i] = (29*colors[i][0]+150*colors[i][1]+77*colors[i][2])/256;
    /* Estimate the former lighting's average, then lower it slightly. A
     * single luminance target prevents hue overlap from creating hotspots. */
    float dark_level = (4+19*color_luma[0]+39*color_luma[1]+16*color_luma[2])*0.85f;
    if(dark_level > 40) dark_level = 40;
    if(dark_level < 3) dark_level = 3;
    float x_step = width > 1 ? 1.0f/(width-1) : 0;
    float y_step = height > 1 ? 1.0f/(height-1) : 0;
    float horizontal[2][320];
    for(unsigned start = 0; start < width; start += 320) {
        unsigned columns = width-start < 320 ? width-start : 320;
        for(unsigned x = 0; x < columns; x++) {
            float tx = (start+x)*x_step;
            for(unsigned i = 0; i < 2; i++) {
                float dx = (tx-center[i][0])/radius[i][0];
                horizontal[i][x] = dx*dx;
            }
        }
        for(unsigned y = 0; y < height; y++) {
            float ty = y*y_step, vertical[2];
            for(unsigned i = 0; i < 2; i++) {
                float dy = (ty-center[i][1])/radius[i][1];
                vertical[i] = dy*dy;
            }
            float calm = smooth(0.60f,0.95f,ty);
            for(unsigned x = 0; x < columns; x++) {
                float field[2];
                for(unsigned i = 0; i < 2; i++) {
                    float value = 1-0.65f*(horizontal[i][x]+vertical[i]);
                    if(value < 0) value = 0;
                    field[i] = value*value;
                }
                /* Give secondary hues room to show. Normalize luminance
                 * below rather than suppressing color to avoid hotspots. */
                float accent_a = 0.04f+0.90f*field[0];
                float accent_b = 0.04f+0.85f*field[1];
                float dominant = 0.55f;
                float total = dominant+accent_a+accent_b;
                float rgb[3], hue[3];
                for(unsigned c = 0; c < 3; c++) {
                    hue[c] = (dominant*colors[0][c]+accent_a*colors[1][c]+accent_b*colors[2][c])/total;
                }
                /* RGB hue blending adds gray when warm/cool colors meet.
                 * Reduce that shared gray before normalizing brightness. */
                float floor = hue[0] < hue[1] ? hue[0] : hue[1];
                if(hue[2] < floor) floor = hue[2];
                floor *= 0.65f;
                for(unsigned c = 0; c < 3; c++) {
                    float color = (hue[c]-floor)/(1-floor);
                    rgb[c] = light ? 175+80*color : 0.06f+0.94f*color;
                }
                float luminance = (29*rgb[0]+150*rgb[1]+77*rgb[2])/256;
                float target = light ? 222-2*calm+(field[0]-field[1])
                                     : dark_level*(1-0.04f*calm+0.025f*(field[0]-field[1]));
                if(light && luminance < target) {
                    float lift = (target-luminance)/(255-luminance);
                    for(unsigned c = 0; c < 3; c++) rgb[c] += (255-rgb[c])*lift;
                }
                else {
                    float scale = luminance > 0 ? target/luminance : 0;
                    for(unsigned c = 0; c < 3; c++) rgb[c] *= scale;
                }
                for(unsigned c = 0; c < 3; c++) {
                    float value = rgb[c] < 0 ? 0 : rgb[c] > 255 ? 255 : rgb[c];
                    pixels[y*stride+(start+x)*3+c] = (uint8_t)(value+0.5f);
                }
            }
        }
    }
}

void cover_gradient_scale_cpu(const uint8_t * input, unsigned width, unsigned height,
                              size_t stride, uint8_t * output,
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
            for(unsigned c = 0; c < 3; c++) output[((size_t)y*out_width+x)*3+c] = (uint8_t)rgb[c];
        }
    }
}

static uint16_t finish_level(unsigned blend, unsigned maximum, unsigned phase)
{
    const unsigned denominator = 255u * 255u;
    /* Sixteen evenly spaced thresholds, placed using a blue-noise mask.
     * Dither the 8-bit source here, before discarding RGB565 precision. */
    return (uint16_t)((blend*maximum + (phase*2u+1u)*denominator/32u) / denominator);
}

void cover_gradient_finish(uint8_t * pixels, unsigned width, unsigned height,
                           uint32_t tint_rgb, uint8_t tint_opacity, uint16_t * lut)
{
    if(!pixels || !lut) return;
    unsigned keep = 255u - tint_opacity;
    unsigned red = ((tint_rgb >> 16) & 255u) * tint_opacity;
    unsigned green = ((tint_rgb >> 8) & 255u) * tint_opacity;
    unsigned blue = (tint_rgb & 255u) * tint_opacity;
    uint16_t (*reds)[256] = (uint16_t (*)[256])lut;
    uint16_t (*greens)[256] = reds + 16;
    uint16_t (*blues)[256] = greens + 16;
    for(unsigned phase = 0; phase < 16; phase++) {
        for(unsigned value = 0; value < 256; value++) {
            reds[phase][value] = (uint16_t)(finish_level(value*keep + red,31,phase) << 11);
            greens[phase][value] = (uint16_t)(finish_level(value*keep + green,63,phase) << 5);
            blues[phase][value] = finish_level(value*keep + blue,31,phase);
        }
    }
    uint16_t * output = (uint16_t *)pixels;
    for(unsigned y = 0; y < height; y++) {
        const uint8_t * phases = cover_gradient_noise[y&63];
        for(unsigned x = 0; x < width; x++) {
            size_t i = (size_t)y * width + x;
            const uint8_t * source = pixels + i*3;
            unsigned phase = phases[x&63];
            uint16_t value = reds[phase][source[2]] | greens[phase][source[1]] | blues[phase][source[0]];
            output[i] = value;
        }
    }
}
