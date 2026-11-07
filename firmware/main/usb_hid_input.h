#ifndef IAP2TAB5_USB_HID_INPUT_H
#define IAP2TAB5_USB_HID_INPUT_H

#include "ui.h"

/** Start the Tab5 USB-A host port and listen for a HID keyboard/knob. */
void usb_hid_input_start(void);

/** Queue a logical key from another input backend (e.g. BLE). */
void usb_hid_input_post_key(ui_key_t key);

/** Queue a press/release state for keys that support hold behaviour
 * (currently previous/next: tap = track skip, hold = rewind/fast-forward). */
void usb_hid_input_post_key_state(ui_key_t key, bool down);

#endif /* IAP2TAB5_USB_HID_INPUT_H */
