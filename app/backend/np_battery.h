#ifndef NP_BATTERY_H
#define NP_BATTERY_H
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    NP_BATTERY_UNKNOWN, NP_BATTERY_ABSENT, NP_BATTERY_IDLE,
    NP_BATTERY_CHARGING, NP_BATTERY_DISCHARGING, NP_BATTERY_FULL
} np_battery_state_t;
typedef struct {
    bool valid, charger_enabled;
    int voltage_mv, current_ma; /* INA226 polarity: negative = charging. */
} np_battery_sample_t;
typedef struct {
    bool available, present, charger_enabled, low;
    int voltage_mv, current_ma, percent; /* voltage estimate; -1 = unavailable */
    np_battery_state_t state;
} np_battery_status_t;
typedef struct {
    np_battery_status_t status;
    np_battery_state_t candidate;
    uint32_t candidate_since, full_since;
    bool candidate_valid, full_pending;
} np_battery_t;

void np_battery_update(np_battery_t *battery, const np_battery_sample_t *sample,
                       uint32_t now_ms);
const char *np_battery_state_name(np_battery_state_t state);
#endif
