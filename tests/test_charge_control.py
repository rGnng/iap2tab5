"""Run production charger control against a high-impedance expander model."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EN, QC = 1 << 7, 1 << 5


class ChargeControlTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        (work / 'bsp').mkdir()
        (work / 'esp_err.h').write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103
''')
        (work / 'esp_log.h').write_text('''#pragma once
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
''')
        (work / 'esp_io_expander.h').write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define IO_EXPANDER_PIN_NUM_5 (1u << 5)
#define IO_EXPANDER_PIN_NUM_6 (1u << 6)
#define IO_EXPANDER_PIN_NUM_7 (1u << 7)
#define IO_EXPANDER_INPUT 0
#define IO_EXPANDER_OUTPUT 1
#define IO_EXPANDER_OUTPUT_MODE_PUSH_PULL 0
typedef struct io *esp_io_expander_handle_t;
struct io {
    esp_err_t (*read_output_reg)(esp_io_expander_handle_t, uint32_t *);
    esp_err_t (*read_direction_reg)(esp_io_expander_handle_t, uint32_t *);
    esp_err_t (*read_highz_reg)(esp_io_expander_handle_t, uint32_t *);
};
esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t, uint32_t, int);
esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t, uint32_t, uint8_t);
esp_err_t esp_io_expander_set_output_mode(esp_io_expander_handle_t, uint32_t, int);
''')
        (work / 'bsp/esp-bsp.h').write_text('''#pragma once
#include "esp_io_expander.h"
esp_io_expander_handle_t bsp_io_expander1_init(void);
''')
        (work / 'model.c').write_text(r'''
#include <stdbool.h>
#include "esp_io_expander.h"
static uint32_t output, direction, highz;
static bool available, fail_qc, fail_enable, fail_enable_mode;
static int premature_enable;
static esp_err_t read_out(esp_io_expander_handle_t h, uint32_t *v) { (void)h; *v=output; return ESP_OK; }
static esp_err_t read_dir(esp_io_expander_handle_t h, uint32_t *v) { (void)h; *v=direction; return ESP_OK; }
static esp_err_t read_highz(esp_io_expander_handle_t h, uint32_t *v) { (void)h; *v=highz; return ESP_OK; }
static struct io io={.read_output_reg=read_out,.read_direction_reg=read_dir,.read_highz_reg=read_highz};
esp_io_expander_handle_t bsp_io_expander1_init(void) { return available ? &io : 0; }
static uint32_t driven_high(void) { return output & direction & ~highz; }
static void check_enable(void) {
    if((driven_high() & (1u<<7)) && !(direction & ~highz & (1u<<5))) premature_enable++;
}
esp_err_t esp_io_expander_set_dir(esp_io_expander_handle_t h, uint32_t pin, int value) {
    (void)h; if(value) direction|=pin; else direction&=~pin; check_enable(); return ESP_OK;
}
esp_err_t esp_io_expander_set_level(esp_io_expander_handle_t h, uint32_t pin, uint8_t value) {
    (void)h; if(pin==(1u<<7) && value && fail_enable) return ESP_FAIL;
    if(value) output|=pin; else output&=~pin; check_enable(); return ESP_OK;
}
esp_err_t esp_io_expander_set_output_mode(esp_io_expander_handle_t h, uint32_t pin, int mode) {
    (void)h; if(pin==(1u<<5) && fail_qc) return ESP_FAIL;
    if(pin==(1u<<7) && (output & pin) && fail_enable_mode) return ESP_FAIL;
    if(mode==IO_EXPANDER_OUTPUT_MODE_PUSH_PULL) highz&=~pin; else highz|=pin;
    check_enable(); return ESP_OK;
}
void reset(void) {
    /* Preserve active Wi-Fi / USB outputs; charger pins start high-Z. */
    output=9; direction=0xff; highz=0xffu & ~9u;
    available=true; fail_qc=fail_enable=fail_enable_mode=false; premature_enable=0;
}
void latch_only(void) { output|=1u<<7; direction|=1u<<7; }
void set_available(int value) { available=value; }
void set_failure(int stage) { fail_qc=stage==1; fail_enable=stage==2; fail_enable_mode=stage==3; }
unsigned outputs(void) { return output; }
unsigned directions(void) { return direction; }
unsigned high_impedance(void) { return highz; }
unsigned driven(void) { return driven_high(); }
int early_enable(void) { return premature_enable; }
''')
        library = work / 'charge.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-I' + str(work),
                        str(ROOT / 'firmware/main/charge.c'), str(work / 'model.c'),
                        '-o', str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        for name in ['charge_set_enabled', 'charge_set_qc_high']:
            getattr(cls.lib, name).argtypes = [ctypes.c_bool]
        cls.lib.charge_read_enabled.restype = ctypes.c_bool
        cls.lib.charge_apply.argtypes = [ctypes.c_bool, ctypes.c_bool]

    def setUp(self):
        self.lib.reset()

    def test_enable_drives_the_charger_pin_instead_of_only_its_latch(self):
        self.assertEqual(self.lib.charge_set_enabled(True), 0)
        self.assertTrue(self.lib.driven() & EN)
        self.assertTrue(self.lib.charge_read_enabled())
        self.assertFalse(self.lib.high_impedance() & EN)

    def test_latched_high_but_high_impedance_is_not_enabled(self):
        self.lib.latch_only()
        self.assertFalse(self.lib.charge_read_enabled())

    def test_qc_control_also_drives_its_pin_and_preserves_other_outputs(self):
        self.assertEqual(self.lib.charge_set_qc_high(True), 0)
        self.assertTrue(self.lib.driven() & QC)
        self.assertEqual(self.lib.driven() & 9, 9)
        self.assertEqual(self.lib.charge_set_qc_high(False), 0)
        self.assertFalse(self.lib.outputs() & QC)
        self.assertFalse(self.lib.high_impedance() & QC)

    def test_apply_configures_divider_and_status_before_enabling(self):
        for high in [False, True]:
            self.lib.reset()
            self.assertEqual(self.lib.charge_apply(True, high), 0)
            self.assertTrue(self.lib.driven() & EN)
            self.assertEqual(bool(self.lib.outputs() & QC), high)
            self.assertFalse(self.lib.high_impedance() & (EN | QC))
            self.assertFalse(self.lib.directions() & (1 << 6))
            self.assertEqual(self.lib.early_enable(), 0)
            self.assertEqual(self.lib.driven() & 9, 9)

    def test_disable_reliably_drives_enable_low(self):
        self.assertEqual(self.lib.charge_apply(True, False), 0)
        self.assertEqual(self.lib.charge_apply(False, False), 0)
        self.assertFalse(self.lib.driven() & EN)
        self.assertFalse(self.lib.charge_read_enabled())
        self.assertFalse(self.lib.outputs() & EN)
        self.assertFalse(self.lib.high_impedance() & EN)

    def test_failed_divider_setup_does_not_enable_charging(self):
        self.lib.set_failure(1)
        self.assertNotEqual(self.lib.charge_apply(True, False), 0)
        self.assertFalse(self.lib.driven() & EN)
        self.assertFalse(self.lib.charge_read_enabled())

    def test_enable_failure_clears_the_enable_latch(self):
        for stage in [2, 3]:
            self.lib.reset()
            self.lib.set_failure(stage)
            self.assertNotEqual(self.lib.charge_apply(True, False), 0)
            self.assertFalse(self.lib.outputs() & EN)
            self.assertFalse(self.lib.driven() & EN)

    def test_unavailable_expander_is_reported(self):
        self.lib.set_available(0)
        self.assertNotEqual(self.lib.charge_apply(True, False), 0)
        self.assertFalse(self.lib.charge_read_enabled())


if __name__ == '__main__':
    unittest.main()
