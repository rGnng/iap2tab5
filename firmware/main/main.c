/*
 * M5Stack Tab5 firmware entry point.
 *
 * Display, touch and backlight come from esp-bsp's m5stack_tab5 BSP. The UI
 * itself is the shared app/ui code also used by the desktop simulator.
 */

#include "app_config.h"
#include "ble_remote.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "bsp/esp-bsp.h"
#include "feedback.h"
#include "serial_input.h"
#include "ui.h"
#include "player.h"
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
    snprintf(out->status, sizeof(out->status), "%s", lockdown_client_status());

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
    out->library_albums = player_album_count();
    out->library_tracks = player_song_count();
    snprintf(out->library_db_item, sizeof(out->library_db_item),
             "%d albums / %d tracks", out->library_albums,
             out->library_tracks);
    struct stat lib_st;
    if(stat(app_config_library_bin_path(), &lib_st) == 0) {
        struct tm tmv;
        localtime_r(&lib_st.st_mtime, &tmv);
        strftime(out->library_updated, sizeof(out->library_updated),
                 "%Y-%m-%d %H:%M", &tmv);
    }
    else {
        snprintf(out->library_updated, sizeof(out->library_updated), "—");
    }

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

static void afc_probe_task(void * arg)
{
    LV_UNUSED(arg);
    lockdown_client_run_afc_probe();
    vTaskDelete(NULL);
}

static void afc_job_task(void * arg);

static bool afc_start_job_task(uint8_t job)
{
    if(job != 0) app_config_set_afc_job(job);
    s_afc_job_active = job ? job : 1;
    s_afc_job_kind = job;
    BaseType_t ok = xTaskCreateWithCaps(
        afc_job_task, "afc_job", 24 * 1024, NULL, 4, NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(ok != pdPASS) {
        s_afc_job_active = 0;
        ESP_LOGE(TAG, "failed to create AFC job task");
        return false;
    }
    return true;
}

static void afc_job_task(void * arg)
{
    LV_UNUSED(arg);
    uint8_t job = app_config_take_afc_job();
    if(job == 0) {
        vTaskDelete(NULL);
        return;
    }

    s_afc_job_active = job;
    s_afc_job_kind = job;
    ESP_LOGW(TAG, "AFC job %u starting %s sync", job,
             job == 1 ? "quick" : "full");

    /* The iPod / lockdownd session may still be coming up. */
    for(int i = 0; i < 240 && !afc_client_is_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if(afc_client_is_ready()) {
        esp_err_t err = lockdown_client_sync_artwork(job == 1 ? 64 : 0);
        ESP_LOGW(TAG, "AFC job %u result: %s", job, esp_err_to_name(err));
    }
    else {
        ESP_LOGW(TAG, "AFC job %u: AFC did not become ready", job);
    }

    s_afc_job_active = 0;
    app_config_set_iap2_mode(true);
    app_config_save();
    ESP_LOGW(TAG, "AFC job done; switching back to iAP2");
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    vTaskDelete(NULL);
}

static void ui_afc_action_handler(int action)
{
    if(action == UI_AFC_ACTION_PAIR) {
        if(app_config_iap2_mode()) {
            app_config_set_iap2_mode(false);
            app_config_save();
            ESP_LOGW(TAG, "AFC pair/connect: switching to AFC config 4");
            vTaskDelay(pdMS_TO_TICKS(350));
            esp_restart();
        }
        else if(afc_client_is_ready()) {
            xTaskCreate(afc_probe_task, "afc_probe", 6144, NULL, 4, NULL);
        }
        return;
    }

    if(action == UI_AFC_ACTION_IPOD_MODE) {
        if(app_config_iap2_mode()) return;
        app_config_set_iap2_mode(true);
        app_config_save();
        ESP_LOGW(TAG, "switching to iAP2 config 2");
        vTaskDelay(pdMS_TO_TICKS(350));
        esp_restart();
        return;
    }

    uint8_t job = action == UI_AFC_ACTION_QUICK_SYNC ? 1 : 2;

    /* Already in the AFC sync UI: start the worker directly. It will
     * switch back to iAP2 and reboot when the sync finishes. */
    if(!app_config_iap2_mode()) {
        if(s_afc_job_active) return;
        afc_start_job_task(job);
        return;
    }

    app_config_set_afc_job(job);
    app_config_set_iap2_mode(false);
    app_config_save();
    ESP_LOGW(TAG, "AFC %s sync queued; rebooting to AFC",
             job == 1 ? "quick" : "full");
    vTaskDelay(pdMS_TO_TICKS(350));
    esp_restart();
}

static void apply_runtime_config(void)
{
    if((int)g_app_config.screen_flip != s_applied_flip) {
        apply_screen_flip();
        s_applied_flip = g_app_config.screen_flip;
    }
    if((int)g_app_config.sound_enabled != s_applied_sound) {
        feedback_set_enabled(g_app_config.sound_enabled);
        s_applied_sound = g_app_config.sound_enabled;
    }
    if((int)g_app_config.volume != s_applied_volume) {
        feedback_set_volume(g_app_config.volume);
        s_applied_volume = g_app_config.volume;
    }
    if((int)g_app_config.wifi_mode != s_applied_wifi_mode) {
        wifi_manager_request_mode(g_app_config.wifi_mode);
        s_applied_wifi_mode = g_app_config.wifi_mode;
    }
    publish_system_config();
}

static void ui_system_config_changed(const ui_system_config_t * cfg)
{
    if(cfg == NULL) return;

    g_app_config.screen_flip = (cfg->screen_flip == 180) ? 180 : 0;
    g_app_config.wifi_mode = (cfg->wifi_mode == 1) ? 1 : 0;
    g_app_config.sound_enabled = cfg->sound_enabled;
    g_app_config.volume = (uint8_t)cfg->volume;
    g_app_config.clock_12h = cfg->clock_12h != 0;
    g_app_config.sort_field = (uint8_t)cfg->sort_field;
    g_app_config.sort_desc = cfg->sort_desc != 0;
    g_app_config.menubar_left = cfg->menubar_left != 0;
    g_app_config.touch_enabled = cfg->touch_enabled != 0;
    g_app_config.np_bg_mode = (uint8_t)cfg->np_bg_mode;
    g_app_config.brightness = (uint8_t)cfg->brightness;
    g_app_config.usb_mode = (uint8_t)cfg->usb_mode;

    s_save_pending = true;
    if(cfg->brightness != s_applied_brightness &&
       cfg->brightness >= 0 && cfg->brightness <= 100) {
        bsp_display_brightness_set(cfg->brightness);
        s_applied_brightness = cfg->brightness;
    }
    apply_runtime_config();
}

static void ui_feedback_handler(int type)
{
    feedback_play((feedback_t)type);
}

static void fill_ui_config(ui_system_config_t * cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->screen_flip = g_app_config.screen_flip;
    cfg->wifi_mode = g_app_config.wifi_mode;
    cfg->sound_enabled = g_app_config.sound_enabled;
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
    cfg->usb_mode = g_app_config.usb_mode;
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

void app_main(void)
{
    ESP_LOGI(TAG, "IAP2 Tab5 preview firmware starting");

    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
       nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    app_config_init();

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

    lv_display_t * display = bsp_display_start_with_config(&display_cfg);
    if(display == NULL) {
        ESP_LOGE(TAG, "display initialisation failed");
        return;
    }
    s_display = display;

    /* The panel is native 720x1280; the UI is designed for 1280x720.
     * screen_flip=180 rotates the landscape UI the other way up. */
    lv_display_set_rotation(display, g_app_config.screen_flip == 180
                                         ? LV_DISPLAY_ROTATION_270
                                         : LV_DISPLAY_ROTATION_90);
    s_applied_flip = g_app_config.screen_flip;
    s_applied_volume = g_app_config.volume;
    s_applied_sound = g_app_config.sound_enabled;
    s_applied_wifi_mode = g_app_config.wifi_mode;
    s_applied_brightness = g_app_config.brightness;

    feedback_init(g_app_config.sound_enabled, g_app_config.volume);

    bsp_display_lock(0);
    ui_system_config_t saved_cfg;
    fill_ui_config(&saved_cfg);
    ui_apply_saved_settings(&saved_cfg);
    ui_set_network_status("WiFi off", "--");
    ui_set_system_config_changed_cb(ui_system_config_changed);
    ui_set_feedback_cb(ui_feedback_handler);
    ui_set_afc_status_cb(ui_afc_status_handler);
    ui_set_afc_action_cb(ui_afc_action_handler);
    ui_set_afc_sync_mode(!g_app_config.iap2_mode);
    ui_set_artwork_dir(app_config_artwork_dir());
    ui_set_thumb_pack_dir(app_config_library_dir());
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
    lv_refr_now(display);
    bsp_display_unlock();

    bsp_display_backlight_on();
    bsp_display_brightness_set(g_app_config.brightness);
    serial_input_start();

    usb_host_core_set_apple_config(g_app_config.iap2_mode ? 2 : 4);
    usb_host_core_set_peripheral_map(g_app_config.usb_mode ? (1u << 1)
                                                           : (1u << 0));
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
            if(pending_job != 0) afc_start_job_task(pending_job);
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

    web_server_set_apply_cb(apply_runtime_config);
    web_server_start();

    esp_err_t wifi_err = wifi_manager_start();
    if(wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi unavailable: %s", esp_err_to_name(wifi_err));
        ui_set_network_status("WiFi unavailable", "--");
    }

    /* BLE is on the C6 via ESP-Hosted as well. Start it after the hosted
     * transport is up from Wi-Fi initialization. */
    ble_remote_start();

    for(;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if(s_save_pending) {
            s_save_pending = false;
            app_config_save();
        }
    }
}
