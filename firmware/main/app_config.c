#include "app_config.h"
#include "screen_brightness.h"
#include "fast_scroll.h"

#include <stdio.h>
#include <fcntl.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* The BSP default allows only 5 open files; the debug log, the thumbnail
 * pack and index and the star store can exhaust that. */
static esp_err_t sd_mount_default(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 16 * 1024,
    };
    bsp_sdcard_cfg_t cfg = {
        .mount = &mount,
    };
    return bsp_sdcard_sdmmc_mount(&cfg);
}

static const char * TAG = "config";

app_config_t g_app_config = {
    .screen_flip = 0,
    .wifi_mode = 0,
    .sound_enabled = true,
    .sound_mode = 2,
    .volume = 65,
    .ap_ssid = "Tab5-Debug",
    .clock_12h = false,
    .sort_field = 0,
    .sort_desc = true,
    .menubar_left = true,
    .touch_enabled = true,
    .np_bg_mode = 2,
    .fast_scroll_pages = 2,
    .fast_scroll_hold_ms = 150,
    .brightness = 70,
    .usb_mode = 0,
    .debug_log = false,
    .iap2_mode = true,
};

static volatile bool s_sd_ready;
static volatile bool s_sd_lost;
static bool s_sd_probe_started;
static TickType_t s_sd_probe_tick;
static unsigned s_sd_probe_failures;
static bool s_nvs_mode_valid;
static bool s_nvs_mode;

static char s_library_id[128];
static char s_library_short[16];
static char s_library_dir[48];
static char s_library_bin[80];
static char s_library_stars[80];
static char s_artwork_dir[80];
static app_config_sd_info_t s_sd_info;
static TickType_t s_sd_info_tick;

static void load_nvs_mode(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if(nvs_open("usbcfg", NVS_READONLY, &h) == ESP_OK) {
        if(nvs_get_u8(h, "iap2", &v) == ESP_OK) {
            s_nvs_mode_valid = true;
            s_nvs_mode = v != 0;
        }
        nvs_close(h);
    }
}

bool app_config_iap2_mode(void)
{
    return s_nvs_mode_valid ? s_nvs_mode : g_app_config.iap2_mode;
}

esp_err_t app_config_set_iap2_mode(bool enabled)
{
    g_app_config.iap2_mode = enabled;
    s_nvs_mode_valid = true;
    s_nvs_mode = enabled;

    nvs_handle_t h;
    esp_err_t err = nvs_open("usbcfg", NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    err = nvs_set_u8(h, "iap2", enabled ? 1 : 0);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_config_set_afc_job(uint8_t job)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("usbcfg", NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    err = nvs_set_u8(h, "afcjob", job);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

uint8_t app_config_peek_afc_job(void)
{
    nvs_handle_t h;
    uint8_t job = 0;
    if(nvs_open("usbcfg", NVS_READONLY, &h) != ESP_OK) return 0;
    if(nvs_get_u8(h, "afcjob", &job) != ESP_OK) job = 0;
    nvs_close(h);
    return job;
}

uint8_t app_config_take_afc_job(void)
{
    nvs_handle_t h;
    uint8_t job = 0;
    if(nvs_open("usbcfg", NVS_READWRITE, &h) != ESP_OK) return 0;
    if(nvs_get_u8(h, "afcjob", &job) != ESP_OK) job = 0;
    esp_err_t err = nvs_erase_key(h, "afcjob");
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? job : 0;
}

static bool is_hex_str(const char * s, size_t n)
{
    if(n == 0) return false;
    for(size_t i = 0; i < n; i++) {
        if(!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

/* iAP2 reports several logical libraries that share one 36-character device
 * GUID: the local iPod library ends in "-MPB-15.7.8", while Apple Music
 * Radio uses "-4954524C-15.7.8". Keep the shared base GUID so the SD cache
 * stays stable across both. */
static void library_base_id(const char * id, char * out, size_t out_len)
{
    out[0] = '\0';
    if(id == NULL) return;

    for(const char * p = id; *p != '\0'; p++) {
        if(strlen(p) < 36) break;
        bool uuid = isxdigit((unsigned char)p[0]) &&
                    isxdigit((unsigned char)p[1]) &&
                    isxdigit((unsigned char)p[2]) &&
                    isxdigit((unsigned char)p[3]) &&
                    isxdigit((unsigned char)p[4]) &&
                    isxdigit((unsigned char)p[5]) &&
                    isxdigit((unsigned char)p[6]) &&
                    isxdigit((unsigned char)p[7]) &&
                    p[8] == '-' &&
                    isxdigit((unsigned char)p[9]) &&
                    isxdigit((unsigned char)p[10]) &&
                    isxdigit((unsigned char)p[11]) &&
                    isxdigit((unsigned char)p[12]) &&
                    p[13] == '-' &&
                    isxdigit((unsigned char)p[14]) &&
                    isxdigit((unsigned char)p[15]) &&
                    isxdigit((unsigned char)p[16]) &&
                    isxdigit((unsigned char)p[17]) &&
                    p[18] == '-' &&
                    isxdigit((unsigned char)p[19]) &&
                    isxdigit((unsigned char)p[20]) &&
                    isxdigit((unsigned char)p[21]) &&
                    isxdigit((unsigned char)p[22]) &&
                    p[23] == '-' &&
                    isxdigit((unsigned char)p[24]) &&
                    isxdigit((unsigned char)p[25]) &&
                    isxdigit((unsigned char)p[26]) &&
                    isxdigit((unsigned char)p[27]) &&
                    isxdigit((unsigned char)p[28]) &&
                    isxdigit((unsigned char)p[29]) &&
                    isxdigit((unsigned char)p[30]) &&
                    isxdigit((unsigned char)p[31]) &&
                    isxdigit((unsigned char)p[32]) &&
                    isxdigit((unsigned char)p[33]) &&
                    isxdigit((unsigned char)p[34]) &&
                    isxdigit((unsigned char)p[35]);
        if(uuid) {
            size_t n = out_len > 36 ? 36 : out_len - 1;
            memcpy(out, p, n);
            out[n] = '\0';
            for(size_t i = 0; i < n; i++) {
                out[i] = (char)toupper((unsigned char)out[i]);
            }
            return;
        }
    }
    snprintf(out, out_len, "%s", id);
}

static void library_short_from_id(const char * id, char * out, size_t out_len)
{
    out[0] = '\0';
    if(id == NULL || id[0] == '\0') return;

    /* Prefer the last hyphen-separated 8-hex-digit segment. For a typical
     * media-library UUID this is the "-4954524C" part, which is exactly a
     * FAT 8.3-safe name. */
    const char * best = NULL;
    const char * p = id;
    while(*p != '\0') {
        const char * start = p;
        while(*p != '\0' && *p != '-') p++;
        size_t n = (size_t)(p - start);
        if(n == 8 && is_hex_str(start, n)) best = start;
        if(*p == '-') p++;
    }
    if(best != NULL) {
        snprintf(out, out_len, "%.8s", best);
        for(size_t i = 0; out[i] != '\0'; i++) {
            out[i] = (char)toupper((unsigned char)out[i]);
        }
        return;
    }

    /* Fallback: last 8 alphanumeric characters of the full string. */
    size_t n = strlen(id);
    size_t used = 0;
    for(size_t i = n; i > 0 && used < 8 && used + 1 < out_len; ) {
        i--;
        if(isalnum((unsigned char)id[i])) {
            memmove(out + 1, out, used);
            out[0] = (char)toupper((unsigned char)id[i]);
            used++;
        }
    }
    out[used] = '\0';
}

static void library_paths_apply(const char * id)
{
    char base[64];
    library_base_id(id, base, sizeof(base));
    snprintf(s_library_id, sizeof(s_library_id), "%s", base);
    library_short_from_id(s_library_id, s_library_short,
                          sizeof(s_library_short));

    if(s_library_short[0] != '\0') {
        snprintf(s_library_dir, sizeof(s_library_dir), "/sdcard/lib/%s",
                 s_library_short);
        snprintf(s_library_bin, sizeof(s_library_bin), "%s/library.bin",
                 s_library_dir);
        snprintf(s_artwork_dir, sizeof(s_artwork_dir), "%s/covers",
                 s_library_dir);
    }
    else {
        snprintf(s_library_dir, sizeof(s_library_dir), "/sdcard");
        snprintf(s_library_bin, sizeof(s_library_bin), "/sdcard/library.bin");
        snprintf(s_artwork_dir, sizeof(s_artwork_dir), "/sdcard/covers");
    }
    snprintf(s_library_stars, sizeof(s_library_stars), "%s/stars.csv",
             s_library_dir);
}

static bool path_exists(const char * path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static bool path_is_empty_dir(const char * path)
{
    DIR * d = opendir(path);
    if(d == NULL) return false;
    bool empty = true;
    struct dirent * ent;
    while((ent = readdir(d)) != NULL) {
        if(strcmp(ent->d_name, ".") != 0 &&
           strcmp(ent->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(d);
    return empty;
}

/* Older builds keyed the cache by the Apple Music Radio suffix
 * ("4954524C") instead of the shared base GUID.  If the new directory has no
 * library yet and exactly one old directory contains one, move it. */
static void library_migrate_previous_dir(void)
{
#if !defined(IAP2TAB5_FULL_LIBRARY_SYNC)
    return;
#endif
    if(s_library_short[0] == '\0' || !s_sd_ready) return;
    if(path_exists(s_library_bin) || !path_exists("/sdcard/lib")) return;

    char candidate[256] = "";
    DIR * d = opendir("/sdcard/lib");
    if(d == NULL) return;
    struct dirent * ent;
    while((ent = readdir(d)) != NULL) {
        if(strcmp(ent->d_name, ".") == 0 ||
           strcmp(ent->d_name, "..") == 0 ||
           strcmp(ent->d_name, s_library_short) == 0) {
            continue;
        }
        char bin[320];
        snprintf(bin, sizeof(bin), "/sdcard/lib/%s/library.bin",
                 ent->d_name);
        if(!path_exists(bin)) continue;
        if(candidate[0] != '\0') {
            candidate[0] = '\0';
            break;
        }
        snprintf(candidate, sizeof(candidate), "%s", ent->d_name);
    }
    closedir(d);

    if(candidate[0] == '\0') return;
    if(path_exists(s_library_dir) && !path_is_empty_dir(s_library_dir)) {
        return;
    }
    if(path_exists(s_library_dir)) rmdir(s_library_dir);

    char old_dir[320];
    snprintf(old_dir, sizeof(old_dir), "/sdcard/lib/%s", candidate);
    if(rename(old_dir, s_library_dir) == 0) {
        ESP_LOGI(TAG, "migrated library dir %s -> %s",
                 old_dir, s_library_dir);
    }
}

/* One-time move from the old flat /sdcard/library.bin + /sdcard/covers
 * layout into the per-library directory. Once the first device is migrated,
 * a second device gets its own empty directory. */
static void library_migrate_legacy(void)
{
    if(s_library_short[0] == '\0' || !s_sd_ready) return;

    mkdir("/sdcard/lib", 0777);
    library_migrate_previous_dir();
    mkdir(s_library_dir, 0777);

    if(!path_exists(s_library_bin) && path_exists("/sdcard/library.bin")) {
        if(rename("/sdcard/library.bin", s_library_bin) == 0) {
            ESP_LOGI(TAG, "migrated library.bin -> %s", s_library_bin);
        }
    }
    /* The first call to app_config_set_library_id() may have created an
     * empty covers directory before the legacy one was migrated. Remove the
     * empty placeholder so the rename can succeed. */
    if(path_exists(s_artwork_dir)) {
        DIR * d = opendir(s_artwork_dir);
        bool empty = true;
        if(d != NULL) {
            struct dirent * ent;
            while((ent = readdir(d)) != NULL) {
                if(strcmp(ent->d_name, ".") != 0 &&
                   strcmp(ent->d_name, "..") != 0) {
                    empty = false;
                    break;
                }
            }
            closedir(d);
        }
        if(empty) rmdir(s_artwork_dir);
    }
    if(!path_exists(s_artwork_dir) && path_exists("/sdcard/covers")) {
        if(rename("/sdcard/covers", s_artwork_dir) == 0) {
            ESP_LOGI(TAG, "migrated covers -> %s", s_artwork_dir);
        }
    }

    char legacy_pack[64] = "/sdcard/thumbs.bin";
    char legacy_idx[64] = "/sdcard/thumbs.idx";
    char new_pack[128];
    char new_idx[128];
    snprintf(new_pack, sizeof(new_pack), "%s/thumbs.bin", s_library_dir);
    snprintf(new_idx, sizeof(new_idx), "%s/thumbs.idx", s_library_dir);
    if(strcmp(new_pack, legacy_pack) != 0 &&
       !path_exists(new_pack) && path_exists(legacy_pack)) {
        rename(legacy_pack, new_pack);
    }
    if(strcmp(new_idx, legacy_idx) != 0 &&
       !path_exists(new_idx) && path_exists(legacy_idx)) {
        rename(legacy_idx, new_idx);
    }
}

static void library_dirs_ensure(void)
{
    if(!s_sd_ready) return;
    if(s_library_short[0] != '\0') {
        mkdir("/sdcard/lib", 0777);
        mkdir(s_library_dir, 0777);
    }
    mkdir(s_artwork_dir, 0777);
}

static void load_nvs_library_id(void)
{
    nvs_handle_t h;
    char buf[128];
    size_t len = sizeof(buf);
    if(nvs_open("usbcfg", NVS_READONLY, &h) != ESP_OK) return;
    if(nvs_get_str(h, "libid", buf, &len) == ESP_OK) {
        library_paths_apply(buf);
        snprintf(g_app_config.library_id, sizeof(g_app_config.library_id),
                 "%s", s_library_id);
    }
    nvs_close(h);
}

const char * app_config_library_id(void) { return s_library_id; }
const char * app_config_library_short(void) { return s_library_short; }
const char * app_config_library_dir(void) { return s_library_dir; }
const char * app_config_library_bin_path(void) { return s_library_bin; }
const char * app_config_library_stars_path(void) { return s_library_stars; }
const char * app_config_artwork_dir(void) { return s_artwork_dir; }

esp_err_t app_config_set_library_id(const char * id)
{
    char base[64];
    library_base_id(id, base, sizeof(base));
    id = base;
    if(strcmp(id, s_library_id) == 0) {
        library_dirs_ensure();
        return ESP_OK;
    }

    library_paths_apply(id);
    snprintf(g_app_config.library_id, sizeof(g_app_config.library_id),
             "%s", s_library_id);
    library_dirs_ensure();

    ESP_LOGI(TAG, "library id '%s' -> short '%s', dir '%s'",
             s_library_id, s_library_short, s_library_dir);

    nvs_handle_t h;
    esp_err_t err = nvs_open("usbcfg", NVS_READWRITE, &h);
    if(err != ESP_OK) return err;
    err = nvs_set_str(h, "libid", s_library_id);
    if(err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void set_defaults(void)
{
    memset(&g_app_config, 0, sizeof(g_app_config));
    g_app_config.screen_flip = 0;
    g_app_config.wifi_mode = 0;
    g_app_config.sound_enabled = true;
    g_app_config.sound_mode = 2;
    g_app_config.volume = 65;
    snprintf(g_app_config.ap_ssid, sizeof(g_app_config.ap_ssid), "Tab5-Debug");
    g_app_config.clock_12h = false;
    g_app_config.sort_field = 0;
    g_app_config.sort_desc = true;
    g_app_config.menubar_left = true;
    g_app_config.touch_enabled = true;
    g_app_config.np_bg_mode = 2;
    g_app_config.fast_scroll_pages = 2;
    g_app_config.fast_scroll_hold_ms = 150;
    g_app_config.brightness = 70;
    g_app_config.screen_off_minutes = 0;
    g_app_config.usb_mode = 0;
    g_app_config.theme = 0;
    g_app_config.boot_screen = 0;
    g_app_config.ble_enabled = true;
    g_app_config.debug_log = false;
    g_app_config.charge_enabled = true;
    g_app_config.qc_high = false;
    g_app_config.iap2_mode = true;
}

static void load_from_json(const char * text)
{
    cJSON * root = cJSON_Parse(text);
    if(root == NULL) {
        ESP_LOGW(TAG, "config.json is not valid JSON, using defaults");
        return;
    }

    const cJSON * item = cJSON_GetObjectItemCaseSensitive(root, "screen_flip");
    if(cJSON_IsNumber(item)) {
        int v = item->valueint;
        g_app_config.screen_flip = (v == 180) ? 180 : 0;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "wifi_mode");
    if(cJSON_IsNumber(item)) {
        g_app_config.wifi_mode = (item->valueint == 1) ? 1 : 0;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "sound_enabled");
    if(cJSON_IsBool(item)) {
        g_app_config.sound_enabled = cJSON_IsTrue(item);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "sound_mode");
    if(cJSON_IsNumber(item)) {
        int mode = item->valueint;
        g_app_config.sound_mode = (uint8_t)(mode < 0 ? 0 : (mode > 2 ? 2 : mode));
    }
    else {
        /* Older configs only had the on/off flag. */
        g_app_config.sound_mode = g_app_config.sound_enabled ? 2 : 0;
    }
    g_app_config.sound_enabled = g_app_config.sound_mode != 0;

    item = cJSON_GetObjectItemCaseSensitive(root, "volume");
    if(cJSON_IsNumber(item)) {
        int v = item->valueint;
        if(v < 0) v = 0;
        if(v > 100) v = 100;
        g_app_config.volume = (uint8_t)v;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "ap_ssid");
    if(cJSON_IsString(item) && item->valuestring[0] != '\0') {
        snprintf(g_app_config.ap_ssid, sizeof(g_app_config.ap_ssid), "%s",
                 item->valuestring);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "clock_12h");
    if(cJSON_IsBool(item)) g_app_config.clock_12h = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "sort_field");
    if(cJSON_IsNumber(item)) g_app_config.sort_field = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "sort_desc");
    if(cJSON_IsBool(item)) g_app_config.sort_desc = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "menubar_left");
    if(cJSON_IsBool(item)) g_app_config.menubar_left = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "touch_enabled");
    if(cJSON_IsBool(item)) g_app_config.touch_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "np_bg_mode");
    if(cJSON_IsNumber(item)) {
        /* The removed album-seeded background now uses Cover gradient. */
        g_app_config.np_bg_mode = item->valueint == 3 ? 2 : (uint8_t)item->valueint;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if(cJSON_IsNumber(item))
        g_app_config.brightness = (uint8_t)screen_brightness_normalize(item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_pages");
    if(cJSON_IsNumber(item) && item->valueint >= 0 && item->valueint <= 5)
        g_app_config.fast_scroll_pages = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_hold_ms");
    if(cJSON_IsNumber(item) && item->valuedouble == item->valueint)
        g_app_config.fast_scroll_hold_ms = (uint16_t)fast_scroll_hold_normalize(item->valueint);
    item = cJSON_GetObjectItemCaseSensitive(root, "screen_off_minutes");
    if(cJSON_IsNumber(item) && item->valuedouble == item->valueint &&
       (item->valueint == 0 || item->valueint == 30 || item->valueint == 60)) {
        g_app_config.screen_off_minutes = (uint8_t)item->valueint;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "usb_mode");
    if(cJSON_IsNumber(item)) {
        g_app_config.usb_mode = (item->valueint == 2) ? 2 : 0;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if(cJSON_IsNumber(item)) {
        int v = item->valueint;
        g_app_config.theme = (uint8_t)(v >= 0 && v <= 2 ? v : 0);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "boot_screen");
    if(cJSON_IsNumber(item)) {
        g_app_config.boot_screen = (uint8_t)(item->valueint == 1 ? 1 : 0);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "ble_enabled");
    if(cJSON_IsBool(item)) g_app_config.ble_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "debug_log");
    if(cJSON_IsBool(item)) g_app_config.debug_log = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "charge_enabled");
    if(cJSON_IsBool(item)) g_app_config.charge_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "qc_high");
    if(cJSON_IsBool(item)) g_app_config.qc_high = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "iap2_mode");
    if(cJSON_IsBool(item)) g_app_config.iap2_mode = cJSON_IsTrue(item);
    item = cJSON_GetObjectItemCaseSensitive(root, "library_id");
    if(cJSON_IsString(item) && item->valuestring[0] != '\0') {
        snprintf(g_app_config.library_id, sizeof(g_app_config.library_id),
                 "%s", item->valuestring);
    }

    cJSON_Delete(root);
}

esp_err_t app_config_init(void)
{
    set_defaults();
    library_paths_apply("");
    load_nvs_mode();
    if(s_nvs_mode_valid) g_app_config.iap2_mode = s_nvs_mode;
    load_nvs_library_id();
    ESP_LOGI(TAG, "NVS USB mode: %s (valid=%d), library='%s'",
             g_app_config.iap2_mode ? "iAP2 config 2" : "AFC config 4",
             (int)s_nvs_mode_valid, s_library_id);

    esp_err_t err = sd_mount_default();
    if(err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_sd_ready = true;
        sdmmc_card_t * card = bsp_sdcard_get_handle();
        if(card != NULL) {
            ESP_LOGI(TAG, "SD card '%s' %u kHz %u-bit%s", card->cid.name,
                     (unsigned)card->real_freq_khz,
                     (unsigned)(1u << card->log_bus_width),
                     card->is_uhs1 ? " UHS-I" : "");
        }
        app_config_sd_info_refresh();
        library_migrate_legacy();
        library_dirs_ensure();
    }
    else {
        ESP_LOGW(TAG, "SD card mount failed (0x%x); using defaults", err);
        return err;
    }

    FILE * f = fopen(APP_CONFIG_PATH, "rb");
    if(f == NULL) {
        ESP_LOGW(TAG, "%s not found; creating defaults", APP_CONFIG_PATH);
        return app_config_save();
    }

    char buffer[1024];
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    buffer[n] = '\0';

    load_from_json(buffer);
    if(s_nvs_mode_valid) g_app_config.iap2_mode = s_nvs_mode;
    if(s_library_id[0] == '\0' && g_app_config.library_id[0] != '\0') {
        app_config_set_library_id(g_app_config.library_id);
    }
    ESP_LOGI(TAG, "loaded config: flip=%u wifi=%u sound=%d volume=%u iap2=%d",
             (unsigned)g_app_config.screen_flip, (unsigned)g_app_config.wifi_mode,
             (int)g_app_config.sound_enabled, (unsigned)g_app_config.volume,
             (int)g_app_config.iap2_mode);
    return ESP_OK;
}

esp_err_t app_config_save(void)
{
    if(!s_sd_ready) return ESP_ERR_INVALID_STATE;

    cJSON * root = cJSON_CreateObject();
    if(root == NULL) return ESP_ERR_NO_MEM;

    cJSON_AddNumberToObject(root, "screen_flip", g_app_config.screen_flip);
    cJSON_AddNumberToObject(root, "wifi_mode", g_app_config.wifi_mode);
    cJSON_AddBoolToObject(root, "sound_enabled", g_app_config.sound_enabled);
    cJSON_AddNumberToObject(root, "sound_mode", g_app_config.sound_mode);
    cJSON_AddNumberToObject(root, "volume", g_app_config.volume);
    cJSON_AddStringToObject(root, "ap_ssid", g_app_config.ap_ssid);
    cJSON_AddBoolToObject(root, "clock_12h", g_app_config.clock_12h);
    cJSON_AddNumberToObject(root, "sort_field", g_app_config.sort_field);
    cJSON_AddBoolToObject(root, "sort_desc", g_app_config.sort_desc);
    cJSON_AddBoolToObject(root, "menubar_left", g_app_config.menubar_left);
    cJSON_AddBoolToObject(root, "touch_enabled", g_app_config.touch_enabled);
    cJSON_AddNumberToObject(root, "np_bg_mode", g_app_config.np_bg_mode);
    cJSON_AddNumberToObject(root, "fast_scroll_pages", g_app_config.fast_scroll_pages);
    cJSON_AddNumberToObject(root, "fast_scroll_hold_ms", g_app_config.fast_scroll_hold_ms);
    cJSON_AddNumberToObject(root, "brightness", g_app_config.brightness);
    cJSON_AddNumberToObject(root, "screen_off_minutes", g_app_config.screen_off_minutes);
    cJSON_AddNumberToObject(root, "usb_mode", g_app_config.usb_mode);
    cJSON_AddNumberToObject(root, "theme", g_app_config.theme);
    cJSON_AddNumberToObject(root, "boot_screen", g_app_config.boot_screen);
    cJSON_AddBoolToObject(root, "ble_enabled", g_app_config.ble_enabled);
    cJSON_AddBoolToObject(root, "debug_log", g_app_config.debug_log);
    cJSON_AddBoolToObject(root, "charge_enabled", g_app_config.charge_enabled);
    cJSON_AddBoolToObject(root, "qc_high", g_app_config.qc_high);
    cJSON_AddBoolToObject(root, "iap2_mode", g_app_config.iap2_mode);
    cJSON_AddStringToObject(root, "library_id", g_app_config.library_id);

    char * text = cJSON_Print(root);
    cJSON_Delete(root);
    if(text == NULL) return ESP_ERR_NO_MEM;

    const char * tmp_path = APP_CONFIG_PATH ".tmp";
    FILE * f = fopen(tmp_path, "wb");
    if(f == NULL) {
        cJSON_free(text);
        return ESP_FAIL;
    }

    size_t len = strlen(text);
    bool ok = fwrite(text, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    fclose(f);
    cJSON_free(text);

    if(!ok) {
        remove(tmp_path);
        return ESP_FAIL;
    }

    remove(APP_CONFIG_PATH);
    if(rename(tmp_path, APP_CONFIG_PATH) != 0) {
        remove(tmp_path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved %s", APP_CONFIG_PATH);
    return ESP_OK;
}

bool app_config_sd_ready(void)
{
    return s_sd_ready && !s_sd_lost;
}

void app_config_sd_poll(void)
{
    if(!s_sd_ready || s_sd_lost) return;
    TickType_t now = xTaskGetTickCount();
    if(s_sd_probe_started && now - s_sd_probe_tick < pdMS_TO_TICKS(2000)) return;
    s_sd_probe_started = true;
    s_sd_probe_tick = now;
    sdmmc_card_t *card = bsp_sdcard_get_handle();
    esp_err_t err = card ? sdmmc_get_status(card) : ESP_ERR_INVALID_STATE;
    if(err == ESP_OK) { s_sd_probe_failures = 0; return; }
    /* One transient status failure must not disable a healthy library. */
    if(++s_sd_probe_failures < 2) return;
    s_sd_lost = true;
    s_sd_ready = false;
    ESP_LOGW(TAG, "SD card unavailable (%s); reinsert and reboot to reload the library", esp_err_to_name(err));
}

static const char * sd_vendor_name(int mfg_id)
{
    switch(mfg_id) {
        case 0x01: return "Panasonic";
        case 0x02: return "Toshiba";
        case 0x03: return "SanDisk";
        case 0x1B: return "Samsung";
        case 0x1D: return "ADATA";
        case 0x27: return "Kingston";
        case 0x28: return "Lexar";
        case 0x41: return "Kingston";
        case 0x74: return "Transcend";
        default: return NULL;
    }
}

void app_config_sd_info_refresh(void)
{
    memset(&s_sd_info, 0, sizeof(s_sd_info));
    s_sd_info_tick = xTaskGetTickCount();
    if(!s_sd_ready) return;

    sdmmc_card_t * card = bsp_sdcard_get_handle();
    if(card == NULL) return;

    s_sd_info.ready = true;
    const char * vendor = sd_vendor_name(card->cid.mfg_id);
    if(vendor != NULL) {
        snprintf(s_sd_info.vendor, sizeof(s_sd_info.vendor), "%s", vendor);
    }
    else {
        snprintf(s_sd_info.vendor, sizeof(s_sd_info.vendor), "%.8s (0x%02X)",
                 card->cid.name, (unsigned)card->cid.mfg_id);
    }

    uint64_t card_bytes = (uint64_t)card->csd.capacity *
                          (uint64_t)card->csd.sector_size;
    const char * standard = card_bytes > (32ULL << 30) ? "SDXC" : "SDHC";
    const char * speed = card->is_uhs1 ? "UHS-I" :
                         (card->is_ddr ? "DDR" : "HS");
    snprintf(s_sd_info.type, sizeof(s_sd_info.type), "%s %s %u-bit %uMHz",
             standard, speed, (unsigned)(1u << card->log_bus_width),
             (unsigned)(card->real_freq_khz / 1000));

    FATFS * fs = NULL;
    DWORD free_clusters = 0;
    FRESULT fr = f_getfree("0:", &free_clusters, &fs);
    const char * fstype = "FAT32";
    if(fr == FR_OK && fs != NULL) {
        switch(fs->fs_type) {
            case FS_FAT12: fstype = "FAT12"; break;
            case FS_FAT16: fstype = "FAT16"; break;
            case FS_FAT32: fstype = "FAT32"; break;
            case FS_EXFAT: fstype = "exFAT"; break;
            default:       fstype = "FAT"; break;
        }
    }
    snprintf(s_sd_info.filesystem, sizeof(s_sd_info.filesystem), "%s",
             fstype);

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if(esp_vfs_fat_info("/sdcard", &total, &free_bytes) == ESP_OK) {
        s_sd_info.total_bytes = total;
        s_sd_info.used_bytes = total >= free_bytes ? total - free_bytes : 0;
    }
}

bool app_config_sd_info(app_config_sd_info_t * out)
{
    if(out == NULL) return false;
    *out = s_sd_info;
    return app_config_sd_ready() && s_sd_info.ready;
}

void app_config_sd_info_refresh_idle(void)
{
    TickType_t now = xTaskGetTickCount();
    if(s_sd_info_tick == 0 || (now - s_sd_info_tick) > pdMS_TO_TICKS(5000)) {
        app_config_sd_info_refresh();
    }
}

esp_err_t app_config_hash_file(const char * path, uint64_t * size,
                               char * hex, size_t hex_len)
{
    int fd = open(path, O_RDONLY);
    if(fd < 0) return ESP_ERR_NOT_FOUND;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    /* Aligned, sector-sized reads use the P4 SDMMC multi-block DMA path.
     * Use POSIX read: size-optimized newlib fread with _IONBF reads single
     * bytes. A small unaligned stdio/stack buffer reads one sector at a
     * time. Keep the old bounded fallback when PSRAM is scarce. */
    uint8_t fallback[4096];
    uint8_t * bulk = heap_caps_aligned_alloc(128, 64 * 1024,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t * buf = bulk != NULL ? bulk : fallback;
    size_t capacity = bulk != NULL ? 64 * 1024 : sizeof(fallback);
    uint64_t total = 0;
    ssize_t n;
    while((n = read(fd, buf, capacity)) > 0) {
        mbedtls_sha256_update(&ctx, buf, n);
        total += n;
    }
    bool ok = n == 0;
    close(fd);
    heap_caps_free(bulk);

    unsigned char digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    if(hex != NULL && hex_len > 0) {
        size_t i;
        for(i = 0; i < 32 && (i * 2 + 2) < hex_len; i++) {
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        hex[i * 2] = '\0';
    }
    if(size != NULL) *size = total;
    return ok ? ESP_OK : ESP_FAIL;
}

bool app_config_sd_format(void)
{
    if(s_sd_ready) {
        bsp_sdcard_unmount();
        s_sd_ready = false;
    }
    else {
        /* Release the stale LDO from the failed boot mount. */
        bsp_sdcard_unmount();
    }

    ESP_LOGW(TAG, "formatting SD card (all data will be lost)");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 12,
        .allocation_unit_size = 16 * 1024,
    };
    bsp_sdcard_cfg_t cfg = {
        .mount = &mount_config,
    };

    esp_err_t err = bsp_sdcard_sdmmc_mount(&cfg);
    if(err == ESP_OK) {
        s_sd_ready = true;
        ESP_LOGI(TAG, "SD card formatted and mounted at %s",
                 APP_CONFIG_SD_MOUNT);
        app_config_sd_info_refresh();
        library_dirs_ensure();
        return true;
    }

    ESP_LOGE(TAG, "SD format/mount failed (0x%x)", err);
    return false;
}

bool app_config_sd_ensure(void)
{
    /* Cached readers and file handles belong to the original mount. */
    if(s_sd_lost) return false;
    if(s_sd_ready) return true;

    /* The first mount attempt at boot leaves the on-chip SD LDO registered
     * even when no card is present. Release it before retrying. */
    bsp_sdcard_unmount();

    esp_err_t err = sd_mount_default();
    if(err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_sd_ready = true;
        sdmmc_card_t * card = bsp_sdcard_get_handle();
        if(card != NULL) {
            ESP_LOGI(TAG, "SD card '%s' %u kHz %u-bit%s", card->cid.name,
                     (unsigned)card->real_freq_khz,
                     (unsigned)(1u << card->log_bus_width),
                     card->is_uhs1 ? " UHS-I" : "");
        }
        ESP_LOGI(TAG, "SD card mounted on demand");
        app_config_sd_info_refresh();
        library_migrate_legacy();
        library_dirs_ensure();
        return true;
    }
    ESP_LOGW(TAG, "SD card still unavailable (0x%x)", err);
    return false;
}

typedef struct {
    char ssid[33];
    char password[65];
} wifi_credentials_t;

static esp_err_t wifi_credentials_load_nvs(wifi_credentials_t * credentials)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_backup", NVS_READONLY, &handle);
    if(err != ESP_OK) return err;
    size_t size = sizeof(*credentials);
    err = nvs_get_blob(handle, "station", credentials, &size);
    nvs_close(handle);
    if(err == ESP_OK && (size != sizeof(*credentials) || !credentials->ssid[0] ||
       !memchr(credentials->ssid, '\0', sizeof(credentials->ssid)) ||
       !memchr(credentials->password, '\0', sizeof(credentials->password)))) {
        return ESP_ERR_INVALID_STATE;
    }
    return err;
}

static void wifi_credentials_save_nvs(const wifi_credentials_t * credentials)
{
    wifi_credentials_t saved;
    if(wifi_credentials_load_nvs(&saved) == ESP_OK &&
       memcmp(&saved, credentials, sizeof(saved)) == 0) return;

    /* One blob keeps the SSID/password pair together. Skip unchanged values
     * above to avoid a flash write on every boot or Wi-Fi mode switch. */
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_backup", NVS_READWRITE, &handle);
    if(err == ESP_OK) {
        err = nvs_set_blob(handle, "station", credentials, sizeof(*credentials));
        if(err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if(err == ESP_OK) ESP_LOGI(TAG, "Wi-Fi credentials backed up to NVS");
    else ESP_LOGW(TAG, "Wi-Fi NVS backup failed: %s", esp_err_to_name(err));
}

static esp_err_t wifi_credentials_read_sd(wifi_credentials_t * credentials)
{
    if(!app_config_sd_ready()) return ESP_ERR_INVALID_STATE;

    FILE * f = fopen(APP_WIFI_CRED_PATH, "rb");
    if(f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[128];
    int field = 0;
    while(field < 2 && fgets(line, sizeof(line), f) != NULL) {
        size_t n = strlen(line);
        while(n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                        line[n - 1] == ' ' || line[n - 1] == '\t')) {
            line[--n] = '\0';
        }
        if(n == 0) continue;
        char * dest = field == 0 ? credentials->ssid : credentials->password;
        size_t capacity = field == 0 ? sizeof(credentials->ssid) : sizeof(credentials->password);
        if(n >= capacity) {
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(dest, line, n + 1);
        field++;
    }
    bool failed = ferror(f) != 0;
    if(fclose(f) != 0) failed = true;
    if(failed) return ESP_FAIL;

    if(field == 0) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}

esp_err_t app_config_read_wifi_credentials(char * ssid, size_t ssid_len,
                                           char * password, size_t password_len)
{
    if(ssid == NULL || ssid_len == 0) return ESP_ERR_INVALID_ARG;
    ssid[0] = '\0';
    if(password != NULL && password_len > 0) password[0] = '\0';
    wifi_credentials_t credentials = {0};
    esp_err_t err = wifi_credentials_read_sd(&credentials);
    if(err == ESP_OK) {
        /* wifi.txt is authoritative: a valid SD file replaces the NVS copy,
         * even if the caller requests only a shortened SSID or no password. */
        wifi_credentials_save_nvs(&credentials);
    }
    else {
        err = wifi_credentials_load_nvs(&credentials);
        if(err != ESP_OK) return err;
        ESP_LOGW(TAG, "SD Wi-Fi credentials unavailable; using NVS backup");
    }
    snprintf(ssid, ssid_len, "%s", credentials.ssid);
    if(password != NULL && password_len > 0) {
        snprintf(password, password_len, "%s", credentials.password);
    }
    return ESP_OK;
}
