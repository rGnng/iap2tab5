#include "feedback.h"

/* Allow /api/loglevel?tag=feedback&level=4 to trace completed tones. */
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <math.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char * TAG = "feedback";

#define FEEDBACK_SAMPLE_RATE 16000
#define FEEDBACK_MAX_SAMPLES 1280

typedef struct {
    uint16_t frequency;
    uint16_t duration_ms;
    uint16_t second_frequency;
} tone_request_t;

static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_queue;
static int16_t s_samples[FEEDBACK_MAX_SAMPLES];
static volatile bool s_enabled;
static volatile uint8_t s_volume;
static bool s_codec_open;

static void write_tone(uint16_t frequency, uint16_t duration_ms)
{
    if(s_codec == NULL) return;

    if(!s_codec_open) {
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = FEEDBACK_SAMPLE_RATE,
            .channel = 1,
            .bits_per_sample = 16,
        };
        int ret = esp_codec_dev_open(s_codec, &fs);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "codec open failed: %d", ret);
            return;
        }
        s_codec_open = true;
    }
    int ret = esp_codec_dev_set_out_mute(s_codec, false);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "codec unmute failed: %d", ret);
        return;
    }

    uint32_t total = (uint32_t)FEEDBACK_SAMPLE_RATE * duration_ms / 1000;
    if(total > FEEDBACK_MAX_SAMPLES) total = FEEDBACK_MAX_SAMPLES;

    int16_t * samples = s_samples;
    const float two_pi = 6.283185307179586f;
    const float step = two_pi * (float)frequency / (float)FEEDBACK_SAMPLE_RATE;
    const uint32_t fade = total / 8 > 1 ? total / 8 : 1;

    for(uint32_t i = 0; i < total; i++) {
        float env = 1.0f;
        if(i < fade) env = (float)i / (float)fade;
        else if(i + fade >= total) env = (float)(total - i) / (float)fade;
        float v = sinf(step * (float)i) * env * 0.18f * 32767.0f;
        samples[i] = (int16_t)v;
    }

    ret = esp_codec_dev_write(s_codec, samples, (int)(total * sizeof(int16_t)));
    if(ret != ESP_OK) ESP_LOGW(TAG, "tone write failed: %d", ret);
    else ESP_LOGD(TAG, "tone: %u Hz, %u ms", (unsigned)frequency,
                  (unsigned)duration_ms);
}

static void feedback_task(void * arg)
{
    (void)arg;
    tone_request_t request;
    for(;;) {
        if(xQueueReceive(s_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        if(!s_enabled || s_volume == 0) continue;
        write_tone(request.frequency, request.duration_ms);
        if(request.second_frequency) write_tone(request.second_frequency, request.duration_ms);
    }
}

bool feedback_init(bool enabled, uint8_t volume)
{
    s_enabled = enabled;
    s_volume = volume;

    s_codec = bsp_audio_codec_speaker_init();
    if(s_codec == NULL) {
        ESP_LOGW(TAG, "speaker codec not available");
        return false;
    }

    esp_codec_dev_set_out_vol(s_codec, volume);

    s_queue = xQueueCreate(4, sizeof(tone_request_t));
    if(s_queue == NULL) {
        ESP_LOGW(TAG, "queue allocation failed");
        return false;
    }

    if(xTaskCreate(feedback_task, "feedback", 8192, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "task creation failed");
        return false;
    }

    ESP_LOGI(TAG, "speaker ready, volume=%u, enabled=%d",
             (unsigned)volume, (int)enabled);
    return true;
}

void feedback_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

void feedback_set_volume(uint8_t volume)
{
    if(volume > 100) volume = 100;
    s_volume = volume;
    if(s_codec != NULL) {
        esp_codec_dev_set_out_vol(s_codec, volume);
    }
}

void feedback_play(feedback_t type)
{
    if(!s_enabled || s_volume == 0 || s_queue == NULL) return;

    tone_request_t request = {0};
    switch(type) {
        case FEEDBACK_FAST_SCROLL:
            request.frequency = 1175;
            request.duration_ms = 22;
            request.second_frequency = 1760;
            break;
        case FEEDBACK_FAST_SCROLL_EXIT:
            request.frequency = 1760;
            request.duration_ms = 22;
            request.second_frequency = 1175;
            break;
        case FEEDBACK_ACTIVATE:
            request.frequency = 880;
            request.duration_ms = 45;
            break;
        case FEEDBACK_BACK:
            request.frequency = 880;
            request.duration_ms = 65;
            break;
        case FEEDBACK_CLICK:
        default:
            request.frequency = 1200;
            request.duration_ms = 22;
            break;
    }
    if(xQueueSend(s_queue, &request, 0) != pdTRUE) {
        ESP_LOGD(TAG, "tone dropped: queue full");
    }
}
