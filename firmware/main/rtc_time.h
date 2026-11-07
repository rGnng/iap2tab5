#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the RX8130CE RTC on the BSP I2C bus.  If the RTC has a valid
 * battery-backed time it is copied to the system clock; otherwise the
 * current preview/build time is written into the RTC. */
esp_err_t rtc_time_init(void);

/* Set the system clock (UTC epoch) and mirror the local wall-clock time into
 * the RX8130CE.  Safe to call from the UI or HTTP task. */
esp_err_t rtc_time_sync_epoch(time_t utc);

/* True when the RTC device was found and initialized. */
bool rtc_time_available(void);

#ifdef __cplusplus
}
#endif
