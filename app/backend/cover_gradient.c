#define _POSIX_C_SOURCE 200809L
#include "cover_gradient.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "driver/ppa.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#define GRADIENT_LOG(...) ESP_LOGI("np_gradient", __VA_ARGS__)
#else
#define GRADIENT_LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while(0)
#endif

#define SMALL_STRIDE (COVER_GRADIENT_SMALL_WIDTH * 3u)
#define SMALL_BYTES (SMALL_STRIDE * COVER_GRADIENT_SMALL_HEIGHT)
/* Keep 8-bit channels through scaling, then pack the finished RGB565 image at
 * the front of the same buffer. No separate full-size conversion image. */
#define WORK_BYTES ((size_t)COVER_GRADIENT_WIDTH * COVER_GRADIENT_HEIGHT * 3u)

typedef struct {
    uint64_t key;
    uint32_t tint_rgb;
    uint8_t tint_opacity;
    cover_gradient_sample_t samples[COVER_GRADIENT_SAMPLES];
} gradient_job_t;

static uint8_t * small_image;
static uint16_t * output[2];
static uint16_t * finish_lut;
static int front = -1, ready = -1;
static uint64_t desired_key, front_key, ready_key, work_key, failed_key;
#ifdef ESP_PLATFORM
static SemaphoreHandle_t state_lock;
static QueueHandle_t jobs;
static ppa_client_handle_t ppa_client;
static bool ppa_attempted;
#endif

static uint64_t now_us(void)
{
#ifdef ESP_PLATFORM
    return (uint64_t)esp_timer_get_time();
#else
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (uint64_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
#endif
}

static void lock(void)
{
#ifdef ESP_PLATFORM
    xSemaphoreTake(state_lock, portMAX_DELAY);
#endif
}

static void unlock(void)
{
#ifdef ESP_PLATFORM
    xSemaphoreGive(state_lock);
#endif
}

static void * allocate(size_t bytes)
{
#ifdef ESP_PLATFORM
    size_t alignment;
    if(esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA, &alignment) != ESP_OK ||
       !alignment) return NULL;
    /* P4 firmware uses 128-byte L2 lines; do not assume the 64-byte L1
     * alignment is enough for PPA DMA outputs. Match the driver's query. */
    size_t aligned = (bytes + alignment - 1u) & ~(alignment - 1u);
    return heap_caps_aligned_alloc(alignment, aligned, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    size_t aligned = (bytes + 63u) & ~(size_t)63u;
    void * pointer = NULL;
    if(posix_memalign(&pointer, 64, aligned) != 0) return NULL;
    return pointer;
#endif
}

static bool wanted(uint64_t key)
{
    lock();
    bool current = desired_key == key;
    unlock();
    return current;
}

static bool scale_hardware(uint16_t * pixels)
{
#ifdef ESP_PLATFORM
    if(!ppa_attempted) {
        ppa_attempted = true;
        ppa_client_config_t config = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
            /* Leave bandwidth for LCD refresh and other PSRAM users. */
            .data_burst_length = PPA_DATA_BURST_LENGTH_64,
        };
        esp_err_t err = ppa_register_client(&config, &ppa_client);
        if(err != ESP_OK) {
            ppa_client = NULL;
            ESP_LOGW("np_gradient", "PPA unavailable (%s); using CPU fallback", esp_err_to_name(err));
        }
    }
    if(ppa_client) {
        ppa_srm_oper_config_t config = {
            .in = {.buffer = small_image, .pic_w = COVER_GRADIENT_SMALL_WIDTH,
                .pic_h = COVER_GRADIENT_SMALL_HEIGHT,
                .block_w = COVER_GRADIENT_SMALL_WIDTH,
                .block_h = COVER_GRADIENT_SMALL_HEIGHT,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB888},
            .out = {.buffer = pixels, .buffer_size = WORK_BYTES,
                .pic_w = COVER_GRADIENT_WIDTH, .pic_h = COVER_GRADIENT_HEIGHT,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB888},
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = 4.0f, .scale_y = 4.0f,
            .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
            /* Blocking waits only in this low-priority worker, never LVGL.
             * The IDF driver synchronizes caches around the DMA operation. */
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t err = ppa_do_scale_rotate_mirror(ppa_client, &config);
        if(err == ESP_OK) return true;
        ESP_LOGW("np_gradient", "PPA scale failed (%s); using CPU fallback", esp_err_to_name(err));
    }
#else
    (void)pixels;
#endif
    return false;
}

static void process(gradient_job_t * job)
{
    uint64_t start = now_us();
    lock();
    if(desired_key != job->key) { unlock(); return; }
    int slot = front == 0 ? 1 : 0;
    work_key = job->key;
    if(ready == slot) { ready = -1; ready_key = 0; }
    unlock();

    bool success = false, accelerated = false;
    uint64_t palette_end = start, mesh_end = start, scale_end = start, finish_end = start;
    if(!small_image) small_image = allocate(SMALL_BYTES);
    if(!output[slot]) output[slot] = allocate(WORK_BYTES);
    if(!finish_lut) finish_lut = allocate(COVER_GRADIENT_FINISH_LUT_ENTRIES * sizeof(*finish_lut));
    if(!small_image || !output[slot] || !finish_lut) goto done;
    cover_gradient_color_t palette[COVER_GRADIENT_COLORS];
    if(!cover_gradient_palette(job->samples, palette)) goto done;
    palette_end = now_us();
    if(!wanted(job->key)) goto done;
    cover_gradient_render(palette, small_image, COVER_GRADIENT_SMALL_WIDTH,
                           COVER_GRADIENT_SMALL_HEIGHT, SMALL_STRIDE, job->tint_rgb);
    mesh_end = now_us();
    if(!wanted(job->key)) goto done;
    accelerated = scale_hardware(output[slot]);
    if(!accelerated) {
        cover_gradient_scale_cpu(small_image, COVER_GRADIENT_SMALL_WIDTH,
            COVER_GRADIENT_SMALL_HEIGHT, SMALL_STRIDE, (uint8_t *)output[slot],
            COVER_GRADIENT_WIDTH, COVER_GRADIENT_HEIGHT);
    }
    scale_end = now_us();
    if(!wanted(job->key)) goto done;
    /* Spatial contrast is already built into the light field; the final
     * theme veil is lighter than the legacy full-screen scrim. */
    cover_gradient_finish((uint8_t *)output[slot], COVER_GRADIENT_WIDTH, COVER_GRADIENT_HEIGHT,
                           job->tint_rgb, job->tint_opacity / 2u, finish_lut);
    finish_end = now_us();
    success = true;
done:
    lock();
    work_key = 0;
    bool current = desired_key == job->key;
    if(current) {
        if(success) { ready = slot; ready_key = job->key; failed_key = 0; }
        else failed_key = job->key;
    }
    unlock();
    if(current && success) {
        GRADIENT_LOG("ready key=%016llx accelerator=%s palette=%u mesh=%u scale=%u finish=%u total=%u us",
            (unsigned long long)job->key, accelerated ? "PPA" : "CPU",
            (unsigned)(palette_end - start), (unsigned)(mesh_end - palette_end),
            (unsigned)(scale_end - mesh_end), (unsigned)(finish_end - scale_end),
            (unsigned)(finish_end - start));
    }
    else if(current) GRADIENT_LOG("failed key=%016llx; retaining theme background",
                                  (unsigned long long)job->key);
}

#ifdef ESP_PLATFORM
static void worker(void * arg)
{
    (void)arg;
    gradient_job_t * job;
    for(;;) {
        if(xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        process(job);
        free(job);
    }
}
#endif

/* Initialization and submission have one producer: the LVGL/UI thread. */
static bool initialize(void)
{
#ifdef ESP_PLATFORM
    if(jobs) return true;
    if(!state_lock) state_lock = xSemaphoreCreateMutex();
    if(!state_lock) return false;
    jobs = xQueueCreate(1, sizeof(gradient_job_t *));
    if(!jobs) return false;
    if(xTaskCreateWithCaps(worker, "np_gradient", 12288, NULL, 2, NULL,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(jobs); jobs = NULL;
        return false;
    }
#endif
    return true;
}

uint64_t cover_gradient_request(const void * pixels, size_t bytes, unsigned width,
                                unsigned height, size_t stride, cover_gradient_format_t format,
                                uint32_t tint_rgb, uint8_t tint_opacity)
{
    if(!initialize()) return 0;
    gradient_job_t * job = allocate(sizeof(*job));
    if(!job) return 0;
    if(!cover_gradient_sample(pixels, bytes, width, height, stride, format, job->samples)) {
        free(job); return 0;
    }
    /* Sample content and tint identify the finished image. Repeated iAP2
     * revisions need no repeated work; a theme change invalidates the tint. */
    uint64_t key = UINT64_C(14695981039346656037);
    const uint8_t * data = (const uint8_t *)job->samples;
    for(size_t i = 0; i < sizeof(job->samples); i++) key = (key ^ data[i]) * UINT64_C(1099511628211);
    for(unsigned shift = 0; shift < 24; shift += 8) key = (key ^ ((tint_rgb >> shift) & 255u)) * UINT64_C(1099511628211);
    key = (key ^ tint_opacity) * UINT64_C(1099511628211);
    if(!key) key = 1;
    job->key = key;
    job->tint_rgb = tint_rgb;
    job->tint_opacity = tint_opacity;
    lock();
    desired_key = key;
    bool reused = front_key == key || ready_key == key || work_key == key;
    bool failed = failed_key == key;
    unlock();
    if(reused || failed) { free(job); return failed ? 0 : key; }
#ifdef ESP_PLATFORM
    /* Queue only the newest artwork. Pointer-sized queue operations keep
     * large PSRAM copies out of FreeRTOS critical sections. */
    gradient_job_t * obsolete;
    if(xQueueReceive(jobs, &obsolete, 0) == pdTRUE) free(obsolete);
    xQueueOverwrite(jobs, &job);
#else
    process(job);
    free(job);
#endif
    return key;
}

cover_gradient_state_t cover_gradient_poll(uint64_t key, const uint16_t ** pixels)
{
#ifdef ESP_PLATFORM
    if(!state_lock) return COVER_GRADIENT_SUPERSEDED;
#endif
    lock();
    cover_gradient_state_t state = COVER_GRADIENT_PENDING;
    if(!key || key != desired_key) state = COVER_GRADIENT_SUPERSEDED;
    else if(failed_key == key) state = COVER_GRADIENT_FAILED;
    else {
        if(ready_key == key && ready >= 0) {
            front = ready; front_key = ready_key;
            ready = -1; ready_key = 0;
        }
        if(front_key == key && front >= 0) {
            if(pixels) *pixels = output[front];
            state = COVER_GRADIENT_READY;
        }
    }
    unlock();
    return state;
}

void cover_gradient_cancel(void)
{
#ifdef ESP_PLATFORM
    if(!state_lock) return;
#endif
    lock(); desired_key = 0; failed_key = 0; unlock();
#ifdef ESP_PLATFORM
    if(jobs) {
        gradient_job_t * job;
        if(xQueueReceive(jobs, &job, 0) == pdTRUE) free(job);
    }
#endif
}
