#ifndef IAP2TAB5_UI_H
#define IAP2TAB5_UI_H

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_KEY_UP = 0,
    UI_KEY_DOWN,
    UI_KEY_ENTER,
    UI_KEY_1,
    UI_KEY_2,
    UI_KEY_3,
    UI_KEY_4,
    UI_KEY_5,
    UI_KEY_6,
    UI_KEY_7,
    UI_KEY_BACK,
    UI_KEY_PAGE_UP,
    UI_KEY_PAGE_DOWN,
    UI_KEY_CONTEXT,
    UI_KEY_SCREEN_PREV,
    UI_KEY_SCREEN_NEXT,
    UI_KEY_PREV_TRACK,
    UI_KEY_NEXT_TRACK,
    UI_KEY_VOLUME_UP,
    UI_KEY_VOLUME_DOWN,
    UI_KEY_MUTE,
    UI_KEY_STAR,
} ui_key_t;

/**
 * Build the application UI and initialise the mock backend.
 * The UI targets 1280x720, matching the M5Stack Tab5 panel.
 */
void ui_init(void);

/** Feed one logical key event from the HID/keyboard layer. */
void ui_handle_key(ui_key_t key);

/** Jump directly to a screen by zero-based index (simulator/debug only). */
void ui_select_screen(int index);

/** Attach the pointer/touch input device created by the platform layer. */
void ui_attach_touch(lv_indev_t *indev);

/** Enable/disable touch input (persisted by the platform layer later). */
void ui_set_touch_enabled(bool enabled);
bool ui_touch_enabled(void);

/** Move the vertical menubar to the left or right edge. */
void ui_set_menubar_left(bool left);
bool ui_menubar_left(void);

/** Hardware/platform configuration shown in the Settings screen. */
typedef struct {
    int screen_flip;       /* 0 or 180 */
    int wifi_mode;         /* 0 = station, 1 = AP */
    bool sound_enabled;
    int volume;            /* 0..100 */
    char ap_ssid[33];
    char wifi_status[64];
    char wifi_ip[32];

    /* Existing UI settings, persisted through the same SD config file. */
    int clock_12h;
    int sort_field;        /* 0 = add date, 1 = name, 2 = year */
    int sort_desc;
    int menubar_left;
    int touch_enabled;
    int np_bg_mode;        /* 0 = theme, 1 = blurred, 2 = gradient */
    int brightness;        /* 0..100 */
    int usb_mode;          /* 0 = host, 1 = OTG */
} ui_system_config_t;

/* Applies saved settings before ui_init(). */
void ui_apply_saved_settings(const ui_system_config_t *cfg);

void ui_set_system_config(const ui_system_config_t *cfg);
void ui_set_system_config_changed_cb(void (*cb)(const ui_system_config_t *cfg));
void ui_set_network_status(const char *status, const char *ip);
void ui_system_toggle_screen_flip(void);

/** Debug: page the active list by one viewport (direction +1/-1). */
void ui_debug_page(int direction);

/** Debug: scroll the active list by dy pixels (negative = content up). */
void ui_debug_scroll(int dy);

typedef enum {
    UI_FEEDBACK_CLICK = 0,
    UI_FEEDBACK_ACTIVATE,
    UI_FEEDBACK_BACK,
} ui_feedback_t;

/** Optional short UI sound hook implemented by the firmware. */
void ui_set_feedback_cb(void (*cb)(int type));

/* Apple File Conduit Sync settings page. The UI is shared with the simulator;
 * the firmware supplies the live status and handles the actions. */
typedef struct {
    bool available;
    bool afc_mode;
    bool connected;
    bool sd_ready;
    bool job_active;
    int job_kind;              /* 1 = quick, 2 = full */
    int cached_albums;
    int thumb_count;
    int cover_count;
    uint64_t thumb_bytes;
    uint64_t cover_bytes;
    uint64_t original_bytes;
    uint64_t cache_bytes;
    int sync_total;
    int sync_phase;            /* 1 = database, 2 = album art, 3 = thumbnail */
    int sync_attempted;
    int sync_copied;
    int sync_failed;
    uint64_t sync_current_id;
    char sync_album[96];
    char sync_artist[96];
    char status[96];

    /* SD card */
    bool sd_info_ready;
    char sd_vendor[32];
    char sd_type[48];
    char sd_fs[16];
    uint64_t sd_used_bytes;
    uint64_t sd_total_bytes;

    /* USB / device */
    char usb_status[64];
    char usb_mode[32];
    char usb_operation[32];
    char library_suffix[16];

    /* Library */
    int library_albums;
    int library_tracks;
    char library_db_item[64];
    char library_updated[32];
} ui_afc_status_t;

enum {
    UI_AFC_ACTION_PAIR = 0,
    UI_AFC_ACTION_QUICK_SYNC,
    UI_AFC_ACTION_FULL_SYNC,
    UI_AFC_ACTION_IPOD_MODE,
};

void ui_set_artwork_dir(const char * dir);
void ui_set_thumb_pack_dir(const char * dir);
void ui_set_thumb_pack_path(const char * path, int album_count);

void ui_set_afc_status_cb(void (*cb)(ui_afc_status_t * out));
void ui_set_afc_action_cb(void (*cb)(int action));

/** Boot into the dedicated AFC sync screen instead of the media UI. */
void ui_set_afc_sync_mode(bool enabled);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IAP2TAB5_UI_H */
