#include "screenshot.h"

#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

bool sim_screenshot(const char * path)
{
    lv_draw_buf_t * snapshot = lv_snapshot_take(lv_screen_active(),
                                               LV_COLOR_FORMAT_ARGB8888);
    if(snapshot == NULL) {
        fprintf(stderr, "[screenshot] lv_snapshot_take() failed\n");
        return false;
    }

    const uint32_t width = snapshot->header.w;
    const uint32_t height = snapshot->header.h;
    const uint32_t stride = snapshot->header.stride;
    const uint8_t * src = snapshot->data;

    /* LVGL stores ARGB8888 as BGRA on little-endian targets. stb expects RGBA. */
    uint8_t * rgba = malloc((size_t)width * height * 4);
    if(rgba == NULL) {
        fprintf(stderr, "[screenshot] out of memory\n");
        lv_draw_buf_destroy(snapshot);
        return false;
    }

    for(uint32_t y = 0; y < height; y++) {
        const uint8_t * row = src + (size_t)y * stride;
        uint8_t * dst = rgba + (size_t)y * width * 4;
        for(uint32_t x = 0; x < width; x++) {
            dst[x * 4 + 0] = row[x * 4 + 2]; /* R */
            dst[x * 4 + 1] = row[x * 4 + 1]; /* G */
            dst[x * 4 + 2] = row[x * 4 + 0]; /* B */
            dst[x * 4 + 3] = 0xFF;           /* opaque */
        }
    }

    const int ok = stbi_write_png(path, (int)width, (int)height, 4,
                                  rgba, (int)width * 4);
    free(rgba);
    lv_draw_buf_destroy(snapshot);

    if(ok == 0) {
        fprintf(stderr, "[screenshot] failed to write %s\n", path);
        return false;
    }

    printf("[screenshot] wrote %s (%ux%u)\n", path, width, height);
    return true;
}
