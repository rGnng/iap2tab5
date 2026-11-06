#ifndef IAP2TAB5_BOOT_H
#define IAP2TAB5_BOOT_H

#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Retro terminal-style boot sequence for the Tab5 UI.
 *
 * The sequence is drawn as a full-screen overlay on the active LVGL screen,
 * so the normal application UI can exist underneath. It is intentionally kept
 * platform-neutral: the firmware can feed real POST results into the same
 * widget set later.
 *
 * Timeline:
 *   0.00 s  POST checks (USB, SD, touch, display, memory, iAP2, network)
 *   3.50 s  "Initialize App"
 *   4.40 s  display/FPS check
 *   5.80 s  overlay removed, UI is revealed
 */

typedef enum {
    BOOT_ITEM_USB_INIT = 0,
    BOOT_ITEM_SD_CARD,
    BOOT_ITEM_TOUCH,
    BOOT_ITEM_DISPLAY,
    BOOT_ITEM_PSRAM,
    BOOT_ITEM_FLASH,
    BOOT_ITEM_USB_HOST,
    BOOT_ITEM_IAP2_CERT,
    BOOT_ITEM_WIFI_C6,
    BOOT_ITEM_NETWORK,
    BOOT_ITEM_COUNT,
} boot_item_t;

typedef enum {
    BOOT_STATE_PENDING = 0,
    BOOT_STATE_CHECKING,
    BOOT_STATE_OK,
    BOOT_STATE_WARN,
    BOOT_STATE_FAIL,
} boot_state_t;

/**
 * Start the animated boot sequence.
 *
 * @param on_done called once after the last screen has been shown, or after
 *                boot_ui_skip(). May be NULL.
 */
void boot_ui_start(void (*on_done)(void));

/**
 * Create the boot overlay and render a single frozen frame at @p t_ms.
 * Used by the simulator for deterministic screenshots. The overlay stays
 * visible until boot_ui_stop()/boot_ui_skip().
 */
void boot_ui_freeze(uint32_t t_ms);

/** Skip directly to the end and invoke the on_done callback. */
void boot_ui_skip(void);

/** Remove the overlay without invoking the callback. */
void boot_ui_stop(void);

bool boot_ui_active(void);

/** Override one POST result before starting the sequence. */
void boot_ui_set_result(boot_item_t item, boot_state_t state);

/** Override the FPS value shown on the display-check screen. Pass <= 0
 *  to use the built-in 59.9990..60.0000 flicker. */
void boot_ui_set_fps(float fps);

#ifdef __cplusplus
}
#endif

#endif /* IAP2TAB5_BOOT_H */
