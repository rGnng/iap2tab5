#pragma once
#include "np_battery.h"
/* Poll outside the display lock. Readers receive only the cached snapshot. */
void np_battery_hw_poll(uint32_t now_ms);
void np_battery_hw_snapshot(np_battery_status_t *out);
