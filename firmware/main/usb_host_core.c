#include "usb_host_core.h"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#if CONFIG_IDF_TARGET_ESP32P4
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "soc/usb_wrap_struct.h"
#endif

static const char * TAG = "usb_core";

/* iPod touch 6th gen / iOS 15.x used for the AFC bring-up. Selecting the
 * multiplexor configuration here means the device exposes the usbmux
 * interface instead of the HID/iAP2 configuration. Other devices keep the
 * normal default configuration. */
#define APPLE_VID           0x05AC
#define IPOD_PID            0x12AA
#define IPOD_USBMUX_CONFIG  4

static uint8_t s_apple_config = IPOD_USBMUX_CONFIG;
static unsigned s_peripheral_map = 1u << 0;

void usb_host_core_set_apple_config(uint8_t config_value)
{
    s_apple_config = config_value;
}

void usb_host_core_set_peripheral_map(unsigned peripheral_map)
{
    s_peripheral_map = peripheral_map != 0 ? peripheral_map : (1u << 0);
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
    esp_err_t err;
    /* The BSP switch powers the USB-A connector.  The USB-C OTG port is
     * powered from its own VBUS, so leave the USB-A switch off when the
     * full-speed/USB-C controller is selected. */
    if(s_peripheral_map & (1u << 0)) {
        err = bsp_feature_enable(BSP_FEATURE_USB, true);
        if(err != ESP_OK) {
            ESP_LOGE(TAG, "USB power enable failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    const bool usb_a = (s_peripheral_map & (1u << 0)) != 0;
    const bool usb_c = (s_peripheral_map & (1u << 1)) != 0;
    ESP_LOGI(TAG, "USB host peripheral_map=0x%x (%s%s%s)", s_peripheral_map,
             usb_a ? "USB-A HS" : "",
             (usb_a && usb_c) ? " + " : "",
             usb_c ? "USB-C OTG FS" : "");

#if CONFIG_IDF_TARGET_ESP32P4
    if(s_peripheral_map & (1u << 1)) {
        /* The Tab5 routes USB-C to FSLS PHY0 (GPIO24/25).  The IDF default
         * maps USB_WRAP to PHY1 (GPIO26/27) and USJ to PHY0, so USB-C would
         * otherwise never see the host controller. This also moves USB
         * Serial/JTAG to PHY1, whose GPIO26/27 are the Tab5's I2S DOUT/BCLK.
         * Disable the serial/JTAG pads BEFORE swapping PHYs so they cannot
         * take over the speaker pins. USB_WRAP keeps its own pads enabled;
         * logs remain available through the web debug stream. */
        usb_serial_jtag_ll_phy_enable_pad(false);
        usb_wrap_ll_phy_select(&USB_WRAP, 0);
        esp_log_level_set("HCD DWC", ESP_LOG_DEBUG);
        esp_log_level_set("USB HOST", ESP_LOG_DEBUG);
        esp_log_level_set("ENUM", ESP_LOG_DEBUG);
        ESP_LOGI(TAG, "USB-C FS host on PHY0; Serial/JTAG pads disabled for I2S GPIO26/27");
    }
#endif

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .enum_filter_cb = apple_enum_filter,
        .peripheral_map = s_peripheral_map,
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
