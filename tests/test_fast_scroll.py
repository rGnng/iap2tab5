"""Exercise production scroll speed policy against physical detents and time."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class State(C.Structure):
    _fields_ = [('ticks', C.c_uint32 * 128), ('last_ms', C.c_uint32),
                ('head', C.c_uint), ('count', C.c_uint), ('direction', C.c_int),
                ('rows', C.c_int), ('pages', C.c_int), ('hold_ms',C.c_int), ('active', C.c_bool)]

class FastScrollTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / 'scroll.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared',
                        '-fPIC', str(ROOT / 'app/backend/fast_scroll.c'), '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.fast_scroll_feed.argtypes = [C.POINTER(State), C.c_uint32,
            C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.POINTER(C.c_bool)]
        cls.lib.fast_scroll_feed.restype = C.c_int
        cls.lib.fast_scroll_expire.argtypes = [C.POINTER(State), C.c_uint32]
        cls.lib.fast_scroll_expire.restype = C.c_bool

    def setUp(self): self.s = State()
    def feed(self, now, count=1, rows=8, pages=2, direction=1, hold=150):
        entered = C.c_bool()
        steps = self.lib.fast_scroll_feed(C.byref(self.s), now, direction,
            count, rows, pages, hold, C.byref(entered))
        return steps, entered.value

    def test_each_threshold_uses_physical_notches_then_one_page_per_notch(self):
        for pages in range(1, 6):
            self.s = State()
            for i in range(pages * 8 - 1): self.assertEqual(self.feed(i*20,pages=pages),(1,False))
            self.assertEqual(self.feed(pages*8*20,pages=pages),(8,True))
            self.assertEqual(self.feed(pages*8*20+20,pages=pages),(8,False))

    def test_slow_and_sparse_input_never_accelerates(self):
        for gap in (160, 250, 500, 1200):
            self.s = State()
            for i in range(100): self.assertEqual(self.feed(i*gap),(1,False))

    def test_window_excludes_old_notches(self):
        self.assertEqual(self.feed(0,15),(15,False))
        self.assertEqual(self.feed(1200),(1,False))
        self.assertFalse(self.s.active)

    def test_page_jumps_do_not_feed_back_into_the_threshold(self):
        self.feed(0,16)
        for i in range(1,20):self.assertEqual(self.feed(i*20),(8,False))
        self.assertLessEqual(self.s.count,35)

    def test_reversal_and_pause_restore_single_row_precision(self):
        self.feed(0,16)
        self.assertEqual(self.feed(20,direction=-1),(1,False))
        self.feed(40,16)
        self.assertTrue(self.lib.fast_scroll_expire(C.byref(self.s),190))
        self.assertEqual(self.feed(200),(1,False))

    def test_short_pauses_keep_fast_mode_without_another_entry_cue(self):
        for pages in range(1,6):
            for rows in (8,10):
                self.s = State()
                self.feed(0,rows*pages,rows=rows,pages=pages)
                for now in (80,180,329):
                    self.assertEqual(self.feed(now,rows=rows,pages=pages),(rows,False))
                self.assertFalse(self.lib.fast_scroll_expire(C.byref(self.s),478))
                self.assertTrue(self.lib.fast_scroll_expire(C.byref(self.s),479))
                self.assertEqual(self.feed(480,rows=rows,pages=pages),(1,False))

    def test_coarse_tick_rounding_does_not_turn_a_burst_into_a_timeout(self):
        self.feed(1000,16)
        self.assertFalse(self.lib.fast_scroll_expire(C.byref(self.s),999))
        self.assertTrue(self.s.active)
        self.assertEqual(self.feed(1020),(8,False))
        self.assertEqual(self.feed(1019),(8,False))
        self.assertTrue(self.s.active)
        self.assertEqual(self.s.last_ms,1020)
        self.assertEqual(self.feed(1021),(8,False))

    def test_each_saved_hold_time_expires_at_its_own_threshold(self):
        for hold in (150,300,500,1500):
            self.s=State();self.feed(100,16,hold=hold)
            self.assertFalse(self.lib.fast_scroll_expire(C.byref(self.s),100+hold-1))
            self.assertTrue(self.lib.fast_scroll_expire(C.byref(self.s),100+hold))
            self.assertEqual(self.feed(101+hold,hold=hold),(1,False))

    def test_invalid_hold_times_use_the_default(self):
        for hold in (0,-1,1,149,151,1000,1501):
            self.s=State();self.feed(100,16,hold=hold)
            self.assertEqual(self.s.hold_ms,150)
            self.assertTrue(self.lib.fast_scroll_expire(C.byref(self.s),250))

    def test_off_invalid_or_single_row_contexts_never_accelerate(self):
        for pages,rows in [(0,8),(-1,8),(6,8),(2,1),(2,0)]:
            self.feed(0,16)
            self.assertEqual(self.feed(10,40,pages=pages,rows=rows),(40,False))
            self.assertFalse(self.s.active)

    def test_changing_page_size_or_threshold_discards_old_velocity(self):
        self.feed(0,15)
        self.assertEqual(self.feed(10,rows=10),(1,False))
        self.assertEqual(self.feed(20,pages=3),(1,False))
        self.assertEqual(self.s.count,1)

    def test_uint32_clock_wrap_keeps_thresholds_and_pause_correct(self):
        start = 2**32-500
        for i in range(15):self.assertEqual(self.feed((start+i*40) % 2**32),(1,False))
        self.assertEqual(self.feed((start+600) % 2**32),(8,True))
        self.assertTrue(self.lib.fast_scroll_expire(C.byref(self.s),(start+750) % 2**32))

if __name__ == '__main__': unittest.main()
