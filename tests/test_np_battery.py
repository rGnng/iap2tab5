"""Qualify real pack measurements without changing any charger controls."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
UNKNOWN,ABSENT,IDLE,CHARGING,DISCHARGING,FULL=range(6)

class NPBatteryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name)
        (p/'wrapper.c').write_text('''
#include "np_battery.h"
static np_battery_t battery;
void reset(void) { battery=(np_battery_t){0}; }
void feed(int valid,int enabled,int mv,int ma,unsigned now) {
 np_battery_sample_t s={valid,enabled,mv,ma};np_battery_update(&battery,&s,now);
}
int state(void) { return battery.status.state; }
int present(void) { return battery.status.present; }
int available(void) { return battery.status.available; }
int percent(void) { return battery.status.percent; }
int low(void) { return battery.status.low; }
''')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
            '-I'+str(ROOT/'app/backend'),str(p/'wrapper.c'),
            str(ROOT/'app/backend/np_battery.c'),'-o',str(p/'battery.so')],check=True)
        cls.lib=C.CDLL(str(p/'battery.so'))
        cls.lib.feed.argtypes=[C.c_int,C.c_int,C.c_int,C.c_int,C.c_uint32]

    def setUp(self): self.lib.reset()
    def feed(self,t,mv=7871,ma=0,enabled=False,valid=True):
        self.lib.feed(valid,enabled,mv,ma,t)
    def qualify(self,**kw): self.feed(0,**kw);self.feed(3000,**kw)

    def test_disabled_idle_pack_is_not_full(self):
        self.qualify(mv=8350)
        self.feed(90000,mv=8350)
        self.assertEqual(self.lib.state(),IDLE)
        self.assertEqual(self.lib.percent(),100)

    def test_present_pack_has_explicit_voltage_estimate(self):
        self.feed(0)
        self.assertEqual(self.lib.percent(),-1)
        self.feed(3000)
        self.assertTrue(self.lib.present())
        self.assertEqual(self.lib.percent(),79)

    def test_current_sign_must_stay_consistent_for_three_seconds(self):
        self.qualify(ma=-500,enabled=True)
        self.assertEqual(self.lib.state(),CHARGING)
        self.feed(4000,ma=300);self.feed(6000,ma=-500,enabled=True)
        self.assertEqual(self.lib.state(),CHARGING)
        self.feed(7000,ma=300);self.feed(9999,ma=300)
        self.assertEqual(self.lib.state(),CHARGING)
        self.feed(10000,ma=300)
        self.assertEqual(self.lib.state(),DISCHARGING)

    def test_idle_mid_charge_never_becomes_full(self):
        self.qualify(enabled=True)
        self.feed(120000,enabled=True)
        self.assertEqual(self.lib.state(),IDLE)

    def test_full_requires_settled_high_voltage_and_has_hysteresis(self):
        self.qualify(ma=-500,enabled=True)
        self.feed(10000,mv=8230,enabled=True)
        self.feed(69999,mv=8230,enabled=True)
        self.assertNotEqual(self.lib.state(),FULL)
        self.feed(70000,mv=8230,enabled=True)
        self.feed(73000,mv=8230,enabled=True)
        self.assertEqual(self.lib.state(),FULL)
        self.feed(80000,mv=8150,enabled=True)
        self.assertEqual(self.lib.state(),FULL)
        self.feed(81000,mv=8050,enabled=True);self.feed(84000,mv=8050,enabled=True)
        self.assertEqual(self.lib.state(),IDLE)

    def test_sensor_failure_cannot_complete_a_charge_or_create_low_warning(self):
        self.qualify(ma=-300,enabled=True)
        self.feed(100000,valid=False,mv=0,ma=0)
        self.assertFalse(self.lib.available())
        self.assertFalse(self.lib.low())
        self.assertEqual(self.lib.percent(),-1)
        self.assertEqual(self.lib.state(),UNKNOWN)

    def test_absent_pack_is_not_zero_percent_or_low_during_removal(self):
        self.qualify(ma=300)
        self.feed(4000,mv=960,ma=0)
        self.assertFalse(self.lib.present());self.assertFalse(self.lib.low())
        self.assertEqual(self.lib.percent(),-1)
        self.feed(7000,mv=960)
        self.assertEqual(self.lib.state(),ABSENT)

    def test_low_warning_hysteresis_and_charging_recovery(self):
        self.qualify(mv=6840,ma=300)
        self.assertTrue(self.lib.low())
        self.feed(4000,mv=6900,ma=300)
        self.assertTrue(self.lib.low())
        self.feed(5000,mv=7000,ma=300)
        self.assertFalse(self.lib.low())
        self.feed(6000,mv=6800,ma=-500,enabled=True)
        self.assertFalse(self.lib.low())
        self.feed(9000,mv=6800,ma=-500,enabled=True)
        self.assertEqual(self.lib.state(),CHARGING)

    def test_failed_sample_restarts_full_qualification(self):
        self.qualify(mv=8230,enabled=True)
        self.feed(59000,mv=8230,enabled=True)
        self.feed(60000,valid=False)
        self.feed(61000,mv=8230,enabled=True)
        self.feed(64000,mv=8230,enabled=True)
        self.assertEqual(self.lib.state(),IDLE)

    def test_timers_work_across_tick_wrap(self):
        self.feed(0xfffffc00,ma=-300,enabled=True)
        self.feed((0xfffffc00+3000)&0xffffffff,ma=-300,enabled=True)
        self.assertEqual(self.lib.state(),CHARGING)

if __name__=='__main__': unittest.main()
