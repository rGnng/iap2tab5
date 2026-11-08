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

/* Shared USB/BLE top-right gesture. Poll at the deadline to fire while held;
 * release only emits Context for a short tap. Keep one state per input source. */
#define ULANZI_TOP_RIGHT_HOLD_MS 600
typedef struct {
    bool down;
    bool fired;
    int64_t pressed_us;
} ulanzi_top_right_t;

typedef enum {
    ULANZI_TOP_RIGHT_DOWN,
    ULANZI_TOP_RIGHT_UP,
    ULANZI_TOP_RIGHT_POLL,
    ULANZI_TOP_RIGHT_CANCEL,
} ulanzi_top_right_event_t;

typedef enum {
    ULANZI_TOP_RIGHT_NONE,
    ULANZI_TOP_RIGHT_CONTEXT,
    ULANZI_TOP_RIGHT_STAR,
} ulanzi_top_right_action_t;

int64_t ulanzi_top_right_deadline(const ulanzi_top_right_t * state);
ulanzi_top_right_action_t ulanzi_top_right_update(ulanzi_top_right_t * state,
    ulanzi_top_right_event_t event, int64_t now_us);

/* Top-left: tap Back on release; hold two seconds to blank the screen.
 * A fired hold remains latched until release, which emits no Back action. */
#define ULANZI_BACK_HOLD_MS 2000
typedef struct {
    bool down;
    bool fired;
    int64_t pressed_us;
} ulanzi_back_t;
typedef enum {
    ULANZI_BACK_DOWN,
    ULANZI_BACK_UP,
    ULANZI_BACK_POLL,
    ULANZI_BACK_CANCEL,
} ulanzi_back_event_t;
typedef enum {
    ULANZI_BACK_NONE,
    ULANZI_BACK_TAP,
    ULANZI_BACK_SCREEN_OFF,
} ulanzi_back_action_t;
int64_t ulanzi_back_deadline(const ulanzi_back_t * state);
ulanzi_back_action_t ulanzi_back_update(ulanzi_back_t * state,
    ulanzi_back_event_t event, int64_t now_us);

/* Top-middle Play/Pause acts on release. A press alone has no action. */
typedef struct { bool down; } ulanzi_play_t;
bool ulanzi_play_update(ulanzi_play_t * state, bool down);
void ulanzi_play_cancel(ulanzi_play_t * state);

/* Knob press/held-turn gesture (IDs 0x07, 0x0B, 0x0C). A plain click emits
 * Enter on release; any held turn cancels that click and changes volume.
 * Normal turns (0x09/0x0A) remain stateless, coalescible navigation events. */
typedef struct {
    bool down;
    bool turned;
} ulanzi_knob_t;

bool ulanzi_knob_update(ulanzi_knob_t * state, uint8_t id, bool down,
                        ui_key_t * key);
void ulanzi_knob_cancel(ulanzi_knob_t * state);

/* Stateless press/rotation events only. Top-row buttons and knob clicks use
 * the stateful helpers above, including their release reports. */
bool ulanzi_d100h_to_ui(uint8_t id, bool down, ui_key_t * key);

#ifdef __cplusplus
}
#endif
