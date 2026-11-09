#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLE_REMOTE_MAX_DEVICES 8

typedef enum {
    BLE_REMOTE_OFF = 0,
    BLE_REMOTE_IDLE,
    BLE_REMOTE_SCANNING,
    BLE_REMOTE_CONNECTING,
    BLE_REMOTE_CONNECTED,
    BLE_REMOTE_ERROR,
} ble_remote_state_t;

typedef struct {
    char name[32];
    char addr[18];
    int rssi;
} ble_remote_device_t;

typedef struct {
    bool connected;
    bool battery_valid;
    uint8_t battery_percent;
    bool charging;
    bool charge_full;
    bool sleep_disabled;
    bool standby_time_valid;
    uint32_t standby_seconds;
    bool standby_state_valid;
    bool standby;
} ble_remote_info_t;

/* Live information for this address only. Battery becomes unknown on loss. */
void ble_remote_get_info(const char * addr, ble_remote_info_t * out);

typedef struct {
    uint32_t notifications;
    uint32_t heartbeat_replies;
    uint32_t link_checks;
    int64_t notification_age_ms;
    int64_t heartbeat_age_ms;
    int last_disconnect_reason;
    char last_recovery[80];
} ble_remote_link_stats_t;
void ble_remote_get_link_stats(ble_remote_link_stats_t * out);

void ble_remote_start(void);
void ble_remote_scan_start(void);
void ble_remote_scan_stop(void);
void ble_remote_set_enabled(bool enabled);
bool ble_remote_enabled(void);
uint32_t ble_remote_revision(void);
void ble_remote_status(ble_remote_state_t * state, char * text,
                       size_t text_len);
int ble_remote_scan_count(void);
bool ble_remote_scan_get(int index, ble_remote_device_t * out);
int ble_remote_paired_count(void);
bool ble_remote_paired_get(int index, ble_remote_device_t * out);
bool ble_remote_connect(const char * addr);
bool ble_remote_unpair(const char * addr);
bool ble_remote_is_supported(void);

#ifdef __cplusplus
}
#endif
