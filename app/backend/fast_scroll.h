#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FAST_SCROLL_TRIGGER_WINDOW_DEFAULT_MS 550u
/* Allow brief gaps in a continuous turn without repeated entry cues. */
#define FAST_SCROLL_PAUSE_MS 300u
#define FAST_SCROLL_HISTORY 128

typedef struct {
    uint32_t ticks[FAST_SCROLL_HISTORY];
    uint32_t last_ms;
    unsigned head, count;
    int direction, rows, pages, trigger_window_ms, hold_ms;
    bool active;
} fast_scroll_t;

int fast_scroll_trigger_window_normalize(int milliseconds);
int fast_scroll_hold_normalize(int milliseconds);
void fast_scroll_reset(fast_scroll_t *state);
bool fast_scroll_expire(fast_scroll_t *state, uint32_t now_ms);
/* pages is the entry threshold in physical pages per trigger window (0 disables).
 * Returns logical rows. Once active, each physical notch advances one page.
 * entered is true only on a transition, for one audible cue per fast spin. */
int fast_scroll_feed(fast_scroll_t *state, uint32_t now_ms, int direction,
                     int notches, int rows, int pages, int trigger_window_ms,
                     int hold_ms, bool *entered);
