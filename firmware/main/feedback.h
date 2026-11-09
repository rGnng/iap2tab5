#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FEEDBACK_CLICK = 0,
    FEEDBACK_ACTIVATE,
    FEEDBACK_BACK,
    FEEDBACK_FAST_SCROLL,
    FEEDBACK_FAST_SCROLL_EXIT,
    FEEDBACK_SCREEN_OFF,
    FEEDBACK_SCREEN_ON,
} feedback_t;

/* Initializes the speaker codec and starts the beep task. Returns false when
 * audio hardware is unavailable; the UI keeps working either way. */
bool feedback_init(bool enabled, uint8_t volume);
void feedback_set_enabled(bool enabled);
void feedback_set_volume(uint8_t volume);
void feedback_play(feedback_t type);

#ifdef __cplusplus
}
#endif
