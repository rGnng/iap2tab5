#include "usb_host_core.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char * TAG = "usb_core";

/* iPod touch 6th gen / iOS 15.x used for the AFC bring-up. Selecting the
 * multiplexor configuration here means the device exposes the usbmux
 * interface instead of the HID/iAP2 configuration. Other devices keep the
 * normal default configuration. */
#define APPLE_VID           0x05AC
#define IPOD_PID            0x12AA
#define IPOD_USBMUX_CONFIG  4

static uint8_t s_apple_config = IPOD_USBMUX_CONFIG;

void usb_host_core_set_apple_config(uint8_t config_value)
{
    s_apple_config = config_value;
}

static bool apple_enum_filter(const usb_device_desc_t * dev_desc,
                              uint8_t * bConfigurationValue)
{
    if(dev_desc != NULL && dev_desc->idVendor == APPLE_VID &&
       dev_desc->idProduct == IPOD_PID &&
       dev_desc->bNumConfigurations >= s_apple_config) {
        *bConfigurationValue = s_apple_config;
        ESP_LOGI(TAG, "iPod %04x:%04x -> USB config %u (%s)",
                 dev_desc->idVendor, dev_desc->idProduct,
                 (unsigned)s_apple_config,
                 s_apple_config == 2 ? "iAP2 HID" : "usbmux");
    }
    return true;
}

static void usb_lib_task(void * arg)
{
    (void)arg;
    for(;;) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if(event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
        if(event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB: all devices freed");
        }
    }
}

esp_err_t usb_host_core_start(void)
{
    esp_err_t err = bsp_feature_enable(BSP_FEATURE_USB, true);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "USB power enable failed: %s", esp_err_to_name(err));
        return err;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .enum_filter_cb = apple_enum_filter,
    };

    err = usb_host_install(&host_config);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        return err;
    }

    if(xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL) != pdPASS) {
        ESP_LOGE(TAG, "USB lib task creation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB host installed (Apple usbmux filter active)");
    return ESP_OK;
}
