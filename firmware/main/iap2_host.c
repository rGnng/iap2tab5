#include "iap2_host.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "app_config.h"
#include "iap2_auth.h"
#include "lockdown_client.h"
#include "player.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

static const char * TAG = "iap2";

static uint64_t be64(const uint8_t * p);

#define APPLE_VID        0x05AC
#define IPOD_PID         0x12AA
#define IPHONE_PID       0x12A8
#define IAP2_AVAILABLE_CURRENT_MA 500

#define LINK_START       0xFF5A
#define CONTROL_SYN      0x80
#define CONTROL_ACK      0x40
#define CONTROL_EAK      0x20
#define CONTROL_RST      0x10

#define CONTROL_SESSION  10
#define EA_SESSION       11
#define FT_SESSION       12

#define IAP2_MARKER_LEN  6
static const uint8_t IAP2_MARKER[IAP2_MARKER_LEN] = {
    0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10,
};

#define IN_XFER_SIZE     (64)
#define RX_STREAM_SIZE   (128 * 1024)
#define LINK_BUF_SIZE    (256 * 1024)
#define CSM_BUF_SIZE     (192 * 1024)
#define TX_QUEUE_LEN     8
#define MAX_CSM_FRAME    2048
#define LINK_MAX_PACKET 4096
#define SESSION_QUEUE_LEN 32
#define RX_PACKET_BUDGET 16
#define RX_TIME_BUDGET_US 4000

#define OUT_REPORT_BASE  13

static const uint16_t s_out_reports[] = {
    5, 9, 13, 17, 25, 49, 95, 193, 255,
};

static const uint8_t HID_REPORT_DESCRIPTOR[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x0C,
    0x09, 0xB0, 0x09, 0xB1, 0x09, 0xB5, 0x09, 0xB6,
    0x09, 0xB9, 0x09, 0xBC, 0x09, 0xE9, 0x09, 0xEA,
    0x09, 0xE2, 0x09, 0xB7, 0x09, 0xB3, 0x09, 0xB4,
    0x81, 0x02, 0xC0,
};

static const struct {
    const char * name;
    uint8_t bit;
} HID_BITS[] = {
    { "play", 0 },       { "pause", 1 },      { "next", 2 },
    { "previous", 3 },   { "shuffle", 4 },    { "repeat", 5 },
    { "volume_up", 6 },  { "volume_down", 7 },{ "mute", 8 },
    { "stop", 9 },       { "fast_forward", 10 }, { "rewind", 11 },
};

enum link_state {
    LINK_IDLE = 0,
    LINK_DETECT,
    LINK_NEGOTIATE,
    LINK_NORMAL,
};

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_interface;
static uint8_t s_ep_in;
static usb_transfer_t * s_in_xfer;
static usb_transfer_t * s_ctrl_xfer;
static SemaphoreHandle_t s_ctrl_done;
static SemaphoreHandle_t s_ctrl_lock;
static bool s_ctrl_inflight;
static StreamBufferHandle_t s_rx_stream;
static TaskHandle_t s_task;

static volatile bool s_connected;
static volatile enum link_state s_state = LINK_IDLE;
static uint8_t s_sent_psn;
static uint8_t s_last_recv_psn;
static uint16_t s_peer_max_len = 65535;
static uint16_t s_peer_ack_timeout = 500;
static uint16_t s_peer_retransmit = 4000;
static TickType_t s_next_marker;
static TickType_t s_next_syn;
static TickType_t s_last_rx;

static uint8_t * s_link_buf;
static size_t s_link_len;
static _Atomic uint32_t s_link_generation;
static volatile bool s_reset_pending;
static volatile bool s_rx_overflow;
static bool s_rx_more;
static bool s_partial_waiting;
static TickType_t s_partial_since;
/* HID framing belongs to the USB event task; reset only on USB reopen. */
static size_t s_report_remaining;
static iap2_link_stats_t s_stats;

typedef struct {
    uint32_t generation;
    uint16_t len;
    uint8_t session;
    uint8_t payload[];
} session_packet_t;
static QueueHandle_t s_session_queue;
static SemaphoreHandle_t s_session_lock;
static void ft_clear_all(void);
static void session_task(void * arg);

static uint8_t * s_csm_buf;
static size_t s_csm_len;

static SemaphoreHandle_t s_tx_lock;
static uint8_t * s_txq[TX_QUEUE_LEN];
static size_t s_txq_len[TX_QUEUE_LEN];
static uint8_t s_txq_session[TX_QUEUE_LEN];
static int s_txq_head;
static int s_txq_tail;

static uint8_t * s_tx_active;
static size_t s_tx_active_len;
static uint8_t s_tx_active_session;
static uint8_t s_tx_seq;
static TickType_t s_tx_deadline;
static int s_tx_retries;
static bool s_tx_busy;
static size_t s_tx_offset;
static size_t s_tx_packet_len;

static bool s_enabled;
static bool s_hid_ready;
static bool s_identified;
static bool s_auth_ok;
static bool s_playing;
static char s_status[96] = "waiting for iPod";
static SemaphoreHandle_t s_state_lock;
static char s_title[128];
static char s_artist[128];
static char s_album[128];
static uint64_t s_track_key;
static char s_library_id[128];
static char s_library_sub_id[128];
static char s_library_revision[128];
static bool s_library_subscribed;
static bool s_library_resynced;
static int s_library_items;
static char s_device_name[64] = "iPod touch";
static uint32_t s_elapsed_ms;
static uint32_t s_duration_ms;
static int s_queue_index = -1;
#define QUEUE_LIST_MAX 512
/* The iPod's own queue, read from the PlaybackQueueList file transfer
 * (0x0003 PID array or 0x0006 MediaItemList).  Guarded by s_state_lock. */
static uint64_t s_queue_list[QUEUE_LIST_MAX];
static int s_queue_list_count;
static int s_queue_list_base;
static uint32_t s_queue_list_rev;
static int s_queue_count;
static bool s_shuffle;
static uint8_t s_repeat;
static int s_battery = -1;
static bool s_charging;
static int s_max_current_ma = -1;
static bool s_device_will_charge;
static uint8_t s_accessory_power_mode;
static bool s_external_charger;
static uint8_t s_charging_state;
static uint8_t s_play_status;
static uint32_t s_revision;

/* iAP2 File Transfer artwork channel. The device pushes the current cover as
 * a file over session 12; a worker decodes it into a 360x360 RGB565 buffer
 * that the UI can copy without blocking the link task. */
#define ART_SIZE          IAP2_ARTWORK_SIZE
#define ART_BYTES         (ART_SIZE * ART_SIZE * 2)
#define FT_MAX_ARTWORK    (2 * 1024 * 1024)
#define FT_MAX_TRANSFERS  4

typedef struct {
    bool active;
    uint8_t id;
    uint16_t type;
    uint64_t expected;
    size_t len;
    size_t cap;
    uint8_t * data;
} ft_transfer_t;

static ft_transfer_t s_ft[FT_MAX_TRANSFERS];
static uint8_t * s_art_pending;
static size_t s_art_pending_len;
static char s_art_pending_album[128];
static char s_art_pending_artist[128];
static uint64_t s_art_pending_key;
static uint8_t * s_art_cover;
static bool s_art_valid;
static uint32_t s_art_rev;
static char s_art_cached_album[128];
static char s_art_cached_artist[128];
static uint64_t s_art_cached_key;
static SemaphoreHandle_t s_art_lock;
static SemaphoreHandle_t s_art_sem;
static TaskHandle_t s_art_task;

static void artwork_invalidate(void);
static void artwork_clear_cover(void);
static bool artwork_album_matches(const char * album);

static void set_status(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* USB HID transport                                                   */
/* ------------------------------------------------------------------ */

static volatile int s_ctrl_status;

static void ctrl_done_cb(usb_transfer_t * xfer)
{
    (void)xfer;
    s_ctrl_status = (int)xfer->status;
    xSemaphoreGive(s_ctrl_done);
}

/* A timed-out transfer still owns the shared DMA buffer until its callback
 * completes. Serialize all callers and never overwrite an in-flight request. */
static bool ctrl_acquire(void)
{
    xSemaphoreTake(s_ctrl_lock, portMAX_DELAY);
    if(s_ctrl_inflight) {
        if(xSemaphoreTake(s_ctrl_done, 0) != pdTRUE) {
            xSemaphoreGive(s_ctrl_lock);
            return false;
        }
        s_ctrl_inflight = false;
    }
    return true;
}

static esp_err_t ctrl_request_locked(uint8_t bmRequestType, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex,
                              const void * data, size_t len)
{
    if(s_dev == NULL || s_ctrl_xfer == NULL) return ESP_ERR_INVALID_STATE;
    if(len > 256) return ESP_ERR_INVALID_SIZE;

    usb_transfer_t * xfer = s_ctrl_xfer;

    usb_setup_packet_t * setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = (uint16_t)len;
    if(len > 0 && data != NULL) {
        memcpy(xfer->data_buffer + 8, data, len);
    }

    xfer->device_handle = s_dev;
    xfer->bEndpointAddress = 0;
    xfer->num_bytes = (int)(8 + len);
    xfer->timeout_ms = 1000;
    xfer->callback = ctrl_done_cb;
    xfer->context = NULL;

    xSemaphoreTake(s_ctrl_done, 0);
    s_ctrl_inflight = true;
    esp_err_t err = usb_host_transfer_submit_control(s_client, xfer);
    if(err != ESP_OK) {
        s_ctrl_inflight = false;
        return err;
    }
    if(xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "control transfer timeout (req 0x%02x)", bRequest);
        return ESP_ERR_TIMEOUT;
    }
    s_ctrl_inflight = false;
    if(s_ctrl_status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "control transfer req 0x%02x status %d", bRequest,
                 s_ctrl_status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ctrl_get_descriptor_locked(uint8_t type, uint8_t index,
                                     uint8_t * out, size_t len)
{
    if(s_dev == NULL || s_ctrl_xfer == NULL || len > 256) {
        return ESP_ERR_INVALID_STATE;
    }

    usb_transfer_t * xfer = s_ctrl_xfer;
    usb_setup_packet_t * setup = (usb_setup_packet_t *)xfer->data_buffer;
    setup->bmRequestType = 0x81;
    setup->bRequest = 0x06;
    setup->wValue = (uint16_t)((type << 8) | index);
    setup->wIndex = s_interface;
    setup->wLength = (uint16_t)len;

    xfer->device_handle = s_dev;
    xfer->bEndpointAddress = 0;
    xfer->num_bytes = (int)(8 + len);
    xfer->timeout_ms = 1000;
    xfer->callback = ctrl_done_cb;
    xfer->context = NULL;

    xSemaphoreTake(s_ctrl_done, 0);
    s_ctrl_inflight = true;
    esp_err_t err = usb_host_transfer_submit_control(s_client, xfer);
    if(err != ESP_OK) { s_ctrl_inflight = false; return err; }
    if(xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ctrl_inflight = false;
    if(s_ctrl_status != USB_TRANSFER_STATUS_COMPLETED) {
        return ESP_FAIL;
    }
    size_t got = xfer->actual_num_bytes > 8 ?
                 (size_t)xfer->actual_num_bytes - 8 : 0;
    if(got > len) got = len;
    memcpy(out, xfer->data_buffer + 8, got);
    return (esp_err_t)got;
}

static esp_err_t ctrl_request(uint8_t bmRequestType, uint8_t bRequest,
                              uint16_t wValue, uint16_t wIndex,
                              const void * data, size_t len)
{
    if(!ctrl_acquire()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ctrl_request_locked(bmRequestType, bRequest, wValue, wIndex, data, len);
    xSemaphoreGive(s_ctrl_lock);
    return err;
}

static esp_err_t ctrl_get_descriptor(uint8_t type, uint8_t index,
                                     uint8_t * out, size_t len)
{
    if(!ctrl_acquire()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ctrl_get_descriptor_locked(type, index, out, len);
    xSemaphoreGive(s_ctrl_lock);
    return err;
}

static int s_report_variant = 2;
static int s_report_try;

static esp_err_t hid_set_report(uint8_t report_id, const uint8_t * report,
                                size_t size)
{
    int v = s_report_variant >= 0 ? s_report_variant : (s_report_try++ & 7);
    bool exclude_id = (v & 1) != 0;
    bool iface_index = (v & 2) != 0;
    uint8_t type = (v & 4) ? 0x03 : 0x02;

    const uint8_t * data = exclude_id ? report + 1 : report;
    size_t len = exclude_id ? size : size + 1;

    esp_err_t err = ctrl_request(0x21, 0x09,
                                 (uint16_t)((type << 8) | report_id),
                                 iface_index ? s_interface : 0,
                                 data, len);
    if(err == ESP_OK) {
        if(s_report_variant != v) {
            ESP_LOGI(TAG, "HID report variant %d works (wIndex=%s type=%02x "
                     "id_in_data=%d)", v, iface_index ? "iface" : "0", type,
                     !exclude_id);
            s_report_variant = v;
        }
    }
    else {
        if(s_report_variant == v) s_report_variant = -1;
        ESP_LOGW(TAG, "HID report variant %d failed: %s", v,
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t hid_write_bytes(const uint8_t * data, size_t len)
{
    bool first = true;

    while(len > 0) {
        int idx = -1;
        size_t cap = 0;
        for(size_t i = 0; i < sizeof(s_out_reports) / sizeof(s_out_reports[0]);
            i++) {
            size_t c = s_out_reports[i] - 1;
            if(c >= len) {
                idx = (int)i;
                cap = c;
                break;
            }
        }
        if(idx < 0) {
            idx = (int)(sizeof(s_out_reports) / sizeof(s_out_reports[0])) - 1;
            cap = s_out_reports[idx] - 1;
        }

        size_t n = len < cap ? len : cap;
        uint8_t lcb = first ? 0 : 0x01;
        if(n < len) lcb |= 0x02;

        uint8_t report[260];
        report[0] = (uint8_t)(OUT_REPORT_BASE + idx);
        report[1] = lcb;
        memcpy(report + 2, data, n);
        memset(report + 2 + n, 0, cap - n);

        static int logged;
        if(logged < 8) {
            ESP_LOGI(TAG, "HID OUT id=%u lcb=0x%02x n=%u total=%u",
                     report[0], report[1], (unsigned)n,
                     (unsigned)(1 + s_out_reports[idx]));
            logged++;
        }

        esp_err_t err = hid_set_report(report[0], report,
                                       s_out_reports[idx]);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "SET_REPORT id=%u failed: %s", report[0],
                     esp_err_to_name(err));
            return err;
        }

        data += n;
        len -= n;
        first = false;
    }
    return ESP_OK;
}

static const uint16_t s_in_report_sizes[13] = {
    0, 5, 9, 13, 17, 25, 49, 95, 193, 257, 385, 513, 767,
};

static size_t input_report_total(uint8_t id)
{
    if(id == 0 || id > 12) return 0;
    size_t total = (size_t)s_in_report_sizes[id] + 1;
    return total;
}

/* Single producer: never admit a partial USB fragment. A full stream is a
 * transport error, not permission to splice the next bytes into a bad frame. */
static void rx_store(const uint8_t * data, size_t len)
{
    if(s_rx_overflow || len == 0) return;
    if(xStreamBufferSpacesAvailable(s_rx_stream) < len ||
       xStreamBufferSend(s_rx_stream, data, len, 0) != len) {
        s_stats.rx_overflows++;
        s_rx_overflow = true;
    }
    if(s_task != NULL) xTaskNotifyGive(s_task);
}

static void in_done_cb(usb_transfer_t * xfer)
{
    if(xfer != s_in_xfer) {
        /* A canceled transfer from a removed device may complete after a
         * replacement device has already opened its own IN transfer. */
        usb_host_transfer_free(xfer);
        return;
    }
    if(xfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        size_t actual = (size_t)xfer->actual_num_bytes;
        const uint8_t * data = xfer->data_buffer;
        if(actual > 0) {
            if(s_report_remaining == 0) {
                size_t total = actual >= 2 ? input_report_total(data[0]) : 0;
                if(total < 2 || actual > total || (data[1] & ~3u) != 0) {
                    s_stats.hid_errors++;
                    s_rx_overflow = true;
                }
                else {
                    s_report_remaining = total - actual;
                    rx_store(data + 2, actual - 2);
                }
            }
            else if(actual <= s_report_remaining) {
                rx_store(data, actual);
                s_report_remaining -= actual;
            }
            else {
                s_stats.hid_errors++;
                s_rx_overflow = true;
                s_report_remaining = 0;
            }
            /* A short USB packet terminates the report. The 258/386/514-byte
             * reports really do end in a 2-byte transfer; retain those bytes. */
            if(actual < IN_XFER_SIZE && s_report_remaining != 0) {
                s_stats.hid_errors++;
                s_rx_overflow = true;
                s_report_remaining = 0;
            }
        }
        if(actual == 0 && s_report_remaining != 0) {
            s_stats.hid_errors++;
            s_rx_overflow = true;
            s_report_remaining = 0;
        }
        xfer->num_bytes = IN_XFER_SIZE;
        if(usb_host_transfer_submit(xfer) != ESP_OK) {
            ESP_LOGW(TAG, "HID IN resubmit failed");
            if(s_in_xfer == xfer) s_in_xfer = NULL;
            usb_host_transfer_free(xfer);
        }
    }
    else {
        ESP_LOGW(TAG, "HID IN stopped: %d", (int)xfer->status);
        s_report_remaining = 0;
        s_reset_pending = true;
        if(s_in_xfer == xfer) s_in_xfer = NULL;
        usb_host_transfer_free(xfer);
    }
}

static void reset_link(void)
{
    /* Only the link task resets transport state. The worker cannot apply an
     * old session packet after this generation changes. */
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_link_generation++;
    s_stats.link_resets++;
    session_packet_t * queued;
    while(xQueueReceive(s_session_queue, &queued, 0) == pdTRUE) free(queued);
    ft_clear_all();
    s_state = LINK_IDLE;
    s_sent_psn = 99;
    s_last_recv_psn = 0;
    s_link_len = 0;
    s_partial_waiting = false;
    s_csm_len = 0;
    s_hid_ready = false;
    s_identified = false;
    s_auth_ok = false;
    s_playing = false;
    s_tx_busy = false;
    s_tx_retries = 0;
    s_tx_offset = s_tx_packet_len = 0;
    s_peer_max_len = 65535;
    s_peer_retransmit = 4000;
    s_title[0] = s_artist[0] = s_album[0] = '\0';
    s_track_key = 0;
    s_library_id[0] = '\0';
    s_library_sub_id[0] = '\0';
    s_library_revision[0] = '\0';
    s_library_subscribed = false;
    s_library_resynced = false;
    s_library_items = 0;
    s_stats.library_complete = false;
    if(s_tx_lock != NULL) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    while(s_txq_tail != s_txq_head) {
        free(s_txq[s_txq_tail]);
        s_txq[s_txq_tail] = NULL;
        s_txq_tail = (s_txq_tail + 1) % TX_QUEUE_LEN;
    }
    free(s_tx_active);
    s_tx_active = NULL;
    s_tx_active_len = 0;
    if(s_tx_lock != NULL) xSemaphoreGive(s_tx_lock);
    artwork_invalidate();
    xSemaphoreGive(s_session_lock);
}

static void open_device(uint8_t address)
{
    if(s_dev != NULL) return;

    usb_device_handle_t dev = NULL;
    esp_err_t err = usb_host_device_open(s_client, address, &dev);
    if(err != ESP_OK) return;

    const usb_device_desc_t * desc = NULL;
    err = usb_host_get_device_descriptor(dev, &desc);
    if(err != ESP_OK || desc == NULL || desc->idVendor != APPLE_VID ||
       (desc->idProduct != IPOD_PID && desc->idProduct != IPHONE_PID)) {
        usb_host_device_close(s_client, dev);
        return;
    }

    const usb_config_desc_t * cfg = NULL;
    err = usb_host_get_active_config_descriptor(dev, &cfg);
    if(err != ESP_OK || cfg == NULL) {
        usb_host_device_close(s_client, dev);
        return;
    }

    const usb_intf_desc_t * intf = NULL;
    int intf_offset = 0;
    for(uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        int offset = 0;
        const usb_intf_desc_t * candidate =
            usb_parse_interface_descriptor(cfg, i, 0, &offset);
        if(candidate == NULL) continue;
        if(candidate->bInterfaceClass == 0x03) {
            intf = candidate;
            intf_offset = offset;
            break;
        }
    }

    if(intf == NULL) {
        ESP_LOGW(TAG, "no HID interface in config %u",
                 (unsigned)cfg->bConfigurationValue);
        usb_host_device_close(s_client, dev);
        return;
    }

    err = usb_host_interface_claim(s_client, dev, intf->bInterfaceNumber, 0);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "HID interface claim failed: %s", esp_err_to_name(err));
        usb_host_device_close(s_client, dev);
        return;
    }

    uint8_t ep_in = 0;
    for(int i = 0; i < intf->bNumEndpoints; i++) {
        int offset = intf_offset;
        const usb_ep_desc_t * ep = usb_parse_endpoint_descriptor_by_index(
            intf, i, cfg->wTotalLength, &offset);
        if(ep == NULL) continue;
        if((ep->bmAttributes & 0x03) == 0x03 &&
           (ep->bEndpointAddress & 0x80)) {
            ep_in = ep->bEndpointAddress;
            break;
        }
    }

    if(ep_in == 0) {
        ESP_LOGW(TAG, "HID interrupt IN endpoint not found");
        usb_host_interface_release(s_client, dev, intf->bInterfaceNumber);
        usb_host_device_close(s_client, dev);
        return;
    }

    s_dev = dev;
    s_interface = intf->bInterfaceNumber;
    s_ep_in = ep_in;

    ESP_LOGI(TAG, "iPod config %u HID if=%u ep_in=0x%02x",
             (unsigned)cfg->bConfigurationValue, s_interface, s_ep_in);

    err = usb_host_transfer_alloc(IN_XFER_SIZE, 0, &s_in_xfer);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "IN transfer alloc failed: %s", esp_err_to_name(err));
        usb_host_interface_release(s_client, dev, s_interface);
        usb_host_device_close(s_client, dev);
        s_dev = NULL;
        return;
    }
    s_report_remaining = 0;
    s_in_xfer->device_handle = s_dev;
    s_in_xfer->bEndpointAddress = s_ep_in;
    s_in_xfer->num_bytes = IN_XFER_SIZE;
    s_in_xfer->callback = in_done_cb;
    s_in_xfer->context = NULL;
    err = usb_host_transfer_submit(s_in_xfer);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "IN submit failed: %s", esp_err_to_name(err));
        usb_host_transfer_free(s_in_xfer);
        s_in_xfer = NULL;
        usb_host_interface_release(s_client, dev, s_interface);
        usb_host_device_close(s_client, dev);
        s_dev = NULL;
        return;
    }

    s_reset_pending = true;
    s_connected = true;
    set_status("iPod HID connected, detecting iAP2");
    ESP_LOGI(TAG, "HID ready; sending iAP2 detection marker");
}

static void close_device(void)
{
    s_connected = false;
    s_identified = false;
    s_hid_ready = false;
    s_state = LINK_IDLE;
    s_reset_pending = true;
    s_in_xfer = NULL;
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    s_link_generation++;
    if(s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        s_play_status = 0;
        s_playing = false;
        s_title[0] = s_artist[0] = s_album[0] = '\0';
        s_track_key = 0;
        s_library_id[0] = '\0';
        s_library_sub_id[0] = '\0';
        s_library_revision[0] = '\0';
        s_library_subscribed = false;
        s_library_resynced = false;
        s_library_items = 0;
        s_elapsed_ms = s_duration_ms = 0;
        s_queue_index = -1;
        s_queue_count = 0;
        s_queue_list_count = 0;
        s_queue_list_base = 0;
        s_battery = -1;
        s_charging = false;
        s_max_current_ma = -1;
        s_device_will_charge = false;
        s_accessory_power_mode = 0;
        s_external_charger = false;
        s_charging_state = 0;
        s_revision++;
        xSemaphoreGive(s_state_lock);
    }
    artwork_invalidate();
    xSemaphoreGive(s_session_lock);
    if(s_dev != NULL) {
        usb_host_interface_release(s_client, s_dev, s_interface);
        usb_host_device_close(s_client, s_dev);
        s_dev = NULL;
    }
    set_status("iPod disconnected");
}

static void client_event_cb(const usb_host_client_event_msg_t * msg, void * arg)
{
    (void)arg;
    if(msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        open_device(msg->new_dev.address);
    }
    else if(msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if(msg->dev_gone.dev_hdl == s_dev) close_device();
    }
}

/* ------------------------------------------------------------------ */
/* Link layer                                                          */
/* ------------------------------------------------------------------ */

static uint8_t checksum(const uint8_t * data, size_t len)
{
    uint8_t sum = 0;
    for(size_t i = 0; i < len; i++) sum = (uint8_t)(sum + data[i]);
    return (uint8_t)(0 - sum);
}

static esp_err_t link_send(uint8_t control, uint8_t seq, uint8_t ack,
                           uint8_t session, const uint8_t * payload,
                           size_t payload_len)
{
    size_t total = 9 + payload_len + (payload_len ? 1 : 0);
    uint8_t * pkt = malloc(total);
    if(pkt == NULL) return ESP_ERR_NO_MEM;

    uint16_t length = (uint16_t)(payload_len ? payload_len + 10 : 9);
    pkt[0] = (uint8_t)(LINK_START >> 8);
    pkt[1] = (uint8_t)LINK_START;
    pkt[2] = (uint8_t)(length >> 8);
    pkt[3] = (uint8_t)length;
    pkt[4] = control;
    pkt[5] = seq;
    pkt[6] = ack;
    pkt[7] = session;
    pkt[8] = checksum(pkt, 8);

    if(payload_len > 0) {
        memcpy(pkt + 9, payload, payload_len);
        pkt[9 + payload_len] = checksum(payload, payload_len);
    }

    esp_err_t err = hid_write_bytes(pkt, total);
    free(pkt);
    return err;
}

static void send_ack(void)
{
    link_send(CONTROL_ACK, s_sent_psn, s_last_recv_psn, 0, NULL, 0);
}

static void send_marker(void)
{
    hid_write_bytes(IAP2_MARKER, IAP2_MARKER_LEN);
}

static void build_lsp(uint8_t * buf, size_t * len)
{
    buf[0] = 0x01;
    /* Conservative stop-and-wait window; ACK every accepted packet. */
    buf[1] = 1;
    buf[2] = (uint8_t)(LINK_MAX_PACKET >> 8);
    buf[3] = (uint8_t)LINK_MAX_PACKET;
    buf[4] = (uint8_t)(4000 >> 8);
    buf[5] = (uint8_t)4000;
    buf[6] = (uint8_t)(500 >> 8);
    buf[7] = (uint8_t)500;
    buf[8] = 4;
    buf[9] = 1;
    buf[10] = CONTROL_SESSION;
    buf[11] = 0;
    buf[12] = 1;
    buf[13] = EA_SESSION;
    buf[14] = 2;
    buf[15] = 1;
    buf[16] = FT_SESSION;
    buf[17] = 1;
    buf[18] = 2;
    *len = 19;
}

static bool parse_lsp(const uint8_t * p, size_t len)
{
    if(len < 10 || p[0] != 1 || p[1] == 0) return false;
    uint16_t max_len = (uint16_t)((p[2] << 8) | p[3]);
    uint16_t retransmit = (uint16_t)((p[4] << 8) | p[5]);
    if(max_len <= 10 || retransmit == 0) return false;
    s_peer_max_len = max_len;
    s_peer_retransmit = retransmit;
    s_peer_ack_timeout = (uint16_t)((p[6] << 8) | p[7]);
    ESP_LOGI(TAG, "peer LSP max_len=%u ack_timeout=%u retransmit=%u",
             s_peer_max_len, s_peer_ack_timeout, s_peer_retransmit);
    return true;
}

/* ------------------------------------------------------------------ */
/* CSM encoding                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t * p;
    size_t len;
    size_t cap;
} pb_t;

static void pb_raw(pb_t * b, const void * data, size_t len)
{
    if(b->len + len > b->cap) {
        ESP_LOGW(TAG, "CSM param buffer overflow");
        return;
    }
    if(len > 0) memcpy(b->p + b->len, data, len);
    b->len += len;
}

static void pb_u16be(pb_t * b, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    pb_raw(b, t, sizeof(t));
}

static void pb_param(pb_t * b, uint16_t pid, const void * data, size_t len)
{
    pb_u16be(b, (uint16_t)(len + 4));
    pb_u16be(b, pid);
    pb_raw(b, data, len);
}

static void pb_str(pb_t * b, uint16_t pid, const char * s)
{
    pb_param(b, pid, s, strlen(s) + 1);
}

static void pb_void(pb_t * b, uint16_t pid)
{
    pb_param(b, pid, NULL, 0);
}

static void pb_u8(pb_t * b, uint16_t pid, uint8_t v)
{
    pb_param(b, pid, &v, 1);
}

static void pb_u16(pb_t * b, uint16_t pid, uint16_t v)
{
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    pb_param(b, pid, t, sizeof(t));
}

static void pb_u32(pb_t * b, uint16_t pid, uint32_t v)
{
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    pb_param(b, pid, t, sizeof(t));
}

static void pb_u16_list(pb_t * b, uint16_t pid, const uint16_t * values,
                        size_t count)
{
    uint8_t t[64];
    size_t n = count * 2;
    if(n > sizeof(t)) n = sizeof(t);
    for(size_t i = 0; i < n / 2; i++) {
        t[i * 2] = (uint8_t)(values[i] >> 8);
        t[i * 2 + 1] = (uint8_t)values[i];
    }
    pb_param(b, pid, t, n);
}

static void tx_queue(uint8_t session, const uint8_t * data, size_t len)
{
    if(len == 0 || len > MAX_CSM_FRAME) return;
    uint8_t * copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if(copy == NULL) return;
    memcpy(copy, data, len);

    if(s_tx_lock != NULL) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    if((s_txq_head + 1) % TX_QUEUE_LEN == s_txq_tail) {
        ESP_LOGW(TAG, "tx queue full, dropping session %u", session);
        free(copy);
    }
    else {
        s_txq[s_txq_head] = copy;
        s_txq_len[s_txq_head] = len;
        s_txq_session[s_txq_head] = session;
        s_txq_head = (s_txq_head + 1) % TX_QUEUE_LEN;
    }
    if(s_tx_lock != NULL) xSemaphoreGive(s_tx_lock);
    if(s_task != NULL) xTaskNotifyGive(s_task);
}

static void csm_queue(const uint8_t * frame, size_t len)
{
    tx_queue(CONTROL_SESSION, frame, len);
}

static void csm_send(uint16_t msg_id, const pb_t * params)
{
    size_t total = 6 + params->len;
    if(total > MAX_CSM_FRAME) {
        ESP_LOGW(TAG, "CSM 0x%04x too large (%u)", msg_id, (unsigned)total);
        return;
    }
    uint8_t * frame = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if(frame == NULL) return;
    frame[0] = 0x40;
    frame[1] = 0x40;
    frame[2] = (uint8_t)(total >> 8);
    frame[3] = (uint8_t)total;
    frame[4] = (uint8_t)(msg_id >> 8);
    frame[5] = (uint8_t)msg_id;
    memcpy(frame + 6, params->p, params->len);
    csm_queue(frame, total);
    free(frame);
}

static bool next_param(const uint8_t ** p, const uint8_t * end,
                       uint16_t * pid, const uint8_t ** value,
                       size_t * value_len)
{
    if((size_t)(end - *p) < 4) return false;
    uint16_t plen = (uint16_t)(((*p)[0] << 8) | (*p)[1]);
    *pid = (uint16_t)(((*p)[2] << 8) | (*p)[3]);
    if(plen < 4 || *p + plen > end) return false;
    *value = *p + 4;
    *value_len = plen - 4;
    *p += plen;
    return true;
}

static void send_auth_certificate(void)
{
    size_t cert_len = 0;
    const uint8_t * cert = iap2_auth_certificate(&cert_len);
    uint8_t buf[700];
    pb_t b = { buf, 0, sizeof(buf) };
    pb_param(&b, 0, cert, cert_len);
    ESP_LOGI(TAG, "sending authentication certificate (%u bytes)",
             (unsigned)cert_len);
    csm_send(0xAA01, &b);
}

static void send_auth_response(const uint8_t * body, size_t len)
{
    const uint8_t * p = body;
    const uint8_t * end = body + len;
    uint16_t pid = 0;
    const uint8_t * value = NULL;
    size_t value_len = 0;
    uint8_t signature[64];

    while(next_param(&p, end, &pid, &value, &value_len)) {
        if(pid != 0) continue;
        ESP_LOGI(TAG, "authentication challenge (%u bytes)",
                 (unsigned)value_len);
        if(iap2_auth_sign(value, value_len, signature) != ESP_OK) {
            ESP_LOGE(TAG, "challenge signing failed");
            return;
        }
        uint8_t buf[80];
        pb_t b = { buf, 0, sizeof(buf) };
        pb_param(&b, 0, signature, sizeof(signature));
        csm_send(0xAA03, &b);
        ESP_LOGI(TAG, "sent authentication response");
        return;
    }
    ESP_LOGW(TAG, "authentication challenge missing param 0");
}

static void send_identification(void)
{
    uint8_t buf[768];
    pb_t b = { buf, 0, sizeof(buf) };

    pb_str(&b, 0, "MediaCtrl");
    pb_str(&b, 1, "MediaCtrl-1");
    pb_str(&b, 2, "MediaCtrl");
    pb_str(&b, 3, "MC-0001");
    pb_str(&b, 4, "0.1");
    pb_str(&b, 5, "1.0");

    static const uint16_t sent[] = {
        0xAA01, 0xAA03, 0x5000, 0x5002, 0x5003,
        0x4C00, 0x4C02, 0x4C03, 0x4C05, 0x4C06, 0x4C07, 0x4C08, 0x4C09,
        0x6800, 0x6802, 0x6803, 0xAE00, 0xAE02, 0xAE03,
    };
    static const uint16_t received[] = {
        0xAA00, 0xAA02, 0xAA04, 0xAA05,
        0x5001, 0x4C01, 0x4C04,
        0x6801, 0x6804,
        0x4E09, 0x4E0A, 0x4E0B, 0x4E0C,
        0xAE01,
    };
    pb_u16_list(&b, 6, sent, sizeof(sent) / sizeof(sent[0]));
    pb_u16_list(&b, 7, received, sizeof(received) / sizeof(received[0]));
    /* PowerProvidingCapability: Advanced (2). The Tab5 is a USB host and
     * provides VBUS to the iPod; it does not draw Accessory Power. The
     * previous None/20 values made the iPod think it was the power source
     * (PowerUpdate AccessoryPowerMode=Low Power) and refuse to charge. */
    pb_u8(&b, 8, 2);
    pb_u16(&b, 9, 0);
    pb_str(&b, 12, "en");
    pb_param(&b, 13, "en\0", 3);

    uint8_t group_buf[128];
    pb_t g = { group_buf, 0, sizeof(group_buf) };
    pb_u16(&g, 0, 0);
    pb_str(&g, 1, "USB Device");
    pb_void(&g, 2);
    pb_param(&b, 15, group_buf, g.len);

    g.len = 0;
    pb_u16(&g, 0, 0x14E9);
    pb_str(&g, 1, "MediaCtrl HID");
    pb_u8(&g, 2, 1);
    pb_param(&b, 0x12, group_buf, g.len);

    ESP_LOGI(TAG, "sending identification");
    csm_send(0x1D01, &b);
}

static void send_now_playing_request(void)
{
    uint8_t buf[512];
    pb_t b = { buf, 0, sizeof(buf) };
    uint8_t group_buf[512];
    pb_t g = { group_buf, 0, sizeof(group_buf) };

    /* Same supported property set a car requests. Asking for unsupported
     * IDs (15/17/20/25) makes the device omit fields, including artwork id 26. */
    static const uint16_t now_ids[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
        16, 18, 19, 21, 22, 23, 24, 26, 27,
    };
    for(size_t i = 0; i < sizeof(now_ids) / sizeof(now_ids[0]); i++) {
        pb_void(&g, now_ids[i]);
    }
    pb_param(&b, 0, group_buf, g.len);

    g.len = 0;
    for(uint16_t i = 0; i <= 16; i++) pb_void(&g, i);
    uint8_t zero[4] = { 0, 0, 0, 0 };
    pb_param(&g, 17, zero, sizeof(zero));
    pb_param(&b, 1, group_buf, g.len);

    g.len = 0;
    static const uint16_t ids[] = { 0, 1, 6, 12, 14, 16, 18 };
    for(size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        pb_void(&g, ids[i]);
    }
    pb_param(&b, 2, group_buf, g.len);

    csm_send(0x5000, &b);
}

static void send_library_request(void)
{
    uint8_t dummy = 0;
    pb_t b = { &dummy, 0, 1 };
    csm_send(0x4C00, &b);
}

static void send_library_updates_request(void)
{
    const char * sub_id = s_library_sub_id[0] != '\0'
                              ? s_library_sub_id : s_library_id;
    if(sub_id[0] == '\0' || s_library_subscribed) return;

    uint8_t buf[512];
    uint8_t group_buf[256];
    uint8_t playlist_buf[128];
    pb_t b = { buf, 0, sizeof(buf) };
    pb_t g = { group_buf, 0, sizeof(group_buf) };
    pb_t pl = { playlist_buf, 0, sizeof(playlist_buf) };

    pb_str(&b, 0, sub_id);
    pb_str(&b, 1, s_library_revision[0] != '\0' ? s_library_revision : "0");
    /* Match the property set used by iap2mediactrl: all MediaItem
     * properties (0-27) plus the MediaPlaylist property group (0-5) and
     * the update/resident/All Songs flags. */
    for(uint16_t i = 0; i <= 27; i++) pb_void(&g, i);
    pb_param(&b, 2, group_buf, g.len);
    for(uint16_t i = 0; i <= 5; i++) pb_void(&pl, i);
    pb_param(&b, 3, playlist_buf, pl.len);
    pb_void(&b, 4);
    pb_void(&b, 5);
    pb_void(&b, 6);
    csm_send(0x4C03, &b);
    s_library_subscribed = true;
    ESP_LOGI(TAG, "subscribed to media library updates: %s (rev=%s)",
             sub_id,
             s_library_revision[0] != '\0' ? s_library_revision : "0");
}

static void send_power_source_update(void)
{
    /* PowerSourceUpdate (0xAE03): tell the device how much current the
     * accessory can provide. Without this an iPod touch can keep
     * external_charger=0 / charging_state=0 even though USB-A VBUS is on.
     * Advertise a 500 mA USB budget. The Tab5's fixed ~1.3 A hardware
     * protection remains separate from this negotiated device limit. */
    uint8_t buf[16];
    pb_t b = { buf, 0, sizeof(buf) };
    pb_u16(&b, 0, IAP2_AVAILABLE_CURRENT_MA);
    /* DeviceBatteryShouldChargeIfPowerIsPresent must be true for the
     * device UI to show charging; the first PowerSourceUpdate after
     * identification must contain both parameters. */
    pb_u8(&b, 1, 1);
    csm_send(0xAE03, &b);
    ESP_LOGI(TAG, "Advertised iPod USB current budget: %u mA",
             (unsigned)IAP2_AVAILABLE_CURRENT_MA);
}

static void send_power_updates_request(void)
{
    uint8_t buf[64];
    pb_t b = { buf, 0, sizeof(buf) };
    /* Do not request AccessoryPowerMode (2): the spec says it must be
     * present if and only if the accessory draws power from the device.
     * The Tab5 provides power, so requesting it made the iPod report
     * AccessoryPowerMode=Low Power and treat the accessory as a load. */
    static const uint16_t ids[] = { 0, 1, 4, 5, 6 };
    for(size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        pb_void(&b, ids[i]);
    }
    csm_send(0xAE00, &b);
}

static void send_hid_start(void)
{
    uint8_t buf[256];
    pb_t b = { buf, 0, sizeof(buf) };
    pb_u16(&b, 0, 0x14E9);
    pb_u16(&b, 1, 0x18D1);
    pb_u16(&b, 2, 0x4E26);
    pb_param(&b, 4, HID_REPORT_DESCRIPTOR, sizeof(HID_REPORT_DESCRIPTOR));
    csm_send(0x6800, &b);
}

static void send_hid_report_raw(const uint8_t report[2])
{
    uint8_t buf[32];
    pb_t b = { buf, 0, sizeof(buf) };
    pb_u16(&b, 0, 0x14E9);
    pb_param(&b, 1, report, 2);
    csm_send(0x6802, &b);
}

static void send_hid_report(uint8_t bit)
{
    uint8_t press[2] = { 0, 0 };
    uint8_t release[2] = { 0, 0 };
    press[bit / 8] = (uint8_t)(1 << (bit % 8));

    /* The HID command is a tap: send the pressed report and then an
     * all-zero release.  Without the release the iPod sees the key as held;
     * next/previous then behave like scan/FF/REW instead of a track skip. */
    send_hid_report_raw(press);
    send_hid_report_raw(release);
    ESP_LOGI(TAG, "HID command bit %u press+release", bit);
}

static bool find_hid_bit(const char * name, uint8_t * bit)
{
    for(size_t i = 0; i < sizeof(HID_BITS) / sizeof(HID_BITS[0]); i++) {
        if(strcmp(HID_BITS[i].name, name) == 0) {
            *bit = HID_BITS[i].bit;
            return true;
        }
    }
    return false;
}

static uint32_t be32(const uint8_t * p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void parse_now_playing(const uint8_t * body, size_t len)
{
    char title[128];
    char artist[128];
    char album[128];
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    int queue_index;
    int queue_count;
    bool shuffle;
    uint8_t repeat;
    uint8_t status;

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    snprintf(title, sizeof(title), "%s", s_title);
    snprintf(artist, sizeof(artist), "%s", s_artist);
    snprintf(album, sizeof(album), "%s", s_album);
    elapsed_ms = s_elapsed_ms;
    duration_ms = s_duration_ms;
    queue_index = s_queue_index;
    queue_count = s_queue_count;
    shuffle = s_shuffle;
    repeat = s_repeat;
    status = s_play_status;
    xSemaphoreGive(s_state_lock);

    const uint8_t * p = body;
    const uint8_t * end = body + len;
    uint16_t pid = 0;
    const uint8_t * value = NULL;
    size_t value_len = 0;

    uint64_t track_key = s_track_key;
    while(next_param(&p, end, &pid, &value, &value_len)) {
        if(pid == 0) {
            const uint8_t * q = value;
            const uint8_t * qend = value + value_len;
            uint16_t cid = 0;
            const uint8_t * cv = NULL;
            size_t cvlen = 0;
            while(next_param(&q, qend, &cid, &cv, &cvlen)) {
                if(cid == 0 && cvlen >= 8) {
                    track_key = 0;
                    for(size_t i = 0; i < 8; i++) {
                        track_key = (track_key << 8) | cv[i];
                    }
                }
                else if(cid == 1) {
                    snprintf(title, sizeof(title), "%.*s", (int)cvlen,
                             (const char *)cv);
                }
                else if(cid == 6) {
                    snprintf(album, sizeof(album), "%.*s", (int)cvlen,
                             (const char *)cv);
                }
                else if(cid == 12) {
                    snprintf(artist, sizeof(artist), "%.*s", (int)cvlen,
                             (const char *)cv);
                }
                else if(cid == 4 && cvlen >= 4) {
                    duration_ms = be32(cv);
                }
            }
        }
        else if(pid == 1) {
            const uint8_t * q = value;
            const uint8_t * qend = value + value_len;
            uint16_t cid = 0;
            const uint8_t * cv = NULL;
            size_t cvlen = 0;
            while(next_param(&q, qend, &cid, &cv, &cvlen)) {
                if(cid == 0 && cvlen >= 1) {
                    status = cv[0];
                }
                else if(cid == 1 && cvlen >= 4) {
                    elapsed_ms = be32(cv);
                }
                else if(cid == 2 && cvlen >= 4) {
                    queue_index = (int)be32(cv);
                }
                else if(cid == 3 && cvlen >= 4) {
                    queue_count = (int)be32(cv);
                }
                else if(cid == 5 && cvlen >= 1) {
                    shuffle = cv[0] != 0;
                }
                else if(cid == 6 && cvlen >= 1) {
                    repeat = cv[0];
                }
            }
        }
    }

    if(album[0] != '\0' && !artwork_album_matches(album)) {
        /* The cached cover belongs to another album. Show the placeholder
         * until this album's artwork arrives. */
        artwork_clear_cover();
    }

    bool changed = false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if(strcmp(title, s_title) != 0) {
        snprintf(s_title, sizeof(s_title), "%s", title);
        changed = true;
    }
    if(strcmp(artist, s_artist) != 0) {
        snprintf(s_artist, sizeof(s_artist), "%s", artist);
        changed = true;
    }
    if(strcmp(album, s_album) != 0) {
        snprintf(s_album, sizeof(s_album), "%s", album);
        changed = true;
    }
    if(track_key != s_track_key) {
        s_track_key = track_key;
        changed = true;
        /* Keep the current cover across track changes. Tracks in the same
         * album share the same artwork and the device does not resend it;
         * a real change arrives as a new FT image, and a zero-length type 2
         * Setup clears it. */
    }
    if(elapsed_ms != s_elapsed_ms || duration_ms != s_duration_ms ||
       queue_index != s_queue_index || queue_count != s_queue_count ||
       shuffle != s_shuffle || repeat != s_repeat || status != s_play_status) {
        changed = true;
    }
    s_elapsed_ms = elapsed_ms;
    s_duration_ms = duration_ms;
    s_queue_index = queue_index;
    s_queue_count = queue_count;
    s_shuffle = shuffle;
    s_repeat = repeat;
    s_play_status = status;
    s_playing = (status == 1);
    if(changed) s_revision++;
    xSemaphoreGive(s_state_lock);

    static char last_title[128];
    static char last_artist[128];
    static uint8_t last_status = 0xFF;
    if(strcmp(last_title, title) != 0 || strcmp(last_artist, artist) != 0 ||
       last_status != status) {
        snprintf(last_title, sizeof(last_title), "%s", title);
        snprintf(last_artist, sizeof(last_artist), "%s", artist);
        last_status = status;
        ESP_LOGI(TAG, "now playing: status=%u title='%s' artist='%s' "
                 "album='%s' elapsed=%ums duration=%ums key=%016llx",
                 status, title, artist, album, (unsigned)elapsed_ms,
                 (unsigned)duration_ms, (unsigned long long)track_key);
        if(title[0] != '\0') {
            set_status("iAP2: %s - %s", artist[0] ? artist : "?", title);
        }
    }
}

static void handle_csm(uint16_t msg_id, const uint8_t * body, size_t len)
{
    switch(msg_id) {
        case 0xAA00:
            send_auth_certificate();
            break;
        case 0xAA02:
            send_auth_response(body, len);
            break;
        case 0xAA04:
            ESP_LOGE(TAG, "authentication failed");
            set_status("iAP2 authentication failed");
            break;
        case 0xAA05:
            s_auth_ok = true;
            ESP_LOGI(TAG, "authentication succeeded");
            set_status("iAP2 authenticated");
            break;
        case 0x1D00:
            send_identification();
            break;
        case 0x1D02:
            s_identified = true;
            ESP_LOGI(TAG, "identification accepted");
            set_status("iAP2 identified");
            send_power_source_update();
            send_now_playing_request();
            send_library_request();
            send_power_updates_request();
            send_hid_start();
            break;
        case 0x1D03:
            ESP_LOGE(TAG, "identification rejected");
            set_status("iAP2 identification rejected");
            break;
        case 0x5001:
            parse_now_playing(body, len);
            break;
        case 0x4C01: {
            const uint8_t * p = body;
            const uint8_t * end = body + len;
            uint16_t pid = 0;
            const uint8_t * value = NULL;
            size_t value_len = 0;
            char local_id[128] = "";
            char any_id[128] = "";
            while(next_param(&p, end, &pid, &value, &value_len)) {
                if(pid != 0) continue;
                const uint8_t * q = value;
                const uint8_t * qend = value + value_len;
                uint16_t cid = 0;
                const uint8_t * cv = NULL;
                size_t cvlen = 0;
                while(next_param(&q, qend, &cid, &cv, &cvlen)) {
                    if(cid == 1 && cvlen > 0) {
                        char uuid[128];
                        snprintf(uuid, sizeof(uuid), "%.*s",
                                 (int)cvlen, (const char *)cv);
                        if(any_id[0] == '\0') {
                            snprintf(any_id, sizeof(any_id), "%s", uuid);
                        }
                        if(strstr(uuid, "-MPB-") != NULL ||
                           strstr(uuid, "-mpb-") != NULL) {
                            snprintf(local_id, sizeof(local_id), "%s", uuid);
                        }
                    }
                }
            }
#if defined(IAP2TAB5_FULL_LIBRARY_SYNC)
            const char * chosen = local_id[0] != '\0' ? local_id : any_id;
#else
            const char * chosen = any_id;
#endif
            ESP_LOGI(TAG, "media library information (%u bytes): local='%s'",
                     (unsigned)len, chosen);
            if(chosen[0] != '\0') {
                snprintf(s_library_sub_id, sizeof(s_library_sub_id), "%s",
                         chosen);
                app_config_set_library_id(chosen);
                s_library_revision[0] = '\0';
                s_library_resynced = false;
                s_library_subscribed = false;
            }
#if defined(IAP2TAB5_FULL_LIBRARY_SYNC)
            send_library_updates_request();
#endif
            break;
        }
        case 0x4C04: {
            /* MediaLibraryUpdate: extract the album PID from each item
             * group so the iAP2-side discovery order can be recorded, and
             * handle the device's delete-phase handshake. */
            const uint8_t * p = body;
            const uint8_t * end = body + len;
            uint16_t pid = 0;
            const uint8_t * value = NULL;
            size_t value_len = 0;
            int progress = -1;
            int last7 = -1;
            bool has_items = false;
            bool has_playlists = false;
            bool reset_seen = false;
            static int update_msgs;
            update_msgs++;
            s_stats.library_messages++;
            while(next_param(&p, end, &pid, &value, &value_len)) {
                if(update_msgs <= 12) {
                    ESP_LOGI(TAG, "library param %u len %u first=%02x",
                             (unsigned)pid, (unsigned)value_len,
                             value_len > 0 ? value[0] : 0);
                }
                if(pid == 1 && value_len > 0) {
                    snprintf(s_library_revision, sizeof(s_library_revision),
                             "%.*s", (int)value_len, (const char *)value);
                }
                else if(pid == 2) {
                    has_items = true;
                    const uint8_t * q = value;
                    const uint8_t * qend = value + value_len;
                    uint16_t cid = 0;
                    const uint8_t * cv = NULL;
                    size_t cvlen = 0;
                    uint64_t album_pid = 0;
                    while(next_param(&q, qend, &cid, &cv, &cvlen)) {
                        /* Property 5 is the album persistent ID; remember
                         * when this album was first seen so the album list
                         * still has a stable order without an AFC date. */
                        if(cid == 5 && cvlen == 8) {
                            album_pid = be64(cv);
                        }
                    }
                    s_library_items++;
                    s_stats.library_items++;
                    if(album_pid != 0) {
                        player_note_iap2_album(album_pid);
                    }
                }
                else if(pid == 3) {
                    has_playlists = true;
                }
                else if(pid == 6) {
                    reset_seen = true;
                }
                else if(pid == 7 && value_len >= 1) {
                    progress = value[0];
                    last7 = value[0];
                }
            }
            if(update_msgs <= 8 || (update_msgs % 1000) == 0) {
                ESP_LOGI(TAG, "library update msg %d: total groups=%d progress=%d",
                         update_msgs, s_library_items, progress);
            }
            if((last7 == 0x5A || reset_seen) &&
               !has_items && !has_playlists &&
               s_library_revision[0] != '\0' && !s_library_resynced) {
                /* The first pass only says "delete everything".  Subscribe
                 * again with the revision the device just reported so it
                 * sends the full item/playlist database. */
                s_library_resynced = true;
                s_library_subscribed = false;
                ESP_LOGI(TAG, "library delete phase complete; resubscribing");
                send_library_updates_request();
            }
            else if(progress >= 100) {
                ESP_LOGI(TAG, "media library updates complete: %d groups",
                         s_library_items);
                s_library_resynced = false;
                s_stats.library_complete = true;
            }
            break;
        }
        case 0x6801:
        case 0x6804:
            s_hid_ready = true;
            ESP_LOGI(TAG, "HID component response 0x%04x", msg_id);
            break;
        case 0xAE01: {
            const uint8_t * p = body;
            const uint8_t * end = body + len;
            uint16_t pid = 0;
            const uint8_t * value = NULL;
            size_t value_len = 0;
            xSemaphoreTake(s_state_lock, portMAX_DELAY);
            while(next_param(&p, end, &pid, &value, &value_len)) {
                unsigned v = 0;
                for(size_t i = 0; i < value_len && i < 4; i++) {
                    v = (v << 8) | value[i];
                }
                /* R29 Table 97-143: 0 max current (u16), 1 device will
                 * charge (bool), 2 accessory power mode (u8), 4 external
                 * charger (bool), 5 charging state (0 none, 1 charging,
                 * 2 charged), 6 battery level (u16). The device sends one
                 * PowerUpdate message per requested parameter. */
                switch(pid) {
                    case 0: s_max_current_ma = (int)v; break;
                    case 1: s_device_will_charge = (v != 0); break;
                    case 2: s_accessory_power_mode = (uint8_t)v; break;
                    case 4: s_external_charger = (v != 0); break;
                    case 5: s_charging_state = (uint8_t)v; break;
                    case 6:
                        if(value_len >= 1 && v <= 100) s_battery = (int)v;
                        break;
                    default: break;
                }
                ESP_LOGI(TAG, "power param %u len=%u value=%u",
                         (unsigned)pid, (unsigned)value_len, v);
            }
            s_charging = (s_charging_state == 1 || s_charging_state == 2 ||
                          s_external_charger);
            s_revision++;
            xSemaphoreGive(s_state_lock);
            ESP_LOGI(TAG, "power update battery=%d charging=%d state=%u "
                     "ext=%d will=%d max=%dmA mode=%u",
                     s_battery, (int)s_charging, (unsigned)s_charging_state,
                     (int)s_external_charger, (int)s_device_will_charge,
                     s_max_current_ma, (unsigned)s_accessory_power_mode);
            break;
        }
        case 0x4E09: {
            const uint8_t * p = body;
            const uint8_t * end = body + len;
            uint16_t pid = 0;
            const uint8_t * value = NULL;
            size_t value_len = 0;
            while(next_param(&p, end, &pid, &value, &value_len)) {
                if(pid == 0) {
                    xSemaphoreTake(s_state_lock, portMAX_DELAY);
                    snprintf(s_device_name, sizeof(s_device_name), "%.*s",
                             (int)value_len, (const char *)value);
                    s_revision++;
                    xSemaphoreGive(s_state_lock);
                    ESP_LOGI(TAG, "device name: %s", s_device_name);
                }
            }
            break;
        }
        case 0x4E0A:
        case 0x4E0B:
        case 0x4E0C:
            ESP_LOGI(TAG, "device notification 0x%04x", msg_id);
            break;
        default:
            ESP_LOGI(TAG, "CSM 0x%04x (%u bytes)", msg_id, (unsigned)len);
            break;
    }
}

static void csm_feed(const uint8_t * data, size_t len)
{
    if(len > CSM_BUF_SIZE - s_csm_len) {
        s_reset_pending = true;
        return;
    }
    memcpy(s_csm_buf + s_csm_len, data, len);
    s_csm_len += len;
    size_t offset = 0;
    while(s_csm_len - offset >= 6) {
        const uint8_t * frame = s_csm_buf + offset;
        if(frame[0] != 0x40 || frame[1] != 0x40) { offset++; continue; }
        uint16_t flen = (uint16_t)((frame[2] << 8) | frame[3]);
        if(flen < 6) { offset++; continue; }
        if(s_csm_len - offset < flen) break;
        uint16_t msg_id = (uint16_t)((frame[4] << 8) | frame[5]);
        handle_csm(msg_id, frame + 6, flen - 6);
        offset += flen;
    }
    if(offset != 0) {
        memmove(s_csm_buf, s_csm_buf + offset, s_csm_len - offset);
        s_csm_len -= offset;
    }
}

static uint64_t be64(const uint8_t * p)
{
    uint64_t v = 0;
    for(int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static ft_transfer_t * ft_find(uint8_t id)
{
    for(int i = 0; i < FT_MAX_TRANSFERS; i++) {
        if(s_ft[i].active && s_ft[i].id == id) return &s_ft[i];
    }
    return NULL;
}

static void ft_release(ft_transfer_t * t)
{
    if(t == NULL) return;
    free(t->data);
    memset(t, 0, sizeof(*t));
}

static ft_transfer_t * ft_alloc(uint8_t id)
{
    ft_transfer_t * slot = NULL;
    for(int i = 0; i < FT_MAX_TRANSFERS; i++) {
        if(!s_ft[i].active) {
            slot = &s_ft[i];
            break;
        }
    }
    if(slot == NULL) {
        /* The device keeps at most a queue file and an artwork transfer in
         * flight; evict the oldest if it ever exceeds our small table. */
        slot = &s_ft[0];
        ft_release(slot);
    }
    memset(slot, 0, sizeof(*slot));
    slot->active = true;
    slot->id = id;
    return slot;
}

static void ft_clear_all(void)
{
    for(int i = 0; i < FT_MAX_TRANSFERS; i++) ft_release(&s_ft[i]);
}

static bool ft_ensure(ft_transfer_t * t, size_t need)
{
    if(t == NULL || need > FT_MAX_ARTWORK) return false;
    if(t->cap >= need) return true;
    size_t cap = t->cap ? t->cap : 64 * 1024;
    while(cap < need) {
        size_t next = cap * 2;
        if(next > FT_MAX_ARTWORK || next < cap) {
            cap = FT_MAX_ARTWORK;
            break;
        }
        cap = next;
    }
    if(cap > FT_MAX_ARTWORK) cap = FT_MAX_ARTWORK;
    uint8_t * data = heap_caps_realloc(t->data, cap, MALLOC_CAP_SPIRAM);
    if(data == NULL) {
        ESP_LOGW(TAG, "artwork: cannot grow FT buffer to %u",
                 (unsigned)cap);
        return false;
    }
    t->data = data;
    t->cap = cap;
    return true;
}

static void ft_send(uint8_t file_id, uint8_t op)
{
    uint8_t datagram[2] = { file_id, op };
    tx_queue(FT_SESSION, datagram, sizeof(datagram));
}

static bool artwork_album_matches(const char * album)
{
    if(album == NULL || album[0] == '\0' || s_art_lock == NULL) return true;
    bool match = true;
    xSemaphoreTake(s_art_lock, portMAX_DELAY);
    if(s_art_cached_album[0] != '\0' &&
       strcmp(s_art_cached_album, album) != 0) {
        match = false;
    }
    xSemaphoreGive(s_art_lock);
    return match;
}

static void art_snapshot_meta(char * album, size_t album_len,
                              char * artist, size_t artist_len,
                              uint64_t * key)
{
    album[0] = '\0';
    artist[0] = '\0';
    *key = 0;
    if(s_state_lock == NULL) return;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    snprintf(album, album_len, "%s", s_album);
    snprintf(artist, artist_len, "%s", s_artist);
    *key = s_track_key;
    xSemaphoreGive(s_state_lock);
}

static void ft_publish_artwork(const uint8_t * data, size_t len)
{
    if(data == NULL || len < 3) return;

    bool jpeg = data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    bool png = len >= 8 && data[0] == 0x89 && data[1] == 'P' &&
               data[2] == 'N' && data[3] == 'G';
    if(!jpeg && !png) {
        ESP_LOGW(TAG, "artwork: unsupported image (%u bytes)",
                 (unsigned)len);
        return;
    }

    if(s_art_lock == NULL || s_art_sem == NULL || s_art_task == NULL) return;

    uint8_t * copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if(copy == NULL) {
        ESP_LOGW(TAG, "artwork: cannot queue %u bytes", (unsigned)len);
        return;
    }
    memcpy(copy, data, len);

    char album[128];
    char artist[128];
    uint64_t key = 0;
    art_snapshot_meta(album, sizeof(album), artist, sizeof(artist), &key);

    xSemaphoreTake(s_art_lock, portMAX_DELAY);
    free(s_art_pending);
    s_art_pending = copy;
    s_art_pending_len = len;
    snprintf(s_art_pending_album, sizeof(s_art_pending_album), "%s", album);
    snprintf(s_art_pending_artist, sizeof(s_art_pending_artist), "%s", artist);
    s_art_pending_key = key;
    xSemaphoreGive(s_art_lock);
    xSemaphoreGive(s_art_sem);

    ESP_LOGI(TAG, "artwork transfer complete (%u bytes)", (unsigned)len);
}

static void queue_list_apply(const uint8_t * blob, size_t len, uint16_t type);

static void ft_feed(const uint8_t * payload, size_t len)
{
    if(payload == NULL || len < 2) return;

    uint8_t id = payload[0];
    uint8_t op = payload[1];
    uint8_t opcode = op & 0x0F;
    const uint8_t * data = payload + 2;
    size_t dlen = len - 2;
    ft_transfer_t * t = ft_find(id);

    if(opcode == 4) { /* Setup */
        t = ft_alloc(id);
        if(dlen < 8 || dlen == 9) {
            ft_release(t);
            return;
        }
        uint64_t expected = be64(data);
        uint16_t type = dlen >= 10
                            ? (uint16_t)((data[8] << 8) | data[9]) : 0;
        if(type == 2) {
            ESP_LOGI(TAG, "artwork setup: %u bytes", (unsigned)expected);
        }
        if(expected > FT_MAX_ARTWORK) {
            ft_release(t);
            return;
        }
        t->type = type;
        t->expected = expected;
        if(expected > 0 && !ft_ensure(t, expected)) {
            ft_release(t);
            return;
        }
        ft_send(id, 0x01); /* Start: the device waits for this */
        if(expected == 0 && type == 2) {
            artwork_clear_cover();
        }
        return;
    }

    if(opcode == 2) { /* Cancel */
        if(t) ft_release(t);
        return;
    }
    if(opcode == 6) { /* Failure */
        ESP_LOGW(TAG, "artwork transfer failed");
        if(t) ft_release(t);
        return;
    }
    if(opcode == 5 || opcode == 1 || opcode == 3) {
        return; /* Success / Start / Pause from the device */
    }

    bool first = (op & 0x80) != 0;
    bool last = (op & 0x40) != 0;

    if(t == NULL) {
        if(!first) return;
        t = ft_alloc(id);
    }
    if(first) t->len = 0;

    if(dlen > FT_MAX_ARTWORK - t->len) {
        ft_send(id, 0x06);
        ft_release(t);
        return;
    }
    if(t->expected != 0 && t->len + dlen > t->expected) {
        ft_send(id, 0x06);
        ft_release(t);
        return;
    }
    if(!ft_ensure(t, t->len + dlen)) {
        ft_send(id, 0x06);
        ft_release(t);
        return;
    }
    if(dlen > 0) {
        memcpy(t->data + t->len, data, dlen);
        t->len += dlen;
    }

    if(last) {
        if(t->expected != 0 && t->len != t->expected) {
            ESP_LOGW(TAG, "artwork size mismatch id=%u: got %u expected %u",
                     (unsigned)id, (unsigned)t->len,
                     (unsigned)t->expected);
            ft_send(id, 0x06);
            ft_release(t);
            return;
        }
        ft_send(id, 0x05);
        if(t->type == 2) {
            if(t->len == 0) {
                artwork_clear_cover();
            }
            else {
                ft_publish_artwork(t->data, t->len);
            }
        }
        else if(t->type == 0x0003 || t->type == 0x0006) {
            queue_list_apply(t->data, t->len, t->type);
        }
        ft_release(t);
    }
}

/* Table 56-17 MediaItemList: count, start index, then MediaItem groups whose
 * property 0 is the persistent ID. */
static void queue_list_apply(const uint8_t * blob, size_t len, uint16_t type)
{
    static uint64_t ids[QUEUE_LIST_MAX];
    int count = 0;
    int base = 0;

    if(type == 0x0003) {
        if(len % 8 != 0) return;
        for(size_t i = 0; i + 8 <= len && count < QUEUE_LIST_MAX; i += 8) {
            ids[count++] = be64(blob + i);
        }
    }
    else {
        const uint8_t * p = blob;
        const uint8_t * end = blob + len;
        uint16_t pid = 0;
        const uint8_t * value = NULL;
        size_t value_len = 0;
        while(next_param(&p, end, &pid, &value, &value_len)) {
            if(pid == 1 && value_len >= 4) {
                base = (int)be32(value);
            }
            else if(pid == 2) {
                const uint8_t * q = value;
                const uint8_t * qend = value + value_len;
                uint16_t cid = 0;
                const uint8_t * cv = NULL;
                size_t cvlen = 0;
                while(next_param(&q, qend, &cid, &cv, &cvlen)) {
                    if(cid == 0 && cvlen >= 8 && count < QUEUE_LIST_MAX) {
                        ids[count++] = be64(cv);
                    }
                }
            }
        }
    }

    if(s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        memcpy(s_queue_list, ids, (size_t)count * sizeof(uint64_t));
        s_queue_list_count = count;
        s_queue_list_base = base;
        s_queue_list_rev++;
        xSemaphoreGive(s_state_lock);
    }
    ESP_LOGI(TAG, "device queue: %d items, base=%d", count, base);
}

int iap2_host_queue_list_count(void)
{
    int count = 0;
    if(s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        count = s_queue_list_count;
        xSemaphoreGive(s_state_lock);
    }
    return count;
}

uint32_t iap2_host_queue_list_revision(void)
{
    uint32_t rev = 0;
    if(s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        rev = s_queue_list_rev;
        xSemaphoreGive(s_state_lock);
    }
    return rev;
}

int iap2_host_queue_list_base(void)
{
    int base = 0;
    if(s_state_lock != NULL) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
        base = s_queue_list_base;
        xSemaphoreGive(s_state_lock);
    }
    return base;
}

bool iap2_host_queue_list_get(int index, uint64_t * key)
{
    bool ok = false;
    if(key == NULL || s_state_lock == NULL) return false;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if(index >= 0 && index < s_queue_list_count) {
        *key = s_queue_list[index];
        ok = true;
    }
    xSemaphoreGive(s_state_lock);
    return ok;
}

static void artwork_clear_cover(void)
{
    if(s_art_lock == NULL) return;
    xSemaphoreTake(s_art_lock, portMAX_DELAY);
    s_art_valid = false;
    s_art_rev++;
    s_art_cached_album[0] = '\0';
    s_art_cached_artist[0] = '\0';
    s_art_cached_key = 0;
    xSemaphoreGive(s_art_lock);
}

static void artwork_invalidate(void)
{
    /* Drop partial transfers on a link reset, but keep the decoded cover.
     * It is shown again when the same album reconnects; parse_now_playing()
     * clears it if the device reports a different album. */
    ft_clear_all();
}

static void artwork_decode_task(void * arg)
{
    (void)arg;
    for(;;) {
        if(xSemaphoreTake(s_art_sem, portMAX_DELAY) != pdTRUE) continue;

        uint8_t * data = NULL;
        uint8_t * cover = NULL;
        size_t len = 0;
        char album[128];
        char artist[128];
        uint64_t key = 0;
        xSemaphoreTake(s_art_lock, portMAX_DELAY);
        data = s_art_pending;
        len = s_art_pending_len;
        snprintf(album, sizeof(album), "%s", s_art_pending_album);
        snprintf(artist, sizeof(artist), "%s", s_art_pending_artist);
        key = s_art_pending_key;
        s_art_pending = NULL;
        s_art_pending_len = 0;
        s_art_pending_album[0] = '\0';
        s_art_pending_artist[0] = '\0';
        s_art_pending_key = 0;
        xSemaphoreGive(s_art_lock);

        if(data == NULL) continue;
        cover = heap_caps_malloc(ART_BYTES, MALLOC_CAP_SPIRAM);
        bool ok = cover != NULL &&
                  lockdown_client_decode_cover_mem(data, len, cover);
        free(data);

        if(ok) {
            xSemaphoreTake(s_art_lock, portMAX_DELAY);
            free(s_art_cover);
            s_art_cover = cover;
            s_art_valid = true;
            s_art_rev++;
            snprintf(s_art_cached_album, sizeof(s_art_cached_album), "%s",
                     album);
            snprintf(s_art_cached_artist, sizeof(s_art_cached_artist), "%s",
                     artist);
            s_art_cached_key = key;
            xSemaphoreGive(s_art_lock);
            ESP_LOGI(TAG, "artwork decoded (%u bytes)", (unsigned)len);
        }
        else {
            free(cover);
            ESP_LOGW(TAG, "artwork decode failed (%u bytes)", (unsigned)len);
        }
    }
}

uint32_t iap2_host_artwork_revision(void)
{
    if(s_art_lock == NULL) return 0;
    xSemaphoreTake(s_art_lock, portMAX_DELAY);
    uint32_t rev = s_art_rev;
    xSemaphoreGive(s_art_lock);
    return rev;
}

bool iap2_host_artwork_copy(uint8_t * dst, size_t len)
{
    if(dst == NULL || len < ART_BYTES || s_art_lock == NULL) return false;
    xSemaphoreTake(s_art_lock, portMAX_DELAY);
    bool ok = s_art_valid && s_art_cover != NULL;
    if(ok) memcpy(dst, s_art_cover, ART_BYTES);
    xSemaphoreGive(s_art_lock);
    return ok;
}

static void link_data(const uint8_t * payload, size_t len, uint8_t session)
{
    if(session == CONTROL_SESSION) {
        csm_feed(payload, len);
    }
    else if(session == EA_SESSION) {
        ESP_LOGI(TAG, "EA session data (%u bytes)", (unsigned)len);
    }
    else if(session == FT_SESSION) {
        ft_feed(payload, len);
    }
    else {
        ESP_LOGW(TAG, "data on unknown session %u", session);
    }
}

/* The worker owns session reassembly and potentially slow library/storage
 * work. A packet becomes ACKable only after its payload is safely queued. */
static bool session_enqueue(uint8_t session, const uint8_t * data, size_t len)
{
    if(uxQueueSpacesAvailable(s_session_queue) == 0) {
        s_stats.queue_stalls++;
        return false;
    }
    session_packet_t * packet = heap_caps_malloc(sizeof(*packet) + len,
                                                  MALLOC_CAP_SPIRAM);
    if(packet == NULL) { s_stats.queue_stalls++; return false; }
    packet->generation = s_link_generation;
    packet->session = session;
    packet->len = (uint16_t)len;
    memcpy(packet->payload, data, len);
    if(xQueueSend(s_session_queue, &packet, 0) != pdTRUE) {
        free(packet);
        s_stats.queue_stalls++;
        return false;
    }
    unsigned pending = uxQueueMessagesWaiting(s_session_queue);
    if(pending > s_stats.queue_high_water) s_stats.queue_high_water = pending;
    return true;
}

static void session_deliver(session_packet_t * packet)
{
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if(s_connected && packet->generation == s_link_generation) {
        link_data(packet->payload, packet->len, packet->session);
    }
    xSemaphoreGive(s_session_lock);
    free(packet);
}

static void session_task(void * arg)
{
    (void)arg;
    unsigned batch = 0;
    for(;;) {
        session_packet_t * packet;
        if(xQueueReceive(s_session_queue, &packet, portMAX_DELAY) != pdTRUE) continue;
        session_deliver(packet);
        if(++batch >= 8) { batch = 0; vTaskDelay(1); }
    }
}

static void handle_link_packet(const uint8_t * buf, size_t len)
{
    /* Header, length and payload checksums were validated by process_rx. */
    uint8_t control = buf[4], seq = buf[5], ack = buf[6], session = buf[7];
    size_t payload_len = len > 9 ? len - 10 : 0;
    const uint8_t * payload = buf + 9;
    s_stats.rx_packets++;

    if(control & CONTROL_RST) {
        ESP_LOGW(TAG, "device reset the iAP2 link");
        reset_link();
        s_state = LINK_DETECT;
        s_next_marker = xTaskGetTickCount();
        return;
    }
    if(control & CONTROL_SYN) {
        if(parse_lsp(payload, payload_len)) {
            s_last_recv_psn = seq;
            s_last_rx = xTaskGetTickCount();
            send_ack();
            if(s_state == LINK_NEGOTIATE) {
                s_state = LINK_NORMAL;
                ESP_LOGI(TAG, "iAP2 link established");
                set_status("iAP2 link established");
            }
        }
        return;
    }
    if(control & (CONTROL_ACK | CONTROL_EAK)) {
        if(s_state == LINK_NEGOTIATE && ack == s_sent_psn) s_state = LINK_NORMAL;
        /* TX is stop-and-wait, so the only admissible cumulative ACK is the
         * outstanding sequence. Stale/unsent ACKs cannot free its payload. */
        if(s_tx_busy && ack == s_tx_seq) {
            s_tx_offset += s_tx_packet_len;
            s_tx_busy = false;
            if(s_tx_offset == s_tx_active_len) {
                free(s_tx_active);
                s_tx_active = NULL;
                s_tx_active_len = s_tx_offset = 0;
            }
        }
        else if((control & CONTROL_EAK) && s_tx_busy) {
            /* With one TX in flight, an EAK requests this missing packet.
             * Its sequence list belongs to the link, never to a session. */
            s_tx_deadline = xTaskGetTickCount();
        }
    }
    /* Pure ACK/EAK never consumes an incoming data sequence number. */
    if(payload_len == 0 || (control & CONTROL_EAK) || s_state != LINK_NORMAL) return;
    if((control & ~CONTROL_ACK) != 0) return;

    uint8_t expected = (uint8_t)(s_last_recv_psn + 1);
    if(seq == expected) {
        if(!session_enqueue(session, payload, payload_len)) return;
        s_last_recv_psn = seq;
    }
    else if(seq == s_last_recv_psn) {
        s_stats.duplicates++;
    }
    else {
        s_stats.sequence_gaps++;
        /* Discard unaccepted data; cumulative ACK asks for the missing
         * sequence without falsely acknowledging any later payload. */
    }
    s_last_rx = xTaskGetTickCount();
    send_ack();
}

static void process_rx(void)
{
    s_rx_more = false;
    if(s_rx_overflow) {
        uint8_t discard[256];
        while(xStreamBufferReceive(s_rx_stream, discard, sizeof(discard), 0)) { }
        s_rx_overflow = false;
        link_send(CONTROL_RST, s_sent_psn, s_last_recv_psn, 0, NULL, 0);
        reset_link();
        s_state = LINK_DETECT;
        s_next_marker = xTaskGetTickCount();
        ESP_LOGW(TAG, "RX transport overflow/framing error; restarting link");
        return;
    }
    size_t n = xStreamBufferReceive(s_rx_stream, s_link_buf + s_link_len,
                                    LINK_BUF_SIZE - s_link_len, 0);
    s_link_len += n;
    if(s_state == LINK_DETECT) {
        size_t offset = 0;
        while(offset + IAP2_MARKER_LEN <= s_link_len &&
              memcmp(s_link_buf + offset, IAP2_MARKER, IAP2_MARKER_LEN) != 0) offset++;
        if(offset + IAP2_MARKER_LEN <= s_link_len) {
            offset += IAP2_MARKER_LEN;
            s_state = LINK_NEGOTIATE;
            s_next_syn = xTaskGetTickCount();
        }
        else if(s_link_len > IAP2_MARKER_LEN) offset = s_link_len - IAP2_MARKER_LEN;
        else offset = 0;
        memmove(s_link_buf, s_link_buf + offset, s_link_len - offset);
        s_link_len -= offset;
        if(s_state == LINK_DETECT) return;
    }

    size_t offset = 0;
    unsigned packets = 0;
    int64_t start = esp_timer_get_time();
    uint32_t generation = s_link_generation;
    /* Scan with a cursor, compact once. Bad bytes used to cause a whole
     * buffer memmove each, turning corruption into quadratic CPU work. */
    while(s_link_len - offset >= 9 && packets < RX_PACKET_BUDGET) {
        if(esp_timer_get_time() - start >= RX_TIME_BUDGET_US) {
            s_rx_more = true;
            break;
        }
        const uint8_t * pkt = s_link_buf + offset;
        if(pkt[0] != (uint8_t)(LINK_START >> 8) || pkt[1] != (uint8_t)LINK_START) {
            offset++;
            s_partial_waiting = false;
            continue;
        }
        /* Never trust a length until its header checksum is valid. */
        uint16_t plen = (uint16_t)((pkt[2] << 8) | pkt[3]);
        if(checksum(pkt, 8) != pkt[8] || plen < 9 || plen == 10 || plen > LINK_MAX_PACKET) {
            s_stats.bad_headers++;
            s_partial_waiting = false;
            offset++;
            continue;
        }
        if(s_link_len - offset < plen) {
            if(!s_partial_waiting) {
                s_partial_waiting = true;
                s_partial_since = xTaskGetTickCount();
            }
            else if((TickType_t)(xTaskGetTickCount() - s_partial_since) >=
                    pdMS_TO_TICKS((uint32_t)s_peer_retransmit * 2u)) {
                s_stats.bad_payloads++;
                s_partial_waiting = false;
                offset++;
                continue;
            }
            break;
        }
        s_partial_waiting = false;
        if(plen > 9 && checksum(pkt + 9, plen - 9) != 0) {
            s_stats.bad_payloads++;
            offset++;
            continue;
        }
        handle_link_packet(pkt, plen);
        /* RST/disconnect may clear this buffer. Do not subtract the old
         * packet length from the reset length (unsigned underflow). */
        if(generation != s_link_generation) return;
        offset += plen;
        packets++;
    }
    if(packets == RX_PACKET_BUDGET && s_link_len - offset >= 9) s_rx_more = true;
    if(offset != 0) {
        memmove(s_link_buf, s_link_buf + offset, s_link_len - offset);
        s_link_len -= offset;
    }
}

static void tx_pump(void)
{
    if(s_state != LINK_NORMAL || !s_connected) return;
    TickType_t now = xTaskGetTickCount();
    if(s_tx_busy) {
        if((TickType_t)(now - s_tx_deadline) < 0x80000000u) {
            if(s_tx_retries++ >= 4) {
                ESP_LOGW(TAG, "link ACK timeout, resetting");
                reset_link();
                s_state = LINK_DETECT;
                s_next_marker = now;
                return;
            }
            s_stats.retransmits++;
            link_send(CONTROL_ACK, s_tx_seq, s_last_recv_psn,
                      s_tx_active_session, s_tx_active + s_tx_offset, s_tx_packet_len);
            s_tx_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(s_peer_retransmit);
        }
        return;
    }
    if(s_tx_active == NULL) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        if(s_txq_tail == s_txq_head) { xSemaphoreGive(s_tx_lock); return; }
        s_tx_active = s_txq[s_txq_tail];
        s_tx_active_len = s_txq_len[s_txq_tail];
        s_tx_active_session = s_txq_session[s_txq_tail];
        s_txq[s_txq_tail] = NULL;
        s_txq_tail = (s_txq_tail + 1) % TX_QUEUE_LEN;
        xSemaphoreGive(s_tx_lock);
        s_tx_offset = 0;
    }
    size_t limit = s_peer_max_len - 10u;
    s_tx_packet_len = s_tx_active_len - s_tx_offset;
    if(s_tx_packet_len > limit) s_tx_packet_len = limit;
    s_tx_seq = ++s_sent_psn;
    s_tx_busy = true;
    s_tx_retries = 0;
    s_tx_deadline = now + pdMS_TO_TICKS(s_peer_retransmit);
    /* Keep the exact sequence/payload on write failure: the device may
     * have received some or all of the transfer. Retry, never skip ahead. */
    link_send(CONTROL_ACK, s_tx_seq, s_last_recv_psn, s_tx_active_session,
              s_tx_active + s_tx_offset, s_tx_packet_len);
}

static void client_task(void * arg)
{
    (void)arg;
    for(;;) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

static void iap2_task(void * arg)
{
    (void)arg;
    bool setup_done = false;
    unsigned work_turns = 0;

    for(;;) {
        if(s_reset_pending) {
            s_reset_pending = false;
            reset_link();
            uint8_t discard[256];
            while(xStreamBufferReceive(s_rx_stream, discard, sizeof(discard), 0)) { }
            s_rx_overflow = false;
            setup_done = false;
            if(s_connected) {
                s_state = LINK_DETECT;
                s_next_marker = xTaskGetTickCount();
                s_next_syn = xTaskGetTickCount();
            }
        }
        if(!s_connected) {
            setup_done = false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        if(!setup_done) {
            uint8_t desc[256];
            esp_err_t derr = ctrl_get_descriptor(0x22, 0, desc, sizeof(desc));
            if((int)derr > 0) {
                char hex[3 * 256 + 1];
                int n = (int)derr < 208 ? (int)derr : 208;
                for(int i = 0; i < n; i++) {
                    snprintf(hex + i * 3, 4, "%02x ", desc[i]);
                }
                hex[n * 3] = '\0';
                ESP_LOGI(TAG, "HID report descriptor (%d bytes): %s",
                         (int)derr, hex);
            }
            else {
                ESP_LOGW(TAG, "HID report descriptor failed: %s",
                         esp_err_to_name(derr));
            }
            setup_done = true;
        }

        TickType_t now = xTaskGetTickCount();

        if(s_state == LINK_DETECT &&
           (TickType_t)(now - s_next_marker) < 0x80000000u) {
            send_marker();
            s_next_marker = now + pdMS_TO_TICKS(1000);
        }
        else if(s_state == LINK_NEGOTIATE &&
                (TickType_t)(now - s_next_syn) < 0x80000000u) {
            uint8_t lsp[32];
            size_t lsp_len = 0;
            build_lsp(lsp, &lsp_len);
            link_send(CONTROL_SYN, s_sent_psn, 0, 0, lsp, lsp_len);
            s_next_syn = now + pdMS_TO_TICKS(500);
        }

        process_rx();
        tx_pump();
        /* USB completion / outgoing work wakes us immediately. Yield at
         * least once per eight turns even during a continuous sync burst. */
        if(++work_turns >= 8) { work_turns = 0; vTaskDelay(1); }
        ulTaskNotifyTake(pdTRUE, s_rx_more ? 0 : pdMS_TO_TICKS(10));
    }
}

/* Older firmware kept one last cover in np.raw/np.txt and published it
 * before the current track was known, which made the Now Playing page flash
 * a stale cover and then go grey.  The UI now has a track-ID cache under
 * <library>/np/; remove the legacy files once. */
static void art_cache_remove_legacy(void)
{
    static const char * const names[] = { "np.raw", "np.txt", "np.tmp",
                                          "nm.tmp" };
    char path[160];
    for(size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", app_config_library_dir(),
                 names[i]);
        remove(path);
    }
}

esp_err_t iap2_host_start(void)
{
    art_cache_remove_legacy();
    s_rx_stream = xStreamBufferCreateWithCaps(RX_STREAM_SIZE, 0,
                                              MALLOC_CAP_SPIRAM);
    s_link_buf = heap_caps_malloc(LINK_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_csm_buf = heap_caps_malloc(CSM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_ctrl_done = xSemaphoreCreateBinary();
    s_ctrl_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    s_session_lock = xSemaphoreCreateMutex();
    s_session_queue = xQueueCreate(SESSION_QUEUE_LEN, sizeof(session_packet_t *));
    if(s_session_lock == NULL || s_session_queue == NULL) return ESP_ERR_NO_MEM;
    if(s_rx_stream == NULL || s_link_buf == NULL || s_csm_buf == NULL ||
       s_ctrl_done == NULL || s_ctrl_lock == NULL ||
       s_tx_lock == NULL || s_state_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_art_lock = xSemaphoreCreateMutex();
    s_art_sem = xSemaphoreCreateBinary();
    if(s_art_lock == NULL || s_art_sem == NULL) {
        ESP_LOGW(TAG, "artwork channel disabled (no memory)");
    }
    else if(xTaskCreateWithCaps(artwork_decode_task, "iap2_art", 32768,
                                NULL, 4, &s_art_task,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "artwork decode task creation failed");
        s_art_task = NULL;
    }

    s_enabled = true;
    if(usb_host_transfer_alloc(8 + 256, 0, &s_ctrl_xfer) != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t err = usb_host_client_register(&cfg, &s_client);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "client register failed: %s", esp_err_to_name(err));
        return err;
    }

    if(xTaskCreateWithCaps(session_task, "iap2_session", 16384, NULL, 4, NULL,
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) return ESP_ERR_NO_MEM;
    if(xTaskCreate(client_task, "iap2_evt", 6144, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if(xTaskCreate(iap2_task, "iap2", 12288, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    uint8_t addresses[8];
    int count = 0;
    if(usb_host_device_addr_list_fill(8, addresses, &count) == ESP_OK) {
        for(int i = 0; i < count; i++) {
            open_device(addresses[i]);
        }
    }

    ESP_LOGI(TAG, "iAP2 host started (USB config 2)");
    return ESP_OK;
}

bool iap2_host_is_enabled(void)
{
    return s_enabled;
}

bool iap2_host_is_connected(void)
{
    return s_connected;
}

bool iap2_host_get_state(iap2_state_t * out)
{
    if(out == NULL || s_state_lock == NULL) return false;
    if(xSemaphoreTake(s_state_lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    out->connected = s_connected;
    out->identified = s_identified;
    out->play_status = s_play_status;
    snprintf(out->title, sizeof(out->title), "%s", s_title);
    snprintf(out->artist, sizeof(out->artist), "%s", s_artist);
    snprintf(out->album, sizeof(out->album), "%s", s_album);
    out->track_key = s_track_key;
    snprintf(out->device_name, sizeof(out->device_name), "%s", s_device_name);
    out->elapsed_ms = s_elapsed_ms;
    out->duration_ms = s_duration_ms;
    out->queue_index = s_queue_index;
    out->queue_count = s_queue_count;
    out->shuffle = s_shuffle;
    out->repeat = s_repeat;
    out->battery = s_battery;
    out->charging = s_charging;
    out->max_current_ma = s_max_current_ma;
    out->device_will_charge = s_device_will_charge;
    out->accessory_power_mode = s_accessory_power_mode;
    out->external_charger = s_external_charger;
    out->charging_state = s_charging_state;
    out->revision = s_revision;
    xSemaphoreGive(s_state_lock);
    return true;
}

esp_err_t iap2_host_request_charging(void)
{
    if(!s_connected || !s_identified) return ESP_ERR_INVALID_STATE;
    /* The USB vendor request uses an offset from 500 mA: 500/500 would
     * advertise 1000 mA. Keep charging control on the iAP2 500 mA path. */
    send_power_source_update();
    return ESP_OK;
}

bool iap2_host_is_identified(void)
{
    return s_identified;
}

bool iap2_host_is_playing(void)
{
    return s_playing;
}

const char * iap2_host_status(void)
{
    return s_status;
}

esp_err_t iap2_host_seek_ms(uint32_t elapsed_ms)
{
    if(!s_connected || !s_identified) return ESP_ERR_INVALID_STATE;
    uint8_t buf[16];
    pb_t b = { buf, 0, sizeof(buf) };
    pb_u32(&b, 0, elapsed_ms);
    csm_send(0x5003, &b);
    return ESP_OK;
}

esp_err_t iap2_host_hid_command(const char * name)
{
    uint8_t bit = 0;
    if(!find_hid_bit(name, &bit)) return ESP_ERR_INVALID_ARG;
    if(!s_connected || !s_identified) {
        return ESP_ERR_INVALID_STATE;
    }
    send_hid_report(bit);
    return ESP_OK;
}

esp_err_t iap2_host_hid_raw_bit(uint8_t bit)
{
    if(bit > 11) return ESP_ERR_INVALID_ARG;
    if(!s_connected || !s_identified) return ESP_ERR_INVALID_STATE;
    send_hid_report(bit);
    return ESP_OK;
}

esp_err_t iap2_host_hid_press_bit(uint8_t bit)
{
    if(bit > 11) return ESP_ERR_INVALID_ARG;
    if(!s_connected || !s_identified) return ESP_ERR_INVALID_STATE;
    uint8_t report[2] = { 0, 0 };
    report[bit / 8] = (uint8_t)(1 << (bit % 8));
    send_hid_report_raw(report);
    ESP_LOGI(TAG, "HID press bit %u", bit);
    return ESP_OK;
}

esp_err_t iap2_host_hid_release(void)
{
    if(!s_connected || !s_identified) return ESP_ERR_INVALID_STATE;
    uint8_t report[2] = { 0, 0 };
    send_hid_report_raw(report);
    ESP_LOGI(TAG, "HID release");
    return ESP_OK;
}

static const char * library_uuid(void)
{
    return s_library_sub_id[0] != '\0' ? s_library_sub_id : s_library_id;
}

esp_err_t iap2_host_play_track_keys(const uint64_t * keys, int count,
                                    int start_index)
{
    if(keys == NULL || count <= 0) return ESP_ERR_INVALID_ARG;
    if(!s_connected || !s_identified) return ESP_ERR_INVALID_STATE;
    const char * uuid = library_uuid();
    if(uuid[0] == '\0') return ESP_ERR_INVALID_STATE;

    /* A CSM frame is limited to MAX_CSM_FRAME; each key is 8 bytes.  The
     * library UUID (param 2) is required by the iPod and takes ~64 bytes. */
    const int max_keys = (MAX_CSM_FRAME - 96) / 8;
    if(count > max_keys) count = max_keys;

    const size_t key_bytes = (size_t)count * 8;
    uint8_t * buf = heap_caps_malloc(MAX_CSM_FRAME, MALLOC_CAP_SPIRAM);
    uint8_t * kb = heap_caps_malloc(key_bytes, MALLOC_CAP_SPIRAM);
    if(buf == NULL || kb == NULL) {
        free(buf);
        free(kb);
        return ESP_ERR_NO_MEM;
    }

    for(int i = 0; i < count; i++) {
        uint64_t key = keys[i];
        for(int b = 0; b < 8; b++) {
            kb[i * 8 + b] = (uint8_t)(key >> (56 - b * 8));
        }
    }

    pb_t b = { buf, 0, MAX_CSM_FRAME };
    pb_param(&b, 0, kb, key_bytes);
    uint8_t start[4] = {
        (uint8_t)(start_index >> 24), (uint8_t)(start_index >> 16),
        (uint8_t)(start_index >> 8), (uint8_t)start_index
    };
    pb_param(&b, 1, start, sizeof(start));
    pb_param(&b, 2, uuid, strlen(uuid) + 1);
    ESP_LOGI(TAG, "play media items: %d keys, start=%d, first=%016llx",
             count, start_index, (unsigned long long)keys[0]);
    csm_send(0x4C07, &b);

    free(kb);
    free(buf);
    return ESP_OK;
}

esp_err_t iap2_host_play_all_songs(uint64_t start_pid)
{
    if(!s_connected || !s_identified) return ESP_ERR_INVALID_STATE;
    const char * uuid = library_uuid();
    if(uuid[0] == '\0') return ESP_ERR_INVALID_STATE;

    uint8_t * buf = heap_caps_malloc(MAX_CSM_FRAME, MALLOC_CAP_SPIRAM);
    if(buf == NULL) return ESP_ERR_NO_MEM;

    /* PlayMediaLibrarySpecial: library UUID, AllSongs (empty), and the
     * starting track.  The device builds the queue itself, so the library
     * is never sent as a PID list. */
    pb_t b = { buf, 0, MAX_CSM_FRAME };
    pb_param(&b, 0, uuid, strlen(uuid) + 1);
    static const uint8_t none = 0;
    pb_param(&b, 1, &none, 0);
    uint8_t pid[8];
    for(int i = 0; i < 8; i++) {
        pid[i] = (uint8_t)(start_pid >> (56 - i * 8));
    }
    pb_param(&b, 2, pid, sizeof(pid));
    ESP_LOGI(TAG, "play all songs from %016llx",
             (unsigned long long)start_pid);
    csm_send(0x4C09, &b);

    free(buf);
    return ESP_OK;
}

esp_err_t iap2_host_play_current_key(void)
{
    if(s_track_key == 0) return ESP_ERR_INVALID_STATE;
    return iap2_host_play_track_keys(&s_track_key, 1, 0);
}

void iap2_host_link_stats(iap2_link_stats_t * out)
{
    if(out != NULL) *out = s_stats;
}
