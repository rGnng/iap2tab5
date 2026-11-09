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

typedef enum {
    ULANZI_STATUS_HEARTBEAT,
    ULANZI_STATUS_SLEEP_READ,
    ULANZI_STATUS_SLEEP_WRITE,
    ULANZI_STATUS_BATTERY,
    ULANZI_STATUS_STANDBY_READ,
    ULANZI_STATUS_STANDBY_WRITE,
    ULANZI_STATUS_STANDBY_STATE,
} ulanzi_status_kind_t;

typedef struct {
    ulanzi_status_kind_t kind;
    uint32_t sleep_seconds;
    uint16_t voltage_raw;
    uint8_t battery_percent;
    uint8_t low_battery;
    bool charging;
    bool charge_full;
    bool power_off;
    bool standby;
} ulanzi_d100h_status_t;

/* Decode vendor status separately from controls; never emit a key for it. */
bool ulanzi_d100h_decode_ble_status(const uint8_t * data, size_t len,
                                   ulanzi_d100h_status_t * out);

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

/* Top-middle: tap Play/Pause on release; hold two seconds to blank the screen.
 * A fired hold remains latched until release, which emits no Play/Pause action. */
#define ULANZI_PLAY_HOLD_MS 2000
typedef struct {
    bool down;
    bool fired;
    int64_t pressed_us;
} ulanzi_play_t;
typedef enum {
    ULANZI_PLAY_DOWN,
    ULANZI_PLAY_UP,
    ULANZI_PLAY_POLL,
    ULANZI_PLAY_CANCEL,
} ulanzi_play_event_t;
typedef enum {
    ULANZI_PLAY_NONE,
    ULANZI_PLAY_TAP,
    ULANZI_PLAY_SCREEN_OFF,
} ulanzi_play_action_t;
int64_t ulanzi_play_deadline(const ulanzi_play_t * state);
ulanzi_play_action_t ulanzi_play_update(ulanzi_play_t * state,
    ulanzi_play_event_t event, int64_t now_us);

/* Top-left Back acts on release. A press alone has no action. */
typedef struct { bool down; } ulanzi_back_t;
bool ulanzi_back_update(ulanzi_back_t * state, bool down);
void ulanzi_back_cancel(ulanzi_back_t * state);

/* Qualify opposite steps before navigation, for both USB/BLE. Normal turns
 * use a 30 ms reversal debounce; rapid turns retain the longer noise filter.
 * Same-direction steps stay immediate. Three opposite reports confirm a
 * reversal early; a lone deliberate reverse is released at its deadline. */
#define ULANZI_ROTARY_NORMAL_DEBOUNCE_MS 30
#define ULANZI_ROTARY_RAPID_MS 80
#define ULANZI_ROTARY_CONFIRM_MS 120
#define ULANZI_ROTARY_CONFIRM_STEPS 3
typedef struct {
    int direction; /* -1 = CCW, +1 = CW, 0 = no preceding turn */
    int64_t last_us;
    int64_t pending_us, pending_last_us;
    bool pending;
    unsigned pending_count;
} ulanzi_rotary_t;
typedef struct {
    int delta;
    int64_t first_us, last_us;
} ulanzi_rotary_step_t;
typedef struct {
    ulanzi_rotary_step_t steps[2];
    unsigned count;
    bool suppressed;
} ulanzi_rotary_result_t;
int64_t ulanzi_rotary_deadline(const ulanzi_rotary_t *state);
ulanzi_rotary_result_t ulanzi_rotary_update(ulanzi_rotary_t *state, int direction, int64_t now_us);
ulanzi_rotary_result_t ulanzi_rotary_poll(ulanzi_rotary_t *state, int64_t now_us);
/* Flush before a following action key so Enter uses the intended selection. */
ulanzi_rotary_result_t ulanzi_rotary_flush(ulanzi_rotary_t *state);
void ulanzi_rotary_cancel(ulanzi_rotary_t *state);

/* Knob press/held-turn gesture (IDs 0x07, 0x0B, 0x0C). A plain click emits
 * Enter on release; any held turn cancels that click and changes volume.
 * Ordinary turns (0x09/0x0A) use the direction filter before navigation. */
typedef struct {
    bool down;
    bool turned;
} ulanzi_knob_t;

bool ulanzi_knob_update(ulanzi_knob_t * state, uint8_t id, bool down,
                        ui_key_t * key);
void ulanzi_knob_cancel(ulanzi_knob_t * state);

/* Stateless press/rotation events only. Top-row buttons and knob clicks use
 * the stateful helpers above, including their release reports. Normal rotary
 * reports must pass through ulanzi_rotary_update() before navigation. */
bool ulanzi_d100h_to_ui(uint8_t id, bool down, ui_key_t * key);

#ifdef __cplusplus
}
#endif
