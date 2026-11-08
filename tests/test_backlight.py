"""Run the production backlight driver against a clock/duty hardware model."""
import ctypes as C
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BacklightTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        (work / 'driver').mkdir()
        (work / 'bsp').mkdir()
        (work / 'esp_err.h').write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
const char *esp_err_to_name(int err);
''')
        (work / 'esp_log.h').write_text('''#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
''')
        (work / 'bsp/esp-bsp.h').write_text('''#pragma once
#include "esp_err.h"
#define BSP_LCD_BACKLIGHT 22
#define CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH 1
int bsp_display_brightness_set(int brightness);
int bsp_display_brightness_init(void);
''')
        (work / 'driver/gpio.h').write_text('''#pragma once
#include <stdint.h>
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type; } gpio_config_t;
int gpio_set_level(int pin,int level);
int gpio_config(const gpio_config_t *cfg);
''')
        (work / 'driver/ledc.h').write_text('''#pragma once
#include <stdint.h>
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_0 0
#define LEDC_TIMER_9_BIT 9
#define LEDC_USE_XTAL_CLK 2
#define LEDC_INTR_DISABLE 0
typedef struct { int speed_mode,duty_resolution,timer_num;uint32_t freq_hz;int clk_cfg; } ledc_timer_config_t;
typedef struct {int gpio_num,speed_mode,channel,timer_sel,intr_type;uint32_t duty;int hpoint;} ledc_channel_config_t;
int ledc_timer_config(const ledc_timer_config_t *cfg);
int ledc_channel_config(const ledc_channel_config_t *cfg);
int ledc_set_duty_and_update(int speed,int channel,uint32_t duty,uint32_t hpoint);
int ledc_fade_func_install(int flags);
uint32_t ledc_get_freq(int speed,int timer);
''')
        (work / 'model.c').write_text(r'''
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
static int failure,timers,restores,updates,bad_order,bits,clock,level_set,pin_set,fade_ready;
static uint32_t frequency,duty,first_nonzero;
const char *esp_err_to_name(int err) { (void)err;return "test"; }
int gpio_set_level(int pin,int level) { if(pin!=22 || level!=0)bad_order++;level_set=1;return failure==1?-1:0; }
int gpio_config(const gpio_config_t *c) {
 if(!level_set || c->pin_bit_mask!=(1ull<<22) || c->mode!=1 || c->pull_up_en || c->pull_down_en)bad_order++;
 pin_set=1;return failure==2?-1:0;
}
int ledc_timer_config(const ledc_timer_config_t *c) {
 if(!pin_set)bad_order++;timers++;bits=c->duty_resolution;clock=c->clk_cfg;frequency=c->freq_hz;
 return failure==3?-1:0;
}
int ledc_channel_config(const ledc_channel_config_t *c) {
 if(c->gpio_num!=22 || c->channel!=1 || c->timer_sel!=0 || c->duty || c->hpoint)bad_order++;
 duty=c->duty;return failure==4?-1:0;
}
int ledc_set_duty_and_update(int speed,int channel,uint32_t d,uint32_t hpoint) {
 if(speed || channel!=1 || hpoint || d>(1u<<bits) || !fade_ready)bad_order++;
 updates++;if(failure==5)return -1;duty=d;if(d && !first_nonzero)first_nonzero=d;return 0;
}
int ledc_fade_func_install(int flags) { (void)flags;if(failure==7)return -1;fade_ready=1;return 0; }
uint32_t ledc_get_freq(int speed,int timer) { (void)speed;(void)timer;return frequency; }
int bsp_display_brightness_init(void) {
 restores++;bits=10;frequency=5000;duty=0;return failure==6?-1:0;
}
int bsp_display_brightness_set(int b) { duty=(uint32_t)b*1023/100;return 0; }
void reset(int f) { failure=f;timers=restores=updates=bad_order=bits=clock=level_set=pin_set=fade_ready=0;frequency=duty=first_nonzero=0; }
void set_failure(int f) { failure=f; }
unsigned value(void) { return duty; }
unsigned first_lit(void) { return first_nonzero; }
int timer_count(void) { return timers; }
int restore_count(void) { return restores; }
int clock_source(void) { return clock; }
int resolution(void) { return bits; }
int violations(void) { return bad_order; }
''')
        library = work / 'backlight.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-misleading-indentation', '-shared', '-fPIC', '-I'+str(work),
                        str(ROOT / 'firmware/main/backlight.c'), str(work / 'model.c'),
                        '-o', str(library)], check=True)
        cls.lib = C.CDLL(str(library))
        cls.lib.backlight_init.argtypes = [C.c_int]
        cls.lib.backlight_set.argtypes = [C.c_int]
        cls.lib.reset.argtypes = [C.c_int]

    def setUp(self):
        self.lib.reset(0)

    def test_reference_carrier_clock_and_glitch_free_start(self):
        self.assertEqual(self.lib.backlight_init(40), 0)
        self.assertEqual(self.lib.resolution(), 9)
        self.assertEqual(self.lib.clock_source(), 2)
        self.assertEqual(self.lib.ledc_get_freq(0, 0), 44100)
        self.assertEqual(self.lib.backlight_pwm_period(), 512)
        self.assertEqual(self.lib.value(), 205)
        self.assertEqual(self.lib.first_lit(), 205)
        self.assertEqual(self.lib.violations(), 0)

    def test_full_on_off_and_wake(self):
        self.lib.backlight_init(40)
        for brightness, duty in [(0,0),(100,512),(0,0),(40,205)]:
            self.assertEqual(self.lib.backlight_set(brightness), 0)
            self.assertEqual(self.lib.value(), duty)
        self.assertEqual(self.lib.timer_count(), 1)
        self.assertEqual(self.lib.violations(), 0)

    def test_all_brightness_steps_are_monotonic_and_do_not_restart_timer(self):
        self.lib.backlight_init(0)
        values=[]
        for brightness in range(101):
            self.lib.backlight_set(brightness)
            values.append(self.lib.value())
        self.assertEqual(values[0], 0)
        self.assertEqual(values[-1], 512)
        self.assertEqual(len(set(values)), 101)
        self.assertEqual(values, sorted(values))
        self.assertEqual(self.lib.timer_count(), 1)

    def test_clamping(self):
        self.lib.backlight_init(-1)
        self.assertEqual(self.lib.value(), 0)
        self.lib.backlight_set(1000)
        self.assertEqual(self.lib.value(), 512)
        self.lib.backlight_set(-1000)
        self.assertEqual(self.lib.value(), 0)

    def test_failed_configuration_restores_bsp_resolution_and_brightness(self):
        for failure in [1,2,3,4,5,7]:
            self.lib.reset(failure)
            self.assertEqual(self.lib.backlight_init(40), 0)
            self.assertEqual(self.lib.restore_count(), 1)
            self.assertEqual(self.lib.backlight_pwm_period(), 1024)
            self.assertEqual(self.lib.resolution(), 10)
            self.assertEqual(self.lib.value(), 409)
            self.lib.backlight_set(0)
            self.assertEqual(self.lib.value(), 0)

    def test_update_error_is_reported(self):
        self.lib.backlight_init(40)
        self.lib.set_failure(5)
        self.assertEqual(self.lib.backlight_set(60), -1)
        self.assertEqual(self.lib.value(), 205)
        self.lib.set_failure(0)
        self.assertEqual(self.lib.backlight_set(60), 0)
        self.assertEqual(self.lib.value(), 307)


if __name__ == '__main__':
    unittest.main()
