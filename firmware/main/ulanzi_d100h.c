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

bool ulanzi_d100h_decode_ble(const uint8_t * data, size_t len,
                             uint8_t * id, bool * down)
{
    if(data == NULL || id == NULL || down == NULL || len != 20) return false;

    uint8_t p[20];
    memcpy(p, data, 20);
    tea_decrypt_block(p);
    tea_decrypt_block(p + 8);

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

bool ulanzi_d100h_to_ui(uint8_t id, bool down, ui_key_t * key)
{
    if(key == NULL || !down) return false;

    switch(id) {
        case 0x01: *key = UI_KEY_BACK;        return true; /* top-left */
        case 0x02: *key = UI_KEY_7;           return true; /* top-middle */
        /* 0x03 top-right is press-duration sensitive: the caller decides
         * between UI_KEY_CONTEXT (short) and UI_KEY_STAR (long). */
        case 0x03: return false;
        case 0x04: *key = UI_KEY_SCREEN_PREV; return true; /* upper-left */
        case 0x05: *key = UI_KEY_PREV_TRACK;  return true; /* upper-right */
        case 0x06: *key = UI_KEY_SCREEN_NEXT; return true; /* lower-left */
        case 0x07: *key = UI_KEY_ENTER;       return true; /* knob click */
        case 0x08: *key = UI_KEY_NEXT_TRACK;  return true; /* lower-right */
        case 0x09: *key = UI_KEY_UP;          return true; /* rotate ccw */
        case 0x0A: *key = UI_KEY_DOWN;        return true; /* rotate cw */
        case 0x0B: *key = UI_KEY_UP;          return true; /* held rotate ccw */
        case 0x0C: *key = UI_KEY_DOWN;        return true; /* held rotate cw */
        default: return false;
    }
}
