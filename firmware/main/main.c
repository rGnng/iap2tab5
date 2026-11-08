/*
 * M5Stack Tab5 firmware entry point.
 *
 * Display, touch and backlight come from esp-bsp's m5stack_tab5 BSP. The UI
 * itself is the shared app/ui code also used by the desktop simulator.
 */

#include "app_config.h"
#include "ble_remote.h"
#include "charge.h"
#include "debug_log.h"
#include "display_diag.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "bsp/esp-bsp.h"
#include "feedback.h"
#include "serial_input.h"
#include "ui.h"
#include "boot.h"
#include "player.h"
#include "rtc_time.h"
#include "usb_hid_input.h"
#include "usbmux_host.h"
#include "lockdown_client.h"
#include "afc_client.h"
#include "thumb_cache.h"
#include "usb_host_core.h"
#include "iap2_host.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

static const char * TAG = "iap2tab5";

static lv_display_t * s_display;
static int s_applied_flip = -1;
static int s_applied_volume = -1;
static int s_applied_sound = -1;
static int s_applied_wifi_mode = -1;
static int s_applied_brightness = -1;
static volatile bool s_save_pending;

/* 0 = USB-A HS only, 1 = USB-C FS only, 2 = both root ports.
 *
 * Dual-host is verified on the Tab5 (USB-A HS iPod + USB-C FS D100H behind
 * a powered hub). */
#define ENABLE_USB_DUAL_HOST 1

static unsigned usb_peripheral_map_for_mode(uint8_t mode)
{
    switch(mode) {
        case 2:
#if ENABLE_USB_DUAL_HOST
            return (1u << 0) | (1u << 1);
#else
            ESP_LOGW(TAG, "USB mode 2 requested; dual host disabled, using USB-A HS");
            return 1u << 0;
#endif
        default: return 1u << 0;
    }
}

static void apply_screen_flip(void)
{
    if(s_display == NULL) return;
    if(bsp_display_lock(0)) {
        lv_display_set_rotation(s_display, g_app_config.screen_flip == 180
                                             ? LV_DISPLAY_ROTATION_270
                                             : LV_DISPLAY_ROTATION_90);
        bsp_display_unlock();
        ESP_LOGI(TAG, "screen flip %u", (unsigned)g_app_config.screen_flip);
    }
}

static void publish_system_config(void);

/* ---- Apple File Conduit settings page ---------------------------------- */
static volatile int s_afc_job_active;
static volatile int s_afc_job_kind;
static bool s_afc_job_finished, s_afc_job_failed;
static char s_afc_job_status[96];
static int s_afc_cached_albums;
static int s_afc_thumb_count;
static int s_afc_cover_count;
static uint64_t s_afc_thumb_bytes;
static uint64_t s_afc_cover_bytes;
static uint64_t s_afc_original_bytes;
static TickType_t s_afc_cache_tick;

static bool name_has_ext(const char * name, const char * ext)
{
    size_t n = strlen(name);
    size_t e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static void afc_scan_cached_artwork(void)
{
    TickType_t now = xTaskGetTickCount();
    if(s_afc_cache_tick != 0 &&
       (now - s_afc_cache_tick) < pdMS_TO_TICKS(5000)) {
        return;
    }
    s_afc_cache_tick = now;
    s_afc_cached_albums = 0;
    s_afc_thumb_count = 0;
    s_afc_cover_count = 0;
    s_afc_thumb_bytes = 0;
    s_afc_cover_bytes = 0;
    s_afc_original_bytes = 0;

    const char * art_dir = app_config_artwork_dir();
    DIR * dir = opendir(art_dir);
    if(dir == NULL) return;
    struct dirent * ent;
    while((ent = readdir(dir)) != NULL) {
        const char * name = ent->d_name;
        char path[300];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", art_dir, name);
        if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if(name_has_ext(name, ".thm")) {
            s_afc_thumb_count++;
            s_afc_thumb_bytes += (uint64_t)st.st_size;
        }
        else if(name_has_ext(name, ".jpg") || name_has_ext(name, ".png") ||
                name_has_ext(name, ".wbp") || name_has_ext(name, ".gif")) {
            s_afc_original_bytes += (uint64_t)st.st_size;
        }
    }
    closedir(dir);

    /* The packed cache is authoritative once it has any valid slot. Legacy
     * .thm files are still counted until the first index pass packs them. */
    uint32_t packed = thumb_cache_count();
    if(packed > 0) {
        s_afc_thumb_count = (int)packed;
        s_afc_thumb_bytes = thumb_cache_bytes();
    }
    s_afc_cached_albums = s_afc_thumb_count > 0 ? s_afc_thumb_count
                                                : s_afc_cover_count;
}

static void ui_afc_status_handler(ui_afc_status_t * out)
{
    memset(out, 0, sizeof(*out));
    out->available = true;
    out->afc_mode = !app_config_iap2_mode();
    out->sd_ready = app_config_sd_ready();
    out->connected = afc_client_is_ready();
    out->usb_connected = usbmux_host_is_ready();
    lockdown_pair_status_t pairing;
    lockdown_client_pair_status(&pairing);
    out->pair_busy = pairing.busy;
    out->pair_trusted = pairing.trusted && out->usb_connected;
    snprintf(out->pair_status, sizeof(out->pair_status), "%s", pairing.message);
    out->job_finished = s_afc_job_finished;
    out->job_failed = s_afc_job_failed;
    lockdown_artwork_progress_t progress;
    lockdown_client_artwork_progress(&progress);
    out->job_active = s_afc_job_active != 0 || progress.active;
    out->job_kind = progress.active ? progress.kind : s_afc_job_kind;
    out->sync_total = progress.total;
    out->sync_phase = progress.phase;
    out->sync_attempted = progress.attempted;
    out->sync_copied = progress.copied;
    out->sync_failed = progress.failed;
    out->sync_current_id = progress.current_id;
    snprintf(out->sync_album, sizeof(out->sync_album), "%s", progress.album);
    snprintf(out->sync_artist, sizeof(out->sync_artist), "%s", progress.artist);
    snprintf(out->status, sizeof(out->status), "%s",
             s_afc_job_finished ? s_afc_job_status : lockdown_client_status());

    snprintf(out->usb_status, sizeof(out->usb_status), "%s",
             lockdown_client_status());
    snprintf(out->usb_mode, sizeof(out->usb_mode), "%s",
             out->afc_mode ? "AFC (config 4)" : "iAP2 (config 2)");
    const char * operation = "idle";
    if(out->job_active) {
        if(progress.phase == 1) operation = "database sync";
        else if(progress.phase == 2) operation = "album art cache";
        else if(progress.phase == 3) operation = "convert thumbnail";
        else operation = "preparing";
    }
    snprintf(out->usb_operation, sizeof(out->usb_operation), "%s", operation);
    snprintf(out->library_suffix, sizeof(out->library_suffix), "%s",
             app_config_library_short());
    out->library_albums = player_library_album_count();
    out->library_tracks = player_library_track_count();
    snprintf(out->library_db_item, sizeof(out->library_db_item),
             "%d albums / %d tracks", out->library_albums,
             out->library_tracks);
    struct stat lib_st;
    if(stat(app_config_library_bin_path(), &lib_st) == 0) {
        out->library_db_bytes = (uint64_t)lib_st.st_size;
        struct tm tmv;
        localtime_r(&lib_st.st_mtime, &tmv);
        strftime(out->library_updated, sizeof(out->library_updated),
                 "%Y-%m-%d %H:%M", &tmv);
    }
    else {
        out->library_db_bytes = 0;
        snprintf(out->library_updated, sizeof(out->library_updated), "—");
    }
    char cache_path[160];
    snprintf(cache_path, sizeof(cache_path), "%s/thumbs.bin",
             app_config_library_dir());
    struct stat cache_st;
    out->cover_cache_bytes = stat(cache_path, &cache_st) == 0
                                 ? (uint64_t)cache_st.st_size : 0;
    snprintf(out->iap2_status, sizeof(out->iap2_status), "%s",
             app_config_iap2_mode() ? iap2_host_status()
                                    : "Disabled (AFC mode)");

    if(!out->job_active) {
        app_config_sd_info_refresh_idle();
    }
    app_config_sd_info_t sd;
    if(app_config_sd_info(&sd)) {
        out->sd_info_ready = true;
        snprintf(out->sd_vendor, sizeof(out->sd_vendor), "%s", sd.vendor);
        snprintf(out->sd_type, sizeof(out->sd_type), "%s", sd.type);
        snprintf(out->sd_fs, sizeof(out->sd_fs), "%s", sd.filesystem);
        out->sd_used_bytes = sd.used_bytes;
        out->sd_total_bytes = sd.total_bytes;
    }
    else {
        out->sd_info_ready = false;
    }
    /* Walking the covers directory costs a stat() per file. Do it when the
     * screen is idle, not every 500 ms while a sync is running: the sync
     * progress fields below are already updated live by the job task. */
    if(!out->job_active) {
        afc_scan_cached_artwork();
    }
    out->cached_albums = s_afc_cached_albums;
    out->thumb_count = s_afc_thumb_count;
    out->cover_count = s_afc_cover_count;
    out->thumb_bytes = s_afc_thumb_bytes;
    out->cover_bytes = s_afc_cover_bytes;
    out->original_bytes = s_afc_original_bytes;
    out->cache_bytes = out->thumb_bytes + out->original_bytes;
}

static void afc_job_task(void * arg);

static bool afc_start_job_task(uint8_t job)
{
    s_afc_job_finished = s_afc_job_failed = false;
    s_afc_job_status[0] = 0;
    if(job != 0) app_config_set_afc_job(job);
    s_afc_job_active = job ? job : 1;
    s_afc_job_kind = job;
    BaseType_t ok = xTaskCreateWithCaps(
        afc_job_task, "afc_job", 24 * 1024, NULL, 4, NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(ok != pdPASS) {
        s_afc_job_active = 0;
        s_afc_job_finished = s_afc_job_failed = true;
        snprintf(s_afc_job_status, sizeof(s_afc_job_status), "Not enough memory to start sync. Try again.");
        ESP_LOGE(TAG, "failed to create AFC job task");
        return false;
    }
    return true;
}

static void afc_job_task(void * arg)
{
    LV_UNUSED(arg);
    uint8_t job = app_config_take_afc_job();
    if(job != 1 && job != 2) {
        s_afc_job_active = 0;
        s_afc_job_finished = s_afc_job_failed = true;
        snprintf(s_afc_job_status, sizeof(s_afc_job_status),
                 "Could not read the sync request. Try again.");
        vTaskDelete(NULL);
        return;
    }

    s_afc_job_active = job;
    s_afc_job_kind = job;
    ESP_LOGW(TAG, "AFC job %u starting sync", job);

    /* The iPod / lockdownd session may still be coming up. */
    for(int i = 0; i < 240 && !afc_client_is_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t result = ESP_ERR_TIMEOUT;
    if(afc_client_is_ready()) {
        esp_err_t err = lockdown_client_sync_artwork();
        result = err;
        ESP_LOGW(TAG, "AFC job %u result: %s", job, esp_err_to_name(err));
    }
    else {
        ESP_LOGW(TAG, "AFC job %u: AFC did not become ready", job);
    }

    lockdown_artwork_progress_t progress;
    lockdown_client_artwork_progress(&progress);
    s_afc_job_failed = result != ESP_OK || progress.failed > 0;
    if(progress.failed > 0 && progress.phase > 1) {
        snprintf(s_afc_job_status, sizeof(s_afc_job_status),
            "%d of %d covers ready; %d could not be %s.",
            progress.copied, progress.total, progress.failed,
            progress.phase == 3 ? "converted" : "copied");
    }
    else {
        snprintf(s_afc_job_status, sizeof(s_afc_job_status), "%s",
            result == ESP_ERR_TIMEOUT ? "Could not connect. Check device trust and try again." :
            s_afc_job_failed ? "Database or artwork sync failed. Check the device and SD card." : "Sync complete");
    }
    s_afc_job_finished = true;
    s_afc_cache_tick = 0;
    s_afc_job_active = 0;
    /* Keep the result visible; Return to player performs the explicit reboot. */
    vTaskDelete(NULL);
}

static void ui_afc_action_handler(int action)
{
    lockdown_pair_status_t pairing;
    lockdown_client_pair_status(&pairing);
    if(s_afc_job_active || pairing.busy) return;
    if(action == UI_AFC_ACTION_PAIR) {
        if(app_config_iap2_mode()) {
            app_config_set_iap2_mode(false);
            app_config_save();
            ESP_LOGW(TAG, "AFC pair/connect: switching to AFC config 4");
            vTaskDelay(pdMS_TO_TICKS(350));
            web_server_reboot();
        }
        else {
            lockdown_client_request_pair();
        }
        return;
    }

    if(action == UI_AFC_ACTION_IPOD_MODE) {
        if(app_config_iap2_mode()) return;
        app_config_set_iap2_mode(true);
        app_config_save();
        ESP_LOGW(TAG, "switching to iAP2 config 2");
        vTaskDelay(pdMS_TO_TICKS(350));
        web_server_reboot();
        return;
    }

    if(action != UI_AFC_ACTION_SYNC) return;
    uint8_t job = 2;

    /* In AFC mode, run the worker and retain its result on screen. */
    if(!app_config_iap2_mode()) {
        if(!afc_client_is_ready() || !app_config_sd_ready()) return;
        afc_start_job_task(job);
        return;
    }

    app_config_set_afc_job(job);
    app_config_set_iap2_mode(false);
    app_config_save();
    ESP_LOGW(TAG, "AFC sync queued; rebooting to AFC");
    vTaskDelay(pdMS_TO_TICKS(350));
    web_server_reboot();
}

static bool web_start_sync(void)
{
    lockdown_pair_status_t pairing;
    lockdown_client_pair_status(&pairing);
    if(app_config_iap2_mode() || s_afc_job_active || pairing.busy ||
       !afc_client_is_ready() || !app_config_sd_ready()) return false;
    return afc_start_job_task(2);
}

static void apply_runtime_config(void)
{
    if((int)g_app_config.screen_flip != s_applied_flip) {
        apply_screen_flip();
        s_applied_flip = g_app_config.screen_flip;
    }
    bool sound_on = g_app_config.sound_enabled && g_app_config.sound_mode != 0;
    if((int)sound_on != s_applied_sound) {
        feedback_set_enabled(sound_on);
        s_applied_sound = sound_on;
    }
    if((int)g_app_config.volume != s_applied_volume) {
        feedback_set_volume(g_app_config.volume);
        s_applied_volume = g_app_config.volume;
    }
    if((int)g_app_config.wifi_mode != s_applied_wifi_mode) {
        wifi_manager_request_mode(g_app_config.wifi_mode);
        s_applied_wifi_mode = g_app_config.wifi_mode;
    }
    if((int)g_app_config.brightness != s_applied_brightness &&
       g_app_config.brightness <= 100) {
        bsp_display_brightness_set(ui_screen_is_on() ? g_app_config.brightness : 0);
        s_applied_brightness = g_app_config.brightness;
    }
    publish_system_config();
}

static void web_apply_runtime_config(void)
{
    /* The web task shares idle state and display settings with LVGL. */
    if(bsp_display_lock(0)) {
        apply_runtime_config();
        bsp_display_unlock();
    }
}

static void ui_system_config_changed(const ui_system_config_t * cfg)
{
    if(cfg == NULL) return;

    g_app_config.screen_flip = (cfg->screen_flip == 180) ? 180 : 0;
    g_app_config.wifi_mode = (cfg->wifi_mode == 1) ? 1 : 0;
    g_app_config.sound_enabled = cfg->sound_enabled;
    g_app_config.sound_mode = (uint8_t)cfg->sound_mode;
    g_app_config.sound_enabled = g_app_config.sound_mode != 0;
    g_app_config.volume = (uint8_t)cfg->volume;
    g_app_config.clock_12h = cfg->clock_12h != 0;
    g_app_config.sort_field = (uint8_t)cfg->sort_field;
    g_app_config.sort_desc = cfg->sort_desc != 0;
    g_app_config.menubar_left = cfg->menubar_left != 0;
    g_app_config.touch_enabled = cfg->touch_enabled != 0;
    g_app_config.np_bg_mode = (uint8_t)cfg->np_bg_mode;
    g_app_config.brightness = (uint8_t)cfg->brightness;
    g_app_config.screen_off_minutes = (uint8_t)cfg->screen_off_minutes;
    g_app_config.usb_mode = (uint8_t)cfg->usb_mode;
    g_app_config.theme = (uint8_t)cfg->theme;
    g_app_config.boot_screen = (uint8_t)cfg->boot_screen;
    g_app_config.ble_enabled = cfg->ble_enabled != 0;
    ble_remote_set_enabled(g_app_config.ble_enabled);

    s_save_pending = true;
    if(cfg->brightness != s_applied_brightness &&
       cfg->brightness >= 0 && cfg->brightness <= 100) {
        bsp_display_brightness_set(ui_screen_is_on() ? cfg->brightness : 0);
        s_applied_brightness = cfg->brightness;
    }
    apply_runtime_config();
}

static void ui_feedback_handler(int type)
{
    feedback_play((feedback_t)type);
}

static void ui_screen_power_handler(bool on)
{
    bsp_display_brightness_set(on ? g_app_config.brightness : 0);
    ESP_LOGI(TAG, "Screen %s", on ? "on" : "off");
}

/* Settings → Clock commits the edited wall clock here.  Keeping this in
 * app_main lets the shared UI code stay platform independent. */
static void ui_time_changed_handler(time_t now)
{
    esp_err_t err = rtc_time_sync_epoch(now);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "time sync failed: %s", esp_err_to_name(err));
    }
}

static void fill_ui_config(ui_system_config_t * cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->screen_flip = g_app_config.screen_flip;
    cfg->wifi_mode = g_app_config.wifi_mode;
    cfg->sound_enabled = g_app_config.sound_enabled;
    cfg->sound_mode = g_app_config.sound_mode;
    cfg->volume = g_app_config.volume;
    snprintf(cfg->ap_ssid, sizeof(cfg->ap_ssid), "%s", g_app_config.ap_ssid);
    cfg->wifi_status[0] = '\0';
    cfg->wifi_ip[0] = '\0';
    cfg->clock_12h = g_app_config.clock_12h ? 1 : 0;
    cfg->sort_field = g_app_config.sort_field;
    cfg->sort_desc = g_app_config.sort_desc ? 1 : 0;
    cfg->menubar_left = g_app_config.menubar_left ? 1 : 0;
    cfg->touch_enabled = g_app_config.touch_enabled ? 1 : 0;
    cfg->np_bg_mode = g_app_config.np_bg_mode;
    cfg->brightness = g_app_config.brightness;
    cfg->screen_off_minutes = g_app_config.screen_off_minutes;
    cfg->usb_mode = g_app_config.usb_mode;
    cfg->theme = g_app_config.theme;
    cfg->boot_screen = g_app_config.boot_screen;
    cfg->ble_enabled = g_app_config.ble_enabled ? 1 : 0;
}

static void publish_system_config(void)
{
    ui_system_config_t cfg;
    fill_ui_config(&cfg);
    ui_set_system_config(&cfg);
}

/* The board has an RX8130CE RTC, but it is not read yet. For the preview,
 * seed the clock from the build timestamp so the menubar shows a plausible
 * time instead of 1970-01-01. */
static void set_preview_clock(void)
{
    static const char * months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char month[4] = {0};
    int day = 1, year = 2026, hour = 0, minute = 0, second = 0;

    if(sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3) return;
    if(sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) return;

    struct tm tm = {0};
    for(int i = 0; i < 12; i++) {
        if(strcmp(month, months[i]) == 0) {
            tm.tm_mon = i;
            break;
        }
    }
    tm.tm_mday = day;
    tm.tm_year = year - 1900;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;

    time_t t = mktime(&tm);
    if(t <= 0) return;

    struct timeval tv = {.tv_sec = t, .tv_usec = 0};
    settimeofday(&tv, NULL);
}

/* The C6 co-processor can stop responding while the P4 keeps running (UI is
 * alive but Wi-Fi/HTTP is dead).  Use the ESP-Hosted heartbeat to detect it
 * and reboot the host, which also resets the C6. */
#define C6_HEARTBEAT_INTERVAL_S 5
#define C6_HEARTBEAT_TIMEOUT_S  30

static esp_timer_handle_t s_c6_timer;
static bool s_c6_reboot_pending;

static void c6_watchdog_timeout(void * arg)
{
    (void)arg;
    ESP_LOGE(TAG, "C6 watchdog timeout; rebooting");
    web_server_reboot();
}

static void c6_event_handler(void * arg, esp_event_base_t base,
                             int32_t id, void * data)
{
    (void)arg;
    (void)data;
    if(base != ESP_HOSTED_EVENT) return;

    if(id == ESP_HOSTED_EVENT_CP_HEARTBEAT) {
        if(s_c6_timer != NULL && !s_c6_reboot_pending) {
            esp_timer_stop(s_c6_timer);
            esp_timer_start_once(s_c6_timer,
                                 (uint64_t)C6_HEARTBEAT_TIMEOUT_S * 1000000ULL);
        }
    }
    else if(id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        if(!s_c6_reboot_pending) {
            s_c6_reboot_pending = true;
            ESP_LOGE(TAG, "C6 transport failure; rebooting");
            if(s_c6_timer != NULL) {
                esp_timer_stop(s_c6_timer);
                esp_timer_start_once(s_c6_timer, 2000000ULL);
            }
        }
    }
}

static void c6_watchdog_start(void)
{
    esp_err_t err = esp_event_handler_instance_register(
        ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, c6_event_handler, NULL, NULL);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "C6 event handler registration failed: %s",
                 esp_err_to_name(err));
    }

    const esp_timer_create_args_t args = {
        .callback = c6_watchdog_timeout,
        .arg = NULL,
        .name = "c6_wd",
    };
    if(esp_timer_create(&args, &s_c6_timer) != ESP_OK) {
        ESP_LOGW(TAG, "C6 watchdog timer creation failed");
        s_c6_timer = NULL;
        return;
    }

    esp_hosted_coprocessor_fwver_t ver;
    if(esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK) {
        ESP_LOGI(TAG, "C6 firmware %u.%u.%u", ver.major1, ver.minor1,
                 ver.patch1);
    }
    else {
        ESP_LOGW(TAG, "C6 firmware version query failed");
    }

    err = esp_hosted_configure_heartbeat(true, C6_HEARTBEAT_INTERVAL_S);
    if(err == ESP_OK) {
        ESP_LOGI(TAG, "C6 heartbeat enabled (%ds interval, %ds timeout)",
                 C6_HEARTBEAT_INTERVAL_S, C6_HEARTBEAT_TIMEOUT_S);
        esp_timer_start_once(s_c6_timer,
                             (uint64_t)C6_HEARTBEAT_TIMEOUT_S * 1000000ULL);
    }
    else {
        ESP_LOGW(TAG, "C6 heartbeat unavailable: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    /* Install the in-memory log mirror before anything else so boot and
     * Wi-Fi/USB logs are available from the web debug page. */
    debug_log_init();

    ESP_LOGI(TAG, "IAP2 Tab5 preview firmware starting (build %s %s)",
             __DATE__, __TIME__);
    ESP_LOGI(TAG, "reset reason: %d", (int)esp_reset_reason());

    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
       nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    app_config_init();
    /* Consume the destructive request durably before doing any deletion.
     * Run before player/UI/cache handles open, and never repeat on reboot. */
    if(app_config_peek_afc_job() == 3) {
        if(app_config_take_afc_job() == 3 && thumb_cache_reset() == ESP_OK) {
            app_config_set_afc_job(2);
        }
        else {
            ESP_LOGE(TAG, "artwork cache reset failed; automatic sync cancelled");
        }
    }
    debug_log_set_sd_enabled(g_app_config.debug_log);

    setenv("TZ", "CST-8", 1);
    tzset();
    set_preview_clock();

    esp_err_t spiffs_err = bsp_spiffs_mount();
    if(spiffs_err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s), formatting storage",
                 esp_err_to_name(spiffs_err));
        esp_err_t fmt_err = esp_spiffs_format("storage");
        if(fmt_err == ESP_OK) {
            spiffs_err = bsp_spiffs_mount();
        }
        else {
            ESP_LOGW(TAG, "SPIFFS format failed: %s",
                     esp_err_to_name(fmt_err));
        }
    }
    if(spiffs_err == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS mounted at %s", BSP_SPIFFS_MOUNT_POINT);
    }
    else {
        ESP_LOGW(TAG, "SPIFFS unavailable; pairing record stays in RAM/SD");
    }

    /* Rendering the 1280x720 UI overflows the LVGL port's default 7 KB task
     * stack inside the font/label draw path, so give it more headroom. */
    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        /* Two full-screen buffers in PSRAM. LVGL still uses partial mode, so
         * small invalidations only render/flush their dirty rectangle; a screen
         * switch can render the whole 1280x720 frame in one pass. */
        .buffer_size = 720 * 1280,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };
    display_cfg.lvgl_port_cfg.task_stack = 32 * 1024;
    /* IDF 5.5 leaves very little internal DMA-capable RAM.  Keep the large
     * LVGL stack in PSRAM so SDMMC thumbnail reads can still allocate DMA
     * buffers. */
    display_cfg.lvgl_port_cfg.task_stack_caps =
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    lv_display_t * display = bsp_display_start_with_config(&display_cfg);
    if(display == NULL) {
        ESP_LOGE(TAG, "display initialisation failed");
        return;
    }
    s_display = display;

    /* The BSP initialises the shared I2C bus while starting the display, so
     * the RTC can only be probed after this point. */
    esp_err_t rtc_err = rtc_time_init();
    if(rtc_err == ESP_OK) {
        ESP_LOGI(TAG, "RTC ready%s", rtc_time_available() ? "" : " (not found)");
    }
    else {
        ESP_LOGW(TAG, "RTC init failed: %s", esp_err_to_name(rtc_err));
    }

    /* The panel is native 720x1280; the UI is designed for 1280x720.
     * screen_flip=180 rotates the landscape UI the other way up. */
    lv_display_set_rotation(display, g_app_config.screen_flip == 180
                                         ? LV_DISPLAY_ROTATION_270
                                         : LV_DISPLAY_ROTATION_90);
    s_applied_flip = g_app_config.screen_flip;
    s_applied_volume = g_app_config.volume;
    s_applied_sound = g_app_config.sound_enabled && g_app_config.sound_mode != 0;
    s_applied_wifi_mode = g_app_config.wifi_mode;
    s_applied_brightness = g_app_config.brightness;

    feedback_init(g_app_config.sound_enabled && g_app_config.sound_mode != 0,
                  g_app_config.volume);

    bsp_display_lock(0);
    ui_system_config_t saved_cfg;
    fill_ui_config(&saved_cfg);
    ui_apply_saved_settings(&saved_cfg);
    ui_set_network_status("WiFi off", "--");
    ui_set_system_config_changed_cb(ui_system_config_changed);
    ui_set_feedback_cb(ui_feedback_handler);
    ui_set_screen_power_cb(ui_screen_power_handler);
    ui_set_time_changed_cb(ui_time_changed_handler);
    ui_set_afc_status_cb(ui_afc_status_handler);
    ui_set_afc_action_cb(ui_afc_action_handler);
    ui_set_afc_sync_mode(!g_app_config.iap2_mode);
    ui_set_artwork_dir(app_config_artwork_dir());
    char np_cache_dir[160];
    snprintf(np_cache_dir, sizeof(np_cache_dir), "%s/np",
             app_config_library_dir());
    ui_set_np_cache_dir(np_cache_dir);
    ui_init();
    if(player_library_album_count() > 0) {
        thumb_cache_open((uint32_t)player_library_album_count());
        char pack[160];
        snprintf(pack, sizeof(pack), "%s/thumbs.bin",
                 app_config_library_dir());
        ui_set_thumb_pack_path(pack, player_library_album_count());
    }
    lv_indev_t * touch = bsp_display_get_input_dev();
    if(touch != NULL) ui_attach_touch(touch);
    if(g_app_config.boot_screen == 1) {
        boot_ui_start(NULL);
    }
    lv_refr_now(display);
    bsp_display_unlock();

    bsp_display_backlight_on();
    bsp_display_brightness_set(g_app_config.brightness);
    bsp_display_lock(0);
    display_diag_init(display);
    bsp_display_unlock();
    esp_err_t charge_err = charge_apply(g_app_config.charge_enabled,
                                        g_app_config.qc_high);
    if(charge_err != ESP_OK) {
        ESP_LOGE(TAG, "charger setup failed: %s", esp_err_to_name(charge_err));
    }
    serial_input_start();

    usb_host_core_set_apple_config(g_app_config.iap2_mode ? 2 : 4);
    usb_host_core_set_peripheral_map(
        usb_peripheral_map_for_mode(g_app_config.usb_mode));
    if(usb_host_core_start() == ESP_OK) {
        /* The HID class driver runs in both modes. It skips the iPod's own
         * HID interface and only owns the Ulanzi D100H / normal keyboards. */
        usb_hid_input_start();
        if(g_app_config.iap2_mode) {
            iap2_host_start();
        }
        else {
            usbmux_host_start();
            lockdown_client_start();
            uint8_t pending_job = app_config_peek_afc_job();
            if(pending_job == 1 || pending_job == 2) afc_start_job_task(pending_job);
        }
    }

    ESP_LOGI(TAG, "UI ready, %dx%d", (int)lv_display_get_horizontal_resolution(display),
             (int)lv_display_get_vertical_resolution(display));

    /* lwIP must be initialized before httpd_start(); otherwise the HTTP
     * server's socket setup hits an invalid TCP/IP mbox. */
    esp_err_t netif_err = esp_netif_init();
    if(netif_err == ESP_OK || netif_err == ESP_ERR_INVALID_STATE) {
        esp_event_loop_create_default();
    }

    web_server_set_apply_cb(web_apply_runtime_config);
    web_server_set_sync_cb(web_start_sync);
    web_server_start();

    esp_err_t wifi_err = wifi_manager_start();
    if(wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi unavailable: %s", esp_err_to_name(wifi_err));
        ui_set_network_status("WiFi unavailable", "--");
    }

    /* Watch the ESP-Hosted/C6 link; a hung C6 otherwise needs a manual
     * power cycle even though the P4 UI keeps running. */
    c6_watchdog_start();

    /* BLE is on the C6 via ESP-Hosted as well. Start it after the hosted
     * transport is up from Wi-Fi initialization. */
    ble_remote_start();
    ble_remote_set_enabled(g_app_config.ble_enabled);

    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        {
            bool present = false;
            iap2_state_t state;
            if(g_app_config.iap2_mode && iap2_host_get_state(&state)) {
                present = state.identified;
            }
            ui_set_device_state(g_app_config.iap2_mode != 0, present,
                                app_config_sd_ready());
        }
        if(s_save_pending) {
            s_save_pending = false;
            app_config_save();
        }
    }
}
