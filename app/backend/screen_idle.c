#include "screen_idle.h"

void screen_idle_configure(screen_idle_t *s, int minutes, uint32_t now)
{
    s->timeout_ms = (minutes == 30 || minutes == 60)
        ? (uint32_t)minutes * 60000u : 0;
    s->quiet_since_ms = now;
}

void screen_idle_update(screen_idle_t *s, bool playing, uint32_t now)
{
    if(playing || s->playing) s->quiet_since_ms = now;
    /* Playback may resume through the iPod itself. A manual blank stays off. */
    if(playing && !s->manual) s->off = false;
    s->playing = playing;
    if(!s->off && !playing && s->timeout_ms &&
       (uint32_t)(now - s->quiet_since_ms) >= s->timeout_ms) {
        s->off = true;
    }
}

bool screen_idle_activity(screen_idle_t *s, uint32_t now)
{
    bool woke = s->off;
    s->quiet_since_ms = now;
    s->off = false;
    s->manual = false;
    return woke;
}

void screen_idle_off(screen_idle_t *s)
{
    s->off = true;
    s->manual = true;
}
