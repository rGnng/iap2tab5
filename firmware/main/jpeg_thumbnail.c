#include "jpeg_thumbnail.h"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

typedef struct
{
    struct jpeg_decompress_struct decoder;
    struct jpeg_error_mgr error;
    struct jpeg_progress_mgr progress;
    jmp_buf jump;
    FILE *file;
    uint8_t *row;
    bool created;
#ifdef ESP_PLATFORM
    int64_t yielded;
#endif
} decode_t;
static void fatal(j_common_ptr common)
{
    decode_t *context = (decode_t *)common->client_data;
#ifdef ESP_PLATFORM
    char error[JMSG_LENGTH_MAX];
    common->err->format_message(common, error);
    ESP_LOGW("jpeg_thumb", "%s", error);
#endif
    longjmp(context->jump, 1);
}
static void message(j_common_ptr common, int level)
{
    /* Reject truncated/corrupt input instead of saving a partially decoded cover. */
    if(level < 0)
        fatal(common);
}
static void progress(j_common_ptr common)
{
#ifdef ESP_PLATFORM
    decode_t *context = (decode_t *)common->client_data;
    int64_t now = esp_timer_get_time();
    if(now - context->yielded > 20000)
    {
        vTaskDelay(1);
        context->yielded = now;
    }
#else
    (void)common;
#endif
}
bool jpeg_thumbnail_decode(const char *path, uint8_t *rgb565, unsigned size)
{
    if(!path || !rgb565 || !size || size > 360)
        return false;
    decode_t *context = calloc(1, sizeof(*context));
    if(!context)
        return false;
    bool ok = false;
    context->decoder.err = jpeg_std_error(&context->error);
    context->error.error_exit = fatal;
    context->error.emit_message = message;
    context->decoder.client_data = context;
    if(setjmp(context->jump))
        goto done;
    context->file = fopen(path, "rb");
    if(!context->file)
        goto done;
    context->created = true;
    jpeg_create_decompress(&context->decoder);
#ifdef JPEG_TEST_MEMORY_LIMIT
    context->decoder.mem->max_memory_to_use = JPEG_TEST_MEMORY_LIMIT;
#elif defined(ESP_PLATFORM)
    size_t available = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t limit = available > 1024 * 1024 ? available - 512 * 1024 : 256 * 1024;
    if(limit > 6 * 1024 * 1024)
        limit = 6 * 1024 * 1024;
    context->decoder.mem->max_memory_to_use = (long)limit;
#endif
    context->decoder.client_data = context;
    context->progress.progress_monitor = progress;
    context->decoder.progress = &context->progress;
    jpeg_stdio_src(&context->decoder, context->file);
    if(jpeg_read_header(&context->decoder, TRUE) != JPEG_HEADER_OK)
        goto done;
    if(!context->decoder.image_width || !context->decoder.image_height
        || context->decoder.image_width > 8192 || context->decoder.image_height > 8192
        || (uint64_t)context->decoder.image_width * context->decoder.image_height > 24000000)
        goto done;
    bool cmyk = context->decoder.jpeg_color_space == JCS_CMYK
        || context->decoder.jpeg_color_space == JCS_YCCK;
    context->decoder.out_color_space = cmyk ? JCS_CMYK : JCS_RGB;
    context->decoder.scale_num = 1;
    for(unsigned scale = 8; scale >= 1; scale /= 2)
    {
        context->decoder.scale_denom = scale;
        jpeg_calc_output_dimensions(&context->decoder);
        if(context->decoder.output_width >= size && context->decoder.output_height >= size)
            break;
    }
    jpeg_start_decompress(&context->decoder);
    unsigned components = context->decoder.output_components;
    if(components != (cmyk ? 4u : 3u))
        goto done;
    unsigned width = context->decoder.output_width, height = context->decoder.output_height;
    context->row = malloc((size_t)width * components);
    if(!context->row)
        goto done;
    unsigned next = 0;
    while(context->decoder.output_scanline < height)
    {
        unsigned source_y = context->decoder.output_scanline;
        JSAMPROW row = context->row;
        if(jpeg_read_scanlines(&context->decoder, &row, 1) != 1)
            goto done;
        while(next < size && ((uint64_t)(next * 2 + 1) * height / (size * 2)) == source_y)
        {
            for(unsigned x = 0; x < size; x++)
            {
                unsigned source_x = (uint64_t)(x * 2 + 1) * width / (size * 2);
                const uint8_t *p = row + source_x * components;
                unsigned r = p[0], g = p[1], b = p[2];
                if(cmyk)
                {
                    if(context->decoder.saw_Adobe_marker)
                    {
                        r = r * p[3] / 255;
                        g = g * p[3] / 255;
                        b = b * p[3] / 255;
                    }
                    else
                    {
                        r = (255 - r) * (255 - p[3]) / 255;
                        g = (255 - g) * (255 - p[3]) / 255;
                        b = (255 - b) * (255 - p[3]) / 255;
                    }
                }
                uint16_t pixel = ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3);
                size_t offset = ((size_t)next * size + x) * 2;
                rgb565[offset] = pixel;
                rgb565[offset + 1] = pixel >> 8;
            }
            next++;
        }
    }
    ok = next == size && jpeg_finish_decompress(&context->decoder);
done:
    if(context->created)
        jpeg_destroy_decompress(&context->decoder);
    if(context->file)
        fclose(context->file);
    free(context->row);
    free(context);
    return ok;
}
