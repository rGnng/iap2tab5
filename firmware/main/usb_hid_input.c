/*
 * USB HID keyboard input for the Tab5 USB-A port.
 *
 * The product knob/keypad enumerates as a standard boot keyboard:
 *   Up / Down, Enter, Backspace, Page Up / Page Down, C, 1..7
 * Key presses are translated into the shared ui_key_t events.
 */

#include "usb_hid_input.h"

#include "usb_host_core.h"

#include "bsp/esp-bsp.h"
#include "iap2_host.h"
#include "ui.h"
#include "ulanzi_d100h.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static const char * TAG = "usb_hid";

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_device_event_t;

typedef struct {
    ui_key_t key;
    bool down;
} key_event_t;

static QueueHandle_t s_device_events;
static QueueHandle_t s_key_events;

/* Previous/next behave as tap-to-skip and hold-to-rewind/fast-forward. */
#define HOLD_MS         500
#define HOLD_MAX_MS     8000

static bool s_hold_pending;
static bool s_hold_sent;
static ui_key_t s_hold_key;
static int64_t s_hold_start_us;
static int64_t s_hold_sent_us;

static uint8_t s_prev_keys[HID_KEYBOARD_KEY_MAX];

#define ULANZI_VID 0xFFF1
#define ULANZI_TOP_RIGHT_ID 0x03
#define ULANZI_LONG_PRESS_MS 600
static hid_host_device_handle_t s_ulanzi_handle;
static SemaphoreHandle_t s_ulanzi_lock;
static uint8_t s_ulanzi_addr;
static int s_ulanzi_report_count;
static int64_t s_ulanzi_top_right_down_us;

static bool handle_is_ulanzi(hid_host_device_handle_t handle)
{
    bool yes = false;
    if(s_ulanzi_lock != NULL) {
        xSemaphoreTake(s_ulanzi_lock, portMAX_DELAY);
        yes = (handle == s_ulanzi_handle);
        xSemaphoreGive(s_ulanzi_lock);
    }
    return yes;
}

static void set_ulanzi_handle(hid_host_device_handle_t handle)
{
    if(s_ulanzi_lock != NULL) {
        xSemaphoreTake(s_ulanzi_lock, portMAX_DELAY);
        s_ulanzi_handle = handle;
        xSemaphoreGive(s_ulanzi_lock);
    }
}

static void ulanzi_send_heartbeat(hid_host_device_handle_t handle)
{
    if(handle == NULL) return;
    static int last_result = -1;
    uint8_t heartbeat[64];
    ulanzi_d100h_heartbeat(heartbeat);
    esp_err_t err = hid_host_device_send_output_report(
        handle, heartbeat, sizeof(heartbeat));
    if(err != ESP_OK) {
        err = hid_class_request_set_report(
            handle, HID_REPORT_TYPE_OUTPUT, 0, heartbeat, sizeof(heartbeat));
    }
    int result = (err == ESP_OK) ? 1 : 0;
    if(result != last_result) {
        last_result = result;
        if(result) {
            ESP_LOGI(TAG, "D100H heartbeat accepted");
        }
        else {
            ESP_LOGW(TAG, "D100H heartbeat failed: %s",
                     esp_err_to_name(err));
        }
    }
}

static void ulanzi_heartbeat_task(void * arg)
{
    LV_UNUSED(arg);
    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        hid_host_device_handle_t handle = NULL;
        if(s_ulanzi_lock != NULL) {
            xSemaphoreTake(s_ulanzi_lock, portMAX_DELAY);
            handle = s_ulanzi_handle;
            xSemaphoreGive(s_ulanzi_lock);
        }
        ulanzi_send_heartbeat(handle);
    }
}

static bool descriptor_has_usage_page(const uint8_t * desc, size_t len,
                                      uint16_t page)
{
    if(desc == NULL) return false;
    for(size_t i = 0; i + 2 < len; i++) {
        if(desc[i] == 0x06 && desc[i + 1] == (uint8_t)(page & 0xFF) &&
           desc[i + 2] == (uint8_t)(page >> 8)) {
            return true;
        }
    }
    return false;
}

static bool hid_key_to_ui(uint8_t code, ui_key_t * out)
{
    switch(code) {
        case HID_KEY_UP:
        case HID_KEY_KEYPAD_8:
        case HID_KEY_VOLUME_UP: *out = UI_KEY_UP; return true;
        case HID_KEY_DOWN:
        case HID_KEY_KEYPAD_2:
        case HID_KEY_VOLUME_DOWN: *out = UI_KEY_DOWN; return true;
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER:
        case HID_KEY_KEYPAD_5: *out = UI_KEY_ENTER; return true;
        case HID_KEY_DEL:
        case HID_KEY_ESC: *out = UI_KEY_BACK; return true;
        case HID_KEY_PAGEUP: *out = UI_KEY_PAGE_UP; return true;
        case HID_KEY_PAGEDOWN: *out = UI_KEY_PAGE_DOWN; return true;
        case HID_KEY_OPEN_BRACKET: *out = UI_KEY_SCREEN_PREV; return true;
        case HID_KEY_CLOSE_BRACKET: *out = UI_KEY_SCREEN_NEXT; return true;
        case HID_KEY_C: *out = UI_KEY_CONTEXT; return true;
        case HID_KEY_1: *out = UI_KEY_1; return true;
        case HID_KEY_2: *out = UI_KEY_2; return true;
        case HID_KEY_3: *out = UI_KEY_3; return true;
        case HID_KEY_4: *out = UI_KEY_4; return true;
        case HID_KEY_5: *out = UI_KEY_5; return true;
        case HID_KEY_6: *out = UI_KEY_6; return true;
        case HID_KEY_7: *out = UI_KEY_7; return true;
        default: return false;
    }
}

static void keyboard_report(const uint8_t * data, int length)
{
    if(length < (int)sizeof(hid_keyboard_input_report_boot_t)) return;
    const hid_keyboard_input_report_boot_t * report =
        (const hid_keyboard_input_report_boot_t *)data;

    for(int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        uint8_t code = report->key[i];
        if(code == 0) continue;

        bool was_down = false;
        for(int j = 0; j < HID_KEYBOARD_KEY_MAX; j++) {
            if(s_prev_keys[j] == code) {
                was_down = true;
                break;
            }
        }
        if(was_down) continue;

        ui_key_t key;
        if(hid_key_to_ui(code, &key)) {
            usb_hid_input_post_key(key);
        }
    }

    memcpy(s_prev_keys, report->key, sizeof(s_prev_keys));
}

static void hid_interface_cb(hid_host_device_handle_t handle,
                             const hid_host_interface_event_t event,
                             void * arg)
{
    LV_UNUSED(arg);

    hid_host_dev_params_t params;
    if(hid_host_device_get_params(handle, &params) != ESP_OK) return;

    switch(event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
            uint8_t data[64] = {0};
            size_t length = 0;
            if(hid_host_device_get_raw_input_report_data(handle, data, sizeof(data),
                                                         &length) != ESP_OK) {
                break;
            }
            if(s_ulanzi_addr != 0 && params.addr == s_ulanzi_addr &&
               params.iface_num == 3 && length >= 8) {
                static const uint8_t beat[8] = {
                    0x21,0xf4,0x5e,0xb7,0x1c,0x35,0x87,0xab
                };
                if(memcmp(data, beat, sizeof(beat)) != 0) {
                    ESP_LOGD(TAG, "D100H vendor raw len=%u: "
                             "%02x %02x %02x %02x %02x %02x %02x %02x",
                             (unsigned)length, data[0], data[1], data[2],
                             data[3], data[4], data[5], data[6], data[7]);
                }
            }
            if(s_ulanzi_addr != 0 && params.addr == s_ulanzi_addr &&
               params.iface_num != 3) {
                bool nonzero = false;
                for(size_t i = 0; i < length && i < sizeof(data); i++) {
                    if(data[i] != 0) {
                        nonzero = true;
                        break;
                    }
                }
                if(nonzero) {
                    ESP_LOGD(TAG, "D100H raw if=%u len=%u: "
                             "%02x %02x %02x %02x %02x %02x %02x %02x",
                             params.iface_num, (unsigned)length,
                             length > 0 ? data[0] : 0,
                             length > 1 ? data[1] : 0,
                             length > 2 ? data[2] : 0,
                             length > 3 ? data[3] : 0,
                             length > 4 ? data[4] : 0,
                             length > 5 ? data[5] : 0,
                             length > 6 ? data[6] : 0,
                             length > 7 ? data[7] : 0);
                }
                s_ulanzi_report_count++;
            }
            if(handle_is_ulanzi(handle)) {
                uint8_t id = 0;
                bool down = false;
                if(ulanzi_d100h_decode(data, length, &id, &down)) {
                    if(id == ULANZI_TOP_RIGHT_ID) {
                        if(down) {
                            s_ulanzi_top_right_down_us = esp_timer_get_time();
                        }
                        else {
                            int64_t held_ms = 0;
                            if(s_ulanzi_top_right_down_us != 0) {
                                held_ms = (esp_timer_get_time() -
                                           s_ulanzi_top_right_down_us) / 1000;
                            }
                            s_ulanzi_top_right_down_us = 0;
                            ui_key_t key =
                                held_ms >= ULANZI_LONG_PRESS_MS
                                    ? UI_KEY_STAR : UI_KEY_CONTEXT;
                            ESP_LOGI(TAG,
                                     "D100H top-right held=%lldms key=%d",
                                     (long long)held_ms, (int)key);
                            usb_hid_input_post_key(key);
                        }
                    }
                    else if(id == 0x05 || id == 0x08) {
                        usb_hid_input_post_key_state(
                            id == 0x05 ? UI_KEY_PREV_TRACK
                                       : UI_KEY_NEXT_TRACK, down);
                    }
                    else {
                        ui_key_t key;
                        bool mapped = ulanzi_d100h_to_ui(id, down, &key);
                        ESP_LOGD(TAG, "D100H event id=%u down=%d mapped=%d key=%d",
                                 (unsigned)id, (int)down, (int)mapped,
                                 mapped ? (int)key : -1);
                        if(mapped) {
                            usb_hid_input_post_key(key);
                        }
                    }
                }
                break;
            }
            if(params.proto == HID_PROTOCOL_KEYBOARD &&
               params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                keyboard_report(data, (int)length);
            }
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            if(handle_is_ulanzi(handle)) {
                set_ulanzi_handle(NULL);
                s_ulanzi_top_right_down_us = 0;
                ESP_LOGI(TAG, "Ulanzi D100H disconnected");
            }
            else {
                ESP_LOGI(TAG, "HID device disconnected");
            }
            memset(s_prev_keys, 0, sizeof(s_prev_keys));
            hid_host_device_close(handle);
            break;
        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "HID transfer error");
            break;
        default:
            break;
    }
}

static void hid_device_cb(hid_host_device_handle_t handle,
                          const hid_host_driver_event_t event, void * arg)
{
    LV_UNUSED(arg);
    hid_device_event_t evt = {.handle = handle, .event = event};
    xQueueSend(s_device_events, &evt, 0);
}

static void hid_event_task(void * arg)
{
    LV_UNUSED(arg);
    hid_device_event_t evt;
    for(;;) {
        if(xQueueReceive(s_device_events, &evt, portMAX_DELAY) != pdTRUE) continue;

        if(evt.event != HID_HOST_DRIVER_EVENT_CONNECTED) continue;

        hid_host_dev_params_t params;
        if(hid_host_device_get_params(evt.handle, &params) != ESP_OK) continue;

        hid_host_dev_info_t info;
        bool have_info = hid_host_get_device_info(evt.handle, &info) == ESP_OK;
        if(have_info && info.VID == 0x05AC) {
            ESP_LOGI(TAG, "skip Apple HID interface (iAP2 owns it)");
            continue;
        }

        ESP_LOGI(TAG, "HID connected: %04x:%04x proto=%d subclass=%d",
                 have_info ? (unsigned)info.VID : 0,
                 have_info ? (unsigned)info.PID : 0,
                 (int)params.proto, (int)params.sub_class);

        const hid_host_device_config_t config = {
            .callback = hid_interface_cb,
            .callback_arg = NULL,
        };
        if(hid_host_device_open(evt.handle, &config) != ESP_OK) {
            ESP_LOGE(TAG, "hid_host_device_open failed");
            continue;
        }

        bool is_ulanzi = false;
        if(have_info && info.VID == ULANZI_VID) {
            size_t desc_len = 0;
            uint8_t * desc = hid_host_get_report_descriptor(evt.handle,
                                                            &desc_len);
            if(descriptor_has_usage_page(desc, desc_len, 0xFFFE)) {
                is_ulanzi = true;
            }
        }

        if(params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
            hid_class_request_set_protocol(evt.handle, HID_REPORT_PROTOCOL_BOOT);
            if(params.proto == HID_PROTOCOL_KEYBOARD) {
                hid_class_request_set_idle(evt.handle, 0, 0);
            }
        }
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        if(hid_host_device_start(evt.handle) != ESP_OK) {
            ESP_LOGE(TAG, "hid_host_device_start failed");
            continue;
        }
        if(is_ulanzi) {
            set_ulanzi_handle(evt.handle);
            s_ulanzi_addr = params.addr;
            s_ulanzi_report_count = 0;
            s_ulanzi_top_right_down_us = 0;
            ulanzi_send_heartbeat(evt.handle);
            ESP_LOGI(TAG, "Ulanzi D100H vendor HID iface=%u addr=%u ready",
                     params.iface_num, params.addr);
        }
    }
}

static uint8_t hold_bit_for_key(ui_key_t key)
{
    /* Tap uses bits 3/2 (previous/next).  Hold uses the dedicated
     * rewind/fast-forward bits 11/10, which the iPod scans while pressed. */
    return key == UI_KEY_PREV_TRACK ? 11 : 10;
}

static void dispatch_key_event(const key_event_t * evt)
{
    const ui_key_t key = evt->key;

    if(key == UI_KEY_PREV_TRACK || key == UI_KEY_NEXT_TRACK) {
        if(evt->down) {
            if(s_hold_pending) return;
            s_hold_pending = true;
            s_hold_sent = false;
            s_hold_key = key;
            s_hold_start_us = esp_timer_get_time();
            return;
        }

        if(!s_hold_pending || s_hold_key != key) return;
        if(s_hold_sent) {
            iap2_host_hid_release();
        }
        else if(bsp_display_lock(1000)) {
            /* Short tap: previous/next track. */
            ui_handle_key(key);
            bsp_display_unlock();
        }
        s_hold_pending = false;
        s_hold_sent = false;
        return;
    }

    if(!evt->down) return;

    int delta = 0;
    if(key == UI_KEY_UP) delta = -1;
    else if(key == UI_KEY_DOWN) delta = 1;

    if(delta != 0) {
        /* Coalesce a rotary burst.  A fast spin otherwise queues one
         * list refresh per detent, so the UI keeps moving after the knob
         * has stopped. */
        for(int i = 0; i < 63; i++) {
            key_event_t next;
            if(xQueuePeek(s_key_events, &next, 0) != pdTRUE) break;
            if(!next.down) break;
            if(next.key != UI_KEY_UP && next.key != UI_KEY_DOWN) break;
            xQueueReceive(s_key_events, &next, 0);
            delta += next.key == UI_KEY_UP ? -1 : 1;
        }
        if(delta != 0 && bsp_display_lock(1000)) {
            ui_handle_key_repeat(delta < 0 ? UI_KEY_UP : UI_KEY_DOWN,
                                 delta < 0 ? -delta : delta);
            bsp_display_unlock();
        }
        return;
    }

    if(bsp_display_lock(1000)) {
        ESP_LOGD(TAG, "dispatch UI key=%d", (int)key);
        ui_handle_key(key);
        bsp_display_unlock();
    }
}

static void key_dispatch_task(void * arg)
{
    LV_UNUSED(arg);
    for(;;) {
        TickType_t wait = portMAX_DELAY;
        if(s_hold_pending) {
            int64_t deadline = s_hold_sent
                ? s_hold_sent_us + (int64_t)HOLD_MAX_MS * 1000
                : s_hold_start_us + (int64_t)HOLD_MS * 1000;
            int64_t remain_us = deadline - esp_timer_get_time();
            wait = remain_us <= 0
                       ? 0
                       : pdMS_TO_TICKS((remain_us + 999) / 1000);
        }

        key_event_t evt;
        if(xQueueReceive(s_key_events, &evt, wait) == pdTRUE) {
            dispatch_key_event(&evt);
            continue;
        }

        if(!s_hold_pending) continue;
        if(!s_hold_sent) {
            iap2_host_hid_press_bit(hold_bit_for_key(s_hold_key));
            s_hold_sent = true;
            s_hold_sent_us = esp_timer_get_time();
        }
        else {
            /* Safety release if the controller disappears while held. */
            iap2_host_hid_release();
            s_hold_pending = false;
            s_hold_sent = false;
        }
    }
}

void usb_hid_input_post_key(ui_key_t key)
{
    usb_hid_input_post_key_state(key, true);
}

void usb_hid_input_post_key_state(ui_key_t key, bool down)
{
    if(s_key_events != NULL) {
        key_event_t evt = { .key = key, .down = down };
        xQueueSend(s_key_events, &evt, 0);
    }
}

void usb_hid_input_start(void)
{
    s_device_events = xQueueCreate(8, sizeof(hid_device_event_t));
    s_key_events = xQueueCreate(32, sizeof(key_event_t));
    s_ulanzi_lock = xSemaphoreCreateMutex();
    if(s_device_events == NULL || s_key_events == NULL ||
       s_ulanzi_lock == NULL) {
        ESP_LOGE(TAG, "queue/lock allocation failed");
        return;
    }

    xTaskCreateWithCaps(key_dispatch_task, "usb_keys", 32 * 1024, NULL, 5,
                        NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreateWithCaps(hid_event_task, "usb_hid_evt", 8192, NULL, 5,
                        NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    xTaskCreateWithCaps(ulanzi_heartbeat_task, "ulanzi_hb", 4096, NULL, 4,
                        NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 8192,
        .core_id = 0,
        .callback = hid_device_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_config));
    ESP_LOGI(TAG, "USB HID host started");
}
