#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"
#include "usbmux_host.h"
#include "lockdown_client.h"
#include "afc_client.h"
#include "iap2_host.h"

static const char * TAG = "web";

static web_config_apply_cb_t s_apply_cb;
static char s_net_status[64] = "WiFi off";
static char s_net_ip[32] = "--";

static const char * INDEX_HTML =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>IAP2 Tab5</title><style>"
"body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:18px}"
"h1{font-size:22px;margin:0 0 12px}.card{background:#1d1d20;border:1px solid #333;"
"border-radius:10px;padding:14px;margin-bottom:14px;max-width:760px}"
"label{display:block;margin:8px 0 4px;color:#aaa}select,input[type=text],"
"input[type=number]{width:100%;padding:8px;background:#111;color:#eee;border:1px solid #444;"
"border-radius:6px}input[type=range]{width:100%}button{padding:9px 16px;margin-top:10px;"
"background:#4b7bec;color:#fff;border:0;border-radius:6px;font-size:15px}"
"button.danger{background:#c0392b}pre{white-space:pre-wrap;color:#8fd}"
"</style></head><body><h1>IAP2 Tab5 debug</h1>"
"<div class='card'><b>Status</b><pre id='status'>loading...</pre>"
"<button onclick='load()'>Refresh</button>"
"<button class='danger' onclick='reboot()'>Reboot</button></div>"
"<div class='card'><b>Config</b>"
"<label>Screen flip</label><select id='screen_flip'><option value='0'>0 deg</option>"
"<option value='180'>180 deg</option></select>"
"<label>WiFi mode</label><select id='wifi_mode'><option value='0'>Station (SD wifi.txt)</option>"
"<option value='1'>AP (open)</option></select>"
"<label>AP SSID</label><input type='text' id='ap_ssid' maxlength='32'>"
"<label><input type='checkbox' id='sound_enabled'> Prompt sound</label>"
"<label><input type='checkbox' id='iap2_mode'> iAP2 mode (USB config 2; reboot)</label>"
"<label>Volume <span id='vol_label'>65</span>%</label>"
"<input type='range' id='volume' min='0' max='100' step='5' oninput='vol_label.textContent=this.value'>"
"<button onclick='save()'>Save config</button></div>"
"<div class='card'><b>Media library</b>"
"<p>Copies MediaLibrary.sqlitedb, -wal and -shm to /sdcard/AFC "
"(8.3 names MLDB.SQL/WAL/SHM) and logs SHA-256 on the serial console.</p>"
"<button onclick='sync_media()'>Copy to SD</button>"
"<pre id='sync_msg'></pre></div>"
"<div class='card'><b>Pair record</b>"
"<p>Upload the lockdownd pair plist from /var/lib/lockdown/ "
"(for example 00008150-000935823E22401C.plist). It is stored in SPIFFS "
"and used for the lockdown TLS session.</p>"
"<input type='file' id='pair'><button onclick='pair_upload()'>Upload "
"pair.plist</button><pre id='pair_msg'></pre></div>"
"<div class='card'><b>Firmware update</b>"
"<p>Upload an .bin built by idf.py. The device reboots when the update is verified.</p>"
"<input type='file' id='fw'><button onclick='ota()'>Upload &amp; reboot</button>"
"<pre id='ota_msg'></pre></div>"
"<script>"
"async function load(){let s=await(await fetch('/api/status')).json();"
"status.textContent=JSON.stringify(s,null,2);let c=await(await fetch('/api/config')).json();"
"screen_flip.value=c.screen_flip;wifi_mode.value=c.wifi_mode;ap_ssid.value=c.ap_ssid;"
"sound_enabled.checked=c.sound_enabled;iap2_mode.checked=c.iap2_mode;"
"volume.value=c.volume;vol_label.textContent=c.volume;}"
"async function save(){let c={screen_flip:+screen_flip.value,wifi_mode:+wifi_mode.value,"
"ap_ssid:ap_ssid.value,sound_enabled:sound_enabled.checked,"
"iap2_mode:iap2_mode.checked,volume:+volume.value};"
"let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(c)});alert(await r.text());load();}"
"async function reboot(){if(confirm('Reboot?')){await fetch('/api/reboot',{method:'POST'});}}"
"async function ota(){let f=fw.files[0];if(!f){alert('choose a .bin');return;}"
"ota_msg.textContent='uploading...';let r=await fetch('/api/ota',{method:'POST',body:f});"
"ota_msg.textContent=await r.text();}"
"async function sync_media(){sync_msg.textContent='started';"
"let r=await fetch('/api/sync',{method:'POST'});sync_msg.textContent=await r.text();}"
"async function pair_upload(){let f=pair.files[0];if(!f){alert('choose a plist');"
"return;}pair_msg.textContent='uploading...';let r=await fetch('/api/pair',"
"{method:'POST',body:f});pair_msg.textContent=await r.text();load();}"
"load();</script></body></html>";

static esp_err_t root_get(httpd_req_t * req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get(httpd_req_t * req)
{
    cJSON * root = cJSON_CreateObject();
    if(root == NULL) return httpd_resp_send_500(req);

    cJSON_AddNumberToObject(root, "screen_flip", g_app_config.screen_flip);
    cJSON_AddNumberToObject(root, "wifi_mode", g_app_config.wifi_mode);
    cJSON_AddBoolToObject(root, "sound_enabled", g_app_config.sound_enabled);
    cJSON_AddNumberToObject(root, "volume", g_app_config.volume);
    cJSON_AddStringToObject(root, "ap_ssid", g_app_config.ap_ssid);
    cJSON_AddBoolToObject(root, "sd_ready", app_config_sd_ready());
    cJSON_AddBoolToObject(root, "iap2_mode", g_app_config.iap2_mode);

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

    const cJSON * item = cJSON_GetObjectItemCaseSensitive(root, "screen_flip");
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
    item = cJSON_GetObjectItemCaseSensitive(root, "iap2_mode");
    if(cJSON_IsBool(item)) {
        app_config_set_iap2_mode(cJSON_IsTrue(item));
    }
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

    cJSON_Delete(root);

    esp_err_t save_err = app_config_save();
    if(s_apply_cb != NULL) s_apply_cb();

    char reply[128];
    snprintf(reply, sizeof(reply), "saved%s",
             save_err == ESP_OK ? "" : " (SD not available; RAM only)");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, reply);
}

static esp_err_t status_get(httpd_req_t * req)
{
    cJSON * root = cJSON_CreateObject();
    if(root == NULL) return httpd_resp_send_500(req);

    const esp_app_desc_t * app = esp_app_get_description();
    cJSON_AddStringToObject(root, "firmware", app->version);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "free_psram",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddBoolToObject(root, "sd_ready", app_config_sd_ready());
    cJSON_AddBoolToObject(root, "iap2_mode", g_app_config.iap2_mode);
    cJSON_AddStringToObject(root, "wifi", s_net_status);
    cJSON_AddStringToObject(root, "ip", s_net_ip);
    cJSON_AddStringToObject(root, "usbmux", usbmux_host_status());
    cJSON_AddStringToObject(root, "lockdown", lockdown_client_status());
    cJSON_AddStringToObject(root, "afc", afc_client_status());
    cJSON_AddStringToObject(root, "iap2", iap2_host_status());
    cJSON_AddBoolToObject(root, "iap2_identified",
                          iap2_host_is_identified());
    iap2_state_t iap2;
    if(iap2_host_get_state(&iap2)) {
        cJSON_AddNumberToObject(root, "device_battery", iap2.battery);
        cJSON_AddBoolToObject(root, "device_charging", iap2.charging);
        cJSON_AddNumberToObject(root, "device_charging_state",
                                iap2.charging_state);
        cJSON_AddBoolToObject(root, "device_external_charger",
                              iap2.external_charger);
        cJSON_AddNumberToObject(root, "device_max_current_ma",
                                iap2.max_current_ma);
    }
    cJSON_AddNumberToObject(root, "screen_flip", g_app_config.screen_flip);
    cJSON_AddNumberToObject(root, "volume", g_app_config.volume);
    const esp_partition_t * ota = esp_ota_get_next_update_partition(NULL);
    cJSON_AddStringToObject(root, "ota_partition", ota ? ota->label : "none");

    char * text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if(text == NULL) return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static esp_err_t reboot_post(httpd_req_t * req)
{
    httpd_resp_sendstr(req, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static void sync_task(void * arg)
{
    (void)arg;
    lockdown_client_sync_media();
    vTaskDelete(NULL);
}

static esp_err_t sync_post(httpd_req_t * req)
{
    if(xTaskCreate(sync_task, "afc_sync", 8192, NULL, 3, NULL) != pdPASS) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "task failed");
    }
    return httpd_resp_sendstr(req, "copy started; watch the serial log");
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
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
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
    config.max_uri_handlers = 8;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
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
    const httpd_uri_t reboot = {
        .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post,
    };
    const httpd_uri_t ota = {
        .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post,
    };
    const httpd_uri_t pair = {
        .uri = "/api/pair", .method = HTTP_POST, .handler = pair_post,
    };
    const httpd_uri_t sync = {
        .uri = "/api/sync", .method = HTTP_POST, .handler = sync_post,
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &cfg_get);
    httpd_register_uri_handler(server, &cfg_post);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &reboot);
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &pair);
    httpd_register_uri_handler(server, &sync);

    ESP_LOGI(TAG, "web server started on port 80");
    return ESP_OK;
}
