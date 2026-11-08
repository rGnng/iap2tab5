#include "wifi_manager.h"

#include <string.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ui.h"
#include "web_server.h"

static const char * TAG = "wifi";

static QueueHandle_t s_cmd_queue;
static esp_netif_t * s_sta_netif;
static esp_netif_t * s_ap_netif;
static volatile int s_current_mode = -1;

static void post_status(const char * status, const char * ip)
{
    web_server_set_network_info(status, ip);
    if(bsp_display_lock(0)) {
        ui_set_network_status(status, ip);
        bsp_display_unlock();
    }
}

static void wifi_event_handler(void * arg, esp_event_base_t base,
                               int32_t id, void * data)
{
    (void)arg;
    (void)data;

    if(base == WIFI_EVENT) {
        if(id == WIFI_EVENT_STA_START) {
            post_status("Connecting...", "--");
            esp_wifi_connect();
        }
        else if(id == WIFI_EVENT_STA_DISCONNECTED) {
            post_status("Station disconnected", "--");
            if(s_current_mode == 0) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_wifi_connect();
            }
        }
        else if(id == WIFI_EVENT_AP_START) {
            post_status("AP started", "192.168.4.1");
        }
        else if(id == WIFI_EVENT_AP_STOP) {
            post_status("AP stopped", "--");
        }
    }
    else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t * event = (ip_event_got_ip_t *)data;
        char ip[32];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "station got IP %s", ip);
        post_status("Station connected", ip);
    }
}

static esp_err_t apply_mode(int mode)
{
    if(mode == s_current_mode) return ESP_OK;

    if(s_current_mode != -1) {
        esp_wifi_stop();
        s_current_mode = -1;
    }

    wifi_config_t cfg = {0};
    esp_err_t err;

    if(mode == 1) {
        size_t ap_len = strlen(g_app_config.ap_ssid);
        if(ap_len > sizeof(cfg.ap.ssid)) ap_len = sizeof(cfg.ap.ssid);
        memcpy(cfg.ap.ssid, g_app_config.ap_ssid, ap_len);
        cfg.ap.ssid_len = ap_len;
        cfg.ap.channel = 6;
        cfg.ap.max_connection = 4;
        cfg.ap.authmode = WIFI_AUTH_OPEN;
        cfg.ap.pmf_cfg.required = false;

        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if(err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
        if(err == ESP_OK) err = esp_wifi_start();
        if(err == ESP_OK) {
            s_current_mode = 1;
            ESP_LOGI(TAG, "open AP '%s'", g_app_config.ap_ssid);
            post_status("AP started", "192.168.4.1");
        }
        return err;
    }

    char ssid[33] = {0};
    char password[65] = {0};
    err = app_config_read_wifi_credentials(ssid, sizeof(ssid),
                                           password, sizeof(password));
    if(err != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "no usable SD or NVS Wi-Fi credentials; starting open AP fallback");
        return apply_mode(1);
    }

    size_t ssid_len = strlen(ssid);
    if(ssid_len > sizeof(cfg.sta.ssid)) ssid_len = sizeof(cfg.sta.ssid);
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    size_t pass_len = strlen(password);
    if(pass_len > sizeof(cfg.sta.password)) pass_len = sizeof(cfg.sta.password);
    memcpy(cfg.sta.password, password, pass_len);
    cfg.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if(err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if(err == ESP_OK) err = esp_wifi_start();
    if(err == ESP_OK) {
        s_current_mode = 0;
        ESP_LOGI(TAG, "station SSID '%s'", ssid);
        post_status("Connecting...", "--");
    }
    return err;
}

static void wifi_manager_task(void * arg)
{
    (void)arg;
    int mode = g_app_config.wifi_mode;
    apply_mode(mode);

    int requested;
    for(;;) {
        if(xQueueReceive(s_cmd_queue, &requested, portMAX_DELAY) == pdTRUE) {
            if(requested != s_current_mode) {
                ESP_LOGI(TAG, "switch mode -> %d", requested);
                esp_err_t err = apply_mode(requested);
                if(err != ESP_OK) {
                    ESP_LOGE(TAG, "mode switch failed: %s", esp_err_to_name(err));
                    post_status("WiFi error", "--");
                }
            }
        }
    }
}

esp_err_t wifi_manager_start(void)
{
    /* The Tab5 C6 module is powered through an IO expander pin; it is off
     * until BSP_FEATURE_WIFI is enabled. */
    esp_err_t power_err = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if(power_err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi power enable failed: %s", esp_err_to_name(power_err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if(err != ESP_OK) return err;

    err = esp_netif_init();
    if(err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if(s_sta_netif == NULL || s_ap_netif == NULL) return ESP_FAIL;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if(err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     wifi_event_handler, NULL);
    if(err != ESP_OK) return err;

    s_cmd_queue = xQueueCreate(4, sizeof(int));
    if(s_cmd_queue == NULL) return ESP_ERR_NO_MEM;

    if(xTaskCreate(wifi_manager_task, "wifi_mgr", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "manager started (mode=%u)", (unsigned)g_app_config.wifi_mode);
    return ESP_OK;
}

void wifi_manager_request_mode(int mode)
{
    if(s_cmd_queue == NULL) return;
    int m = (mode == 1) ? 1 : 0;
    xQueueSend(s_cmd_queue, &m, 0);
}
