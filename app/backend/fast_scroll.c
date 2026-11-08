#include "fast_scroll.h"
#include <string.h>

void fast_scroll_reset(fast_scroll_t *s) { memset(s, 0, sizeof(*s)); }

static unsigned pause_ms(const fast_scroll_t *s)
{
    unsigned threshold = (unsigned)(s->rows * s->pages);
    unsigned pause = threshold ? 2 * FAST_SCROLL_WINDOW_MS / threshold : 300;
    return pause < 100 ? 100 : pause > 300 ? 300 : pause;
}

bool fast_scroll_expire(fast_scroll_t *s, uint32_t now)
{
    bool active = s->active;
    if(s->count && (uint32_t)(now - s->last_ms) >= pause_ms(s)) fast_scroll_reset(s);
    return active && !s->active;
}

int fast_scroll_feed(fast_scroll_t *s, uint32_t now, int direction,
                     int notches, int rows, int pages, bool *entered)
{
    *entered = false;
    if(notches <= 0) return 0;
    if(pages <= 0 || pages > 5 || rows < 2) {
        fast_scroll_reset(s);
        return notches;
    }
    fast_scroll_expire(s, now);
    if(s->direction != direction || s->rows != rows || s->pages != pages) fast_scroll_reset(s);
    s->direction = direction; s->rows = rows; s->pages = pages;
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
