#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_SD_MOUNT   "/sdcard"
#define APP_CONFIG_PATH       APP_CONFIG_SD_MOUNT "/config.json"
#define APP_WIFI_CRED_PATH    APP_CONFIG_SD_MOUNT "/wifi.txt"

typedef struct {
    uint16_t screen_flip;      /* 0 or 180 degrees */
    uint8_t wifi_mode;         /* 0 = station, 1 = AP */
    bool sound_enabled;
    uint8_t sound_mode;         /* 0 = none, 1 = touch, 2 = touch + key */
    uint8_t volume;            /* 0..100 */
    char ap_ssid[33];
    char library_id[128];      /* full iAP2/AFC media-library UUID */

    /* Existing UI settings, persisted for a consistent boot. */
    bool clock_12h;
    uint8_t sort_field;
    bool sort_desc;
    bool menubar_left;
    bool touch_enabled;
    uint8_t np_bg_mode;
    uint8_t brightness;        /* 1..100; a requested 0 falls back to 5 */
    uint8_t screen_off_minutes; /* 0 = disabled (default), 30, 60 */
    uint8_t usb_mode;
    uint8_t theme;              /* 0 = Gray, 1 = Aqua, 2 = Windows 95 */
    uint8_t boot_screen;        /* 0 = off, 1 = KCN4M1 boot animation */
    bool ble_enabled;           /* Bluetooth remote enabled */
    bool debug_log;             /* mirror ESP_LOG to /sdcard/debug.log */
    bool charge_enabled;        /* IP2326 CHG_EN (E2.P7) */
    bool qc_high;               /* raw E2.P5 nCHG_QC_EN level */
    bool iap2_mode;             /* false = AFC/usbmux config 4, true = iAP2 config 2 */
} app_config_t;

extern app_config_t g_app_config;

/* Mounts the SD card (once) and loads /sdcard/config.json. Missing files or a
 * missing card are not fatal: built-in defaults are used instead. */
esp_err_t app_config_init(void);

/* Writes the current config atomically to /sdcard/config.json. */
esp_err_t app_config_save(void);

bool app_config_sd_ready(void);
/* Background card-status check, outside the display lock. A live card loss
 * latches unavailable until reboot; mounted files are never torn down here. */
void app_config_sd_poll(void);

typedef struct {
    bool ready;
    char vendor[32];
    char type[48];
    char filesystem[16];
    uint64_t used_bytes;
    uint64_t total_bytes;
} app_config_sd_info_t;

/* Cached SD information; refresh it when the screen is idle. */
bool app_config_sd_info(app_config_sd_info_t * out);
void app_config_sd_info_refresh(void);
void app_config_sd_info_refresh_idle(void);

/* Media-library UUID. The full string is kept in NVS; the short ID is the
 * last 8-character hex segment and is used as the FAT-safe directory name
 * /sdcard/lib/<short8>/. */
const char * app_config_library_id(void);
const char * app_config_library_short(void);
const char * app_config_library_dir(void);
const char * app_config_library_bin_path(void);
const char * app_config_library_stars_path(void);
const char * app_config_artwork_dir(void);
esp_err_t app_config_set_library_id(const char * id);

/* iAP2/AFC mode is stored in NVS so it survives a missing or unreadable SD
 * card. app_config_set_iap2_mode() also updates g_app_config. */
bool app_config_iap2_mode(void);
esp_err_t app_config_set_iap2_mode(bool enabled);

/* One-shot AFC job flag in NVS: 0 none, 2 sync, 3 reset artwork then sync.
 * Legacy job 1 is treated as an ordinary sync. */
esp_err_t app_config_set_afc_job(uint8_t job);
uint8_t app_config_peek_afc_job(void);
uint8_t app_config_take_afc_job(void);



/* Retries mounting the SD card if it was not present at boot. */
bool app_config_sd_ensure(void);

/* Unmounts and reformats the SD card as FAT, then mounts it. Destructive. */
bool app_config_sd_format(void);

/* Hashes a local file (used to verify SD copies). */
esp_err_t app_config_hash_file(const char * path, uint64_t * size,
                               char * hex, size_t hex_len);

/* Reads /sdcard/wifi.txt. Format: first non-empty line = SSID,
 * second non-empty line = password (may be empty for an open network). */
esp_err_t app_config_read_wifi_credentials(char * ssid, size_t ssid_len,
                                           char * password, size_t password_len);

#ifdef __cplusplus
}
#endif
