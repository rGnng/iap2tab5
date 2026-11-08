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

typedef enum {
    D100H_INPUT_USB,
    D100H_INPUT_BLE,
    D100H_INPUT_WEB,
    D100H_INPUT_COUNT,
} d100h_input_source_t;

/** Top-left: Back on release, or Screen Off after holding two seconds. */
void usb_hid_input_post_back(d100h_input_source_t source, bool down);
/** Top-middle: Play/Pause on release. */
void usb_hid_input_post_play(d100h_input_source_t source, bool down);
/** Top-right: tap for Context, hold 600 ms for Star while still pressed. */
void usb_hid_input_post_top_right(d100h_input_source_t source, bool down);
/** Knob click / held rotation: vendor IDs 0x07, 0x0B and 0x0C. */
void usb_hid_input_post_knob(d100h_input_source_t source, uint8_t id, bool down);
/** Cancel a source's pending gestures when it disconnects. */
void usb_hid_input_cancel_gestures(d100h_input_source_t source);

#endif /* IAP2TAB5_USB_HID_INPUT_H */
