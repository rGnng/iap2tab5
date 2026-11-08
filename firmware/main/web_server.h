#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*web_config_apply_cb_t)(void);

/* Starts the debugging web UI on port 80. It is reachable as soon as the
 * network interface gets an address. */
esp_err_t web_server_start(void);

/* Registered by main.c: applies g_app_config to the display/audio/Wi-Fi. */
void web_server_set_apply_cb(web_config_apply_cb_t cb);
void web_server_set_sync_cb(bool (*cb)(void));

/* Updated by the Wi-Fi manager so /api/status and the UI show live state. */
void web_server_set_network_info(const char * status, const char * ip);

/* Debug helper: GET http://192.168.4.1/api/status from the device itself. */
void web_server_self_test(void);

/* Hard reset used by OTA/mode changes.  esp_restart() can stall while the
 * USB host stack is active; this resets through the ROM instead. */
void web_server_reboot(void);

#ifdef __cplusplus
}
#endif
