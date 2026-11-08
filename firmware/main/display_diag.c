#include "display_diag.h"
#include "backlight.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static display_diag_sample_t *s_ring;
static uint32_t s_next = 1, s_count, s_marker, s_marker_ms;
static SemaphoreHandle_t s_ring_lock;
static portMUX_TYPE s_render_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_render_start, s_renders, s_render_last, s_render_max;
static lv_display_t *s_display;

static void render_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if(code != LV_EVENT_RENDER_START && code != LV_EVENT_RENDER_READY) return;
    uint32_t now = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_render_lock);
    if(code == LV_EVENT_RENDER_START) s_render_start = now;
    else {
        s_render_last = now - s_render_start;
        if(s_render_last > s_render_max) s_render_max = s_render_last;
        s_renders++;
    }
    portEXIT_CRITICAL(&s_render_lock);
}

static void sampler(void *arg)
{
    (void)arg;
    display_diag_sample_t sample = {0};
    TickType_t wake = xTaskGetTickCount();
    uint32_t ticks = 0, previous_duty = 0, previous_hz = 0;
    for(;;) {
        sample.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
        sample.pwm_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH);
        sample.pwm_hz = ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0);
        sample.pwm_period = backlight_pwm_period();
        if(ticks) {
            if(sample.pwm_duty != previous_duty || sample.pwm_hz != previous_hz)
                ESP_LOGI("display_diag", "PWM changed: duty %u -> %u, frequency %u -> %u Hz",
                    (unsigned)previous_duty, (unsigned)sample.pwm_duty,
                    (unsigned)previous_hz, (unsigned)sample.pwm_hz);
            if(sample.pwm_duty != previous_duty) sample.pwm_duty_changes++;
            if(sample.pwm_hz != previous_hz) sample.pwm_freq_changes++;
        }
        previous_duty = sample.pwm_duty;
        previous_hz = sample.pwm_hz;
        lvgl_port_display_get_stats(s_display, &sample.display);
        portENTER_CRITICAL(&s_render_lock);
        sample.renders = s_renders;
        sample.render_last_us = s_render_last;
        sample.render_max_us = s_render_max;
        portEXIT_CRITICAL(&s_render_lock);
        if(ticks % 10 == 0) {
            sample.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            sample.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        /* PSRAM history uses a task mutex: never mask interrupts while
         * copying a history page or formatting JSON. No periodic SD writes. */
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        sample.seq = s_next++;
        s_ring[sample.seq % DISPLAY_DIAG_CAPACITY] = sample;
        if(s_count < DISPLAY_DIAG_CAPACITY) s_count++;
        xSemaphoreGive(s_ring_lock);
        ticks++;
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(DISPLAY_DIAG_INTERVAL_MS));
    }
}

bool display_diag_init(lv_display_t *display)
{
    if(s_ring || !display) return false;
    s_ring = heap_caps_calloc(DISPLAY_DIAG_CAPACITY, sizeof(*s_ring),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ring_lock = xSemaphoreCreateMutex();
    if(!s_ring || !s_ring_lock) goto fail;
    s_display = display;
    if(xTaskCreate(sampler, "display_diag", 3072, NULL, 1, NULL) != pdPASS) goto fail;
    lv_display_add_event_cb(display, render_event, LV_EVENT_ALL, NULL);
    ESP_LOGI("display_diag", "Passive recorder: %d ms samples, %d samples in PSRAM",
             DISPLAY_DIAG_INTERVAL_MS, DISPLAY_DIAG_CAPACITY);
    return true;
fail:
    free(s_ring); s_ring = NULL;
    if(s_ring_lock) vSemaphoreDelete(s_ring_lock);
    s_ring_lock = NULL;
    ESP_LOGW("display_diag", "Recorder unavailable (allocation failed)");
    return false;
}

bool display_diag_latest(display_diag_sample_t *out)
{
    if(!s_ring || !out) return false;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    bool ready = s_count != 0;
    if(ready) *out = s_ring[(s_next - 1) % DISPLAY_DIAG_CAPACITY];
    xSemaphoreGive(s_ring_lock);
    return ready;
}

size_t display_diag_read(uint32_t since, display_diag_sample_t *out, size_t cap,
                         uint32_t *next, uint32_t *dropped)
{
    *next = since; *dropped = 0;
    if(!s_ring || !out || !cap) return 0;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    uint32_t oldest = s_next - s_count;
    if(!since) since = oldest;
    if((int32_t)(since - oldest) < 0) { *dropped = oldest - since; since = oldest; }
    if((int32_t)(s_next - since) < 0) since = s_next;
    size_t n = 0;
    while(since != s_next && n < cap) out[n++] = s_ring[since++ % DISPLAY_DIAG_CAPACITY];
    *next = since;
    xSemaphoreGive(s_ring_lock);
    return n;
}

uint32_t display_diag_mark(uint32_t *uptime_ms)
{
    if(!s_ring) { *uptime_ms = 0; return 0; }
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    s_marker_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t id = ++s_marker;
    *uptime_ms = s_marker_ms;
    xSemaphoreGive(s_ring_lock);
    ESP_LOGI("display_diag", "Flicker marker %u at uptime %u ms", (unsigned)id, (unsigned)*uptime_ms);
    return id;
}

void display_diag_last_marker(uint32_t *id, uint32_t *uptime_ms)
{
    *id = *uptime_ms = 0;
    if(!s_ring) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    *id = s_marker; *uptime_ms = s_marker_ms;
    xSemaphoreGive(s_ring_lock);
}
