#pragma once

#include "esp_lvgl_port_disp.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_DIAG_INTERVAL_MS 100
#define DISPLAY_DIAG_CAPACITY 1024
typedef struct {
    uint32_t seq, uptime_ms, pwm_duty, pwm_hz;
    uint32_t pwm_duty_changes, pwm_freq_changes;
    uint32_t renders, render_last_us, render_max_us;
    uint32_t internal_free, internal_largest;
    lvgl_port_display_stats_t display;
    uint32_t pwm_period;
} display_diag_sample_t;

/* Register on the LVGL thread, after display/backlight initialization. */
bool display_diag_init(lv_display_t *display);
bool display_diag_latest(display_diag_sample_t *out);
/* since=0 returns the oldest retained sample; next is the next unread seq. */
size_t display_diag_read(uint32_t since, display_diag_sample_t *out, size_t cap,
                         uint32_t *next, uint32_t *dropped);
uint32_t display_diag_mark(uint32_t *uptime_ms);
void display_diag_last_marker(uint32_t *id, uint32_t *uptime_ms);
