#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FAST_SCROLL_WINDOW_MS 1200u
/* Allow brief gaps in a continuous turn without repeated entry cues. */
#define FAST_SCROLL_PAUSE_MS 300u
#define FAST_SCROLL_HISTORY 128

typedef struct {
    uint32_t ticks[FAST_SCROLL_HISTORY];
    uint32_t last_ms;
    unsigned head, count;
    int direction, rows, pages;
    bool active;
} fast_scroll_t;

void fast_scroll_reset(fast_scroll_t *state);
bool fast_scroll_expire(fast_scroll_t *state, uint32_t now_ms);
/* pages is the entry threshold in physical pages per 1200 ms (0 disables).
 * Returns logical rows. Once active, each physical notch advances one page.
 * entered is true only on a transition, for one audible cue per fast spin. */
int fast_scroll_feed(fast_scroll_t *state, uint32_t now_ms, int direction,
                     int notches, int rows, int pages, bool *entered);
