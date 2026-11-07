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
                                               LV_COLOR_FORMAT_RGB565);
    if(snapshot == NULL) {
        fprintf(stderr, "[screenshot] lv_snapshot_take() failed\n");
        return false;
    }

    const uint32_t width = snapshot->header.w;
    const uint32_t height = snapshot->header.h;
    const uint32_t stride = snapshot->header.stride;
    const uint8_t * src = snapshot->data;

    /* Expand RGB565 to 8 bits per channel the same way the firmware
     * screenshot does, so the PNG shows what the Tab5 panel gets. */
    uint8_t * rgba = malloc((size_t)width * height * 4);
    if(rgba == NULL) {
        fprintf(stderr, "[screenshot] out of memory\n");
        lv_draw_buf_destroy(snapshot);
        return false;
    }

    for(uint32_t y = 0; y < height; y++) {
        const uint16_t * row = (const uint16_t *)(src + (size_t)y * stride);
        uint8_t * dst = rgba + (size_t)y * width * 4;
        for(uint32_t x = 0; x < width; x++) {
            const uint16_t px = row[x];
            const uint32_t r5 = (px >> 11) & 0x1F;
            const uint32_t g6 = (px >> 5) & 0x3F;
            const uint32_t b5 = px & 0x1F;
            dst[x * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            dst[x * 4 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            dst[x * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
            dst[x * 4 + 3] = 0xFF;
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
