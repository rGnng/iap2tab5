#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "artwork_quality.h"

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* iAP2 over the iPod's USB configuration 2 HID interface. Requires
 * usb_host_core_set_apple_config(2) before enumeration. */
esp_err_t iap2_host_start(void);

/* Diagnostic counters are cumulative for this boot; snapshots are approximate. */
typedef struct {
    uint32_t rx_packets, bad_headers, bad_payloads, duplicates, sequence_gaps;
    uint32_t rx_overflows, hid_errors, queue_stalls, queue_high_water;
    uint32_t link_resets, retransmits, library_messages, library_items;
    bool library_complete;
} iap2_link_stats_t;
void iap2_host_link_stats(iap2_link_stats_t * out);

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
    int max_current_ma;       /* PowerUpdate param 0, -1 unknown */
    bool device_will_charge;  /* PowerUpdate param 1 */
    uint8_t accessory_power_mode; /* PowerUpdate param 2 */
    bool external_charger;    /* PowerUpdate param 4 */
    uint8_t charging_state;   /* PowerUpdate param 5: 0 none, 1 charging, 2 charged */
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
/* Diagnostic resubscription; preserves track, queue and playback position. */
esp_err_t iap2_host_refresh_now_playing(void);

/* Re-send the iAP2 500 mA USB budget with battery charging enabled.
 * Requires a connected, identified device; sends no USB vendor request. */
esp_err_t iap2_host_request_charging(void);

/* Sends one consumer-control HID bit: play, pause, next, previous,
 * volume_up, volume_down, mute, stop, shuffle, repeat. */
esp_err_t iap2_host_hid_command(const char * name);

/* Send one raw consumer-control report bit (0..11). Debug aid for mapping
 * the iAP2 HID command bits on real hardware. */
esp_err_t iap2_host_hid_raw_bit(uint8_t bit);

/* Press-and-hold helpers: press leaves the report bit set, release sends the
 * all-zero report. Used for hold-to-rewind / hold-to-fast-forward. */
esp_err_t iap2_host_hid_press_bit(uint8_t bit);
esp_err_t iap2_host_hid_release(void);

/* PlayMediaLibraryItems (0x4C07): keys are persistent media item IDs
 * (the SQLite item_pid values), start_index is the first queue entry. */
esp_err_t iap2_host_play_track_keys(const uint64_t * keys, int count,
                                    int start_index);

/* Plays the whole media library in device order from start_pid
 * (PlayMediaLibrarySpecial, 0x4C09). */
esp_err_t iap2_host_play_all_songs(uint64_t start_pid);

/* The iPod's own queue (PlaybackQueueList file transfer).  Count is 0 until
 * the device has sent one. */
int iap2_host_queue_list_count(void);
int iap2_host_queue_list_base(void);
uint32_t iap2_host_queue_list_revision(void);
bool iap2_host_queue_list_get(int index, uint64_t * key);

/* Play the currently reported track's persistent key (debug/recovery). */
esp_err_t iap2_host_play_current_key(void);

/* Latest Now Playing artwork as 360x360 RGB565. The revision changes when a
 * new image is decoded or the current track changes (invalidating the old
 * image). Copy leaves dst untouched when unavailable or below minimum quality.
 * actual receives the source dimensions of the available image, or zeros. */
#define IAP2_ARTWORK_SIZE 360
uint32_t iap2_host_artwork_revision(void);
bool iap2_host_artwork_copy(uint8_t * dst, size_t len, artwork_quality_t minimum,
                            artwork_quality_t * actual);

#ifdef __cplusplus
}
#endif
