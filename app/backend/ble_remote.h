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

void ble_remote_start(void);
void ble_remote_scan_start(void);
void ble_remote_scan_stop(void);
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
