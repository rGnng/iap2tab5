#include "fast_scroll.h"
#include <string.h>

int fast_scroll_hold_normalize(int milliseconds)
{
    switch(milliseconds) {
        case 150: case 300: case 500: case 1500: return milliseconds;
        default: return FAST_SCROLL_PAUSE_MS;
    }
}

void fast_scroll_reset(fast_scroll_t *s) { memset(s, 0, sizeof(*s)); }

bool fast_scroll_expire(fast_scroll_t *s, uint32_t now)
{
    bool active = s->active;
    int32_t elapsed = (int32_t)(now - s->last_ms);
    if(s->count && elapsed >= fast_scroll_hold_normalize(s->hold_ms)) fast_scroll_reset(s);
    return active && !s->active;
}

int fast_scroll_feed(fast_scroll_t *s, uint32_t now, int direction,
                     int notches, int rows, int pages, int hold_ms, bool *entered)
{
    *entered = false;
    hold_ms = fast_scroll_hold_normalize(hold_ms);
    if(notches <= 0) return 0;
    if(pages <= 0 || pages > 5 || rows < 2) {
        fast_scroll_reset(s);
        return notches;
    }
    /* Report age and the 5 ms LVGL tick are sampled separately. Queued
     * reports in the same BLE burst can map a few milliseconds backwards.
     * Keep input time monotonic instead of mistaking unsigned underflow for
     * a long pause (or pruning the entire rolling window). */
    if(s->count && (int32_t)(now - s->last_ms) < 0) now = s->last_ms;
    fast_scroll_expire(s, now);
    if(s->direction != direction || s->rows != rows || s->pages != pages || s->hold_ms != hold_ms) fast_scroll_reset(s);
    s->direction = direction; s->rows = rows; s->pages = pages; s->hold_ms = hold_ms;
    while(s->count && (uint32_t)(now - s->ticks[s->head]) >= FAST_SCROLL_WINDOW_MS) {
        s->head = (s->head + 1) % FAST_SCROLL_HISTORY; s->count--;
    }
    int steps = 0;
    for(int i = 0; i < notches; i++) {
        if(s->count == FAST_SCROLL_HISTORY) {
            s->head = (s->head + 1) % FAST_SCROLL_HISTORY; s->count--;
        }
        s->ticks[(s->head + s->count++) % FAST_SCROLL_HISTORY] = now;
        if(!s->active && s->count >= (unsigned)(rows * pages)) {
            s->active = true; *entered = true;
        }
        steps += s->active ? rows : 1;
    }
    s->last_ms = now;
    return steps;
}
