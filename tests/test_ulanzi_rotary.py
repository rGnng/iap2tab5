"""Replay real D100H noise through the production input filter and fast policy."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_fast_scroll import State as FastState

ROOT=Path(__file__).resolve().parents[1]
class Rotary(C.Structure):
    _fields_=[('direction',C.c_int),('last_us',C.c_int64),('pending_us',C.c_int64),('pending_last_us',C.c_int64),('pending',C.c_bool),('pending_count',C.c_uint)]
class Step(C.Structure):
    _fields_=[('delta',C.c_int),('first_us',C.c_int64),('last_us',C.c_int64)]
class Result(C.Structure):
    _fields_=[('steps',Step*2),('count',C.c_uint),('suppressed',C.c_bool)]

class UlanziRotaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name);(p/'lvgl.h').write_text('typedef struct lv_indev_t lv_indev_t;\n')
        lib=p/'rotary.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
            '-I'+str(p),'-I'+str(ROOT/'app/ui'),str(ROOT/'firmware/main/ulanzi_d100h.c'),
            str(ROOT/'app/backend/fast_scroll.c'),'-o',str(lib)],check=True)
        cls.lib=C.CDLL(str(lib))
        cls.lib.ulanzi_rotary_update.argtypes=[C.POINTER(Rotary),C.c_int,C.c_int64]
        for name in ('ulanzi_rotary_update','ulanzi_rotary_poll','ulanzi_rotary_flush'):
            getattr(cls.lib,name).restype=Result
        cls.lib.ulanzi_rotary_poll.argtypes=[C.POINTER(Rotary),C.c_int64]
        cls.lib.ulanzi_rotary_flush.argtypes=[C.POINTER(Rotary)]
        cls.lib.ulanzi_rotary_cancel.argtypes=[C.POINTER(Rotary)]
        cls.lib.ulanzi_rotary_deadline.argtypes=[C.POINTER(Rotary)]
        cls.lib.ulanzi_rotary_deadline.restype=C.c_int64
        cls.lib.fast_scroll_feed.argtypes=[C.POINTER(FastState),C.c_uint32,C.c_int,C.c_int,C.c_int,C.c_int,C.c_int,C.c_int,C.POINTER(C.c_bool)]
        cls.lib.fast_scroll_feed.restype=C.c_int

    def setUp(self):self.s=Rotary()
    def update(self,direction,t):return self.lib.ulanzi_rotary_update(C.byref(self.s),direction,t)
    def poll(self,t):return self.lib.ulanzi_rotary_poll(C.byref(self.s),t)
    def deltas(self,result):return [result.steps[i].delta for i in range(result.count)]
    def rapid(self):self.update(1,0);self.update(1,20000)

    def test_regular_same_direction_steps_are_immediate_at_any_speed(self):
        for t in (0,20000,200000,400000,1000000):
            self.assertEqual(self.deltas(self.update(1,t)),[1])
            self.assertFalse(self.s.pending)

    def test_normal_single_reverse_waits_exactly_30_ms_and_keeps_arrival_time(self):
        self.update(1,0)
        self.assertEqual(self.deltas(self.update(-1,200000)),[])
        self.assertEqual(self.lib.ulanzi_rotary_deadline(C.byref(self.s)),230000)
        self.assertEqual(self.deltas(self.poll(229999)),[])
        out=self.poll(230000)
        self.assertEqual(self.deltas(out),[-1])
        self.assertEqual((out.steps[0].first_us,out.steps[0].last_us),(200000,200000))
        self.assertEqual(self.deltas(self.poll(400000)),[])
        self.assertEqual(self.deltas(self.update(-1,600000)),[-1])

    def test_normal_reverse_glitch_is_discarded_when_turn_resumes_within_30_ms(self):
        for count in (1,2):
            with self.subTest(reverse_reports=count):
                self.s=Rotary();self.update(1,0);self.update(1,200000)
                for i in range(count):self.assertEqual(self.deltas(self.update(-1,400000+i*10000)),[])
                out=self.update(1,429999)
                self.assertTrue(out.suppressed)
                self.assertEqual(self.deltas(out),[1])
                self.assertEqual(self.deltas(self.poll(500000)),[])

    def test_normal_two_reverse_steps_keep_the_first_deadline_and_both_notches(self):
        self.update(1,0);self.update(-1,200000)
        self.assertEqual(self.deltas(self.update(-1,220000)),[])
        self.assertEqual(self.lib.ulanzi_rotary_deadline(C.byref(self.s)),230000)
        out=self.poll(230000)
        self.assertEqual(self.deltas(out),[-2])
        self.assertEqual((out.steps[0].first_us,out.steps[0].last_us),(200000,220000))

    def test_normal_three_consistent_reverse_reports_confirm_early(self):
        self.update(1,0);self.update(-1,200000);self.update(-1,210000)
        self.assertEqual(self.deltas(self.update(-1,220000)),[-3])
        self.assertEqual(self.lib.ulanzi_rotary_deadline(C.byref(self.s)),0)

    def test_normal_and_rapid_deadlines_use_the_physical_gap(self):
        for gap,deadline in ((80000,200000),(80001,110001)):
            with self.subTest(gap=gap):
                self.s=Rotary();self.update(1,0);self.update(-1,gap)
                self.assertEqual(self.lib.ulanzi_rotary_deadline(C.byref(self.s)),deadline)

    def test_isolated_opposite_step_is_filtered_before_navigation(self):
        self.rapid();self.assertEqual(self.deltas(self.update(-1,30000)),[])
        out=self.update(1,55000)
        self.assertEqual(self.deltas(out),[1]);self.assertTrue(out.suppressed)
        self.assertEqual(self.deltas(self.poll(100000)),[])

    def test_three_reverse_steps_confirm_without_losing_any(self):
        self.rapid();self.update(-1,30000);self.assertEqual(self.deltas(self.update(-1,45000)),[])
        out=self.update(-1,60000)
        self.assertEqual(self.deltas(out),[-3])
        self.assertEqual((out.steps[0].first_us,out.steps[0].last_us),(30000,60000))
        self.assertEqual(self.deltas(self.update(-1,70000)),[-1])
        self.assertFalse(self.s.pending)

    def test_single_intentional_reverse_is_released_by_deadline(self):
        self.rapid();self.update(-1,30000)
        self.assertEqual(self.lib.ulanzi_rotary_deadline(C.byref(self.s)),150000)
        self.assertEqual(self.deltas(self.poll(149999)),[])
        out=self.poll(150000);self.assertEqual(self.deltas(out),[-1])
        self.assertEqual(out.steps[0].first_us,30000)
        self.assertEqual(self.deltas(self.poll(250000)),[])

    def test_late_report_preserves_opposite_event_order(self):
        self.rapid();self.update(-1,30000)
        self.assertEqual(self.deltas(self.update(1,200000)),[-1])
        self.assertEqual(self.deltas(self.poll(230000)),[1])

    def test_two_report_glitch_is_held_and_discarded_on_original_direction(self):
        self.rapid();self.update(-1,30000)
        self.assertEqual(self.deltas(self.update(-1,38000)),[])
        out=self.update(1,52000)
        self.assertEqual(self.deltas(out),[1]);self.assertTrue(out.suppressed)

    def test_two_intentional_reverse_steps_are_preserved_on_timeout(self):
        self.rapid();self.update(-1,30000);self.update(-1,60000)
        out=self.poll(150000)
        self.assertEqual(self.deltas(out),[-2])
        self.assertEqual((out.steps[0].first_us,out.steps[0].last_us),(30000,60000))

    def test_late_original_report_within_new_window_is_filtered(self):
        self.rapid();self.update(-1,30000)
        out=self.update(1,120000)
        self.assertEqual(self.deltas(out),[1]);self.assertTrue(out.suppressed)

    def test_flush_before_enter_and_cancel_on_disconnect(self):
        self.rapid();self.update(-1,30000)
        self.assertEqual(self.deltas(self.lib.ulanzi_rotary_flush(C.byref(self.s))),[-1])
        self.update(1,40000);self.lib.ulanzi_rotary_cancel(C.byref(self.s))
        self.assertEqual(self.deltas(self.poll(200000)),[])
        self.assertEqual(self.deltas(self.update(-1,210000)),[-1])

    def test_usb_and_ble_filters_are_independent(self):
        self.rapid();self.update(-1,30000);other=Rotary()
        self.lib.ulanzi_rotary_update(C.byref(other),-1,40000)
        self.assertEqual(self.deltas(self.update(1,50000)),[1])
        self.assertEqual(other.direction,-1)

    def test_captured_fast_spins_have_no_extra_activations_from_reverse_glitches(self):
        for fixture in ('d100h_fast_spin.csv','d100h_fast_spin_late_reverses.csv'):
            with self.subTest(fixture=fixture):
                self.s=Rotary();rows=[]
                for line in (ROOT/'tests/fixtures'/fixture).read_text().splitlines():
                    if line and not line.startswith('#'):rows.append(tuple(map(int,line.split(','))))
                raw=FastState();filtered=FastState();clean=FastState();raw_entries=entries=clean_entries=suppressed=0;directions=[]
                def feed(s,t,d,count):
                    entered=C.c_bool();self.lib.fast_scroll_feed(C.byref(s),t//1000,d,count,8,2,550,150,C.byref(entered));return entered.value
                for t,d in rows:
                    raw_entries+=feed(raw,t,d,1)
                    if d>0:clean_entries+=feed(clean,t,d,1)
                    out=self.update(d,t);suppressed+=out.suppressed
                    for i in range(out.count):
                        step=out.steps[i];directions.append(1 if step.delta>0 else -1)
                        entries+=feed(filtered,step.last_us,1 if step.delta>0 else -1,abs(step.delta))
                self.assertGreater(raw_entries,clean_entries,'Noise must cause extra activations before input qualification')
                self.assertGreater(suppressed,0)
                self.assertGreaterEqual(clean_entries,1)
                self.assertEqual(entries,clean_entries,
                    'Filtered input must match clean input, including legitimate exits after 150 ms pauses')
                self.assertTrue(filtered.active)
                self.assertEqual(set(directions),{1})

if __name__=='__main__':unittest.main()
