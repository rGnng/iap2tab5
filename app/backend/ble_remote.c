#include "ble_remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ui.h"

#if defined(ESP_PLATFORM)

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "store/config/ble_store_config.h"
#include "ulanzi_d100h.h"
#include "usb_hid_input.h"

static const char * TAG = "ble_remote";

#define BLE_REMOTE_NVS_NS "ble_remote"
#define BLE_REMOTE_NVS_KEY "paired"

#define D100H_SERVICE_UUID16  0x1971
#define D100H_WRITE_UUID16    0xfd01
#define D100H_NOTIFY_UUID16   0xfd00

/* All GAP/GATT operations run on the NimBLE host event queue. Public UI
 * functions only update requests under s_lock; the supervisor consumes them. */
#define CONNECT_TIMEOUT_MS 15000
#define SETUP_TIMEOUT_US 20000000LL
#define QUIET_LINK_PROBE_US 10000000LL
#define LINK_PROBE_TIMEOUT_US 10000000LL

static SemaphoreHandle_t s_lock;
static ble_remote_state_t s_state = BLE_REMOTE_OFF;
static char s_status[80] = "Bluetooth off";
static char s_current[32];
static char s_current_addr[18];
static bool s_supported;
static bool s_enabled = true;
static ble_remote_device_t s_scan[BLE_REMOTE_MAX_DEVICES];
static uint8_t s_scan_type[BLE_REMOTE_MAX_DEVICES];
static int s_scan_count;
static ble_remote_device_t s_paired[BLE_REMOTE_MAX_DEVICES];
static uint8_t s_paired_type[BLE_REMOTE_MAX_DEVICES];
static int s_paired_count;
static uint32_t s_rev;

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type;
static bool s_nimble_ready;
static bool s_discovery_started;
static bool s_scan_pending;
static bool s_connect_pending;
static ble_addr_t s_pending_addr;
static ble_addr_t s_target_addr;
static bool s_have_target;
static bool s_scan_stop_pending;
static bool s_disconnect_requested;
static ble_addr_t s_forget[BLE_REMOTE_MAX_DEVICES];
static int s_forget_count;
static bool s_reconnect;
static int64_t s_reconnect_at;
static unsigned s_retry_count;
static int64_t s_setup_deadline;
static int64_t s_closing_deadline;
static ble_remote_info_t s_info;

/* 0x1971 vendor service state. */
static bool s_vendor_ready;
static uint16_t s_vendor_start;
static uint16_t s_vendor_end;
static uint16_t s_vendor_notify_handle;
static uint16_t s_vendor_write_handle;
static uint16_t s_vendor_cccd_handle;
static struct ble_npl_callout s_hb_callout;
static int64_t s_last_heartbeat_rx;
static int64_t s_last_vendor_rx;
static int64_t s_last_link_check;
static ble_remote_link_stats_t s_link_stats;
static uint32_t s_probe_generation;
/* 0: none, 1: read CCCD, 2: refresh subscription with an acknowledged write. */
static unsigned s_probe_phase;
static unsigned s_probe_failures;
static int64_t s_probe_deadline;
static int64_t s_next_heartbeat;
static unsigned s_write_failures;
/* 1: initial read, 2: zero write/ack, 3: verifying readback. */
static unsigned s_sleep_phase;
static unsigned s_sleep_attempts;
static int64_t s_sleep_retry_at;
static int64_t s_sleep_deadline;
/* Separate standby timer from the official SDK. Unsupported/failed standby
 * setup is diagnostic only: it must not tear down a responsive connection. */
static unsigned s_standby_phase;
static unsigned s_standby_attempts;
static int64_t s_standby_retry_at;
static int64_t s_standby_deadline;

static void lock(void)
{
    if(s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    if(s_lock != NULL) xSemaphoreGive(s_lock);
}

static void set_state_locked(ble_remote_state_t state, const char * text)
{
    s_state = state;
    if(text != NULL) {
        snprintf(s_status, sizeof(s_status), "%s", text);
    }
    s_rev++;
}

static void set_state(ble_remote_state_t state, const char * text)
{
    lock();
    set_state_locked(state, text);
    unlock();
}

static void schedule_reconnect(void)
{
    lock();
    s_reconnect = s_enabled && s_have_target;
    unsigned delay_s = 3u << (s_retry_count < 3 ? s_retry_count : 3);
    if(delay_s > 15) delay_s = 15;
    if(s_retry_count < 3) s_retry_count++;
    s_reconnect_at = esp_timer_get_time() + (int64_t)delay_s * 1000000;
    if(!s_enabled) set_state_locked(BLE_REMOTE_OFF, "Bluetooth off");
    else if(!s_scan_pending && !ble_gap_disc_active())
        set_state_locked(BLE_REMOTE_IDLE, s_reconnect ? "Reconnecting..." : "Idle");
    unlock();
}

static void addr_to_str(const ble_addr_t * addr, char * out, size_t out_len)
{
    if(addr == NULL || out == NULL || out_len == 0) return;
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
}

static bool addr_from_str(const char * text, ble_addr_t * out)
{
    unsigned v[6];
    if(text == NULL || out == NULL) return false;
    if(sscanf(text, "%x:%x:%x:%x:%x:%x",
              &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for(int i = 0; i < 6; i++) {
        if(v[i] > 0xff) return false;
        out->val[5 - i] = (uint8_t)v[i];
    }
    out->type = BLE_ADDR_PUBLIC;
    return true;
}

static void copy_name(char * dst, size_t dst_len, const char * src)
{
    if(dst_len == 0) return;
    snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

static bool uuid_matches(const ble_uuid_t * uuid, uint16_t uuid16,
                          const ble_uuid128_t * uuid128)
{
    if(uuid->type == BLE_UUID_TYPE_16) {
        return ((const ble_uuid16_t *)uuid)->value == uuid16;
    }
    if(uuid->type == BLE_UUID_TYPE_128 && uuid128 != NULL) {
        return ble_uuid_cmp(uuid, &uuid128->u) == 0;
    }
    return false;
}

static void paired_save_locked(void)
{
    char buf[BLE_REMOTE_MAX_DEVICES * 64];
    size_t used = 0;
    buf[0] = '\0';
    for(int i = 0; i < s_paired_count; i++) {
        int n = snprintf(buf + used, sizeof(buf) - used, "%s,%u,%s\n",
                         s_paired[i].addr, (unsigned)s_paired_type[i],
                         s_paired[i].name);
        if(n < 0 || (size_t)n >= sizeof(buf) - used) break;
        used += (size_t)n;
    }
    nvs_handle_t h;
    if(nvs_open(BLE_REMOTE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, BLE_REMOTE_NVS_KEY, buf);
    nvs_commit(h);
    nvs_close(h);
}

static void paired_load(void)
{
    nvs_handle_t h;
    if(nvs_open(BLE_REMOTE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if(nvs_get_str(h, BLE_REMOTE_NVS_KEY, NULL, &len) != ESP_OK || len == 0) {
        nvs_close(h);
        return;
    }
    char * buf = malloc(len + 1);
    if(buf == NULL) {
        nvs_close(h);
        return;
    }
    if(nvs_get_str(h, BLE_REMOTE_NVS_KEY, buf, &len) == ESP_OK) {
        char * line = buf;
        while(line != NULL && *line != '\0' &&
              s_paired_count < BLE_REMOTE_MAX_DEVICES) {
            char * next = strchr(line, '\n');
            if(next != NULL) *next++ = '\0';
            char * first = strchr(line, ',');
            if(first == NULL) break;
            *first++ = '\0';
            char * second = strchr(first, ',');
            if(second == NULL) break;
            *second++ = '\0';
            ble_remote_device_t * dev = &s_paired[s_paired_count];
            memset(dev, 0, sizeof(*dev));
            copy_name(dev->addr, sizeof(dev->addr), line);
            s_paired_type[s_paired_count] = (uint8_t)atoi(first);
            copy_name(dev->name, sizeof(dev->name), second);
            s_paired_count++;
            line = next;
        }
    }
    free(buf);
    nvs_close(h);
}

static int paired_find_locked(const char * addr)
{
    for(int i = 0; i < s_paired_count; i++) {
        if(strcasecmp(s_paired[i].addr, addr) == 0) return i;
    }
    return -1;
}

static void paired_add(const char * addr, uint8_t addr_type, const char * name)
{
    if(addr == NULL || addr[0] == '\0') return;
    lock();
    int index = paired_find_locked(addr);
    if(index < 0) {
        if(s_paired_count >= BLE_REMOTE_MAX_DEVICES) {
            index = 0;
        }
        else {
            index = s_paired_count++;
        }
    }
    ble_remote_device_t * dev = &s_paired[index];
    memset(dev, 0, sizeof(*dev));
    copy_name(dev->addr, sizeof(dev->addr), addr);
    copy_name(dev->name, sizeof(dev->name),
              (name != NULL && name[0] != '\0') ? name : addr);
    s_paired_type[index] = addr_type;
    /* Persist the last successful device as the boot reconnect target. */
    if(index > 0) {
        ble_remote_device_t preferred = s_paired[index];
        uint8_t preferred_type = s_paired_type[index];
        memmove(s_paired + 1, s_paired, (size_t)index * sizeof(s_paired[0]));
        memmove(s_paired_type + 1, s_paired_type, (size_t)index);
        s_paired[0] = preferred;
        s_paired_type[0] = preferred_type;
    }
    paired_save_locked();
    s_rev++;
    unlock();
}

static void paired_remove(const char * addr)
{
    lock();
    int index = paired_find_locked(addr);
    if(index >= 0) {
        for(int i = index; i + 1 < s_paired_count; i++) {
            s_paired[i] = s_paired[i + 1];
            s_paired_type[i] = s_paired_type[i + 1];
        }
        s_paired_count--;
        paired_save_locked();
        s_rev++;
    }
    unlock();
}

static const ble_remote_device_t * find_known_locked(const char * addr,
                                                     uint8_t * type)
{
    for(int i = 0; i < s_scan_count; i++) {
        if(strcasecmp(s_scan[i].addr, addr) == 0) {
            *type = s_scan_type[i];
            return &s_scan[i];
        }
    }
    for(int i = 0; i < s_paired_count; i++) {
        if(strcasecmp(s_paired[i].addr, addr) == 0) {
            *type = s_paired_type[i];
            return &s_paired[i];
        }
    }
    return NULL;
}

static void scan_add(const ble_addr_t * addr, const char * name, int rssi)
{
    char text[18];
    addr_to_str(addr, text, sizeof(text));
    lock();
    int index = -1;
    for(int i = 0; i < s_scan_count; i++) {
        if(strcasecmp(s_scan[i].addr, text) == 0) {
            index = i;
            break;
        }
    }
    if(index < 0) {
        if(s_scan_count >= BLE_REMOTE_MAX_DEVICES) {
            unlock();
            return;
        }
        index = s_scan_count++;
    }
    ble_remote_device_t * dev = &s_scan[index];
    copy_name(dev->addr, sizeof(dev->addr), text);
    copy_name(dev->name, sizeof(dev->name),
              (name != NULL && name[0] != '\0') ? name : text);
    dev->rssi = rssi;
    s_scan_type[index] = addr->type;
    s_rev++;
    unlock();
}

static bool adv_name(const struct ble_gap_disc_desc * disc,
                     char * name, size_t name_len)
{
    const uint8_t * data = disc->data;
    uint8_t len = disc->length_data;
    bool has_service = false;
    name[0] = '\0';
    for(uint8_t i = 0; i + 1 < len; ) {
        uint8_t ad_len = data[i];
        if(ad_len == 0 || (int)i + 1 + ad_len > len) break;
        uint8_t type = data[i + 1];
        const uint8_t * value = data + i + 2;
        uint8_t value_len = ad_len - 1;
        if(type == 0x08 || type == 0x09) {
            size_t n = value_len < name_len - 1 ? value_len : name_len - 1;
            memcpy(name, value, n);
            name[n] = '\0';
        }
        else if((type == 0x02 || type == 0x03) && value_len >= 2) {
            for(uint8_t j = 0; j + 1 < value_len; j += 2) {
                if((value[j] | (value[j + 1] << 8)) == 0x1971) {
                    has_service = true;
                }
            }
        }
        i += ad_len + 1;
    }
    if(has_service) return true;
    if(name[0] == '\0') return false;
    return strstr(name, "D100H") != NULL || strstr(name, "Ulanzi") != NULL;
}

static int gap_event(struct ble_gap_event * event, void * arg);

void ble_store_config_init(void);

static int on_svc(uint16_t conn_handle, const struct ble_gatt_error * error,
                  const struct ble_gatt_svc * service, void * arg);
static int on_chr(uint16_t conn_handle, const struct ble_gatt_error * error,
                  const struct ble_gatt_chr * chr, void * arg);
static int on_dsc(uint16_t conn_handle, const struct ble_gatt_error * error,
                  uint16_t chr_val_handle,
                  const struct ble_gatt_dsc * dsc, void * arg);
static int on_write(uint16_t conn_handle, const struct ble_gatt_error * error,
                    struct ble_gatt_attr * attr, void * arg);
static void clear_link(void)
{
    s_probe_generation++;
    s_probe_phase = s_probe_failures = 0;
    s_vendor_ready = false;
    s_vendor_start = s_vendor_end = 0;
    s_vendor_notify_handle = s_vendor_write_handle = s_vendor_cccd_handle = 0;
    s_discovery_started = false;
    s_setup_deadline = s_closing_deadline = 0;
    s_sleep_phase = s_sleep_attempts = s_write_failures = 0;
    s_standby_phase = s_standby_attempts = 0;
    usb_hid_input_cancel_gestures(D100H_INPUT_BLE);
    lock();
    s_last_vendor_rx = s_last_heartbeat_rx = 0;
    memset(&s_info, 0, sizeof(s_info));
    s_current[0] = s_current_addr[0] = '\0';
    s_rev++;
    unlock();
}

static bool target_matches_attempt(void)
{
    return s_enabled && !s_disconnect_requested && s_have_target &&
           ble_addr_cmp(&s_pending_addr, &s_target_addr) == 0;
}

static bool active_callback(uint16_t conn_handle)
{
    return conn_handle == s_conn_handle && target_matches_attempt() &&
           s_closing_deadline == 0;
}

static void vendor_fail(const char * text)
{
    ESP_LOGW(TAG, "%s", text);
    s_vendor_ready = false;
    s_sleep_phase = 0;
    s_standby_phase = 0;
    usb_hid_input_cancel_gestures(D100H_INPUT_BLE);
    lock();
    snprintf(s_link_stats.last_recovery, sizeof(s_link_stats.last_recovery), "%s", text);
    memset(&s_info, 0, sizeof(s_info));
    set_state_locked(s_enabled ? BLE_REMOTE_CONNECTING : BLE_REMOTE_OFF, text);
    unlock();
    if(s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        s_closing_deadline = esp_timer_get_time() + 5000000;
        int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if(rc == BLE_HS_ENOTCONN) {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            clear_link();
            schedule_reconnect();
        }
    }
    else schedule_reconnect();
}

static int vendor_send_plain(const uint8_t * plain, size_t len)
{
    if(!s_vendor_ready || s_vendor_write_handle == 0 ||
       s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return BLE_HS_ENOTCONN;
    uint8_t msg[20];
    ulanzi_d100h_encode_ble(plain, len, msg);
    int rc = ble_gattc_write_no_rsp_flat(s_conn_handle, s_vendor_write_handle,
                                        msg, sizeof(msg));
    if(rc != 0) ESP_LOGW(TAG, "vendor write failed: handle=%u rc=%d",
                         s_vendor_write_handle, rc);
    return rc;
}

static void vendor_send_heartbeat(void)
{
    static const uint8_t heartbeat[] = { 0x06, 0x01, 0x23, 0x00, 0x01 };
    if(vendor_send_plain(heartbeat, sizeof(heartbeat)) == 0) s_write_failures = 0;
    else if(++s_write_failures >= 3) vendor_fail("Heartbeat write stalled");
    s_next_heartbeat = esp_timer_get_time() + 1000000;
}

static void link_probe_failed(int status)
{
    s_probe_phase = 0;
    s_last_link_check = esp_timer_get_time();
    ESP_LOGW(TAG, "GATT link probe failed: status=%d attempt=%u", status, s_probe_failures + 1);
    if(++s_probe_failures >= 3) vendor_fail("GATT link probe failed");
}

static int on_link_probe(uint16_t conn_handle, const struct ble_gatt_error * error,
                         struct ble_gatt_attr * attr, void * arg)
{
    if((uint32_t)(uintptr_t)arg != s_probe_generation || !active_callback(conn_handle) ||
       !s_vendor_ready || !s_probe_phase) return 0;
    if(error == NULL || error->status != 0) {
        link_probe_failed(error != NULL ? error->status : BLE_HS_EINVAL);
        return 0;
    }
    if(s_probe_phase == 1) {
        uint8_t value[2];
        if(attr == NULL || attr->om == NULL || OS_MBUF_PKTLEN(attr->om) != 2 ||
           os_mbuf_copydata(attr->om, 0, 2, value) != 0) {
            link_probe_failed(BLE_HS_EINVAL);
            return 0;
        }
        /* Refresh FD00 after proving the ATT server responds. Never remove a
         * healthy idle link just because vendor heartbeat replies went quiet. */
        const uint8_t enabled[2] = {1, 0};
        s_probe_phase = 2;
        s_probe_deadline = esp_timer_get_time() + LINK_PROBE_TIMEOUT_US;
        int rc = ble_gattc_write_flat(conn_handle, s_vendor_cccd_handle, enabled,
                                      sizeof(enabled), on_link_probe, arg);
        if(rc != 0) link_probe_failed(rc);
    }
    else {
        s_probe_phase = s_probe_failures = 0;
        s_last_link_check = esp_timer_get_time();
        lock();
        uint32_t checks = ++s_link_stats.link_checks;
        unlock();
        ESP_LOGI(TAG, "Quiet vendor channel: GATT responsive, subscription refreshed (check=%lu)",
                 (unsigned long)checks);
        vendor_send_heartbeat();
    }
    return 0;
}

static void start_link_probe(void)
{
    s_probe_generation++;
    s_probe_phase = 1;
    s_probe_deadline = esp_timer_get_time() + LINK_PROBE_TIMEOUT_US;
    int rc = ble_gattc_read(s_conn_handle, s_vendor_cccd_handle, on_link_probe,
                            (void *)(uintptr_t)s_probe_generation);
    if(rc != 0) link_probe_failed(rc);
}

static void send_sleep_request(void)
{
    const uint8_t read[] = { 0x01, 0x01, 0x42, 0x01 };
    const uint8_t off[] = { 0x01, 0x01, 0x42, 0x02, 0, 0, 0, 0 };
    if(s_sleep_phase == 2) vendor_send_plain(off, sizeof(off));
    else vendor_send_plain(read, sizeof(read));
    s_sleep_attempts++;
    /* Under Wi-Fi/library load this dial has taken almost six seconds to
     * answer. Keep retries sparse and allow that response to arrive. */
    s_sleep_retry_at = esp_timer_get_time() + 3000000;
}

static void send_standby_request(void)
{
    const uint8_t read[] = {1, 1, 0x2c, 1};
    const uint8_t off[] = {1, 1, 0x2c, 2, 0, 0, 0, 0};
    vendor_send_plain(s_standby_phase == 2 ? off : read,
                      s_standby_phase == 2 ? sizeof(off) : sizeof(read));
    s_standby_attempts++;
    s_standby_retry_at = esp_timer_get_time() + 3000000;
}

static void read_standby_state(void)
{
    const uint8_t read[] = {1, 1, 0x0d, 1};
    vendor_send_plain(read, sizeof(read));
}

static void handle_vendor_status(const ulanzi_d100h_status_t * st)
{
    if(st->kind == ULANZI_STATUS_STANDBY_READ || st->kind == ULANZI_STATUS_STANDBY_WRITE) {
        ESP_LOGI(TAG, "D100H standby timer: %lu seconds (kind=%d)",
                 (unsigned long)st->sleep_seconds, st->kind);
        if(st->kind == ULANZI_STATUS_STANDBY_READ) {
            lock();
            s_info.standby_time_valid = true;
            s_info.standby_seconds = st->sleep_seconds;
            s_rev++;
            unlock();
            if(s_standby_phase == 1 || s_standby_phase == 3) {
                if(st->sleep_seconds == 0) {
                    s_standby_phase = 0;
                    ESP_LOGI(TAG, "D100H standby timer zero (readback confirmed)");
                    read_standby_state();
                }
                else {
                    s_standby_phase = 2;
                    s_standby_attempts = 0;
                    send_standby_request();
                }
            }
        }
        else if(s_standby_phase == 2 && st->sleep_seconds == 0) {
            s_standby_phase = 3;
            s_standby_attempts = 0;
            send_standby_request();
        }
    }
    else if(st->kind == ULANZI_STATUS_STANDBY_STATE) {
        lock();
        s_info.standby_state_valid = true;
        s_info.standby = st->standby;
        s_rev++;
        unlock();
        ESP_LOGI(TAG, "D100H standby state: %d", st->standby);
    }
    else if(st->kind == ULANZI_STATUS_HEARTBEAT) {
        lock();
        s_last_heartbeat_rx = esp_timer_get_time();
        s_link_stats.heartbeat_replies++;
        unlock();
    }
    else if(st->kind == ULANZI_STATUS_BATTERY) {
        lock();
        if(!s_info.battery_valid || s_info.battery_percent != st->battery_percent ||
           s_info.charging != st->charging || s_info.charge_full != st->charge_full) {
            s_info.battery_valid = true;
            s_info.battery_percent = st->battery_percent;
            s_info.charging = st->charging;
            s_info.charge_full = st->charge_full;
            s_rev++;
            ESP_LOGI(TAG, "D100H battery=%u%% charging=%d full=%d voltage_raw=%u",
                     st->battery_percent, st->charging, st->charge_full, st->voltage_raw);
        }
        unlock();
    }
    else if(s_sleep_phase != 0) {
        ESP_LOGI(TAG, "D100H sleep reply: %lu seconds, phase=%u",
                 (unsigned long)st->sleep_seconds, s_sleep_phase);
        if(st->kind == ULANZI_STATUS_SLEEP_READ && s_sleep_phase != 2) {
            if(st->sleep_seconds == 0) {
                s_sleep_phase = 0;
                s_retry_count = 0;
                lock();
                s_info.sleep_disabled = true;
                s_rev++;
                unlock();
                ESP_LOGI(TAG, "D100H sleep disabled (readback confirmed)");
                return;
            }
            s_sleep_phase = 2;
            s_sleep_attempts = 0;
            send_sleep_request();
        }
        else if(st->kind == ULANZI_STATUS_SLEEP_WRITE && s_sleep_phase == 2 &&
                st->sleep_seconds == 0) {
            s_sleep_phase = 3;
            s_sleep_attempts = 0;
            send_sleep_request();
        }
    }
}

static void heartbeat_callout(struct ble_npl_event * ev);

static void start_discovery(void)
{
    if(s_discovery_started) return;
    s_discovery_started = true;
    s_vendor_ready = false;
    s_vendor_start = 0;
    s_vendor_end = 0;
    s_vendor_notify_handle = 0;
    s_vendor_write_handle = 0;
    s_vendor_cccd_handle = 0;

    int rc = ble_gattc_disc_all_svcs(s_conn_handle, on_svc, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "service discovery failed: %d", rc);
        vendor_fail("Service discovery failed");
    }
}

static int on_svc(uint16_t conn_handle, const struct ble_gatt_error * error,
                  const struct ble_gatt_svc * service, void * arg)
{
    (void)arg;
    if(!active_callback(conn_handle)) return 0;
    if(error != NULL && error->status == 0 && service != NULL) {
        char uuid[40];
        ble_uuid_to_str(&service->uuid.u, uuid);
        ESP_LOGI(TAG, "service %s handles %u-%u", uuid,
                 service->start_handle, service->end_handle);
        if(uuid_matches(&service->uuid.u, D100H_SERVICE_UUID16, NULL)) {
            s_vendor_start = service->start_handle;
            s_vendor_end = service->end_handle;
        }
        return 0;
    }
    if(error != NULL && error->status == BLE_HS_EDONE) {
        if(s_vendor_start == 0) {
            vendor_fail("D100H vendor service not found");
            return 0;
        }
        ESP_LOGI(TAG, "D100H vendor service handles %u-%u",
                 s_vendor_start, s_vendor_end);
        int rc = ble_gattc_disc_all_chrs(conn_handle, s_vendor_start,
                                         s_vendor_end, on_chr, NULL);
        if(rc != 0) {
            ESP_LOGW(TAG, "characteristic discovery failed: %d", rc);
            vendor_fail("Characteristic discovery failed");
        }
    }
    return 0;
}

static int on_chr(uint16_t conn_handle, const struct ble_gatt_error * error,
                  const struct ble_gatt_chr * chr, void * arg)
{
    (void)arg;
    if(!active_callback(conn_handle)) return 0;
    if(error != NULL && error->status == 0 && chr != NULL) {
        char uuid[40];
        ble_uuid_to_str(&chr->uuid.u, uuid);
        ESP_LOGI(TAG, "vendor chr %s val=%u props=0x%x", uuid,
                 chr->val_handle, chr->properties);
        if(uuid_matches(&chr->uuid.u, D100H_NOTIFY_UUID16, NULL)) {
            s_vendor_notify_handle = chr->val_handle;
        }
        else if(uuid_matches(&chr->uuid.u, D100H_WRITE_UUID16, NULL)) {
            s_vendor_write_handle = chr->val_handle;
        }
        return 0;
    }
    if(error != NULL && error->status == BLE_HS_EDONE) {
        if(s_vendor_notify_handle == 0 || s_vendor_write_handle == 0) {
            vendor_fail("D100H vendor characteristics not found");
            return 0;
        }
        ESP_LOGI(TAG, "D100H notify=%u write=%u",
                 s_vendor_notify_handle, s_vendor_write_handle);
        int rc = ble_gattc_disc_all_dscs(conn_handle, s_vendor_notify_handle,
                                         s_vendor_end, on_dsc, NULL);
        if(rc != 0) {
            ESP_LOGW(TAG, "descriptor discovery failed: %d", rc);
            vendor_fail("Descriptor discovery failed");
        }
    }
    return 0;
}

static int on_dsc(uint16_t conn_handle, const struct ble_gatt_error * error,
                  uint16_t chr_val_handle,
                  const struct ble_gatt_dsc * dsc, void * arg)
{
    (void)arg;
    if(!active_callback(conn_handle)) return 0;
    if(error != NULL && error->status == 0 && dsc != NULL) {
        char uuid[40];
        ble_uuid_to_str(&dsc->uuid.u, uuid);
        ESP_LOGI(TAG, "vendor dsc %s handle=%u cb_chr=%u", uuid,
                 dsc->handle, chr_val_handle);
        if(uuid_matches(&dsc->uuid.u, BLE_GATT_DSC_CLT_CFG_UUID16, NULL) &&
           dsc->handle > s_vendor_notify_handle &&
           (s_vendor_write_handle == 0 ||
            dsc->handle < s_vendor_write_handle)) {
            s_vendor_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if(error != NULL && error->status == BLE_HS_EDONE) {
        if(s_vendor_cccd_handle == 0) {
            vendor_fail("D100H notify CCCD not found");
            return 0;
        }
        ESP_LOGI(TAG, "D100H notify CCCD=%u", s_vendor_cccd_handle);
        uint8_t value[2] = { 1, 0 };
        int rc = ble_gattc_write_flat(conn_handle, s_vendor_cccd_handle,
                                      value, sizeof(value), on_write, NULL);
        if(rc != 0) {
            ESP_LOGW(TAG, "CCCD subscribe failed: %d", rc);
            vendor_fail("CCCD subscribe failed");
        }
    }
    return 0;
}

static int on_write(uint16_t conn_handle, const struct ble_gatt_error * error,
                    struct ble_gatt_attr * attr, void * arg)
{
    (void)attr;
    (void)arg;
    if(!active_callback(conn_handle)) return 0;
    if(error != NULL && error->status != 0) {
        ESP_LOGW(TAG, "CCCD write failed: %d", error->status);
        vendor_fail("CCCD write failed");
        return 0;
    }

    char addr[18];
    addr_to_str(&s_pending_addr, addr, sizeof(addr));
    lock();
    uint8_t type = BLE_ADDR_PUBLIC;
    const ble_remote_device_t * known = find_known_locked(addr, &type);
    char name[32];
    copy_name(name, sizeof(name), known != NULL ? known->name : addr);
    copy_name(s_current, sizeof(s_current), name);
    copy_name(s_current_addr, sizeof(s_current_addr), addr);
    set_state_locked(BLE_REMOTE_CONNECTED, "Connected");
    unlock();

    s_vendor_ready = true;
    s_setup_deadline = 0;
    lock();
    s_info.connected = true;
    s_last_vendor_rx = s_last_heartbeat_rx = 0;
    unlock();
    s_last_link_check = esp_timer_get_time();
    s_probe_phase = s_probe_failures = 0;
    vendor_send_heartbeat();
    s_sleep_phase = 1;
    s_sleep_attempts = 0;
    s_sleep_deadline = esp_timer_get_time() + 20000000;
    s_standby_phase = 1;
    s_standby_attempts = 0;
    s_standby_retry_at = s_standby_deadline = 0;
    send_sleep_request();
    paired_add(addr, type, name);
    ESP_LOGI(TAG, "connected to %s (%s) via vendor service", name, addr);
    return 0;
}

static int gap_event(struct ble_gap_event * event, void * arg)
{
    (void)arg;
    switch(event->type) {
        case BLE_GAP_EVENT_DISC: {
            char name[32];
            if(adv_name(&event->disc, name, sizeof(name))) {
                scan_add(&event->disc.addr, name, event->disc.rssi);
                ESP_LOGI(TAG, "found %s rssi=%d", name,
                         event->disc.rssi);
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "scan complete, reason=%d state=%d",
                     event->disc_complete.reason, (int)s_state);
            if(s_state == BLE_REMOTE_SCANNING) {
                set_state(!s_enabled ? BLE_REMOTE_OFF : s_vendor_ready
                          ? BLE_REMOTE_CONNECTED : BLE_REMOTE_IDLE,
                          !s_enabled ? "Bluetooth off" : s_vendor_ready ? "Connected" : "Idle");
            }
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            if(event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_discovery_started = false;
                s_reconnect = false;
                s_setup_deadline = esp_timer_get_time() + SETUP_TIMEOUT_US;
                if(!target_matches_attempt() || s_scan_pending) {
                    vendor_fail("Connection cancelled");
                    return 0;
                }
                set_state(BLE_REMOTE_CONNECTING, "Securing...");
                int rc = ble_gap_security_initiate(s_conn_handle);
                if(rc == BLE_HS_EALREADY) start_discovery();
                else if(rc != 0) vendor_fail("Bluetooth security failed");
            }
            else {
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                clear_link();
                schedule_reconnect();
            }
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "encryption change: status=%d", event->enc_change.status);
            if(active_callback(event->enc_change.conn_handle)) {
                if(event->enc_change.status != 0) {
                    vendor_fail("Bluetooth encryption failed");
                    return 0;
                }
                start_discovery();
            }
            return 0;
        case BLE_GAP_EVENT_PASSKEY_ACTION:
            ESP_LOGW(TAG, "passkey action=%d numcmp=%lu",
                     event->passkey.params.action,
                     (unsigned long)event->passkey.params.numcmp);
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            if(event->disconnect.conn.conn_handle != s_conn_handle) return 0;
            ESP_LOGW(TAG, "disconnect: reason=0x%x", event->disconnect.reason);
            lock();
            s_link_stats.last_disconnect_reason = event->disconnect.reason;
            unlock();
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            clear_link();
            schedule_reconnect();
            return 0;
        case BLE_GAP_EVENT_NOTIFY_RX: {
            if(!active_callback(event->notify_rx.conn_handle) || !s_vendor_ready) return 0;
            uint16_t handle = event->notify_rx.attr_handle;
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            /* Vendor FD00 (handle 77) and HID report 0xFB (handle 69) carry
             * the same 20-byte encrypted event frame on this unit.  Handling
             * both would inject every key twice, so prefer FD00 and use 69
             * only as a fallback while the vendor handle is unknown. */
            if(handle == 69 && s_vendor_notify_handle != 0) {
                return 0;
            }
            if(handle != s_vendor_notify_handle && handle != 69) {
                return 0;
            }
            if(len != 20) {
                ESP_LOGW(TAG, "vendor frame h=%u len=%u (expected 20)",
                         handle, len);
                return 0;
            }
            uint8_t data[20];
            if(os_mbuf_copydata(event->notify_rx.om, 0, len, data) != 0) {
                return 0;
            }
            lock();
            s_last_vendor_rx = esp_timer_get_time();
            s_link_stats.notifications++;
            unlock();
            /* Any notification proves the connection is alive, including
             * controls and status packets we do not yet know how to parse. */
            s_probe_phase = s_probe_failures = 0;
            uint8_t id = 0;
            bool down = false;
            if(!ulanzi_d100h_decode_ble(data, len, &id, &down)) {
                ulanzi_d100h_status_t st;
                if(ulanzi_d100h_decode_ble_status(data, len, &st)) handle_vendor_status(&st);
                return 0;
            }
            ESP_LOGD(TAG, "D100H event id=0x%02x down=%d", id, (int)down);
            if(id == 0x01) {
                usb_hid_input_post_back(D100H_INPUT_BLE, down);
                return 0;
            }
            if(id == 0x02) {
                usb_hid_input_post_play(D100H_INPUT_BLE, down);
                return 0;
            }
            if(id == 0x03) {
                usb_hid_input_post_top_right(D100H_INPUT_BLE, down);
                return 0;
            }
            if(id == 0x07 || id == 0x0B || id == 0x0C) {
                usb_hid_input_post_knob(D100H_INPUT_BLE, id, down);
                return 0;
            }
            if(id == 0x09 || id == 0x0A) {
                usb_hid_input_post_rotary(D100H_INPUT_BLE, id, down);
                return 0;
            }
            if(id == 0x05 || id == 0x08) {
                usb_hid_input_post_key_state(
                    id == 0x05 ? UI_KEY_PREV_TRACK : UI_KEY_NEXT_TRACK,
                    down);
                return 0;
            }
            ui_key_t key;
            if(ulanzi_d100h_to_ui(id, down, &key)) {
                usb_hid_input_post_key(key);
            }
            return 0;
        }
        default:
            return 0;
    }
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced");
    int rc = ble_hs_util_ensure_addr(0);
    if(rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
        set_state(BLE_REMOTE_ERROR, "Bluetooth address failed");
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if(rc != 0) {
        ESP_LOGE(TAG, "infer addr failed: %d", rc);
        set_state(BLE_REMOTE_ERROR, "Bluetooth address failed");
        return;
    }
    s_nimble_ready = true;
    s_supported = true;
    lock();
    if(!s_have_target && s_paired_count > 0 &&
       addr_from_str(s_paired[0].addr, &s_target_addr)) {
        s_target_addr.type = s_paired_type[0];
        s_have_target = true;
    }
    s_reconnect = s_enabled && s_have_target;
    s_reconnect_at = esp_timer_get_time();
    set_state_locked(s_enabled ? BLE_REMOTE_IDLE : BLE_REMOTE_OFF,
                     s_enabled ? "Idle" : "Bluetooth off");
    unlock();
    ble_npl_callout_reset(&s_hb_callout, ble_npl_time_ms_to_ticks32(250));
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
    s_nimble_ready = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_npl_callout_stop(&s_hb_callout);
    clear_link();
    set_state(s_enabled ? BLE_REMOTE_IDLE : BLE_REMOTE_OFF,
              s_enabled ? "Bluetooth reset" : "Bluetooth off");
}

static void host_task(void * arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();
    ESP_LOGW(TAG, "NimBLE host task ended");
    nimble_port_freertos_deinit();
}

/* Supervisor runs even when disconnected. Never gate retries on the display
 * state: ERROR, cancelled scans and failed immediate connects must all recover. */
static void heartbeat_callout(struct ble_npl_event * ev)
{
    (void)ev;
    if(!s_nimble_ready) return;
    const int64_t now = esp_timer_get_time();
    lock();
    bool enabled = s_enabled;
    bool scan = s_scan_pending;
    bool stop_scan = s_scan_stop_pending;
    s_scan_stop_pending = false;
    bool connect = s_connect_pending;
    bool have_target = s_have_target;
    ble_addr_t target = s_target_addr;
    bool retry = s_reconnect && now >= s_reconnect_at;
    ble_addr_t forgotten[BLE_REMOTE_MAX_DEVICES];
    int forgotten_count = s_forget_count;
    memcpy(forgotten, s_forget, sizeof(forgotten));
    s_forget_count = 0;
    unlock();

    for(int i = 0; i < forgotten_count; i++) ble_store_util_delete_peer(&forgotten[i]);
    if(stop_scan || !enabled || connect) {
        if(ble_gap_disc_active()) ble_gap_disc_cancel();
    }
    if(!enabled || !have_target || s_disconnect_requested ||
       (s_conn_handle != BLE_HS_CONN_HANDLE_NONE && !target_matches_attempt())) {
        if(ble_gap_conn_active()) ble_gap_conn_cancel();
        if(s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
           (!s_closing_deadline || now >= s_closing_deadline))
            vendor_fail(enabled ? "Changing device" : "Bluetooth off");
        if(s_conn_handle == BLE_HS_CONN_HANDLE_NONE && !ble_gap_conn_active()) {
            lock();
            s_disconnect_requested = false;
            unlock();
        }
        if(!enabled) goto done;
    }
    if(s_closing_deadline) {
        if(now >= s_closing_deadline) vendor_fail("Retrying disconnect");
        goto done;
    }
    if(scan) {
        if(ble_gap_conn_active()) {
            ble_gap_conn_cancel();
            goto done;
        }
        lock();
        s_scan_pending = false;
        s_scan_count = 0;
        set_state_locked(BLE_REMOTE_SCANNING, "Scanning...");
        unlock();
        struct ble_gap_disc_params params = {0};
        params.itvl = 0x50;
        params.window = 0x30;
        params.filter_duplicates = 1;
        int rc = ble_gap_disc(s_own_addr_type, 15000, &params, gap_event, NULL);
        if(rc != 0) {
            ESP_LOGW(TAG, "scan failed: %d", rc);
            set_state(s_vendor_ready ? BLE_REMOTE_CONNECTED : BLE_REMOTE_IDLE, "Scan failed");
        }
    }
    if(s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        if(s_vendor_ready) {
            lock();
            if(target_matches_attempt()) {
                s_connect_pending = false;
                if(s_state == BLE_REMOTE_CONNECTING)
                    set_state_locked(BLE_REMOTE_CONNECTED, "Connected");
            }
            unlock();
            if(s_sleep_phase && now >= s_sleep_deadline)
                vendor_fail("Sleep setup stalled");
            else if(s_probe_phase && now >= s_probe_deadline)
                vendor_fail("GATT link probe timed out");
            else {
                if(now >= s_next_heartbeat) vendor_send_heartbeat();
                int64_t last_alive = s_last_vendor_rx > s_last_link_check
                                   ? s_last_vendor_rx : s_last_link_check;
                if(s_vendor_ready && !s_probe_phase && !s_sleep_phase &&
                   now - last_alive >= QUIET_LINK_PROBE_US) start_link_probe();
                if(s_vendor_ready && s_sleep_phase && now >= s_sleep_retry_at) {
                    if(s_sleep_attempts >= 5) vendor_fail("Sleep setting not confirmed");
                    else send_sleep_request();
                }
                if(s_vendor_ready && !s_sleep_phase && s_standby_phase) {
                    if(!s_standby_deadline) s_standby_deadline = now + 20000000;
                    if(now >= s_standby_deadline ||
                       (now >= s_standby_retry_at && s_standby_attempts >= 5)) {
                        s_standby_phase = 0;
                        ESP_LOGW(TAG, "D100H standby setting not confirmed; keeping responsive link");
                        read_standby_state();
                    }
                    else if(now >= s_standby_retry_at) send_standby_request();
                }
            }
        }
        else if(s_setup_deadline && now >= s_setup_deadline)
            vendor_fail("Bluetooth setup stalled");
    }
    else if(ble_gap_conn_active()) {
        if(connect || scan || (s_setup_deadline && now >= s_setup_deadline)) {
            ble_gap_conn_cancel();
            /* CONNECT cancellation callback schedules the next retry. */
        }
    }
    else if(have_target && !scan && !ble_gap_disc_active() && (connect || retry)) {
        lock();
        s_pending_addr = target;
        s_connect_pending = false;
        s_reconnect = false;
        s_setup_deadline = now + SETUP_TIMEOUT_US;
        set_state_locked(BLE_REMOTE_CONNECTING, "Connecting...");
        unlock();
        int rc = ble_gap_connect(s_own_addr_type, &target, CONNECT_TIMEOUT_MS,
                                 NULL, gap_event, NULL);
        if(rc != 0) {
            ESP_LOGW(TAG, "connect failed to start: %d", rc);
            s_setup_deadline = 0;
            schedule_reconnect();
        }
    }
 done:
    ble_npl_callout_reset(&s_hb_callout, ble_npl_time_ms_to_ticks32(250));
}

void ble_remote_start(void)
{
    if(s_lock == NULL) s_lock = xSemaphoreCreateMutex();
    if(s_lock == NULL) return;

    lock();
    s_paired_count = 0;
    paired_load();
    s_rev++;
    unlock();

    esp_err_t err = esp_hosted_connect_to_slave();
    if(err != 0) {
        ESP_LOGW(TAG, "hosted connect: %d", err);
    }
    /* The Tab5 C6 advertises "HCI over SDIO / BLE only". Older co-processor
     * firmware does not implement the bt_controller_init RPC, but the VHCI
     * transport still carries HCI directly. Try the RPC and continue on
     * failure; NimBLE will report if the controller really is unavailable. */
    err = esp_hosted_bt_controller_init();
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "BT controller init RPC failed: %s (trying VHCI)",
                 esp_err_to_name(err));
    }
    err = esp_hosted_bt_controller_enable();
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "BT controller enable RPC failed: %s (trying VHCI)",
                 esp_err_to_name(err));
    }

    esp_err_t nerr = nimble_port_init();
    if(nerr != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(nerr));
        set_state(BLE_REMOTE_ERROR, "NimBLE init failed");
        return;
    }
    ESP_LOGI(TAG, "nimble_port_init ok");
    ble_npl_callout_init(&s_hb_callout, nimble_port_get_dflt_eventq(),
                         heartbeat_callout, NULL);

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* Just Works bonding.  The D100H vendor channel is only useful after the
     * link is encrypted; bond keys are kept in NVS by the NimBLE store. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC |
                                  BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    set_state(BLE_REMOTE_IDLE, "Starting Bluetooth...");
    ESP_LOGI(TAG, "BLE central started");
}

void ble_remote_scan_start(void)
{
    lock();
    if(s_enabled) {
        s_scan_pending = true;
        s_scan_stop_pending = false;
        s_connect_pending = false;
        set_state_locked(BLE_REMOTE_SCANNING, "Starting scan...");
    }
    unlock();
}

void ble_remote_scan_stop(void)
{
    lock();
    s_scan_pending = false;
    s_scan_stop_pending = true;
    if(s_state == BLE_REMOTE_SCANNING)
        set_state_locked(!s_enabled ? BLE_REMOTE_OFF : s_info.connected
                         ? BLE_REMOTE_CONNECTED : BLE_REMOTE_IDLE,
                         !s_enabled ? "Bluetooth off" : s_info.connected ? "Connected" : "Idle");
    unlock();
}

uint32_t ble_remote_revision(void)
{
    if(s_lock == NULL) return 0;
    lock();
    uint32_t rev = s_rev;
    unlock();
    return rev;
}

void ble_remote_status(ble_remote_state_t * state, char * text,
                       size_t text_len)
{
    lock();
    if(state != NULL) *state = s_state;
    if(text != NULL && text_len > 0) {
        if(s_state == BLE_REMOTE_CONNECTED && s_current[0] != '\0') {
            snprintf(text, text_len, "Connected: %s", s_current);
        }
        else {
            snprintf(text, text_len, "%s", s_status);
        }
    }
    unlock();
}

int ble_remote_scan_count(void)
{
    lock();
    int count = s_scan_count;
    unlock();
    return count;
}

bool ble_remote_scan_get(int index, ble_remote_device_t * out)
{
    if(out == NULL) return false;
    lock();
    bool ok = index >= 0 && index < s_scan_count;
    if(ok) *out = s_scan[index];
    unlock();
    return ok;
}

int ble_remote_paired_count(void)
{
    lock();
    int count = s_paired_count;
    unlock();
    return count;
}

bool ble_remote_paired_get(int index, ble_remote_device_t * out)
{
    if(out == NULL) return false;
    lock();
    bool ok = index >= 0 && index < s_paired_count;
    if(ok) *out = s_paired[index];
    unlock();
    return ok;
}

void ble_remote_get_info(const char * addr, ble_remote_info_t * out)
{
    if(out == NULL) return;
    lock();
    *out = (ble_remote_info_t){0};
    if(s_enabled && addr != NULL && strcasecmp(addr, s_current_addr) == 0)
        *out = s_info;
    unlock();
}

void ble_remote_get_link_stats(ble_remote_link_stats_t * out)
{
    if(out == NULL) return;
    lock();
    *out = s_link_stats;
    int64_t now = esp_timer_get_time();
    out->notification_age_ms = s_info.connected && s_last_vendor_rx > 0
                             ? (now - s_last_vendor_rx) / 1000 : -1;
    out->heartbeat_age_ms = s_info.connected && s_last_heartbeat_rx > 0
                          ? (now - s_last_heartbeat_rx) / 1000 : -1;
    unlock();
}

bool ble_remote_connect(const char * addr)
{
    ble_addr_t target;
    if(!addr_from_str(addr, &target)) return false;
    lock();
    if(!s_enabled) { unlock(); return false; }
    uint8_t type = BLE_ADDR_PUBLIC;
    find_known_locked(addr, &type);
    target.type = type;
    s_target_addr = target;
    s_have_target = true;
    s_connect_pending = true;
    s_scan_pending = false;
    s_scan_stop_pending = true;
    s_retry_count = 0;
    set_state_locked(BLE_REMOTE_CONNECTING, "Connecting...");
    unlock();
    return true;
}

bool ble_remote_unpair(const char * addr)
{
    ble_addr_t peer;
    if(!addr_from_str(addr, &peer)) return false;
    lock();
    uint8_t type = BLE_ADDR_PUBLIC;
    find_known_locked(addr, &type);
    peer.type = type;
    if(s_have_target && ble_addr_cmp(&peer, &s_target_addr) == 0) {
        s_have_target = false;
        s_connect_pending = s_reconnect = false;
    }
    if(s_forget_count < BLE_REMOTE_MAX_DEVICES) s_forget[s_forget_count++] = peer;
    unlock();
    paired_remove(addr);
    return true;
}

void ble_remote_set_enabled(bool enabled)
{
    lock();
    if(s_enabled != enabled) {
        s_enabled = enabled;
        s_scan_pending = s_connect_pending = false;
        s_scan_stop_pending = true;
        s_retry_count = 0;
        s_reconnect = enabled && s_have_target;
        s_reconnect_at = esp_timer_get_time();
        if(!enabled) {
            s_disconnect_requested = true;
            memset(&s_info, 0, sizeof(s_info));
        }
        set_state_locked(enabled ? BLE_REMOTE_IDLE : BLE_REMOTE_OFF,
                         enabled ? "Idle" : "Bluetooth off");
    }
    unlock();
}

bool ble_remote_enabled(void)
{
    lock();
    bool enabled = s_enabled;
    unlock();
    return enabled;
}

bool ble_remote_is_supported(void)
{
    return s_supported;
}

#else /* !ESP_PLATFORM: simulator stub */

void ble_remote_get_info(const char * addr, ble_remote_info_t * out)
{
    (void)addr;
    if(out != NULL) *out = (ble_remote_info_t){0};
}
void ble_remote_get_link_stats(ble_remote_link_stats_t * out)
{
    if(out != NULL) *out = (ble_remote_link_stats_t){
        .notification_age_ms = -1, .heartbeat_age_ms = -1};
}
void ble_remote_start(void) {}
void ble_remote_scan_start(void) {}
void ble_remote_scan_stop(void) {}
void ble_remote_set_enabled(bool enabled) { (void)enabled; }
bool ble_remote_enabled(void) { return false; }
uint32_t ble_remote_revision(void) { return 0; }

void ble_remote_status(ble_remote_state_t * state, char * text,
                       size_t text_len)
{
    if(state != NULL) *state = BLE_REMOTE_OFF;
    if(text != NULL && text_len > 0) {
        snprintf(text, text_len, "Bluetooth unavailable in simulator");
    }
}

int ble_remote_scan_count(void) { return 0; }
bool ble_remote_scan_get(int index, ble_remote_device_t * out)
{
    (void)index;
    (void)out;
    return false;
}
int ble_remote_paired_count(void) { return 0; }
bool ble_remote_paired_get(int index, ble_remote_device_t * out)
{
    (void)index;
    (void)out;
    return false;
}
bool ble_remote_connect(const char * addr) { (void)addr; return false; }
bool ble_remote_unpair(const char * addr) { (void)addr; return false; }
bool ble_remote_is_supported(void) { return false; }

#endif
