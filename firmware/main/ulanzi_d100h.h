#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ulanzi D100H wired vendor interface: usage page 0xFFFE, report ID 0,
 * 64-byte encrypted input/output reports. */

bool ulanzi_d100h_decode(const uint8_t * data, size_t len,
                         uint8_t * id, bool * down);

/* BLE vendor notifications are 20 bytes (two encrypted TEA blocks plus four
 * trailing bytes). */
bool ulanzi_d100h_decode_ble(const uint8_t * data, size_t len,
                             uint8_t * id, bool * down);

/* Encode a plaintext vendor command into the 20-byte BLE wire format. */
void ulanzi_d100h_encode_ble(const uint8_t * plain, size_t len,
                             uint8_t out[20]);

void ulanzi_d100h_heartbeat(uint8_t out[64]);

/* Returns true when this event should be forwarded to the UI. Only press /
 * rotation-step events are emitted; releases are intentionally ignored. */
bool ulanzi_d100h_to_ui(uint8_t id, bool down, ui_key_t * key);

#ifdef __cplusplus
}
#endif
