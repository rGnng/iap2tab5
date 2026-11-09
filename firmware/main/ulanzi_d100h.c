#include "ulanzi_d100h.h"

#include <string.h>

/* TEA variant used by the D100H, little-endian 32-bit words, 32 rounds.
 * The fixed key is from the BLE protocol capture; the wired USB vendor
 * interface uses the same cipher and a 64-byte payload. */
static const uint32_t TEA_KEY[4] = {
    0xcaa5baca, 0xbc2a8a6d, 0xca5a9eba, 0x9bb88bca,
};

static uint32_t load32(const uint8_t * p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 0);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void tea_encrypt_block(uint8_t * p)
{
    uint32_t a = load32(p);
    uint32_t b = load32(p + 4);
    uint32_t sum = 0;
    for(unsigned round = 0; round < 32; round++) {
        sum += 0x9e3779b9u;
        a += ((b << 4) + TEA_KEY[0]) ^ (b + sum) ^ ((b >> 5) + TEA_KEY[1]);
        b += ((a << 4) + TEA_KEY[2]) ^ (a + sum) ^ ((a >> 5) + TEA_KEY[3]);
    }
    store32(p, a);
    store32(p + 4, b);
}

static void tea_decrypt_block(uint8_t * p)
{
    uint32_t a = load32(p);
    uint32_t b = load32(p + 4);
    uint32_t sum = 0xc6ef3720u;
    for(unsigned round = 0; round < 32; round++) {
        b -= ((a << 4) + TEA_KEY[2]) ^ (a + sum) ^ ((a >> 5) + TEA_KEY[3]);
        a -= ((b << 4) + TEA_KEY[0]) ^ (b + sum) ^ ((b >> 5) + TEA_KEY[1]);
        sum -= 0x9e3779b9u;
    }
    store32(p, a);
    store32(p + 4, b);
}

void ulanzi_d100h_heartbeat(uint8_t out[64])
{
    memset(out, 0, 64);
    /* Plaintext derived from the captured BLE heartbeat:
     * 06 01 23 00 01 00 00 00 ... padded to 64 bytes, then encrypted. */
    out[0] = 0x06;
    out[1] = 0x01;
    out[2] = 0x23;
    out[3] = 0x00;
    out[4] = 0x01;
    for(size_t i = 0; i < 64; i += 8) {
        tea_encrypt_block(out + i);
    }
}

void ulanzi_d100h_encode_ble(const uint8_t * plain, size_t len,
                             uint8_t out[20])
{
    memset(out, 0, 20);
    if(plain != NULL) {
        if(len > 20) len = 20;
        memcpy(out, plain, len);
    }
    tea_encrypt_block(out);
    tea_encrypt_block(out + 8);
    /* Bytes 16..19 are not encrypted and remain zero for a 20-byte value. */
}

bool ulanzi_d100h_decode(const uint8_t * data, size_t len,
                         uint8_t * id, bool * down)
{
    if(data == NULL || id == NULL || down == NULL) return false;

    /* Some host APIs include a leading report ID 0 even when the descriptor
     * is unnumbered. The D100H wired descriptor has no report ID. */
    if(len == 65 && data[0] == 0) {
        data++;
        len = 64;
    }
    if(len != 64) return false;

    uint8_t p[64];
    memcpy(p, data, 64);
    for(size_t i = 0; i < 64; i += 8) {
        tea_decrypt_block(p + i);
    }

    if(p[0] != 0x8b || p[1] != 0x10) return false;
    if(p[2] < 1 || p[2] > 12) return false;
    if(p[3] > 1) return false;
    /* The event lives in the first 8-byte block. The wired 64-byte report
     * can carry unrelated bytes in the remaining blocks (observed on knob
     * clicks), so only validate the first block's padding. */
    if(p[4] != 0 || p[5] != 0 || p[6] != 0 || p[7] != 0) return false;
    /* Rotation IDs are step events and must be down. */
    if(p[2] >= 9 && p[3] != 1) return false;

    *id = p[2];
    *down = p[3] != 0;
    return true;
}

static bool decode_ble_plain(const uint8_t * data, size_t len, uint8_t p[20])
{
    if(data == NULL || len != 20) return false;
    memcpy(p, data, 20);
    tea_decrypt_block(p);
    tea_decrypt_block(p + 8);
    return true;
}

bool ulanzi_d100h_decode_ble_status(const uint8_t * data, size_t len,
                                   ulanzi_d100h_status_t * out)
{
    uint8_t p[20];
    if(out == NULL || !decode_ble_plain(data, len, p)) return false;
    ulanzi_d100h_status_t st = {0};
    if(p[0] == 0x0b && p[1] == 0x7b) {
        if(p[5] > 100) return false;
        st.kind = ULANZI_STATUS_BATTERY;
        st.voltage_raw = p[2] | ((uint16_t)p[3] << 8);
        st.low_battery = p[4] & 3;
        st.power_off = (p[4] & 4) != 0;
        st.charging = (p[4] & 8) != 0;
        st.charge_full = (p[4] & 16) != 0;
        st.battery_percent = p[5];
    }
    else if(p[0] == 0x06 && p[1] == 0x01 && p[2] == 0x23 &&
            p[3] == 0x10 && p[4] <= 1) {
        st.kind = ULANZI_STATUS_HEARTBEAT;
    }
    else if(p[0] == 0x01 && p[1] == 0x01 &&
            (p[2] == 0x42 || p[2] == 0x2c) &&
            (p[3] == 0x11 || p[3] == 0x12)) {
        if(p[2] == 0x42)
            st.kind = p[3] == 0x11 ? ULANZI_STATUS_SLEEP_READ : ULANZI_STATUS_SLEEP_WRITE;
        else
            st.kind = p[3] == 0x11 ? ULANZI_STATUS_STANDBY_READ : ULANZI_STATUS_STANDBY_WRITE;
        /* Ulanzi Studio's MessageHelper and parser use a 32-bit LE integer. */
        st.sleep_seconds = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                           ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    }
    else if(p[0] == 0x01 && p[1] == 0x01 && p[2] == 0x0d &&
            (p[3] == 0x10 || p[3] == 0x11) && p[4] <= 1) {
        st.kind = ULANZI_STATUS_STANDBY_STATE;
        st.standby = p[4] != 0;
    }
    else return false;
    *out = st;
    return true;
}

bool ulanzi_d100h_decode_ble(const uint8_t * data, size_t len,
                             uint8_t * id, bool * down)
{
    uint8_t p[20];
    if(id == NULL || down == NULL || !decode_ble_plain(data, len, p)) return false;

    if(p[0] != 0x8b || p[1] != 0x10) return false;
    if(p[2] < 1 || p[2] > 12) return false;
    if(p[3] > 1) return false;
    for(size_t i = 4; i < 20; i++) {
        if(p[i] != 0) return false;
    }
    if(p[2] >= 9 && p[3] != 1) return false;

    *id = p[2];
    *down = p[3] != 0;
    return true;
}

int64_t ulanzi_top_right_deadline(const ulanzi_top_right_t * state)
{
    return state->down && !state->fired
        ? state->pressed_us + (int64_t)ULANZI_TOP_RIGHT_HOLD_MS * 1000 : 0;
}

ulanzi_top_right_action_t ulanzi_top_right_update(ulanzi_top_right_t * state,
    ulanzi_top_right_event_t event, int64_t now_us)
{
    if(event == ULANZI_TOP_RIGHT_CANCEL) {
        *state = (ulanzi_top_right_t){0};
        return ULANZI_TOP_RIGHT_NONE;
    }
    if(event == ULANZI_TOP_RIGHT_DOWN) {
        if(!state->down) {
            *state = (ulanzi_top_right_t){.down = true, .pressed_us = now_us};
        }
        return ULANZI_TOP_RIGHT_NONE;
    }
    if(!state->down) return ULANZI_TOP_RIGHT_NONE;

    ulanzi_top_right_action_t action = ULANZI_TOP_RIGHT_NONE;
    if(!state->fired) {
        if(now_us >= ulanzi_top_right_deadline(state)) {
            state->fired = true;
            action = ULANZI_TOP_RIGHT_STAR;
        }
        else if(event == ULANZI_TOP_RIGHT_UP) {
            action = ULANZI_TOP_RIGHT_CONTEXT;
        }
    }
    if(event == ULANZI_TOP_RIGHT_UP) state->down = false;
    return action;
}

int64_t ulanzi_play_deadline(const ulanzi_play_t * state)
{
    return state->down && !state->fired
        ? state->pressed_us + (int64_t)ULANZI_PLAY_HOLD_MS * 1000 : 0;
}

ulanzi_play_action_t ulanzi_play_update(ulanzi_play_t * state,
    ulanzi_play_event_t event, int64_t now_us)
{
    if(event == ULANZI_PLAY_CANCEL) {
        *state = (ulanzi_play_t){0};
        return ULANZI_PLAY_NONE;
    }
    if(event == ULANZI_PLAY_DOWN) {
        if(!state->down) *state = (ulanzi_play_t){.down = true, .pressed_us = now_us};
        return ULANZI_PLAY_NONE;
    }
    if(!state->down) return ULANZI_PLAY_NONE;
    ulanzi_play_action_t action = ULANZI_PLAY_NONE;
    if(!state->fired) {
        if(now_us >= ulanzi_play_deadline(state)) {
            state->fired = true;
            action = ULANZI_PLAY_SCREEN_OFF;
        }
        else if(event == ULANZI_PLAY_UP) action = ULANZI_PLAY_TAP;
    }
    if(event == ULANZI_PLAY_UP) state->down = false;
    return action;
}

bool ulanzi_back_update(ulanzi_back_t * state, bool down)
{
    bool click = !down && state->down;
    state->down = down;
    return click;
}

void ulanzi_back_cancel(ulanzi_back_t * state)
{
    state->down = false;
}

void ulanzi_rotary_cancel(ulanzi_rotary_t *state)
{
    *state = (ulanzi_rotary_t){0};
}

int64_t ulanzi_rotary_deadline(const ulanzi_rotary_t *state)
{
    if(!state->pending) return 0;
    /* Choose from physical arrival times, independently of UI Fast Scroll
     * or dispatch delays. last_us stays at the preceding accepted direction
     * until this candidate is confirmed or discarded. */
    int wait_ms = state->pending_us - state->last_us <=
        (int64_t)ULANZI_ROTARY_RAPID_MS * 1000
        ? ULANZI_ROTARY_CONFIRM_MS : ULANZI_ROTARY_NORMAL_DEBOUNCE_MS;
    return state->pending_us + (int64_t)wait_ms * 1000;
}

ulanzi_rotary_result_t ulanzi_rotary_flush(ulanzi_rotary_t *state)
{
    ulanzi_rotary_result_t out = {0};
    if(state->pending) {
        state->direction = -state->direction;
        state->last_us = state->pending_last_us;
        out.steps[out.count++] = (ulanzi_rotary_step_t){
            (int)state->pending_count*state->direction, state->pending_us, state->pending_last_us};
        state->pending = false;
        state->pending_count = 0;
    }
    return out;
}

ulanzi_rotary_result_t ulanzi_rotary_poll(ulanzi_rotary_t *state, int64_t now_us)
{
    if(state->pending && now_us >= ulanzi_rotary_deadline(state)) return ulanzi_rotary_flush(state);
    return (ulanzi_rotary_result_t){0};
}

ulanzi_rotary_result_t ulanzi_rotary_update(ulanzi_rotary_t *state, int direction, int64_t now_us)
{
    ulanzi_rotary_result_t out = {0};
    if(direction != -1 && direction != 1) return out;
    if(state->pending && now_us >= ulanzi_rotary_deadline(state)) out = ulanzi_rotary_flush(state);
    if(state->pending) {
        if(direction != state->direction) {
            state->pending_count++;
            state->pending_last_us = now_us;
            if(state->pending_count < ULANZI_ROTARY_CONFIRM_STEPS) return out;
            /* Fast turns can contain two-report glitches. Require a third
             * consistent report, then preserve all held detents and times. */
            out.steps[out.count++] = (ulanzi_rotary_step_t){
                (int)state->pending_count*direction, state->pending_us, now_us};
            state->pending = false;
            state->pending_count = 0;
            state->direction = direction;
            state->last_us = now_us;
            return out;
        }
        state->pending = false;
        state->pending_count = 0;
        out.suppressed = true;
    }
    if(state->direction && direction != state->direction) {
        state->pending = true;
        state->pending_count = 1;
        state->pending_us = state->pending_last_us = now_us;
        return out;
    }
    state->direction = direction;
    state->last_us = now_us;
    out.steps[out.count++] = (ulanzi_rotary_step_t){direction, now_us, now_us};
    return out;
}

void ulanzi_knob_cancel(ulanzi_knob_t * state)
{
    *state = (ulanzi_knob_t){0};
}

bool ulanzi_knob_update(ulanzi_knob_t * state, uint8_t id, bool down,
                        ui_key_t * key)
{
    if(key == NULL) return false;
    if(id == 0x07) {
        if(down) {
            if(!state->down) *state = (ulanzi_knob_t){.down = true};
            return false;
        }
        bool click = state->down && !state->turned;
        ulanzi_knob_cancel(state);
        if(click) *key = UI_KEY_ENTER;
        return click;
    }
    if(down && (id == 0x0B || id == 0x0C)) {
        state->turned = true;
        return ulanzi_d100h_to_ui(id, down, key);
    }
    return false;
}

bool ulanzi_d100h_to_ui(uint8_t id, bool down, ui_key_t * key)
{
    if(key == NULL || !down) return false;

    switch(id) {
        case 0x01: return false; /* Back on release */
        case 0x02: return false; /* stateful Play/Pause / screen-off gesture */
        /* 0x03 top-right is press-duration sensitive: the caller decides
         * between UI_KEY_CONTEXT (short) and UI_KEY_STAR (long). */
        case 0x03: return false;
        case 0x04: return false; /* upper-left: release handled by caller */
        case 0x05: *key = UI_KEY_PREV_TRACK;  return true; /* upper-right */
        case 0x06: return false; /* lower-left: release handled by caller */
        case 0x07: return false; /* stateful click / held-turn gesture */
        case 0x08: *key = UI_KEY_NEXT_TRACK;  return true; /* lower-right */
        case 0x09: *key = UI_KEY_UP;          return true; /* rotate ccw */
        case 0x0A: *key = UI_KEY_DOWN;        return true; /* rotate cw */
        case 0x0B: *key = UI_KEY_VOLUME_DOWN; return true; /* held rotate ccw */
        case 0x0C: *key = UI_KEY_VOLUME_UP;   return true; /* held rotate cw */
        default: return false;
    }
}
