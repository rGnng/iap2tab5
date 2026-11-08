#ifndef IAP2TAB5_ARTWORK_REFRESH_H
#define IAP2TAB5_ARTWORK_REFRESH_H

#include <stdbool.h>
#include <stdint.h>

#define ARTWORK_REFRESH_DELAY_MS 2000u
#define ARTWORK_REFRESH_TARGET 360u

typedef struct {
    uint64_t track;
    uint32_t since_ms;
    bool checked;
} artwork_refresh_t;

typedef struct {
    uint32_t since_ms;
    bool waiting, empty_pending;
} artwork_refresh_snapshot_t;

static inline void artwork_refresh_snapshot_begin(artwork_refresh_snapshot_t * state,
                                                   uint32_t now_ms)
{
    *state = (artwork_refresh_snapshot_t){ .since_ms = now_ms, .waiting = true };
}

/* The iPod briefly reports an empty item during a stop/start subscription.
 * Hold that synthetic snapshot for at most one second, while still accepting
 * playback updates and any real media identity that arrives. */
static inline bool artwork_refresh_snapshot_hold(artwork_refresh_snapshot_t * state,
                                                 uint32_t now_ms, bool empty,
                                                 bool identity_received)
{
    if(!state->waiting) return false;
    if(identity_received) {
        *state = (artwork_refresh_snapshot_t){0};
        return false;
    }
    if((uint32_t)(now_ms - state->since_ms) >= 1000u) {
        if(empty) *state = (artwork_refresh_snapshot_t){0};
        return false;
    }
    if(empty) state->empty_pending = true;
    return empty;
}

static inline bool artwork_refresh_snapshot_expire(artwork_refresh_snapshot_t * state,
                                                    uint32_t now_ms)
{
    if(!state->waiting || (uint32_t)(now_ms - state->since_ms) < 1000u) return false;
    bool clear = state->empty_pending;
    *state = (artwork_refresh_snapshot_t){0};
    return clear;
}

/* One retry after a stable track selection. A resubscription can temporarily
 * report PID 0; it must not rearm the same track into an endless retry loop. */
static inline bool artwork_refresh_needed(artwork_refresh_t * state, uint64_t track,
                                          uint32_t now_ms, bool busy, unsigned quality)
{
    if(!track) return false;
    if(track != state->track) {
        *state = (artwork_refresh_t){ .track = track, .since_ms = now_ms };
    }
    if(state->checked || busy || (uint32_t)(now_ms - state->since_ms) < ARTWORK_REFRESH_DELAY_MS)
        return false;
    state->checked = true;
    return quality < ARTWORK_REFRESH_TARGET;
}

#endif
