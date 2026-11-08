#pragma once

#include "esp_err.h"
#include <stdint.h>

/* Match the M5GFX Tab5 reference: 44.1 kHz carrier, nine duty bits. */
#define BACKLIGHT_PWM_HZ 44100u
#define BACKLIGHT_PWM_BITS 9u
#define BACKLIGHT_PWM_PERIOD (1u << BACKLIGHT_PWM_BITS)

/* Called once after the BSP creates the display, while its backlight is off. */
esp_err_t backlight_init(int brightness);
esp_err_t backlight_set(int brightness);
uint32_t backlight_pwm_period(void);
