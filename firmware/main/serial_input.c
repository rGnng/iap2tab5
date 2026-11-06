/*
 * Debug key input over the USB Serial/JTAG console.
 *
 * The real product uses a USB HID knob/keypad. Until that is wired up this
 * lets us drive the UI remotely while the board is only connected by USB-C:
 *
 *   w / s      knob up / down
 *   Enter      knob press
 *   b          back
 *   p / n      Page Up / Page Down (list paging)
 *   [ / ]      previous / next screen
 *   c          context menu
 *   1 .. 6     jump to screen
 *   7          play / pause
 *   g          dump the current screen as RGB565 (preview/debug)
 *   m          open Settings (debug)
 *   r          toggle screen flip 0/180 (debug)
 *   h          HTTP self-test of /api/status (debug)
 *   k          benchmark a full-screen redraw (debug)
 *   l          benchmark one list page (debug)
 *   d          scroll active list by 240 px and benchmark (debug)
 *   a          print usbmux/AFC status (debug)
 *   8          SD read/write benchmark, 4 MB (debug)
 *   u          upload /sdcard/pair.plist over serial (end with EOF)
 *   f          re-run the AFC directory/stat/read probe (debug)
 *   y          copy MediaLibrary.sqlitedb (+ WAL/SHM) to /sdcard/AFC
 *   z          hash MediaLibrary.sqlitedb (+ WAL/SHM), no SD needed
 *   Y          format the SD card (destructive) and mount it
 *   V          verify /sdcard/AFC/MLDB.* by hashing the local files
 *   i          dump all USB configuration descriptors (iAP2 bring-up)
 *   x          run the embedded MFi P-256 signing self-test
 *   J          toggle iAP2 (USB config 2) / AFC (config 4) and reboot
 *   ( / )      select USB-C OTG FS / USB-A HS host and reboot
 *   S          remount the SD card and print the AFC file sizes
 *   T / W      dump MLDB.SQL / MLDB.WAL as base64 over the console
 *   Q          upload /sdcard/library.bin as base64, end with EOF
 *   B          upload /sdcard/JPFONT.TTF as base64, end with EOF
 *   C / E      AFC artwork sync: first 64 albums / all albums
 *   # <id>     AFC artwork sync for one album ID
 *   9 <id>     dump the current library's <id>.cov as base64 (debug)
 *   0          delete the packed thumb cache (debug)
 *   v          print the artwork cache slow log
 *   @          dump the per-library stars.csv as base64
 *   !          star/unstar the current track (debug)
 *   $          clear all local stars and delete stars.csv (debug)
 *   o          iAP2 play / pause toggle
 *   , / .      iAP2 previous / next track
 *   P          re-send the Apple USB power-capability vendor request
 *   *          print the USB-A power switch / IO-expander state
 *   &          print the INA226 battery bus voltage/current
 */

#include "serial_input.h"

#include "app_config.h"

#include <string.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "ui.h"
#include "usbmux_host.h"
#include "lockdown_client.h"
#include "afc_client.h"
#include "thumb_cache.h"
#include "iap2_auth.h"
#include "iap2_host.h"
#include "player.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "web_server.h"

#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Send the current screen as a hex-encoded RGB565 frame over the USB console.
 * The host looks for "IAP2SHOTHEX <w> <h> <stride>\n" and then reads two hex
 * digits per byte (stride*h bytes total). Used for remote preview/debugging. */
static void send_screenshot(void)
{
    if(!bsp_display_lock(0)) return;

    lv_draw_buf_t * snapshot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if(snapshot == NULL) {
        bsp_display_unlock();
        return;
    }

    esp_log_level_set("*", ESP_LOG_NONE);

    const uint32_t w = snapshot->header.w;
    const uint32_t h = snapshot->header.h;
    const uint32_t stride = snapshot->header.stride;
    const uint32_t bytes = stride * h;

    /* Hex-encode the frame: the USB console is a terminal and would insert
     * CR bytes into a raw binary stream (0x0a -> 0x0d 0x0a), corrupting rows. */
    static const char hex[] = "0123456789abcdef";
    char line[512];
    size_t n = 0;

    printf("\nIAP2SHOTHEX %u %u %u\n", (unsigned)w, (unsigned)h, (unsigned)stride);
    for(uint32_t i = 0; i < bytes; i++) {
        uint8_t v = snapshot->data[i];
        line[n++] = hex[v >> 4];
        line[n++] = hex[v & 0x0f];
        if(n == sizeof(line)) {
            fwrite(line, 1, n, stdout);
            n = 0;
        }
        /* Keep the idle task (and therefore the task watchdog) alive during
         * the multi-megabyte dump. */
        if((i & 0x3fff) == 0) vTaskDelay(1);
    }
    if(n > 0) fwrite(line, 1, n, stdout);
    printf("\nIAP2SHOT_END\n");
    fflush(stdout);

    esp_log_level_set("*", ESP_LOG_INFO);

    lv_draw_buf_destroy(snapshot);
    bsp_display_unlock();
}

static void benchmark_full_redraw(void)
{
    if(!bsp_display_lock(0)) return;

    lv_display_t * disp = lv_display_get_default();
    for(int i = 0; i < 3; i++) {
        uint32_t t0 = (uint32_t)esp_timer_get_time();
        lv_obj_invalidate(lv_screen_active());
        lv_obj_update_layout(lv_screen_active());
        uint32_t t1 = (uint32_t)esp_timer_get_time();
        lv_refr_now(NULL);
        uint32_t t2 = (uint32_t)esp_timer_get_time();
        lvgl_port_display_wait_flush(disp, 3000);
        uint32_t t3 = (uint32_t)esp_timer_get_time();
        ESP_LOGI("perf", "full redraw %d: layout=%u render=%u flush=%u total=%u us", i,
                 (unsigned)(t1 - t0), (unsigned)(t2 - t1),
                 (unsigned)(t3 - t2), (unsigned)(t3 - t0));
    }

    bsp_display_unlock();
}

static void benchmark_list_page(void)
{
    if(!bsp_display_lock(0)) return;
    lv_display_t * disp = lv_display_get_default();
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    ui_debug_page(1);
    uint32_t t1 = (uint32_t)esp_timer_get_time();
    lv_obj_update_layout(lv_screen_active());
    uint32_t t1b = (uint32_t)esp_timer_get_time();
    lv_refr_now(NULL);
    uint32_t t2 = (uint32_t)esp_timer_get_time();
    lvgl_port_display_wait_flush(disp, 3000);
    uint32_t t3 = (uint32_t)esp_timer_get_time();
    ESP_LOGI("perf", "list page: update=%u layout=%u render=%u flush=%u total=%u us",
             (unsigned)(t1 - t0), (unsigned)(t1b - t1),
             (unsigned)(t2 - t1b), (unsigned)(t3 - t2),
             (unsigned)(t3 - t0));
    bsp_display_unlock();
}

static void benchmark_list_scroll(void)
{
    if(!bsp_display_lock(0)) return;
    lv_display_t * disp = lv_display_get_default();
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    ui_debug_scroll(-240);
    uint32_t t1 = (uint32_t)esp_timer_get_time();
    lv_refr_now(NULL);
    uint32_t t2 = (uint32_t)esp_timer_get_time();
    lvgl_port_display_wait_flush(disp, 3000);
    uint32_t t3 = (uint32_t)esp_timer_get_time();
    ESP_LOGI("perf", "list scroll: scroll=%u render=%u flush=%u total=%u us",
             (unsigned)(t1 - t0), (unsigned)(t2 - t1),
             (unsigned)(t3 - t2), (unsigned)(t3 - t0));
    bsp_display_unlock();
}

static void benchmark_focus(int screen, const char * name, int steps)
{
    if(!bsp_display_lock(0)) return;
    lv_display_t * disp = lv_display_get_default();
    ui_select_screen(screen);
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(NULL);
    lvgl_port_display_wait_flush(disp, 3000);

    uint32_t t0 = (uint32_t)esp_timer_get_time();
    for(int i = 0; i < steps; i++) {
        ui_handle_key(UI_KEY_DOWN);
    }
    uint32_t t1 = (uint32_t)esp_timer_get_time();
    lv_obj_update_layout(lv_screen_active());
    uint32_t t2 = (uint32_t)esp_timer_get_time();
    lv_refr_now(NULL);
    uint32_t t3 = (uint32_t)esp_timer_get_time();
    lvgl_port_display_wait_flush(disp, 3000);
    uint32_t t4 = (uint32_t)esp_timer_get_time();

    ESP_LOGI("perf", "%s focus x%d: handle=%u layout=%u render=%u flush=%u total=%u us",
             name, steps, (unsigned)(t1 - t0), (unsigned)(t2 - t1),
             (unsigned)(t3 - t2), (unsigned)(t4 - t3),
             (unsigned)(t4 - t0));
    bsp_display_unlock();
}

static bool map_serial_key(int c, ui_key_t * key)
{
    switch(c) {
        case 'w': case 'W': *key = UI_KEY_UP; return true;
        case 's': case 'S': *key = UI_KEY_DOWN; return true;
        case '\r': case '\n': *key = UI_KEY_ENTER; return true;
        case 'b': case 'B': *key = UI_KEY_BACK; return true;
        case 'p': case 'P': *key = UI_KEY_PAGE_UP; return true;
        case 'n': case 'N': *key = UI_KEY_PAGE_DOWN; return true;
        case '[': *key = UI_KEY_SCREEN_PREV; return true;
        case ']': *key = UI_KEY_SCREEN_NEXT; return true;
        case 'c': case 'C': *key = UI_KEY_CONTEXT; return true;
        case '1': *key = UI_KEY_1; return true;
        case '2': *key = UI_KEY_2; return true;
        case '3': *key = UI_KEY_3; return true;
        case '4': *key = UI_KEY_4; return true;
        case '5': *key = UI_KEY_5; return true;
        case '6': *key = UI_KEY_6; return true;
        case '7': *key = UI_KEY_7; return true;
        default: return false;
    }
}



static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_val(char c)
{
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

static size_t b64_decode(const char * in, size_t len, uint8_t * out,
                         size_t cap)
{
    size_t o = 0;
    int quad[4];
    int qn = 0;
    for(size_t i = 0; i < len; i++) {
        char c = in[i];
        int v;
        if(c == '=') {
            v = -2;
        }
        else {
            v = b64_val(c);
            if(v < 0) continue;
        }
        quad[qn++] = v;
        if(qn == 4) {
            if(o < cap && quad[0] >= 0 && quad[1] >= 0) {
                out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
            }
            if(o < cap && quad[1] >= 0 && quad[2] >= 0) {
                out[o++] = (uint8_t)(((quad[1] & 0x0F) << 4) |
                                     (quad[2] >> 2));
            }
            if(o < cap && quad[2] >= 0 && quad[3] >= 0) {
                out[o++] = (uint8_t)(((quad[2] & 0x03) << 6) | quad[3]);
            }
            qn = 0;
        }
    }
    return o;
}

static size_t b64_encode(const uint8_t * in, size_t len, char * out)
{
    size_t o = 0;
    for(size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if(i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if(i + 2 < len) v |= in[i + 2];
        out[o++] = B64_CHARS[(v >> 18) & 0x3F];
        out[o++] = B64_CHARS[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64_CHARS[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < len) ? B64_CHARS[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

static void serial_file_upload(const char * path)
{
    if(!app_config_sd_ensure()) {
        printf("IAP2UP_ERR sdcard\n");
        fflush(stdout);
        return;
    }

    FILE * f = fopen(path, "wb");
    if(f == NULL) {
        printf("IAP2UP_ERR open\n");
        fflush(stdout);
        return;
    }

    printf("IAP2UP_READY\n");
    fflush(stdout);

    char line[512];
    uint8_t decoded[384];
    size_t line_len = 0;
    size_t total = 0;
    bool done = false;
    bool write_error = false;
    TickType_t start = xTaskGetTickCount();

    while(!done && !write_error &&
          (xTaskGetTickCount() - start) < pdMS_TO_TICKS(180000)) {
        int c = fgetc(stdin);
        if(c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if(c == '\r') continue;
        if(c == '\n') {
            line[line_len] = '\0';
            if(strcmp(line, "EOF") == 0) {
                done = true;
                break;
            }
            size_t n = b64_decode(line, line_len, decoded, sizeof(decoded));
            if(n > 0 && fwrite(decoded, 1, n, f) != n) {
                write_error = true;
                break;
            }
            total += n;
            line_len = 0;
        }
        else if(line_len < sizeof(line) - 1) {
            line[line_len++] = (char)c;
        }
    }

    fclose(f);
    if(write_error) {
        printf("IAP2UP_ERR write\n");
    }
    else if(!done) {
        printf("IAP2UP_ERR timeout\n");
    }
    else {
        printf("IAP2UP_OK %u\n", (unsigned)total);
    }
    fflush(stdout);
    ESP_LOGI("sd", "uploaded %s (%u bytes)", path, (unsigned)total);
}

static void serial_print_file(const char * path)
{
    FILE * f = fopen(path, "r");
    if(f == NULL) {
        printf("IAP2LOG_ERROR %s\n", path);
        fflush(stdout);
        return;
    }
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("IAP2LOG_BEGIN %s\n", path);
    char line[256];
    while(fgets(line, sizeof(line), f) != NULL) {
        fputs(line, stdout);
    }
    fclose(f);
    printf("IAP2LOG_END\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
}

static void serial_dump_file(const char * path)
{
    FILE * f = fopen(path, "rb");
    if(f == NULL) {
        printf("IAP2FILE_ERROR %s\n", path);
        fflush(stdout);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    esp_log_level_set("*", ESP_LOG_NONE);
    printf("IAP2FILE %s %ld\n", path, size);
    fflush(stdout);

    uint8_t in[192];
    char out[260];
    long sent = 0;
    size_t n;
    while((n = fread(in, 1, sizeof(in), f)) > 0) {
        size_t len = b64_encode(in, n, out);
        fwrite(out, 1, len, stdout);
        fputc('\n', stdout);
        sent += (long)n;
    }
    fclose(f);
    printf("IAP2FILE_END %s %ld\n", path, sent);
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI("sd", "dumped %s (%ld bytes)", path, sent);
}

static void serial_pair_upload(void)
{
    const size_t cap = 256 * 1024;
    char * buf = heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if(buf == NULL) {
        ESP_LOGE("pair", "no memory for pair upload");
        return;
    }

    ESP_LOGI("pair", "send pair.plist now; finish with a line containing EOF");
    char line[512];
    size_t line_len = 0;
    size_t len = 0;
    bool done = false;
    TickType_t start = xTaskGetTickCount();

    while(!done && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(120000)) {
        int c = fgetc(stdin);
        if(c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if(c == '\r') continue;
        if(c == '\n') {
            line[line_len] = '\0';
            if(strcmp(line, "EOF") == 0) {
                done = true;
                break;
            }
            if(len + line_len + 1 < cap) {
                memcpy(buf + len, line, line_len);
                len += line_len;
                buf[len++] = '\n';
            }
            line_len = 0;
        }
        else if(line_len < sizeof(line) - 1) {
            line[line_len++] = (char)c;
        }
    }

    buf[len] = '\0';
    if(!done) {
        ESP_LOGW("pair", "upload timed out after %u bytes", (unsigned)len);
        free(buf);
        return;
    }

    esp_err_t err = lockdown_client_set_pair_record(buf, len);
    ESP_LOGI("pair", "received %u bytes: %s", (unsigned)len,
             esp_err_to_name(err));
    free(buf);
}

static void benchmark_sd_io(void)
{
    if(!app_config_sd_ensure()) {
        ESP_LOGE("sdbench", "SD not available");
        return;
    }

    const size_t total_bytes = 4 * 1024 * 1024;
    static const size_t chunks[] = {
        64 * 1024
    };
    uint8_t * buf = heap_caps_aligned_alloc(128, 1024 * 1024,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(buf == NULL) {
        buf = heap_caps_aligned_alloc(128, 1024 * 1024, MALLOC_CAP_8BIT);
    }
    if(buf == NULL) {
        ESP_LOGE("sdbench", "buffer alloc failed");
        return;
    }
    memset(buf, 0x5a, 1024 * 1024);

    const char * path = "/sdcard/sdbench.tmp";

    for(size_t s = 0; s < sizeof(chunks) / sizeof(chunks[0]); s++) {
        size_t chunk = chunks[s];
        uint64_t total_us = 0, max_us = 0, close_us = 0;
        size_t written = 0;

        FILE * f = fopen(path, "wb");
        if(f == NULL) {
            ESP_LOGE("sdbench", "fopen %s failed", path);
            break;
        }
        setvbuf(f, NULL, _IONBF, 0);
        for(size_t off = 0; off < total_bytes; off += chunk) {
            uint64_t t0 = (uint64_t)esp_timer_get_time();
            size_t n = fwrite(buf, 1, chunk, f);
            uint64_t dt = (uint64_t)esp_timer_get_time() - t0;
            total_us += dt;
            if(dt > max_us) max_us = dt;
            if(n != chunk) {
                ESP_LOGE("sdbench", "write failed at %u: %u",
                         (unsigned)off, (unsigned)n);
                break;
            }
            written += n;
        }
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        fclose(f);
        close_us = (uint64_t)esp_timer_get_time() - t0;

        uint64_t write_us = total_us + close_us;
        ESP_LOGI("sdbench", "write %uKB chunk=%uKB: %llums (%lluKB/s) "
                 "max=%llums close=%llums", (unsigned)(written / 1024),
                 (unsigned)(chunk / 1024),
                 (unsigned long long)(write_us / 1000),
                 (unsigned long long)(written * 1000 /
                                      (write_us ? write_us : 1)),
                 (unsigned long long)(max_us / 1000),
                 (unsigned long long)(close_us / 1000));

        total_us = max_us = close_us = 0;
        size_t read_total = 0;
        f = fopen(path, "rb");
        if(f == NULL) {
            ESP_LOGE("sdbench", "reopen %s failed", path);
            break;
        }
        for(size_t off = 0; off < total_bytes; off += chunk) {
            uint64_t tr = (uint64_t)esp_timer_get_time();
            size_t n = fread(buf, 1, chunk, f);
            uint64_t dt = (uint64_t)esp_timer_get_time() - tr;
            total_us += dt;
            if(dt > max_us) max_us = dt;
            if(n != chunk) break;
            read_total += n;
        }
        t0 = (uint64_t)esp_timer_get_time();
        fclose(f);
        close_us = (uint64_t)esp_timer_get_time() - t0;
        uint64_t read_us = total_us + close_us;
        ESP_LOGI("sdbench", "read %uKB chunk=%uKB: %llums (%lluKB/s) "
                 "max=%llums close=%llums", (unsigned)(read_total / 1024),
                 (unsigned)(chunk / 1024),
                 (unsigned long long)(read_us / 1000),
                 (unsigned long long)(read_total * 1000 /
                                      (read_us ? read_us : 1)),
                 (unsigned long long)(max_us / 1000),
                 (unsigned long long)(close_us / 1000));

        remove(path);
    }

    remove(path);
    free(buf);
}

static void serial_input_task(void * arg)
{
    (void)arg;
    for(;;) {
        int c = fgetc(stdin);
        if(c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if(c == 'g' || c == 'G') {
            send_screenshot();
            continue;
        }

        if(c == 'm' || c == 'M') {
            if(bsp_display_lock(0)) {
                ui_select_screen(5);
                bsp_display_unlock();
            }
            continue;
        }

        if(c == 'r' || c == 'R') {
            if(bsp_display_lock(0)) {
                ui_system_toggle_screen_flip();
                bsp_display_unlock();
            }
            continue;
        }

        if(c == 'h' || c == 'H') {
            web_server_self_test();
            continue;
        }

        if(c == '8') {
            benchmark_sd_io();
            continue;
        }

        if(c == 'k' || c == 'K') {
            benchmark_full_redraw();
            continue;
        }

        if(c == 'l' || c == 'L') {
            benchmark_list_page();
            continue;
        }

        if(c == 'd' || c == 'D') {
            benchmark_list_scroll();
            continue;
        }

        if(c == 'u' || c == 'U') {
            serial_pair_upload();
            continue;
        }

        if(c == 'f' || c == 'F') {
            esp_err_t err = lockdown_client_run_afc_probe();
            ESP_LOGI("afc", "probe: %s", esp_err_to_name(err));
            continue;
        }

        if(c == 'Y') {
            bool ok = app_config_sd_format();
            ESP_LOGI("sd", "format: %s", ok ? "ok" : "failed");
            continue;
        }

        if(c == 'V') {
            esp_err_t err = lockdown_client_verify_sd_media();
            ESP_LOGI("sd", "verify: %s", esp_err_to_name(err));
            continue;
        }

        if(c == 'i' || c == 'I') {
            esp_err_t err = usbmux_host_dump_configs();
            ESP_LOGI("usb", "config dump: %s", esp_err_to_name(err));
            continue;
        }

        if(c == 'x' || c == 'X') {
            esp_err_t err = iap2_auth_self_test();
            ESP_LOGI("iap2", "mfi self-test: %s", esp_err_to_name(err));
            continue;
        }

        if(c == '#') {
            char line[32];
            size_t n = 0;
            TickType_t start = xTaskGetTickCount();
            while((xTaskGetTickCount() - start) < pdMS_TO_TICKS(10000)) {
                int ch = fgetc(stdin);
                if(ch == EOF) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                if(ch == '\r') continue;
                if(ch == '\n') break;
                if(n < sizeof(line) - 1) line[n++] = (char)ch;
            }
            line[n] = '\0';
            int album_id = atoi(line);
            esp_err_t err = lockdown_client_sync_artwork_id(album_id);
            ESP_LOGI("art", "sync id %d: %s", album_id, esp_err_to_name(err));
            continue;
        }

        if(c == 'v') {
            char path[160];
            snprintf(path, sizeof(path), "%s/slow.log",
                     app_config_library_dir());
            serial_print_file(path);
            continue;
        }

        if(c == '@') {
            serial_dump_file(app_config_library_stars_path());
            continue;
        }

        if(c == '!') {
            if(bsp_display_lock(0)) {
                player_toggle_star();
                bsp_display_unlock();
            }
            ESP_LOGI("stars", "toggled current track");
            continue;
        }

        if(c == '$') {
            if(bsp_display_lock(0)) {
                player_clear_stars();
                bsp_display_unlock();
            }
            ESP_LOGI("stars", "cleared local stars");
            continue;
        }

        if(c == '0') {
            thumb_cache_close();
            char path[160];
            snprintf(path, sizeof(path), "%s/thumbs.bin",
                     app_config_library_dir());
            int rb = remove(path);
            snprintf(path, sizeof(path), "%s/thumbs.idx",
                     app_config_library_dir());
            int ri = remove(path);
            ESP_LOGI("thumbs", "pack removed: bin=%d idx=%d", rb, ri);
            continue;
        }

        if(c == '9') {
            char line[32];
            size_t n = 0;
            TickType_t start = xTaskGetTickCount();
            while((xTaskGetTickCount() - start) < pdMS_TO_TICKS(10000)) {
                int ch = fgetc(stdin);
                if(ch == EOF) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                if(ch == '\r') continue;
                if(ch == '\n') break;
                if(n < sizeof(line) - 1) line[n++] = (char)ch;
            }
            line[n] = '\0';
            char path[128];
            snprintf(path, sizeof(path), "%s/%d.cov",
                     app_config_artwork_dir(), atoi(line));
            serial_dump_file(path);
            continue;
        }

        if(c == 'C') {
            esp_err_t err = lockdown_client_sync_artwork(64);
            ESP_LOGI("art", "sync 64: %s", esp_err_to_name(err));
            continue;
        }

        if(c == 'E') {
            esp_err_t err = lockdown_client_sync_artwork(0);
            ESP_LOGI("art", "sync all: %s", esp_err_to_name(err));
            continue;
        }

        if(c == 'Q') {
            serial_file_upload(app_config_library_bin_path());
            continue;
        }

        if(c == 'B') {
            serial_file_upload("/sdcard/JPFONT.TTF");
            continue;
        }

        if(c == 'T') {
            if(app_config_sd_ensure()) {
                serial_dump_file("/sdcard/AFC/MLDB.SQL");
            }
            continue;
        }

        if(c == 'W') {
            if(app_config_sd_ensure()) {
                serial_dump_file("/sdcard/AFC/MLDB.WAL");
            }
            continue;
        }

        if(c == 'S') {
            bool ready = app_config_sd_ensure();
            ESP_LOGI("sd", "ready=%d", (int)ready);
            if(ready) {
                const char * files[] = {
                    "/sdcard/AFC/MLDB.SQL",
                    "/sdcard/AFC/MLDB.WAL",
                    "/sdcard/AFC/MLDB.SHM",
                    "/sdcard/library.json",
                    app_config_library_bin_path(),
                    app_config_library_stars_path(),
                };
                for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
                    struct stat st;
                    if(stat(files[i], &st) == 0) {
                        ESP_LOGI("sd", "%s: %ld bytes", files[i],
                                 (long)st.st_size);
                    }
                    else {
                        ESP_LOGI("sd", "%s: missing", files[i]);
                    }
                }
            }
            continue;
        }

        if(c == '(' || c == ')') {
            g_app_config.usb_mode = (c == '(') ? 1 : 0;
            app_config_save();
            ESP_LOGI("usb", "host peripheral -> %s; rebooting",
                     g_app_config.usb_mode ? "USB-C OTG FS"
                                           : "USB-A HS");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
            continue;
        }

        if(c == 'J') {
            app_config_set_iap2_mode(!g_app_config.iap2_mode);
            app_config_save();
            ESP_LOGW("iap2", "USB mode -> %s, rebooting",
                     g_app_config.iap2_mode ? "iAP2 (config 2)" :
                                              "AFC (config 4)");
            vTaskDelay(pdMS_TO_TICKS(400));
            esp_restart();
            continue;
        }

        if(c == 'o' || c == 'O') {
            const char * cmd = iap2_host_is_playing() ? "pause" : "play";
            ESP_LOGI("iap2", "%s: %s", cmd,
                     esp_err_to_name(iap2_host_hid_command(cmd)));
            continue;
        }

        if(c == ',') {
            ESP_LOGI("iap2", "previous: %s",
                     esp_err_to_name(iap2_host_hid_command("previous")));
            continue;
        }

        if(c == '.') {
            ESP_LOGI("iap2", "next: %s",
                     esp_err_to_name(iap2_host_hid_command("next")));
            continue;
        }

        if(c == 'y') {
            esp_err_t err = lockdown_client_sync_media();
            ESP_LOGI("afc", "sync: %s", esp_err_to_name(err));
            continue;
        }

        if(c == 'z' || c == 'Z') {
            esp_err_t err = lockdown_client_hash_media();
            ESP_LOGI("afc", "hash: %s", esp_err_to_name(err));
            continue;
        }

        if(c == '~') {
            esp_err_t err = iap2_host_play_current_key();
            ESP_LOGI("art", "play current key: %s", esp_err_to_name(err));
            continue;
        }

        if(c == 'a' || c == 'A') {
            ESP_LOGI("usbmux", "status: %s (ready=%d)", usbmux_host_status(),
                     (int)usbmux_host_is_ready());
            ESP_LOGI("lockdown", "status: %s", lockdown_client_status());
            ESP_LOGI("afc", "status: %s", afc_client_status());
            ESP_LOGI("iap2", "status: %s (connected=%d identified=%d)",
                     iap2_host_status(), (int)iap2_host_is_connected(),
                     (int)iap2_host_is_identified());
            iap2_state_t st;
            if(iap2_host_get_state(&st)) {
                ESP_LOGI("iap2", "power: battery=%d charging=%d state=%u "
                         "ext=%d will=%d max=%dmA mode=%u",
                         st.battery, (int)st.charging,
                         (unsigned)st.charging_state,
                         (int)st.external_charger,
                         (int)st.device_will_charge, st.max_current_ma,
                         (unsigned)st.accessory_power_mode);
            }
            player_snapshot_t snap;
            player_get_snapshot(&snap);
            ESP_LOGI("ui", "snapshot: real=%d status=%d title='%s' artist='%s' "
                     "album='%s' elapsed=%u dur=%u queue=%d/%d bat=%d chg=%d dev='%s' key=%016llx",
                     (int)snap.real_device, (int)snap.status, snap.track.title,
                     snap.track.artist, snap.track.album,
                     (unsigned)snap.elapsed_sec, (unsigned)snap.duration_sec,
                     player_queue_index(), player_queue_count(), snap.battery,
                     (int)snap.charging, snap.device_name,
                     (unsigned long long)snap.track.id);
            ESP_LOGI("heap", "internal=%u largest_dma=%u psram=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            continue;
        }

        if(c == 'P') {
            esp_err_t err = iap2_host_request_charging();
            ESP_LOGI("iap2", "charging request: %s", esp_err_to_name(err));
            continue;
        }

        if(c == '*') {
            esp_err_t en = bsp_feature_enable(BSP_FEATURE_USB, true);
            esp_io_expander_handle_t io = bsp_io_expander1_init();
            uint32_t level = 0, out = 0, dir = 0;
            esp_err_t rd = io ? esp_io_expander_get_level(
                io, IO_EXPANDER_PIN_NUM_3 | IO_EXPANDER_PIN_NUM_5 |
                    IO_EXPANDER_PIN_NUM_6 | IO_EXPANDER_PIN_NUM_7,
                &level) : ESP_ERR_INVALID_STATE;
            if(io && io->read_output_reg) io->read_output_reg(io, &out);
            if(io && io->read_direction_reg) io->read_direction_reg(io, &dir);
            ESP_LOGI("power", "usb enable=%s read=%s in=0x%02x out=0x%02x "
                     "dir=0x%02x usb5v_en=%d nchg_qc_en=%d chg_stat=%d chg_en=%d",
                     esp_err_to_name(en), esp_err_to_name(rd), (unsigned)level,
                     (unsigned)out, (unsigned)dir,
                     (out & IO_EXPANDER_PIN_NUM_3) != 0,
                     (out & IO_EXPANDER_PIN_NUM_5) != 0,
                     (level & IO_EXPANDER_PIN_NUM_6) != 0,
                     (out & IO_EXPANDER_PIN_NUM_7) != 0);
            continue;
        }

        if(c == '&') {
            i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = 0x41,
                .scl_speed_hz = 100000,
            };
            i2c_master_dev_handle_t dev = NULL;
            esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
            if(err == ESP_OK) {
                uint8_t reg;
                uint8_t raw[2];
                uint16_t bus_mv = 0, shunt_uv = 0, mid = 0, did = 0;
                reg = 0x02;
                if(i2c_master_transmit_receive(dev, &reg, 1, raw, 2, 100) == ESP_OK)
                    bus_mv = (uint16_t)((raw[0] << 8) | raw[1]);
                reg = 0x01;
                if(i2c_master_transmit_receive(dev, &reg, 1, raw, 2, 100) == ESP_OK)
                    shunt_uv = (uint16_t)((raw[0] << 8) | raw[1]);
                reg = 0xFE;
                if(i2c_master_transmit_receive(dev, &reg, 1, raw, 2, 100) == ESP_OK)
                    mid = (uint16_t)((raw[0] << 8) | raw[1]);
                reg = 0xFF;
                if(i2c_master_transmit_receive(dev, &reg, 1, raw, 2, 100) == ESP_OK)
                    did = (uint16_t)((raw[0] << 8) | raw[1]);
                i2c_master_bus_rm_device(dev);
                ESP_LOGI("power", "INA226 mid=0x%04x did=0x%04x "
                         "bus=%umV shunt_raw=%d current=%dmA",
                         mid, did, (unsigned)(bus_mv * 5 / 4),
                         (int)(int16_t)shunt_uv,
                         (int)((int16_t)shunt_uv * 5 / 10));
            }
            else {
                ESP_LOGW("power", "INA226 add device: %s",
                         esp_err_to_name(err));
            }
            continue;
        }

        if(c == '^') {
            benchmark_focus(1, "albums", 20);
            continue;
        }

        if(c == '%') {
            benchmark_focus(2, "artists", 20);
            continue;
        }

        if(c == '_') {
            benchmark_focus(3, "songs", 20);
            continue;
        }

        if(c == ':') {
            benchmark_focus(4, "playlists", 20);
            continue;
        }

        if(c == ';') {
            benchmark_focus(6, "settings", 20);
            continue;
        }

        if(c == '?') {
            static bool trace_on;
            trace_on = !trace_on;
            ui_debug_trace(trace_on);
            continue;
        }

        ui_key_t key;
        if(map_serial_key(c, &key)) {
            if(bsp_display_lock(0)) {
                ui_handle_key(key);
                bsp_display_unlock();
            }
        }
    }
}

void serial_input_start(void)
{
    /* 32 KB: taking a full-screen snapshot also runs the LVGL draw path.
     * Keep the stack in PSRAM so the internal DMA heap stays available for
     * SDMMC and the hardware JPEG decoder. */
    xTaskCreateWithCaps(serial_input_task, "serial_keys", 32 * 1024, NULL, 5,
                        NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
