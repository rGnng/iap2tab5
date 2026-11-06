#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iAP2 over the iPod's USB configuration 2 HID interface. Requires
 * usb_host_core_set_apple_config(2) before enumeration. */
esp_err_t iap2_host_start(void);

typedef struct {
    bool connected;
    bool identified;
    uint8_t play_status;      /* 0 stopped, 1 playing, 2 paused */
    char title[128];
    char artist[128];
    char album[128];
    uint64_t track_key;       /* iAP2 persistent media item ID */
    char device_name[64];
    uint32_t elapsed_ms;
    uint32_t duration_ms;
    int queue_index;
    int queue_count;
    bool shuffle;
    uint8_t repeat;           /* iAP2 repeat mode */
    int battery;              /* -1 unknown */
    bool charging;
    uint32_t revision;
} iap2_state_t;

bool iap2_host_get_state(iap2_state_t * out);

/* SetNowPlayingInformation (0x5003) seek request. */
esp_err_t iap2_host_seek_ms(uint32_t elapsed_ms);

bool iap2_host_is_enabled(void);
bool iap2_host_is_connected(void);
bool iap2_host_is_identified(void);
bool iap2_host_is_playing(void);
const char * iap2_host_status(void);

/* Sends one consumer-control HID bit: play, pause, next, previous,
 * volume_up, volume_down, mute, stop, shuffle, repeat. */
esp_err_t iap2_host_hid_command(const char * name);

/* PlayMediaLibraryItems (0x4C07): keys are persistent media item IDs
 * (the SQLite item_pid values), start_index is the first queue entry. */
esp_err_t iap2_host_play_track_keys(const uint64_t * keys, int count,
                                    int start_index);

/* Play the currently reported track's persistent key (debug/recovery). */
esp_err_t iap2_host_play_current_key(void);

/* Latest Now Playing artwork as 360x360 RGB565. The revision changes when a
 * new image is decoded or the current track changes (invalidating the old
 * image). Copy returns false when no artwork is available. */
#define IAP2_ARTWORK_SIZE 360
uint32_t iap2_host_artwork_revision(void);
bool iap2_host_artwork_copy(uint8_t * dst, size_t len);

#ifdef __cplusplus
}
#endif
