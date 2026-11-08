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

typedef enum {
    KEY_EVENT_KEY,
    KEY_EVENT_TOP_RIGHT,
    KEY_EVENT_KNOB,
    KEY_EVENT_GESTURE_CANCEL,
    KEY_EVENT_ACTIVITY,
    KEY_EVENT_BACK,
    KEY_EVENT_PLAY,
} key_event_kind_t;

typedef struct {
    ui_key_t key;
    bool down;
    key_event_kind_t kind;
    d100h_input_source_t source;
    uint8_t id;
    int64_t time_us;
} key_event_t;

static QueueHandle_t s_device_events;
static QueueHandle_t s_key_events;
static ulanzi_top_right_t s_top_right[D100H_INPUT_COUNT];
static ulanzi_knob_t s_knob[D100H_INPUT_COUNT];
static ulanzi_play_t s_play[D100H_INPUT_COUNT];
static ulanzi_back_t s_back[D100H_INPUT_COUNT];
static uint16_t s_button_down[D100H_INPUT_COUNT];
static uint16_t s_wake_consumed[D100H_INPUT_COUNT];

/* Previous/next behave as tap-to-skip and hold-to-rewind/fast-forward. */
#define HOLD_MS         500
#define HOLD_MAX_MS     8000

static bool s_hold_pending;
static bool s_hold_sent;
static ui_key_t s_hold_key;
static int64_t s_hold_start_us;
static int64_t s_hold_sent_us;

static uint8_t s_prev_keys[HID_KEYBOARD_KEY_MAX];
static uint8_t s_prev_modifiers;
static void post_gesture_event(d100h_input_source_t source, key_event_kind_t kind,
                                uint8_t id, bool down);

#define ULANZI_VID 0xFFF1
#define ULANZI_TOP_RIGHT_ID 0x03
static hid_host_device_handle_t s_ulanzi_handle;
static SemaphoreHandle_t s_ulanzi_lock;
static uint8_t s_ulanzi_addr;
static int s_ulanzi_report_count;

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
    bool new_input = (report->modifier.val & ~s_prev_modifiers) != 0;
    bool mapped = false;

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
        new_input = true;

        ui_key_t key;
        if(hid_key_to_ui(code, &key)) {
            mapped = true;
            usb_hid_input_post_key(key);
        }
    }

    /* Unmapped keys and modifier-only presses still count as user activity.
     * Do not add a separate wake event before an already mapped key. */
    if(new_input && !mapped)
        post_gesture_event(D100H_INPUT_USB, KEY_EVENT_ACTIVITY, 0, true);

    memcpy(s_prev_keys, report->key, sizeof(s_prev_keys));
    s_prev_modifiers = report->modifier.val;
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
                    if(id == 0x01) {
                        usb_hid_input_post_back(D100H_INPUT_USB, down);
                    }
                    else if(id == 0x02) {
                        usb_hid_input_post_play(D100H_INPUT_USB, down);
                    }
                    else if(id == ULANZI_TOP_RIGHT_ID) {
                        usb_hid_input_post_top_right(D100H_INPUT_USB, down);
                    }
                    else if(id == 0x07 || id == 0x0B || id == 0x0C) {
                        usb_hid_input_post_knob(D100H_INPUT_USB, id, down);
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
            /* The D100H vendor stream owns press/release timing. Its
             * auxiliary keyboard interface must not also act on key-down. */
            if(s_ulanzi_addr != 0 && params.addr == s_ulanzi_addr) break;
            if(params.proto == HID_PROTOCOL_KEYBOARD &&
               params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
                keyboard_report(data, (int)length);
            }
            break;
        }
        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            if(handle_is_ulanzi(handle)) {
                set_ulanzi_handle(NULL);
                s_ulanzi_addr = 0;
                usb_hid_input_cancel_gestures(D100H_INPUT_USB);
                ESP_LOGI(TAG, "Ulanzi D100H disconnected");
            }
            else {
                ESP_LOGI(TAG, "HID device disconnected");
            }
            memset(s_prev_keys, 0, sizeof(s_prev_keys));
            s_prev_modifiers = 0;
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
        s_prev_modifiers = 0;
        if(hid_host_device_start(evt.handle) != ESP_OK) {
            ESP_LOGE(TAG, "hid_host_device_start failed");
            continue;
        }
        if(is_ulanzi) {
            set_ulanzi_handle(evt.handle);
            s_ulanzi_addr = params.addr;
            s_ulanzi_report_count = 0;
            usb_hid_input_cancel_gestures(D100H_INPUT_USB);
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

static uint16_t gesture_button_bit(const key_event_t * evt)
{
    if(evt->kind == KEY_EVENT_BACK || evt->kind == KEY_EVENT_PLAY ||
       evt->kind == KEY_EVENT_TOP_RIGHT || (evt->kind == KEY_EVENT_KNOB && evt->id == 0x07))
        return (uint16_t)(1u << evt->id);
    return 0;
}

static void dispatch_play(d100h_input_source_t source,
                          ulanzi_play_event_t event, int64_t now_us)
{
    ulanzi_play_action_t action = ulanzi_play_update(&s_play[source], event, now_us);
    if(action == ULANZI_PLAY_NONE) return;
    ESP_LOGI(TAG, "D100H play source=%d action=%s", (int)source,
             action == ULANZI_PLAY_SCREEN_OFF ? "screen_off" : "play_pause");
    if(bsp_display_lock(1000)) {
        if(action == ULANZI_PLAY_SCREEN_OFF) {
            ui_play_key_feedback(UI_KEY_7);
            ui_screen_off();
        }
        else if(ui_screen_is_on()) ui_handle_key(UI_KEY_7);
        bsp_display_unlock();
    }
}

static void dispatch_top_right(d100h_input_source_t source,
                               ulanzi_top_right_event_t event, int64_t now_us)
{
    ulanzi_top_right_t * state = &s_top_right[source];
    ulanzi_top_right_action_t action =
        ulanzi_top_right_update(state, event, now_us);
    if(action == ULANZI_TOP_RIGHT_NONE) return;
    ESP_LOGI(TAG, "D100H top-right source=%d held=%lldms action=%s down=%d",
             (int)source, (long long)((now_us - state->pressed_us) / 1000),
             action == ULANZI_TOP_RIGHT_STAR ? "star" : "context", state->down);
    if(bsp_display_lock(1000)) {
        ui_handle_key(action == ULANZI_TOP_RIGHT_STAR
                      ? UI_KEY_STAR : UI_KEY_CONTEXT);
        bsp_display_unlock();
    }
}

static void dispatch_key_event(const key_event_t * evt)
{
    uint16_t bit = gesture_button_bit(evt);
    if(bit) {
        if(evt->down) {
            /* Repeated down reports from the held Play/Pause key must not wake
             * the screen immediately after its long-press switched it off. */
            if(s_button_down[evt->source] & bit) return;
            s_button_down[evt->source] |= bit;
        }
        else {
            s_button_down[evt->source] &= ~bit;
            if(s_wake_consumed[evt->source] & bit) {
                s_wake_consumed[evt->source] &= ~bit;
                return;
            }
        }
    }
    /* Record physical presses before gestures defer their logical action.
     * Consuming a wake press also cancels its later click/long-press action. */
    if(evt->kind != KEY_EVENT_GESTURE_CANCEL && evt->down && bsp_display_lock(1000)) {
        bool woke = ui_user_activity();
        bsp_display_unlock();
        if(woke) {
            /* Wake is global: another keyboard or transport can wake while
             * a vendor button is still held. Consume those releases too. */
            for(int i = 0; i < D100H_INPUT_COUNT; i++) {
                s_wake_consumed[i] |= s_button_down[i];
                ulanzi_knob_cancel(&s_knob[i]);
                ulanzi_back_cancel(&s_back[i]);
                dispatch_play((d100h_input_source_t)i, ULANZI_PLAY_CANCEL, evt->time_us);
                dispatch_top_right((d100h_input_source_t)i, ULANZI_TOP_RIGHT_CANCEL, evt->time_us);
            }
            return;
        }
    }
    if(evt->kind == KEY_EVENT_ACTIVITY) return;
    if(evt->kind == KEY_EVENT_PLAY) {
        dispatch_play(evt->source, evt->down ? ULANZI_PLAY_DOWN : ULANZI_PLAY_UP, evt->time_us);
        return;
    }
    if(evt->kind == KEY_EVENT_BACK) {
        if(ulanzi_back_update(&s_back[evt->source], evt->down) && bsp_display_lock(1000)) {
            if(ui_screen_is_on()) {
                ESP_LOGI(TAG, "D100H back source=%d action=back", (int)evt->source);
                ui_handle_key(UI_KEY_BACK);
            }
            bsp_display_unlock();
        }
        return;
    }
    if(evt->kind == KEY_EVENT_KNOB) {
        ui_key_t key;
        if(ulanzi_knob_update(&s_knob[evt->source], evt->id, evt->down, &key) &&
           bsp_display_lock(1000)) {
            ESP_LOGI(TAG, "D100H knob source=%d action=%s", (int)evt->source,
                key == UI_KEY_VOLUME_UP ? "volume_up" :
                key == UI_KEY_VOLUME_DOWN ? "volume_down" : "enter");
            ui_handle_key(key);
            bsp_display_unlock();
        }
        return;
    }
    if(evt->kind != KEY_EVENT_KEY) {
        if(evt->kind == KEY_EVENT_GESTURE_CANCEL) {
            s_button_down[evt->source] = s_wake_consumed[evt->source] = 0;
            ulanzi_knob_cancel(&s_knob[evt->source]);
            ulanzi_back_cancel(&s_back[evt->source]);
            dispatch_play(evt->source, ULANZI_PLAY_CANCEL, evt->time_us);
        }
        dispatch_top_right(evt->source,
            evt->kind == KEY_EVENT_GESTURE_CANCEL ? ULANZI_TOP_RIGHT_CANCEL
                : evt->down ? ULANZI_TOP_RIGHT_DOWN : ULANZI_TOP_RIGHT_UP,
            evt->time_us);
        return;
    }
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
        int64_t last_arrival_us = evt->time_us;
        for(int i = 0; i < 63; i++) {
            key_event_t next;
            if(xQueuePeek(s_key_events, &next, 0) != pdTRUE) break;
            if(next.kind != KEY_EVENT_KEY || !next.down) break;
            if(next.key != key || next.source != evt->source) break;
            xQueueReceive(s_key_events, &next, 0);
            delta += next.key == UI_KEY_UP ? -1 : 1;
            last_arrival_us = next.time_us;
        }
        if(delta != 0 && bsp_display_lock(1000)) {
            uint32_t tick = lv_tick_get();
            int64_t now_us = esp_timer_get_time();
            ui_handle_key_repeat_timed(key, delta < 0 ? -delta : delta,
                tick - (uint32_t)((now_us - evt->time_us) / 1000),
                tick - (uint32_t)((now_us - last_arrival_us) / 1000));
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
        int64_t deadline = 0;
        if(s_hold_pending) {
            deadline = s_hold_sent
                ? s_hold_sent_us + (int64_t)HOLD_MAX_MS * 1000
                : s_hold_start_us + (int64_t)HOLD_MS * 1000;
        }
        for(int i = 0; i < D100H_INPUT_COUNT; i++) {
            int64_t back_deadline = ulanzi_play_deadline(&s_play[i]);
            if(back_deadline && (!deadline || back_deadline < deadline)) deadline = back_deadline;
            int64_t top_right_deadline = ulanzi_top_right_deadline(&s_top_right[i]);
            if(top_right_deadline && (!deadline || top_right_deadline < deadline)) {
                deadline = top_right_deadline;
            }
        }
        if(deadline) {
            int64_t remain_us = deadline - esp_timer_get_time();
            wait = remain_us <= 0
                       ? 0
                       : pdMS_TO_TICKS((remain_us + 999) / 1000);
            if(remain_us > 0 && wait == 0) wait = 1;
        }

        key_event_t evt;
        if(xQueueReceive(s_key_events, &evt, wait) == pdTRUE) {
            dispatch_key_event(&evt);
        }

        /* Check timers even during continuous input. Use the next queued
         * event's arrival time so a queued short release/cancel wins over
         * a timeout if the UI was busy when that event arrived. */
        int64_t now_us = esp_timer_get_time();
        key_event_t next;
        int64_t gesture_now = xQueuePeek(s_key_events, &next, 0) == pdTRUE
            ? next.time_us : now_us;
        for(int i = 0; i < D100H_INPUT_COUNT; i++) {
            dispatch_play((d100h_input_source_t)i, ULANZI_PLAY_POLL, gesture_now);
            dispatch_top_right((d100h_input_source_t)i,
                               ULANZI_TOP_RIGHT_POLL, gesture_now);
        }

        if(!s_hold_pending) continue;
        int64_t hold_deadline = s_hold_sent
            ? s_hold_sent_us + (int64_t)HOLD_MAX_MS * 1000
            : s_hold_start_us + (int64_t)HOLD_MS * 1000;
        if(now_us < hold_deadline) continue;
        if(!s_hold_sent) {
            if(bsp_display_lock(1000)) {
                ui_play_key_feedback(s_hold_key);
                bsp_display_unlock();
            }
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
        key_event_t evt = { .key = key, .down = down,
                            .time_us = esp_timer_get_time() };
        xQueueSend(s_key_events, &evt, 0);
    }
}

static void post_gesture_event(d100h_input_source_t source, key_event_kind_t kind,
                                uint8_t id, bool down)
{
    if((unsigned)source >= D100H_INPUT_COUNT || s_key_events == NULL) return;
    key_event_t evt = {
        .kind = kind,
        .source = source,
        .id = id,
        .down = down,
        .time_us = esp_timer_get_time(),
    };
    if(xQueueSend(s_key_events, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "D100H gesture input queue full");
    }
}

void usb_hid_input_post_back(d100h_input_source_t source, bool down)
{
    post_gesture_event(source, KEY_EVENT_BACK, 0x01, down);
}

void usb_hid_input_post_play(d100h_input_source_t source, bool down)
{
    post_gesture_event(source, KEY_EVENT_PLAY, 0x02, down);
}

void usb_hid_input_post_top_right(d100h_input_source_t source, bool down)
{
    post_gesture_event(source, KEY_EVENT_TOP_RIGHT, ULANZI_TOP_RIGHT_ID, down);
}

void usb_hid_input_post_knob(d100h_input_source_t source, uint8_t id, bool down)
{
    if(id != 0x07 && id != 0x0B && id != 0x0C) return;
    post_gesture_event(source, KEY_EVENT_KNOB, id, down);
}

void usb_hid_input_cancel_gestures(d100h_input_source_t source)
{
    post_gesture_event(source, KEY_EVENT_GESTURE_CANCEL, 0, false);
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
