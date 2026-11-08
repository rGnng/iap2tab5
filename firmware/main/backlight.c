#include "backlight.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include <stdbool.h>

static bool s_reference_pwm;

static int clamp_brightness(int brightness)
{
    return brightness < 0 ? 0 : brightness > 100 ? 100 : brightness;
}

uint32_t backlight_pwm_period(void)
{
    return s_reference_pwm ? BACKLIGHT_PWM_PERIOD : 1024u;
}

esp_err_t backlight_set(int brightness)
{
    brightness = clamp_brightness(brightness);
    if(!s_reference_pwm) return bsp_display_brightness_set(brightness);
    /* 0 and 512 are constant low/high. Full scale is the complete period,
     * not 511, which would leave a short off pulse even at 100 percent. */
    uint32_t duty = ((uint32_t)brightness * BACKLIGHT_PWM_PERIOD + 50u) / 100u;
    esp_err_t err = ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,
        CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH, duty, 0);
    if(err == ESP_OK)
        ESP_LOGI("backlight", "Brightness %d%%, duty %u/%u, PWM %u Hz", brightness,
            (unsigned)duty, (unsigned)BACKLIGHT_PWM_PERIOD,
            (unsigned)ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0));
    else ESP_LOGE("backlight", "Brightness update failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t backlight_init(int brightness)
{
    s_reference_pwm = false;
    /* Latch low before routing GPIO22 back to the LEDC peripheral. Clear
     * any inherited open-drain/pull state explicitly, as M5GFX does. */
    gpio_config_t pin = {
        .pin_bit_mask = 1ULL << BSP_LCD_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_set_level(BSP_LCD_BACKLIGHT, 0);
    if(err == ESP_OK) err = gpio_config(&pin);
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_9_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BACKLIGHT_PWM_HZ,
        /* The crystal clock stays fixed across CPU frequency changes. */
        .clk_cfg = LEDC_USE_XTAL_CLK,
    };
    if(err == ESP_OK) err = ledc_timer_config(&timer);
    const ledc_channel_config_t channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .duty = 0,
        .hpoint = 0,
    };
    if(err == ESP_OK) err = ledc_channel_config(&channel);
    if(err == ESP_OK) {
        /* Required by LEDC's thread-safe immediate duty-update API. No
         * brightness fade is started. Another owner may have installed it. */
        err = ledc_fade_func_install(0);
        if(err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    }
    if(err == ESP_OK) {
        s_reference_pwm = true;
        err = backlight_set(brightness);
        if(err == ESP_OK) return ESP_OK;
    }

    /* Keep a usable display if the new clock/channel configuration fails.
     * Restore both timer resolution and channel before using the BSP setter. */
    s_reference_pwm = false;
    ESP_LOGW("backlight", "Reference PWM unavailable (%s), restoring BSP PWM", esp_err_to_name(err));
    esp_err_t restore = bsp_display_brightness_init();
    return restore == ESP_OK ? backlight_set(brightness) : restore;
}
