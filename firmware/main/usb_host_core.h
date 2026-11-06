#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the USB host library with an enumeration filter that selects
 * configuration 4 for the iPod/iPhone used by the AFC path. The HID driver
 * is installed on top of this by usb_hid_input_start(). */
esp_err_t usb_host_core_start(void);

/* Selects the USB configuration used for the Apple device (4 = usbmux/AFC,
 * 2 = iAP2 HID). Must be called before enumeration. */
void usb_host_core_set_apple_config(uint8_t config_value);

/* Selects the USB OTG peripheral used by the host stack.
 * On ESP32-P4: BIT0 = high-speed peripheral (USB-A), BIT1 = full-speed
 * peripheral (USB-C OTG). Must be called before usb_host_core_start(). */
void usb_host_core_set_peripheral_map(unsigned peripheral_map);

#ifdef __cplusplus
}
#endif
