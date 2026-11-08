#ifndef IAP2TAB5_SCREEN_IDLE_H
#define IAP2TAB5_SCREEN_IDLE_H

#include <stdbool.h>
#include <stdint.h>

/* Monotonic milliseconds; unsigned subtraction also handles clock wrap. */
typedef struct {
    uint32_t timeout_ms;
    uint32_t quiet_since_ms;
    bool playing;
    bool off;
    bool manual;
} screen_idle_t;

void screen_idle_configure(screen_idle_t *s, int minutes, uint32_t now);
void screen_idle_update(screen_idle_t *s, bool playing, uint32_t now);
/* Returns true when this input woke the screen and should be consumed. */
bool screen_idle_activity(screen_idle_t *s, uint32_t now);
void screen_idle_off(screen_idle_t *s);

#endif
