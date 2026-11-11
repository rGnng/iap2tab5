"""Exercise the production read-only INA226 sampler with a fake I2C bus."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class NPBatteryHardwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name)
        for folder in ['bsp','driver','freertos']:(p/folder).mkdir()
        (p/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n')
        (p/'freertos/FreeRTOS.h').write_text('''#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
''')
        (p/'driver/i2c_master.h').write_text('''#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length,device_address,scl_speed_hz; } i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 7
esp_err_t i2c_master_bus_add_device(void *,const i2c_device_config_t *,void **);
esp_err_t i2c_master_transmit_receive(void *,const uint8_t *,size_t,uint8_t *,size_t,int);
''')
        (p/'bsp/esp-bsp.h').write_text('#include "driver/i2c_master.h"\nvoid *bsp_i2c_get_handle(void);\n')
        (p/'sampler.c').write_text(r'''
#include "np_battery_hw.c"
static uint16_t config,bus_reg,shunt;
static bool failed,enabled;
void *bsp_i2c_get_handle(void) { return (void*)1; }
int i2c_master_bus_add_device(void *bus,const i2c_device_config_t *cfg,void **out) {
 (void)bus;if(cfg->device_address!=0x41 || cfg->scl_speed_hz!=100000)return -1;
 *out=(void*)1;return 0;
}
int i2c_master_transmit_receive(void *dev,const uint8_t *reg,size_t a,uint8_t *raw,size_t b,int ms) {
 (void)dev;(void)a;(void)b;(void)ms;if(failed)return -1;
 uint16_t value=*reg==0?config:*reg==2?bus_reg:shunt;
 raw[0]=value>>8;raw[1]=value;return 0;
}
bool charge_read_enabled(void) { return enabled; }
void reset(void) {
 model=(np_battery_t){0};cached=(np_battery_status_t){.percent=-1};
 config=0x4127;bus_reg=6297;shunt=0;failed=enabled=false;
}
void sample(unsigned t,int shunt_raw,int en,int fail,int mode) {
 shunt=(uint16_t)shunt_raw;enabled=en;failed=fail;config=(config&~7u)|(unsigned)mode;
 np_battery_hw_poll(t);
}
int value(int which) {
 np_battery_status_t s;np_battery_hw_snapshot(&s);
 int values[]={s.available,s.state,s.voltage_mv,s.current_ma,s.percent,s.charger_enabled};
 return values[which];
}
''')
        lib=p/'sampler.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
            '-I'+str(p),'-I'+str(ROOT/'firmware/main'),'-I'+str(ROOT/'app/backend'),
            str(p/'sampler.c'),str(ROOT/'app/backend/np_battery.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        cls.lib.sample.argtypes=[C.c_uint32,C.c_int,C.c_int,C.c_int,C.c_int]

    def setUp(self):self.lib.reset()
    def test_voltage_and_charge_polarity_from_actual_register_format(self):
        self.lib.sample(0,-1000,1,0,7);self.lib.sample(3000,-1000,1,0,7)
        self.assertEqual([self.lib.value(i) for i in range(6)],[1,3,7871,-500,79,1])
    def test_read_error_does_not_publish_zero_current_as_idle_or_full(self):
        self.lib.sample(0,-1000,1,0,7);self.lib.sample(3000,-1000,1,0,7)
        self.lib.sample(4000,0,1,1,7)
        self.assertEqual([self.lib.value(i) for i in [0,1,4]],[0,0,-1])
    def test_disabled_adc_is_not_a_fresh_battery_sample(self):
        self.lib.sample(0,0,0,0,0);self.lib.sample(60000,0,0,0,0)
        self.assertEqual(self.lib.value(0),0)
    def test_positive_raw_shunt_is_discharge_even_with_control_enabled(self):
        self.lib.sample(0,1000,1,0,7);self.lib.sample(3000,1000,1,0,7)
        self.assertEqual(self.lib.value(1),4)
        self.assertEqual(self.lib.value(3),500)

if __name__=='__main__':unittest.main()
