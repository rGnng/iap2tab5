#ifndef IAP2TAB5_SCREENSHOT_H
#define IAP2TAB5_SCREENSHOT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Render the active LVGL screen into a PNG file.
 *
 * Works without a GPU and without an X11/Wayland display, which makes it
 * suitable for CI and for reviewing UI changes over SSH.
 */
bool sim_screenshot(const char * path);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IAP2TAB5_SCREENSHOT_H */
