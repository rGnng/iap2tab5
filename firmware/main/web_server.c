#include "web_server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "app_config.h"
#include "ble_remote.h"
#include "screen_brightness.h"
#include "charge.h"
#include "cJSON.h"
#include "rtc_time.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ui.h"
#include "player.h"
#include "bsp/esp-bsp.h"
#include "esp_io_expander.h"
#include "driver/i2c_master.h"
#include "serial_input.h"
#include "usb_hid_input.h"
#include "usbmux_host.h"
#include "lockdown_client.h"
#include "afc_client.h"
#include "iap2_host.h"
#include "debug_log.h"
#include "display_diag.h"

static const char * TAG = "web";

/* esp_restart() can stall while the USB host stack is active and then trip
 * the watchdog.  Reset through the ROM instead; the bootloader reinitialises
 * the PHYs and the C6 co-processor. */
void web_server_reboot(void)
{
    esp_rom_software_reset_system();
    esp_restart();
    for(;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

#define SSE_QUEUE_LEN 2
#define SSE_BUF_SIZE  16384

typedef struct __attribute__((packed)) {
    uint16_t bf_type;
    uint32_t bf_size;
    uint16_t bf_reserved1;
    uint16_t bf_reserved2;
    uint32_t bf_off_bits;
    uint32_t bi_size;
    int32_t bi_width;
    int32_t bi_height;
    uint16_t bi_planes;
    uint16_t bi_bit_count;
    uint32_t bi_compression;
    uint32_t bi_size_image;
    int32_t bi_x_pels_per_meter;
    int32_t bi_y_pels_per_meter;
    uint32_t bi_clr_used;
    uint32_t bi_clr_important;
} bmp_header_t;

_Static_assert(sizeof(bmp_header_t) == 54, "BMP header must be 54 bytes");

static web_config_apply_cb_t s_apply_cb;
static bool (*s_sync_cb)(void);
static QueueHandle_t s_sse_queue;
static char s_net_status[64] = "WiFi off";
static char s_net_ip[32] = "--";

#include "web_index.h"

static bool query_value(httpd_req_t * req, const char * key,
                        char * out, size_t out_len)
{
    char query[160];
    if(httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, out, out_len) == ESP_OK;
}

/* httpd_query_key_value() does not percent-decode.  The debug page sends
 * Enter/Prev/Next as %0D/%5B/%5D, so decode the value before mapping it to a
 * serial key. */
static void url_decode_inplace(char * value)
{
    char * out = value;
    for(char * p = value; *p != '\0'; p++) {
        if(*p == '%' && isxdigit((unsigned char)p[1]) &&
           isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], '\0' };
            *out++ = (char)strtol(hex, NULL, 16);
            p += 2;
        }
        else if(*p == '+') {
            *out++ = ' ';
        }
        else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

static esp_err_t root_get(httpd_req_t * req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t debug_get(httpd_req_t * req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, DEBUG_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t asset_get(httpd_req_t * req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_type(req, strstr(req->uri, ".css") ? "text/css; charset=utf-8" :
                        "application/javascript; charset=utf-8");
    return httpd_resp_sendstr(req, (const char *)req->user_ctx);
}

static esp_err_t debuglog_get(httpd_req_t * req)
{
    FILE * f = fopen("/sdcard/debug.log", "r");
    if(f == NULL) {
        return httpd_resp_send_err(
            req, HTTPD_404_NOT_FOUND,
            "SD debug log not found; enable Debug log to SD first");
    }

    httpd_resp_set_type(req, "text/plain");
    char buf[4096];
    size_t n;
    while((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if(httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t log_get(httpd_req_t * req)
{
    uint32_t since = 0;
    char value[24];
    if(query_value(req, "since", value, sizeof(value))) {
        since = (uint32_t)strtoul(value, NULL, 10);
    }

    const size_t cap = 32 * 1024;
    char * buf = malloc(cap);
    if(buf == NULL) return httpd_resp_send_500(req);

    size_t len = 0;
    uint32_t dropped = 0;
    uint32_t next = debug_log_read_text(since, buf, cap, &len, &dropped);

    char header[24], dropped_header[24];
    snprintf(header, sizeof(header), "%u", (unsigned)next);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Log-Seq", header);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if(dropped > 0) {
        snprintf(dropped_header, sizeof(dropped_header), "%u", (unsigned)dropped);
        httpd_resp_set_hdr(req, "X-Log-Dropped", dropped_header);
    }

    esp_err_t err = httpd_resp_send(req, buf, (ssize_t)len);
    free(buf);
    return err;
}

static esp_err_t loglevel_post(httpd_req_t * req)
{
    char tag[32] = "*";
    char value[8] = "4";
    query_value(req, "tag", tag, sizeof(tag));
    query_value(req, "level", value, sizeof(value));

    int level = atoi(value);
    if(level < ESP_LOG_NONE) level = ESP_LOG_NONE;
    if(level > ESP_LOG_VERBOSE) level = ESP_LOG_VERBOSE;
    esp_log_level_set(tag, (esp_log_level_t)level);

    char reply[96];
    snprintf(reply, sizeof(reply), "log level: tag '%s' -> %d", tag, level);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, reply);
}

static esp_err_t usbmode_post(httpd_req_t * req)
{
    char value[8] = "0";
    query_value(req, "mode", value, sizeof(value));
    int mode = atoi(value);
    if(mode != 0 && mode != 2) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(
            req, "invalid mode: 0 = USB-A HS, 2 = Both A+C");
    }

    g_app_config.usb_mode = (uint8_t)mode;
    app_config_save();

    const char * reply = mode == 2
        ? "USB-A HS + USB-C FS (Both A+C) selected; rebooting"
        : "USB-A HS host selected; rebooting";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, reply);
    vTaskDelay(pdMS_TO_TICKS(800));
    web_server_reboot();
    return ESP_OK;
}

static bool web_key_parse(const char *name, ui_key_t *key)
{
    static const struct { const char *name; ui_key_t key; } keys[] = {
        {"up", UI_KEY_UP},
        {"down", UI_KEY_DOWN},
        {"enter", UI_KEY_ENTER},
        {"back", UI_KEY_BACK},
        {"context", UI_KEY_CONTEXT},
        {"queue", UI_KEY_PAGE_UP},
        {"pageup", UI_KEY_PAGE_UP},
        {"pagedown", UI_KEY_PAGE_DOWN},
        {"tabprev", UI_KEY_SCREEN_PREV},
        {"tabnext", UI_KEY_SCREEN_NEXT},
        {"prev", UI_KEY_PREV_TRACK},
        {"next", UI_KEY_NEXT_TRACK},
        {"play", UI_KEY_7},
        {"star", UI_KEY_STAR},
        {"shuffle", UI_KEY_SHUFFLE},
        {"repeat", UI_KEY_REPEAT},
        {"volup", UI_KEY_VOLUME_UP},
        {"voldown", UI_KEY_VOLUME_DOWN},
        {"mute", UI_KEY_MUTE},
        {"nowplaying", UI_KEY_1},
        {"albums", UI_KEY_2},
        {"artists", UI_KEY_3},
        {"songs", UI_KEY_4},
        {"playlists", UI_KEY_5},
        {"w", UI_KEY_UP},
        {"W", UI_KEY_UP},
        {"s", UI_KEY_DOWN},
        {"S", UI_KEY_DOWN},
        {"b", UI_KEY_BACK},
        {"B", UI_KEY_BACK},
        {"p", UI_KEY_PAGE_UP},
        {"P", UI_KEY_PAGE_UP},
        {"n", UI_KEY_PAGE_DOWN},
        {"N", UI_KEY_PAGE_DOWN},
        {"[", UI_KEY_SCREEN_PREV},
        {"]", UI_KEY_SCREEN_NEXT},
        {"c", UI_KEY_CONTEXT},
        {"C", UI_KEY_CONTEXT},
        {"1", UI_KEY_1},
        {"2", UI_KEY_2},
        {"3", UI_KEY_3},
        {"4", UI_KEY_4},
        {"5", UI_KEY_5},
        {"6", UI_KEY_6},
        {"7", UI_KEY_7},
        {"\r", UI_KEY_ENTER}, {"\n", UI_KEY_ENTER},
    };
    for(size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if(strcmp(name, keys[i].name) == 0) { *key = keys[i].key; return true; }
    }
    return false;
}

static esp_err_t key_post(httpd_req_t * req)
{
    char value[32] = {0};
    if(!query_value(req, "k", value, sizeof(value)) || value[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "missing k");
    }
    url_decode_inplace(value);

    if(strcmp(value, "nprefresh") == 0) {
        esp_err_t err = iap2_host_refresh_now_playing();
        if(err != ESP_OK) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "iPod not identified");
        }
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Now Playing refresh requested");
    }

    /* Expose the existing UI diagnostics for remote scrolling/perf checks. */
    if(strcmp(value, "scroll") == 0 || strcmp(value, "trace") == 0) {
        char argument[16] = "0";
        bool scroll = strcmp(value, "scroll") == 0;
        query_value(req, scroll ? "dy" : "en", argument, sizeof(argument));
        int amount = atoi(argument);
        if(!bsp_display_lock(1000)) return httpd_resp_send_500(req);
        if(scroll) {
            if(amount < -4096) amount = -4096;
            if(amount > 4096) amount = 4096;
            ui_debug_scroll(amount);
        }
        else ui_debug_trace(amount != 0);
        bsp_display_unlock();
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "ok");
    }

    if(!bsp_display_lock(1000)) return httpd_resp_send_500(req);
    bool ok = true;
    if(strcmp(value, "screenoff") == 0) ui_screen_off();
    else if(strcmp(value, "wake") == 0) ui_user_activity();
    else if(strcmp(value, "settings") == 0) {
        if(!ui_user_activity()) ui_select_screen(6);
    }
    else {
        ui_key_t key;
        ok = web_key_parse(value, &key);
        if(ok) ui_handle_key(key);
    }
    bsp_display_unlock();
    if(!ok) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown key");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t power_get(httpd_req_t * req)
{
    cJSON * root = cJSON_CreateObject();
    if(root == NULL) return httpd_resp_send_500(req);

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x41,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = bus != NULL
                        ? i2c_master_bus_add_device(bus, &dev_cfg, &dev)
                        : ESP_ERR_INVALID_STATE;
    int bus_mv = 0;
    int current_ma = 0;
    if(err == ESP_OK) {
        const uint8_t regs[] = {0x00, 0x02, 0x01};
        uint16_t values[3] = {0};
        uint8_t raw[2];
        for(size_t i = 0; i < 3 && err == ESP_OK; i++) {
            err = i2c_master_transmit_receive(dev, &regs[i], 1, raw, 2, 100);
            if(err == ESP_OK) values[i] = (uint16_t)((raw[0] << 8) | raw[1]);
        }
        i2c_master_bus_rm_device(dev);
        if(err == ESP_OK) {
            int16_t shunt_raw = (int16_t)values[2];
            bus_mv = values[1] * 5 / 4;
            current_ma = shunt_raw * 5 / 10;
            cJSON_AddNumberToObject(root, "shunt_raw", shunt_raw);
            cJSON_AddNumberToObject(root, "ina226_config", values[0]);
        }
    }

    cJSON_AddBoolToObject(root, "ina226_ok", err == ESP_OK);
    cJSON_AddNumberToObject(root, "bus_mv", bus_mv);
    cJSON_AddNumberToObject(root, "current_ma", current_ma);

    esp_io_expander_handle_t io = bsp_io_expander1_init();
    uint32_t level = 0, out = 0, direction = 0, highz = 0xff;
    if(io != NULL) {
        esp_io_expander_get_level(io, IO_EXPANDER_PIN_NUM_3 |
                                          IO_EXPANDER_PIN_NUM_5 |
                                          IO_EXPANDER_PIN_NUM_6 |
                                          IO_EXPANDER_PIN_NUM_7, &level);
        if(io->read_output_reg) io->read_output_reg(io, &out);
        if(io->read_direction_reg) io->read_direction_reg(io, &direction);
        if(io->read_highz_reg) io->read_highz_reg(io, &highz);
    }
    cJSON_AddNumberToObject(root, "expander_output", out);
    cJSON_AddNumberToObject(root, "expander_direction", direction);
    cJSON_AddNumberToObject(root, "expander_highz", highz);
    cJSON_AddBoolToObject(root, "usb5v_en",
                          (out & IO_EXPANDER_PIN_NUM_3) != 0);
    cJSON_AddBoolToObject(root, "nchg_qc_en",
                          (out & IO_EXPANDER_PIN_NUM_5) != 0);
    cJSON_AddBoolToObject(root, "chg_stat",
                          (level & IO_EXPANDER_PIN_NUM_6) != 0);
    cJSON_AddBoolToObject(root, "chg_en", charge_read_enabled());
    cJSON_AddBoolToObject(root, "qc_driven",
                          (direction & ~highz & IO_EXPANDER_PIN_NUM_5) != 0);
    uint32_t lcd_level = 0;
    esp_io_expander_handle_t lcd_io = bsp_io_expander_init();
    bool lcd_ok = lcd_io && esp_io_expander_get_level(lcd_io, BSP_LCD_EN, &lcd_level) == ESP_OK;
    cJSON_AddBoolToObject(root, "lcd_enable_ok", lcd_ok);
    if(lcd_ok) cJSON_AddBoolToObject(root, "lcd_enabled", (lcd_level & BSP_LCD_EN) != 0);

    char * text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(text == NULL) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return send_err;
}

static esp_err_t charge_post(httpd_req_t * req)
{
    char value[8] = "1";
    char qc[8] = {0};
    query_value(req, "en", value, sizeof(value));
    bool enable = atoi(value) != 0;
    bool qc_high = g_app_config.qc_high;
    if(query_value(req, "qc", qc, sizeof(qc))) {
        qc_high = atoi(qc) != 0;
    }

    esp_err_t err = charge_apply(enable, qc_high);
    if(err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "charger configuration failed");
    }
    g_app_config.qc_high = qc_high;
    g_app_config.charge_enabled = enable;
    app_config_save();

    char reply[128];
    snprintf(reply, sizeof(reply), "CHG_EN %d: %s, nCHG_QC_EN %d: %s",
             enable ? 1 : 0, esp_err_to_name(err),
             qc_high ? 1 : 0, esp_err_to_name(err));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, reply);
}

static esp_err_t keystate_post(httpd_req_t * req)
{
    char value[16] = {0};
    char down[8] = "0";
    query_value(req, "k", value, sizeof(value));
    query_value(req, "down", down, sizeof(down));
    url_decode_inplace(value);

    if(strcmp(value, "cancel") == 0) {
        usb_hid_input_cancel_gestures(D100H_INPUT_WEB);
        return httpd_resp_sendstr(req, "ok");
    }
    if(strcmp(value, "topright") == 0) {
        usb_hid_input_post_top_right(D100H_INPUT_WEB, atoi(down) != 0);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "ok");
    }
    if(strcmp(value, "back") == 0 || strcmp(value, "play") == 0) {
        if(strcmp(value, "back") == 0)
            usb_hid_input_post_back(D100H_INPUT_WEB, atoi(down) != 0);
        else usb_hid_input_post_play(D100H_INPUT_WEB, atoi(down) != 0);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "ok");
    }
    if(strcmp(value, "sideprev") == 0 || strcmp(value, "sidenext") == 0) {
        usb_hid_input_post_side(D100H_INPUT_WEB,
            strcmp(value, "sideprev") == 0 ? 0x04 : 0x06, atoi(down) != 0);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "ok");
    }
    if(strcmp(value, "knob") == 0 || strcmp(value, "knobcw") == 0 ||
       strcmp(value, "knobccw") == 0) {
        uint8_t id = strcmp(value, "knob") == 0 ? 0x07 :
                     strcmp(value, "knobcw") == 0 ? 0x0C : 0x0B;
        usb_hid_input_post_knob(D100H_INPUT_WEB, id, atoi(down) != 0);
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "ok");
    }

    ui_key_t key;
    if(strcmp(value, "prev") == 0) {
        key = UI_KEY_PREV_TRACK;
    }
    else if(strcmp(value, "next") == 0) {
        key = UI_KEY_NEXT_TRACK;
    }
    else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "k must be back, play, sideprev, sidenext, prev, next, topright, knob, knobcw, or knobccw");
    }

    usb_hid_input_post_key_state(key, atoi(down) != 0);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t hidbit_post(httpd_req_t * req)
{
    char value[8] = "0";
    query_value(req, "bit", value, sizeof(value));
    int bit = atoi(value);
    if(bit < 0 || bit > 11) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "bit must be 0..11");
    }

    char mode[8] = "tap";
    query_value(req, "mode", mode, sizeof(mode));

    esp_err_t err;
    if(strcmp(mode, "press") == 0) {
        err = iap2_host_hid_press_bit((uint8_t)bit);
    }
    else if(strcmp(mode, "release") == 0) {
        err = iap2_host_hid_release();
    }
    else {
        err = iap2_host_hid_raw_bit((uint8_t)bit);
    }

    char reply[64];
    snprintf(reply, sizeof(reply), "bit %d %s: %s", bit, mode,
             esp_err_to_name(err));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, reply);
}

static esp_err_t screenshot_get(httpd_req_t * req)
{
    if(!bsp_display_lock(1000)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "display busy");
    }
    lv_draw_buf_t * snap = lv_snapshot_take(lv_screen_active(),
                                            LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();
    if(snap == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "snapshot failed");
    }

    const uint32_t w = snap->header.w;
    const uint32_t h = snap->header.h;
    const uint32_t stride = snap->header.stride;
    const uint32_t row_bytes = (w * 3 + 3) & ~3u;
    const uint32_t image_bytes = row_bytes * h;

    bmp_header_t header = {
        .bf_type = 0x4D42, /* 'B', 'M' */
        .bf_size = sizeof(bmp_header_t) + image_bytes,
        .bf_off_bits = sizeof(bmp_header_t),
        .bi_size = 40,
        .bi_width = (int32_t)w,
        .bi_height = (int32_t)h,
        .bi_planes = 1,
        .bi_bit_count = 24,
        .bi_size_image = image_bytes,
    };

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "inline; filename=tab5.bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if(httpd_resp_send_chunk(req, (const char *)&header,
                             sizeof(header)) != ESP_OK) {
        lv_draw_buf_destroy(snap);
        return ESP_FAIL;
    }

    uint8_t * line = malloc(row_bytes);
    if(line == NULL) {
        lv_draw_buf_destroy(snap);
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    for(int32_t y = (int32_t)h - 1; y >= 0; y--) {
        const uint16_t * src = (const uint16_t *)(snap->data +
                                                  (uint32_t)y * stride);
        for(uint32_t x = 0; x < w; x++) {
            uint16_t px = src[x];
            uint8_t r = (px >> 11) & 0x1F;
            uint8_t g = (px >> 5) & 0x3F;
            uint8_t b = px & 0x1F;
            r = (uint8_t)((r << 3) | (r >> 2));
            g = (uint8_t)((g << 2) | (g >> 4));
            b = (uint8_t)((b << 3) | (b >> 2));
            line[x * 3 + 0] = b;
            line[x * 3 + 1] = g;
            line[x * 3 + 2] = r;
        }
        for(uint32_t i = w * 3; i < row_bytes; i++) line[i] = 0;
        err = httpd_resp_send_chunk(req, (const char *)line, row_bytes);
        if(err != ESP_OK) break;
    }

    free(line);
    lv_draw_buf_destroy(snap);
    if(err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void sse_stream(httpd_req_t * req)
{
    /* Request headers are released when the first response chunk is sent. */
    uint32_t seq = 0;
    char cursor[24] = {0};
    if(query_value(req, "since", cursor, sizeof(cursor))) seq = (uint32_t)strtoul(cursor, NULL, 10);
    if(httpd_req_get_hdr_value_str(req, "Last-Event-ID", cursor, sizeof(cursor)) == ESP_OK)
        seq = (uint32_t)strtoul(cursor, NULL, 10);
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char * buf = malloc(SSE_BUF_SIZE);
    if(buf == NULL) return;

    if(httpd_resp_send_chunk(req, "retry: 1000\n\n",
                             HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        free(buf);
        return;
    }

    TickType_t last_send = xTaskGetTickCount();

    for(;;) {
        size_t len = 0;
        uint32_t dropped = 0;
        seq = debug_log_read_sse(seq, buf, SSE_BUF_SIZE, &len, &dropped);
        if(len > 0) {
            if(httpd_resp_send_chunk(req, buf, (ssize_t)len) != ESP_OK) break;
            last_send = xTaskGetTickCount();
            /* Drain the ring quickly while there is new data. */
            continue;
        }
        if((xTaskGetTickCount() - last_send) >= pdMS_TO_TICKS(10000)) {
            if(httpd_resp_send_chunk(req, ": ping\n\n",
                                     HTTPD_RESP_USE_STRLEN) != ESP_OK) break;
            last_send = xTaskGetTickCount();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    free(buf);
}

static void sse_worker_task(void * arg)
{
    (void)arg;
    httpd_req_t * req = NULL;
    for(;;) {
        if(xQueueReceive(s_sse_queue, &req, portMAX_DELAY) == pdTRUE) {
            sse_stream(req);
            httpd_req_async_handler_complete(req);
        }
    }
}

static esp_err_t log_stream_get(httpd_req_t * req)
{
    if(s_sse_queue == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "log stream not started");
    }

    httpd_req_t * copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if(err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "async handler failed");
    }

    if(xQueueSend(s_sse_queue, &copy, 0) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "log stream busy");
    }
    return ESP_OK;
}

static esp_err_t config_get(httpd_req_t * req)
{
    cJSON * root = cJSON_CreateObject();
    if(root == NULL) return httpd_resp_send_500(req);

    cJSON_AddNumberToObject(root, "screen_flip", g_app_config.screen_flip);
    cJSON_AddNumberToObject(root, "wifi_mode", g_app_config.wifi_mode);
    cJSON_AddBoolToObject(root, "sound_enabled", g_app_config.sound_enabled);
    cJSON_AddNumberToObject(root, "sound_mode", g_app_config.sound_mode);
    cJSON_AddNumberToObject(root, "theme", g_app_config.theme);
    cJSON_AddNumberToObject(root, "volume", g_app_config.volume);
    cJSON_AddNumberToObject(root, "brightness", g_app_config.brightness);
    cJSON_AddNumberToObject(root, "screen_off_minutes", g_app_config.screen_off_minutes);
    cJSON_AddBoolToObject(root, "fast_scroll_enabled", g_app_config.fast_scroll_enabled);
    cJSON_AddNumberToObject(root, "fast_scroll_pages", g_app_config.fast_scroll_pages);
    cJSON_AddNumberToObject(root, "fast_scroll_trigger_window_ms", g_app_config.fast_scroll_trigger_window_ms);
    cJSON_AddNumberToObject(root, "fast_scroll_hold_ms", g_app_config.fast_scroll_hold_ms);
    cJSON_AddBoolToObject(root, "debug_log", g_app_config.debug_log);
    cJSON_AddStringToObject(root, "ap_ssid", g_app_config.ap_ssid);
    cJSON_AddBoolToObject(root, "sd_ready", app_config_sd_ready());
    cJSON_AddBoolToObject(root, "iap2_mode", g_app_config.iap2_mode);
    cJSON_AddNumberToObject(root, "usb_mode", g_app_config.usb_mode);
    cJSON_AddBoolToObject(root, "charge_enabled", g_app_config.charge_enabled);
    cJSON_AddBoolToObject(root, "qc_high", g_app_config.qc_high);

    char * text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(text == NULL) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static esp_err_t config_post(httpd_req_t * req)
{
    if(req->content_len == 0 || req->content_len > 2048) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }

    char * body = malloc(req->content_len + 1);
    if(body == NULL) return httpd_resp_send_500(req);

    int received = 0;
    while(received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if(n <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        }
        received += n;
    }
    body[received] = '\0';

    cJSON * root = cJSON_Parse(body);
    free(body);
    if(root == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    const cJSON * enabled = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_enabled");
    if(enabled != NULL && !cJSON_IsBool(enabled)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid Fast Scroll enable value");
    }
    const cJSON * trigger = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_trigger_window_ms");
    if(trigger != NULL && (!cJSON_IsNumber(trigger) || trigger->valuedouble != trigger->valueint ||
       (trigger->valueint != 550 && trigger->valueint != 800 && trigger->valueint != 1200))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid Fast Scroll trigger time");
    }
    const cJSON * item = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_hold_ms");
    if(item != NULL && (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
       (item->valueint!=150 && item->valueint!=300 && item->valueint!=500 && item->valueint!=1500))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid Fast Scroll hold time");
    }
    if(item != NULL) g_app_config.fast_scroll_hold_ms=(uint16_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_pages");
    if(item != NULL && (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
       item->valueint < 0 || item->valueint > 5)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid Fast Scroll value");
    }
    if(item != NULL) {
        /* Keep legacy zero-page requests usable as Disable, while retaining
         * the independently selected threshold for the next Enable. */
        if(item->valueint == 0) g_app_config.fast_scroll_enabled = false;
        else g_app_config.fast_scroll_pages = (uint8_t)item->valueint;
    }
    if(enabled != NULL) g_app_config.fast_scroll_enabled = cJSON_IsTrue(enabled);
    if(trigger != NULL) g_app_config.fast_scroll_trigger_window_ms = (uint16_t)trigger->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "screen_off_minutes");
    if(item != NULL && (!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
       (item->valueint != 0 && item->valueint != 30 && item->valueint != 60))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid Screen Off value");
    }
    if(item != NULL) g_app_config.screen_off_minutes = (uint8_t)item->valueint;
    item = cJSON_GetObjectItemCaseSensitive(root, "theme");
    if(item != NULL) {
        if(!cJSON_IsNumber(item) || item->valuedouble != item->valueint ||
           item->valueint < 0 || item->valueint > 2) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid theme");
        }
        g_app_config.theme = (uint8_t)item->valueint;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "screen_flip");
    if(cJSON_IsNumber(item)) {
        g_app_config.screen_flip = (item->valueint == 180) ? 180 : 0;
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
        g_app_config.sound_enabled = g_app_config.sound_mode != 0;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "debug_log");
    if(cJSON_IsBool(item)) {
        bool enabled = cJSON_IsTrue(item);
        g_app_config.debug_log = enabled;
        debug_log_set_sd_enabled(enabled);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "iap2_mode");
    if(cJSON_IsBool(item)) {
        app_config_set_iap2_mode(cJSON_IsTrue(item));
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "usb_mode");
    if(cJSON_IsNumber(item)) {
        g_app_config.usb_mode = (item->valueint == 2) ? 2 : 0;
    }
    const cJSON * charge_item = cJSON_GetObjectItemCaseSensitive(root, "charge_enabled");
    const cJSON * qc_item = cJSON_GetObjectItemCaseSensitive(root, "qc_high");
    if(cJSON_IsBool(charge_item) || cJSON_IsBool(qc_item)) {
        bool enable = cJSON_IsBool(charge_item) ? cJSON_IsTrue(charge_item)
                                                : g_app_config.charge_enabled;
        bool qc_high = cJSON_IsBool(qc_item) ? cJSON_IsTrue(qc_item)
                                            : g_app_config.qc_high;
        if(charge_apply(enable, qc_high) != ESP_OK) {
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "charger configuration failed");
        }
        g_app_config.charge_enabled = enable;
        g_app_config.qc_high = qc_high;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "volume");
    if(cJSON_IsNumber(item)) {
        int v = item->valueint;
        if(v < 0) v = 0;
        if(v > 100) v = 100;
        g_app_config.volume = (uint8_t)v;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "brightness");
    if(cJSON_IsNumber(item)) {
        g_app_config.brightness = (uint8_t)screen_brightness_normalize(item->valueint);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "ap_ssid");
    if(cJSON_IsString(item) && item->valuestring[0] != '\0') {
        snprintf(g_app_config.ap_ssid, sizeof(g_app_config.ap_ssid), "%s",
                 item->valuestring);
    }

    cJSON_Delete(root);

    esp_err_t save_err = app_config_save();
    if(s_apply_cb != NULL) s_apply_cb();

    char reply[128];
    snprintf(reply, sizeof(reply), "saved%s",
             save_err == ESP_OK ? "" : " (SD not available; RAM only)");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, reply);
}

static void display_diag_json(cJSON *root)
{
    display_diag_sample_t sample;
    if(!display_diag_latest(&sample)) {
        cJSON_AddBoolToObject(root, "available", false);
        return;
    }
    cJSON_AddBoolToObject(root, "available", true);
    cJSON_AddNumberToObject(root, "sample_interval_ms", DISPLAY_DIAG_INTERVAL_MS);
    cJSON_AddNumberToObject(root, "history_capacity", DISPLAY_DIAG_CAPACITY);
    cJSON_AddNumberToObject(root, "requested_brightness", g_app_config.brightness);
#define DIAG_FIELD(name) cJSON_AddNumberToObject(root, #name, sample.name)
    DIAG_FIELD(seq); DIAG_FIELD(uptime_ms); DIAG_FIELD(pwm_duty); DIAG_FIELD(pwm_hz);
    DIAG_FIELD(pwm_period);
    DIAG_FIELD(pwm_duty_changes); DIAG_FIELD(pwm_freq_changes);
    DIAG_FIELD(renders); DIAG_FIELD(render_last_us); DIAG_FIELD(render_max_us);
    DIAG_FIELD(internal_free); DIAG_FIELD(internal_largest);
#undef DIAG_FIELD
#define PORT_FIELD(name) cJSON_AddNumberToObject(root, #name, sample.display.name)
    PORT_FIELD(vsync_count); PORT_FIELD(vsync_gap_us); PORT_FIELD(vsync_max_gap_us);
    PORT_FIELD(vsync_gaps_over_25ms); PORT_FIELD(copy_submitted); PORT_FIELD(copy_completed);
    PORT_FIELD(copy_last_us); PORT_FIELD(copy_max_us); PORT_FIELD(dma_wait_timeouts);
    PORT_FIELD(draw_errors); PORT_FIELD(rotation_errors);
#undef PORT_FIELD
    uint32_t marker, marker_ms;
    display_diag_last_marker(&marker, &marker_ms);
    cJSON_AddNumberToObject(root, "marker", marker);
    cJSON_AddNumberToObject(root, "marker_uptime_ms", marker_ms);
}

static esp_err_t display_diag_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if(!root) return httpd_resp_send_500(req);
    display_diag_json(root);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(!body) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t display_mark_post(httpd_req_t *req)
{
    uint32_t uptime_ms;
    if(!display_diag_mark(&uptime_ms)) return httpd_resp_send_500(req);
    return display_diag_get(req);
}

static int display_sample_csv(char *out, size_t cap, const display_diag_sample_t *s)
{
    const lvgl_port_display_stats_t *d = &s->display;
    return snprintf(out, cap,
        "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
        (unsigned long)s->seq, (unsigned long)s->uptime_ms, (unsigned long)s->pwm_duty,
        (unsigned long)s->pwm_hz, (unsigned long)s->pwm_duty_changes, (unsigned long)s->pwm_freq_changes,
        (unsigned long)d->vsync_count, (unsigned long)d->vsync_gap_us, (unsigned long)d->vsync_max_gap_us,
        (unsigned long)d->vsync_gaps_over_25ms, (unsigned long)d->copy_submitted, (unsigned long)d->copy_completed,
        (unsigned long)d->copy_last_us, (unsigned long)d->copy_max_us, (unsigned long)d->dma_wait_timeouts,
        (unsigned long)d->draw_errors, (unsigned long)d->rotation_errors,
        (unsigned long)s->renders, (unsigned long)s->render_last_us, (unsigned long)s->render_max_us,
        (unsigned long)s->internal_free, (unsigned long)s->internal_largest,
        (unsigned long)s->pwm_period);
}

static esp_err_t display_trace_get(httpd_req_t *req)
{
    char cursor[24] = "0";
    query_value(req, "since", cursor, sizeof(cursor));
    display_diag_sample_t *samples = heap_caps_malloc(DISPLAY_DIAG_CAPACITY * sizeof(*samples),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!samples || !chunk) { free(samples); free(chunk); return httpd_resp_send_500(req); }
    uint32_t next, dropped, marker, marker_ms;
    size_t count = display_diag_read((uint32_t)strtoul(cursor, NULL, 10), samples,
                                     DISPLAY_DIAG_CAPACITY, &next, &dropped);
    display_diag_last_marker(&marker, &marker_ms);
    char header[96];
    snprintf(header, sizeof(header), "attachment; filename=tab5-display-%u-%ums.csv", (unsigned)marker, (unsigned)marker_ms);
    httpd_resp_set_hdr(req, "Content-Disposition", header);
    char next_text[16], dropped_text[16];
    snprintf(next_text, sizeof(next_text), "%u", (unsigned)next);
    snprintf(dropped_text, sizeof(dropped_text), "%u", (unsigned)dropped);
    httpd_resp_set_hdr(req, "X-Diag-Next", next_text);
    httpd_resp_set_hdr(req, "X-Diag-Dropped", dropped_text);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    esp_err_t err = httpd_resp_send_chunk(req,
        "seq,uptime_ms,pwm_duty,pwm_hz,pwm_duty_changes,pwm_freq_changes,vsync_count,vsync_gap_us,vsync_max_gap_us,vsync_gaps_over_25ms,copy_submitted,copy_completed,copy_last_us,copy_max_us,dma_wait_timeouts,draw_errors,rotation_errors,renders,render_last_us,render_max_us,internal_free,internal_largest,pwm_period\n",
        HTTPD_RESP_USE_STRLEN);
    size_t used = 0;
    for(size_t i = 0; err == ESP_OK && i < count; i++) {
        if(used + 512 > 4096) { err = httpd_resp_send_chunk(req, chunk, used); used = 0; }
        if(err == ESP_OK) used += (size_t)display_sample_csv(chunk + used, 4096 - used, &samples[i]);
    }
    if(err == ESP_OK && used) err = httpd_resp_send_chunk(req, chunk, used);
    free(samples); free(chunk);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static cJSON * bluetooth_json(void)
{
    cJSON * bt = cJSON_CreateObject();
    if(bt == NULL) return NULL;
    ble_remote_state_t state;
    char text[80];
    ble_remote_status(&state, text, sizeof(text));
    cJSON_AddBoolToObject(bt, "enabled", ble_remote_enabled());
    cJSON_AddBoolToObject(bt, "supported", ble_remote_is_supported());
    cJSON_AddBoolToObject(bt, "scanning", state == BLE_REMOTE_SCANNING);
    cJSON_AddNumberToObject(bt, "revision", ble_remote_revision());
    cJSON_AddNumberToObject(bt, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(bt, "state", state);
    cJSON_AddStringToObject(bt, "status", text);
    ble_remote_link_stats_t stats;
    ble_remote_get_link_stats(&stats);
    cJSON * link = cJSON_AddObjectToObject(bt, "link");
    cJSON_AddNumberToObject(link, "notifications", stats.notifications);
    cJSON_AddNumberToObject(link, "heartbeat_replies", stats.heartbeat_replies);
    cJSON_AddNumberToObject(link, "responsive_link_checks", stats.link_checks);
    cJSON_AddNumberToObject(link, "notification_age_ms", (double)stats.notification_age_ms);
    cJSON_AddNumberToObject(link, "heartbeat_age_ms", (double)stats.heartbeat_age_ms);
    cJSON_AddNumberToObject(link, "last_disconnect_reason", stats.last_disconnect_reason);
    cJSON_AddStringToObject(link, "last_recovery", stats.last_recovery);
    cJSON * devices = cJSON_AddArrayToObject(bt, "devices");
    for(int i = 0; i < ble_remote_paired_count(); i++) {
        ble_remote_device_t dev;
        if(!ble_remote_paired_get(i, &dev)) continue;
        ble_remote_info_t info;
        ble_remote_get_info(dev.addr, &info);
        cJSON * item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", dev.name);
        cJSON_AddStringToObject(item, "addr", dev.addr);
        cJSON_AddBoolToObject(item, "connected", info.connected);
        if(info.battery_valid) {
            cJSON_AddNumberToObject(item, "battery", info.battery_percent);
            cJSON_AddBoolToObject(item, "charging", info.charging);
            cJSON_AddBoolToObject(item, "charge_full", info.charge_full);
        }
        else {
            cJSON_AddNullToObject(item, "battery");
            cJSON_AddNullToObject(item, "charging");
            cJSON_AddNullToObject(item, "charge_full");
        }
        cJSON_AddBoolToObject(item, "sleep_disabled", info.sleep_disabled);
        if(info.standby_time_valid)
            cJSON_AddNumberToObject(item, "standby_seconds", info.standby_seconds);
        else cJSON_AddNullToObject(item, "standby_seconds");
        if(info.standby_state_valid)
            cJSON_AddBoolToObject(item, "standby", info.standby);
        else cJSON_AddNullToObject(item, "standby");
        cJSON_AddItemToArray(devices, item);
    }
    cJSON * discovered = cJSON_AddArrayToObject(bt, "discovered");
    for(int i = 0; i < ble_remote_scan_count(); i++) {
        ble_remote_device_t dev;
        if(!ble_remote_scan_get(i, &dev)) continue;
        bool paired = false;
        for(int j = 0; j < ble_remote_paired_count(); j++) {
            ble_remote_device_t saved;
            if(ble_remote_paired_get(j, &saved) && strcasecmp(dev.addr, saved.addr) == 0) {
                paired = true;
                break;
            }
        }
        if(paired) continue;
        cJSON * item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", dev.name);
        cJSON_AddStringToObject(item, "addr", dev.addr);
        cJSON_AddNumberToObject(item, "rssi", dev.rssi);
        cJSON_AddItemToArray(discovered, item);
    }
    return bt;
}

static esp_err_t bluetooth_reply(httpd_req_t * req, const char * message)
{
    cJSON * bt = bluetooth_json();
    if(bt == NULL) return httpd_resp_send_500(req);
    if(message != NULL) cJSON_AddStringToObject(bt, "message", message);
    char * text = cJSON_PrintUnformatted(bt);
    cJSON_Delete(bt);
    if(text == NULL) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t bluetooth_get(httpd_req_t * req)
{
    return bluetooth_reply(req, NULL);
}

static bool bluetooth_known(const char * addr, bool paired)
{
    int count = paired ? ble_remote_paired_count() : ble_remote_scan_count();
    for(int i = 0; i < count; i++) {
        ble_remote_device_t dev;
        bool ok = paired ? ble_remote_paired_get(i, &dev) : ble_remote_scan_get(i, &dev);
        if(ok && strcasecmp(addr, dev.addr) == 0) return true;
    }
    return false;
}

static esp_err_t bluetooth_post(httpd_req_t * req)
{
    if(req->content_len <= 0 || req->content_len > 256)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    char body[257];
    int received = 0;
    while(received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if(n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        received += n;
    }
    body[received] = '\0';
    cJSON * root = cJSON_ParseWithOpts(body, NULL, true);
    const cJSON * action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if(!cJSON_IsObject(root) || !cJSON_IsString(action)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action required");
    }
    const char * command = action->valuestring;
    const char * message = NULL;
    const char * error = NULL;
    httpd_err_code_t error_code = HTTPD_400_BAD_REQUEST;
    if(strcmp(command, "enable") == 0) {
        const cJSON * enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        if(!cJSON_IsBool(enabled)) error = "enabled must be a boolean";
        else {
            g_app_config.ble_enabled = cJSON_IsTrue(enabled);
            ble_remote_set_enabled(g_app_config.ble_enabled);
            bool saved = app_config_save() == ESP_OK;
            if(s_apply_cb != NULL) s_apply_cb();
            message = saved ? (g_app_config.ble_enabled ? "Bluetooth enabled" : "Bluetooth disabled")
                            : "Bluetooth updated; setting could not be saved to SD";
        }
    }
    else if(strcmp(command, "scan") == 0 || strcmp(command, "stop_scan") == 0) {
        if(!ble_remote_enabled()) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "Enable Bluetooth first");
        }
        if(strcmp(command, "scan") == 0) {
            ble_remote_scan_start();
            message = "Scanning for controllers";
        }
        else {
            ble_remote_scan_stop();
            message = "Scan stopped";
        }
    }
    else if(strcmp(command, "connect") == 0 || strcmp(command, "pair") == 0 ||
            strcmp(command, "forget") == 0) {
        const cJSON * address = cJSON_GetObjectItemCaseSensitive(root, "addr");
        if(!cJSON_IsString(address) || strlen(address->valuestring) != 17) error = "valid addr required";
        else {
            const char * addr = address->valuestring;
            bool forget = strcmp(command, "forget") == 0;
            bool pair = strcmp(command, "pair") == 0;
            if(!bluetooth_known(addr, !pair)) {
                error = pair ? "Controller not found; scan again" : "Paired controller not found";
                error_code = HTTPD_404_NOT_FOUND;
            }
            else if(!forget && !ble_remote_enabled()) {
                cJSON_Delete(root);
                httpd_resp_set_status(req, "409 Conflict");
                return httpd_resp_sendstr(req, "Enable Bluetooth first");
            }
            else if(forget ? !ble_remote_unpair(addr) : !ble_remote_connect(addr)) {
                error = "Bluetooth request failed";
                error_code = HTTPD_500_INTERNAL_SERVER_ERROR;
            }
            else message = forget ? "Controller deleted" : pair ? "Pairing requested" : "Connection requested";
        }
    }
    else error = "Unknown Bluetooth action";
    cJSON_Delete(root);
    if(error != NULL) return httpd_resp_send_err(req, error_code, error);
    return bluetooth_reply(req, message);
}

static esp_err_t status_get(httpd_req_t * req)
{
    cJSON * root = cJSON_CreateObject();
    if(root == NULL) return httpd_resp_send_500(req);

    const esp_app_desc_t * app = esp_app_get_description();
    cJSON_AddStringToObject(root, "firmware", app->version);
    cJSON_AddStringToObject(root, "build", __DATE__ " " __TIME__);
    cJSON_AddItemToObject(root, "bluetooth", bluetooth_json());
    cJSON_AddNumberToObject(root, "reset_reason", (int)esp_reset_reason());
    time_t now = time(NULL);
    cJSON_AddNumberToObject(root, "epoch", (double)now);
    cJSON_AddBoolToObject(root, "rtc", rtc_time_available());
    struct tm local_tm;
    if(localtime_r(&now, &local_tm) != NULL) {
        char local_text[32];
        strftime(local_text, sizeof(local_text), "%Y-%m-%d %H:%M:%S",
                 &local_tm);
        cJSON_AddStringToObject(root, "local_time", local_text);
    }
    cJSON_AddNumberToObject(root, "usb_mode", g_app_config.usb_mode);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_psram",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(root, "sd_ready", app_config_sd_ready());
    app_config_sd_info_t sd_info;
    if(app_config_sd_info(&sd_info)) {
        cJSON_AddNumberToObject(root, "sd_total", (double)sd_info.total_bytes);
        cJSON_AddNumberToObject(root, "sd_used", (double)sd_info.used_bytes);
    }
    cJSON_AddBoolToObject(root, "debug_log", g_app_config.debug_log);
    cJSON_AddBoolToObject(root, "debug_log_sd_open", debug_log_sd_file_open());
    cJSON_AddNumberToObject(root, "debug_log_sd_lines",
                            (double)debug_log_sd_lines());
    cJSON_AddNumberToObject(root, "debug_log_sd_errors",
                            (double)debug_log_sd_errors());
    cJSON_AddBoolToObject(root, "iap2_mode", g_app_config.iap2_mode);
    cJSON_AddBoolToObject(root, "charge_enabled", g_app_config.charge_enabled);
    cJSON_AddBoolToObject(root, "qc_high", g_app_config.qc_high);
    cJSON_AddStringToObject(root, "wifi", s_net_status);
    cJSON_AddStringToObject(root, "ip", s_net_ip);
    cJSON_AddStringToObject(root, "usbmux", usbmux_host_status());
    cJSON_AddStringToObject(root, "lockdown", lockdown_client_status());
    lockdown_pair_status_t pairing;
    lockdown_client_pair_status(&pairing);
    cJSON_AddBoolToObject(root, "apple_pair_busy", pairing.busy);
    cJSON_AddBoolToObject(root, "apple_trusted", pairing.trusted && usbmux_host_is_ready());
    cJSON_AddStringToObject(root, "apple_pair_status", pairing.message);
    cJSON_AddStringToObject(root, "afc", afc_client_status());
    lockdown_artwork_progress_t progress;
    lockdown_client_artwork_progress(&progress);
    cJSON * sync = cJSON_AddObjectToObject(root, "afc_sync");
    if(sync != NULL) {
        cJSON_AddBoolToObject(sync, "active", progress.active);
        cJSON_AddNumberToObject(sync, "phase", progress.phase);
        cJSON_AddNumberToObject(sync, "total", progress.total);
        cJSON_AddNumberToObject(sync, "attempted", progress.attempted);
        cJSON_AddNumberToObject(sync, "cached", progress.copied);
        cJSON_AddNumberToObject(sync, "failed", progress.failed);
    }
    cJSON_AddStringToObject(root, "iap2", iap2_host_status());
    cJSON_AddBoolToObject(root, "iap2_identified",
                          iap2_host_is_identified());
    iap2_link_stats_t link;
    iap2_host_link_stats(&link);
    cJSON * transport = cJSON_AddObjectToObject(root, "iap2_link");
    if(transport != NULL) {
#if defined(IAP2TAB5_FULL_LIBRARY_SYNC)
        cJSON_AddBoolToObject(transport, "full_library_sync", true);
#else
        cJSON_AddBoolToObject(transport, "full_library_sync", false);
#endif
#define LINK_STAT(name) cJSON_AddNumberToObject(transport, #name, link.name)
        LINK_STAT(rx_packets); LINK_STAT(bad_headers); LINK_STAT(bad_payloads);
        LINK_STAT(duplicates); LINK_STAT(sequence_gaps); LINK_STAT(rx_overflows);
        LINK_STAT(hid_errors); LINK_STAT(queue_stalls); LINK_STAT(queue_high_water);
        LINK_STAT(link_resets); LINK_STAT(retransmits);
        LINK_STAT(library_messages); LINK_STAT(library_items);
#undef LINK_STAT
        cJSON_AddBoolToObject(transport, "library_complete", link.library_complete);
    }
    iap2_state_t iap2;
    if(iap2_host_get_state(&iap2)) {
        cJSON_AddNumberToObject(root, "play_status", iap2.play_status);
        cJSON_AddStringToObject(root, "title", iap2.title);
        cJSON_AddStringToObject(root, "artist", iap2.artist);
        cJSON_AddStringToObject(root, "album", iap2.album);
        cJSON_AddStringToObject(root, "device_name", iap2.device_name);
        cJSON_AddNumberToObject(root, "elapsed_ms", iap2.elapsed_ms);
        cJSON_AddNumberToObject(root, "duration_ms", iap2.duration_ms);
        cJSON_AddBoolToObject(root, "shuffle", iap2.shuffle);
        cJSON_AddNumberToObject(root, "repeat", iap2.repeat);
        cJSON_AddNumberToObject(root, "device_battery", iap2.battery);
        cJSON_AddBoolToObject(root, "device_charging", iap2.charging);
        cJSON_AddNumberToObject(root, "device_charging_state",
                                iap2.charging_state);
        cJSON_AddBoolToObject(root, "device_external_charger",
                              iap2.external_charger);
        cJSON_AddNumberToObject(root, "device_max_current_ma",
                                iap2.max_current_ma);
    }
    if(bsp_display_lock(100)) {
        player_snapshot_t snap;
        player_get_snapshot(&snap);
        cJSON_AddBoolToObject(root, "starred", snap.starred);
        cJSON_AddBoolToObject(root, "screen_on", ui_screen_is_on());
        cJSON_AddBoolToObject(root, "library_available", ui_library_available());
        bsp_display_unlock();
    }
    cJSON_AddNumberToObject(root, "screen_off_minutes", g_app_config.screen_off_minutes);
    cJSON_AddBoolToObject(root, "fast_scroll_enabled", g_app_config.fast_scroll_enabled);
    cJSON_AddNumberToObject(root, "fast_scroll_pages", g_app_config.fast_scroll_pages);
    cJSON_AddNumberToObject(root, "fast_scroll_trigger_window_ms", g_app_config.fast_scroll_trigger_window_ms);
    cJSON_AddNumberToObject(root, "fast_scroll_hold_ms", g_app_config.fast_scroll_hold_ms);
    cJSON_AddNumberToObject(root, "screen_flip", g_app_config.screen_flip);
    cJSON_AddNumberToObject(root, "volume", g_app_config.volume);
    const esp_partition_t * ota = esp_ota_get_next_update_partition(NULL);
    cJSON *display = cJSON_AddObjectToObject(root, "display");
    if(display) display_diag_json(display);
    cJSON_AddStringToObject(root, "ota_partition", ota ? ota->label : "none");

    char * text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(text == NULL) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static esp_err_t time_post(httpd_req_t * req)
{
    if(req->content_len == 0 || req->content_len > 256) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }

    char body[257];
    int received = 0;
    while(received < (int)req->content_len) {
        int n = httpd_req_recv(req, body + received,
                               (int)req->content_len - received);
        if(n <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "recv failed");
        }
        received += n;
    }
    body[received] = '\0';

    cJSON * root = cJSON_Parse(body);
    if(root == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    }
    cJSON * epoch_item = cJSON_GetObjectItem(root, "epoch");
    if(!cJSON_IsNumber(epoch_item)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing epoch");
    }

    double epoch_value = epoch_item->valuedouble;
    cJSON_Delete(root);

    /* RX8130CE stores 2000-2099.  Keep a little slack for browser clocks. */
    if(epoch_value < 946684800.0 || epoch_value > 4102444799.0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "epoch out of range");
    }

    esp_err_t err = rtc_time_sync_epoch((time_t)epoch_value);
    if(bsp_display_lock(1000)) {
        ui_clock_synced();
        bsp_display_unlock();
    }

    httpd_resp_set_type(req, "text/plain");
    if(err == ESP_OK) {
        return httpd_resp_sendstr(req, "time synced to RTC");
    }
    if(err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_sendstr(req,
                                  "system time updated; RTC unavailable");
    }
    return httpd_resp_sendstr(req,
                              "RTC write failed; system time updated");
}

static esp_err_t reboot_post(httpd_req_t * req)
{
    httpd_resp_sendstr(req, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    web_server_reboot();
    return ESP_OK;
}

/* Raw library.bin upload for development.  The new image is written to the
 * per-library path and the device reboots so player_init() can load it. */
static esp_err_t library_post(httpd_req_t * req)
{
    if(!app_config_sd_ready()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "SD not ready");
    }
    if(req->content_len <= 0 || req->content_len > 32 * 1024 * 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "bad library size");
    }

    const char * path = app_config_library_bin_path();
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE * f = fopen(tmp, "wb");
    if(f == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "open failed");
    }

    char buffer[4096];
    uint8_t head[12];
    size_t head_len = 0;
    int remaining = req->content_len;
    bool ok = true;
    while(remaining > 0) {
        int want = remaining < (int)sizeof(buffer) ? remaining
                                                   : (int)sizeof(buffer);
        int n = httpd_req_recv(req, buffer, want);
        if(n <= 0) {
            ok = false;
            break;
        }
        if(head_len < sizeof(head)) {
            size_t copy = sizeof(head) - head_len;
            if(copy > (size_t)n) copy = (size_t)n;
            memcpy(head + head_len, buffer, copy);
            head_len += copy;
        }
        if(fwrite(buffer, 1, (size_t)n, f) != (size_t)n) {
            ok = false;
            break;
        }
        remaining -= n;
    }
    if(fclose(f) != 0) ok = false;
    if(ok) {
        ok = head_len >= sizeof(head) &&
             memcmp(head, "T5LIB003", 8) == 0 &&
             head[8] == 3 && head[9] == 0 && head[10] == 0 && head[11] == 0;
    }
    if(!ok) {
        remove(tmp);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid library image");
    }
    /* FATFS rename() does not replace an existing target. */
    remove(path);
    if(rename(tmp, path) != 0) {
        remove(tmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "rename failed");
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "library OK, rebooting");
    vTaskDelay(pdMS_TO_TICKS(800));
    web_server_reboot();
    return ESP_OK;
}

static esp_err_t sync_post(httpd_req_t * req)
{
    if(!bsp_display_lock(1000)) return httpd_resp_send_500(req);
    bool started = s_sync_cb && s_sync_cb();
    bsp_display_unlock();
    if(!started) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Sync unavailable: use AFC mode, pair the device and insert an SD card; wait for any active job.");
    }
    return httpd_resp_sendstr(req, "Sync started");
}

static esp_err_t cache_reset_post(httpd_req_t * req)
{
    lockdown_artwork_progress_t progress;
    lockdown_pair_status_t pairing;
    lockdown_client_artwork_progress(&progress);
    lockdown_client_pair_status(&pairing);
    if(progress.active || pairing.busy || app_config_peek_afc_job() ||
       !app_config_sd_ready()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
            "Wait for sync or pairing to finish and ensure the SD card is ready.");
    }
    if(app_config_set_iap2_mode(false) != ESP_OK ||
       app_config_set_afc_job(3) != ESP_OK) return httpd_resp_send_500(req);
    httpd_resp_sendstr(req, "Rebooting to clear artwork cache and sync.");
    vTaskDelay(pdMS_TO_TICKS(800));
    web_server_reboot();
    return ESP_OK;
}

static esp_err_t trust_post(httpd_req_t * req)
{
    esp_err_t err = lockdown_client_request_pair();
    if(err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
        "Connect a device in AFC mode and wait for any sync to finish.");
    return httpd_resp_sendstr(req, "Pairing started. Unlock the device and tap Trust.");
}

static esp_err_t pair_post(httpd_req_t * req)
{
    if(req->content_len == 0 || req->content_len > 256 * 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }

    char * body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM);
    if(body == NULL) return httpd_resp_send_500(req);

    int received = 0;
    while(received < (int)req->content_len) {
        int n = httpd_req_recv(req, body + received,
                               (int)req->content_len - received);
        if(n <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "recv failed");
        }
        received += n;
    }
    body[received] = '\0';

    esp_err_t err = lockdown_client_set_pair_record(body, (size_t)received);
    free(body);

    httpd_resp_set_type(req, "text/plain");
    if(err != ESP_OK) {
        return httpd_resp_sendstr(req, "pair record rejected");
    }
    return httpd_resp_sendstr(req, "pair record stored; starting session");
}

static esp_err_t ota_post(httpd_req_t * req)
{
    const esp_partition_t * part = esp_ota_get_next_update_partition(NULL);
    if(part == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no OTA partition");
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota);
    if(err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "ota begin failed");
    }

    char buffer[4096];
    int remaining = req->content_len;
    while(remaining > 0) {
        int n = httpd_req_recv(req, buffer,
                               remaining < (int)sizeof(buffer) ? remaining
                                                               : (int)sizeof(buffer));
        if(n <= 0) {
            esp_ota_abort(ota);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        }
        err = esp_ota_write(ota, buffer, n);
        if(err != ESP_OK) {
            esp_ota_abort(ota);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "ota write failed");
        }
        remaining -= n;
    }

    err = esp_ota_end(ota);
    if(err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "ota image invalid");
    }
    err = esp_ota_set_boot_partition(part);
    if(err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "set boot partition failed");
    }

    httpd_resp_sendstr(req, "OTA OK, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    web_server_reboot();
    return ESP_OK;
}

void web_server_self_test(void)
{
    esp_http_client_config_t cfg = {
        .url = "http://192.168.4.1/api/status",
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(client == NULL) {
        ESP_LOGE(TAG, "http client init failed");
        return;
    }

    char buffer[257];

    if(esp_http_client_open(client, 0) == ESP_OK) {
        int len = esp_http_client_fetch_headers(client);
        int n = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
        if(n < 0) n = 0;
        buffer[n] = '\0';
        ESP_LOGI(TAG, "self-test HTTP %d, %d bytes: %s",
                 esp_http_client_get_status_code(client), len, buffer);
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);

    char body[512] = {0};
    int body_len = 0;

    cfg.url = "http://192.168.4.1/api/config";
    client = esp_http_client_init(&cfg);
    if(client != NULL) {
        if(esp_http_client_open(client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(client);
            body_len = esp_http_client_read(client, body, sizeof(body) - 1);
            if(body_len < 0) body_len = 0;
            body[body_len] = '\0';
            ESP_LOGI(TAG, "config GET: %s", body);
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
    }

    if(body_len > 0) {
        cfg.url = "http://192.168.4.1/api/config";
        client = esp_http_client_init(&cfg);
        if(client != NULL) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            esp_http_client_set_header(client, "Content-Type", "application/json");
            if(esp_http_client_open(client, body_len) == ESP_OK) {
                esp_http_client_write(client, body, body_len);
                esp_http_client_fetch_headers(client);
                int n = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
                if(n < 0) n = 0;
                buffer[n] = '\0';
                ESP_LOGI(TAG, "config POST: %s (HTTP %d)", buffer,
                         esp_http_client_get_status_code(client));
                esp_http_client_close(client);
            }
            esp_http_client_cleanup(client);
        }
    }
}

void web_server_set_sync_cb(bool (*cb)(void)) { s_sync_cb = cb; }

void web_server_set_apply_cb(web_config_apply_cb_t cb)
{
    s_apply_cb = cb;
}

void web_server_set_network_info(const char * status, const char * ip)
{
    if(status != NULL) snprintf(s_net_status, sizeof(s_net_status), "%s", status);
    if(ip != NULL) snprintf(s_net_ip, sizeof(s_net_ip), "%s", ip);
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 40;
    config.stack_size = 12288;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_sse_queue = xQueueCreate(SSE_QUEUE_LEN, sizeof(httpd_req_t *));
    if(s_sse_queue == NULL) {
        ESP_LOGW(TAG, "SSE queue allocation failed");
    }
    else if(xTaskCreate(sse_worker_task, "sse_log", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "SSE worker task creation failed");
        vQueueDelete(s_sse_queue);
        s_sse_queue = NULL;
    }

    const httpd_uri_t root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get,
    };
    const httpd_uri_t cfg_get = {
        .uri = "/api/config", .method = HTTP_GET, .handler = config_get,
    };
    const httpd_uri_t cfg_post = {
        .uri = "/api/config", .method = HTTP_POST, .handler = config_post,
    };
    const httpd_uri_t status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_get,
    };
    const httpd_uri_t bluetooth_routes[] = {
        {.uri="/api/bluetooth", .method=HTTP_GET, .handler=bluetooth_get},
        {.uri="/api/bluetooth", .method=HTTP_POST, .handler=bluetooth_post},
    };
    for(size_t i=0; i < sizeof(bluetooth_routes)/sizeof(bluetooth_routes[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &bluetooth_routes[i]));
    const httpd_uri_t time_sync = {
        .uri = "/api/time", .method = HTTP_POST, .handler = time_post,
    };
    const httpd_uri_t reboot = {
        .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post,
    };
    const httpd_uri_t ota = {
        .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post,
    };
    const httpd_uri_t library = {
        .uri = "/api/library", .method = HTTP_POST, .handler = library_post,
    };
    const httpd_uri_t trust = {
        .uri = "/api/trust", .method = HTTP_POST, .handler = trust_post,
    };
    const httpd_uri_t pair = {
        .uri = "/api/pair", .method = HTTP_POST, .handler = pair_post,
    };
    const httpd_uri_t sync = {
        .uri = "/api/sync", .method = HTTP_POST, .handler = sync_post,
    };
    const httpd_uri_t cache_reset = {
        .uri = "/api/afc/cache/reset", .method = HTTP_POST, .handler = cache_reset_post,
    };
    const httpd_uri_t debug = {
        .uri = "/debug", .method = HTTP_GET, .handler = debug_get,
    };
    const httpd_uri_t log = {
        .uri = "/api/log", .method = HTTP_GET, .handler = log_get,
    };
    const httpd_uri_t debuglog = {
        .uri = "/api/debuglog", .method = HTTP_GET, .handler = debuglog_get,
    };
    const httpd_uri_t log_stream = {
        .uri = "/api/log/stream", .method = HTTP_GET, .handler = log_stream_get,
    };
    const httpd_uri_t loglevel = {
        .uri = "/api/loglevel", .method = HTTP_POST, .handler = loglevel_post,
    };
    const httpd_uri_t usbmode = {
        .uri = "/api/usbmode", .method = HTTP_POST, .handler = usbmode_post,
    };
    const httpd_uri_t key = {
        .uri = "/api/key", .method = HTTP_POST, .handler = key_post,
    };
    const httpd_uri_t screenshot = {
        .uri = "/api/screenshot.bmp", .method = HTTP_GET,
        .handler = screenshot_get,
    };
    const httpd_uri_t hidbit = {
        .uri = "/api/hidbit", .method = HTTP_POST, .handler = hidbit_post,
    };
    const httpd_uri_t keystate = {
        .uri = "/api/keystate", .method = HTTP_POST, .handler = keystate_post,
    };
    const httpd_uri_t power = {
        .uri = "/api/power", .method = HTTP_GET, .handler = power_get,
    };
    const httpd_uri_t charge = {
        .uri = "/api/charge", .method = HTTP_POST, .handler = charge_post,
    };
    const httpd_uri_t diagnostics[] = {
        {.uri="/api/display", .method=HTTP_GET, .handler=display_diag_get},
        {.uri="/api/display/trace", .method=HTTP_GET, .handler=display_trace_get},
        {.uri="/api/display/mark", .method=HTTP_POST, .handler=display_mark_post},
    };
    for(size_t i=0; i < sizeof(diagnostics)/sizeof(diagnostics[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &diagnostics[i]));

    const httpd_uri_t assets[] = {
        {.uri="/style.css", .method=HTTP_GET, .handler=asset_get, .user_ctx=(void *)WEB_STYLE_CSS},
        {.uri="/common.js", .method=HTTP_GET, .handler=asset_get, .user_ctx=(void *)WEB_COMMON_JS},
        {.uri="/dashboard.js", .method=HTTP_GET, .handler=asset_get, .user_ctx=(void *)WEB_DASHBOARD_JS},
        {.uri="/debug.js", .method=HTTP_GET, .handler=asset_get, .user_ctx=(void *)WEB_DEBUG_JS},
    };
    for(size_t i=0; i < sizeof(assets)/sizeof(assets[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &assets[i]));
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &cfg_get);
    httpd_register_uri_handler(server, &cfg_post);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &time_sync);
    httpd_register_uri_handler(server, &reboot);
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &library);
    httpd_register_uri_handler(server, &pair);
    httpd_register_uri_handler(server, &trust);
    httpd_register_uri_handler(server, &sync);
    httpd_register_uri_handler(server, &cache_reset);
    httpd_register_uri_handler(server, &debug);
    httpd_register_uri_handler(server, &log);
    httpd_register_uri_handler(server, &debuglog);
    httpd_register_uri_handler(server, &log_stream);
    httpd_register_uri_handler(server, &loglevel);
    httpd_register_uri_handler(server, &usbmode);
    httpd_register_uri_handler(server, &key);
    httpd_register_uri_handler(server, &screenshot);
    httpd_register_uri_handler(server, &hidbit);
    httpd_register_uri_handler(server, &keystate);
    httpd_register_uri_handler(server, &power);
    httpd_register_uri_handler(server, &charge);

    ESP_LOGI(TAG, "web server started on port 80");
    return ESP_OK;
}
