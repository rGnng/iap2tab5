#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes NVS/Wi-Fi and starts the manager task using the current
 * g_app_config.wifi_mode. Station credentials come from /sdcard/wifi.txt. */
esp_err_t wifi_manager_start(void);

/* Requests a mode change (0 = station, 1 = AP) without blocking the caller. */
void wifi_manager_request_mode(int mode);

#ifdef __cplusplus
}
#endif
