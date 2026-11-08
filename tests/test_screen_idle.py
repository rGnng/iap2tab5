"""Exercise the actual idle policy with a monotonic, controllable clock."""
import ctypes as C
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class State(C.Structure):
    _fields_ = [('timeout', C.c_uint32), ('since', C.c_uint32),
                ('playing', C.c_bool), ('off', C.c_bool), ('manual', C.c_bool)]


class ScreenIdleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        lib = Path(cls.tmp.name) / 'idle.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', str(ROOT / 'app/backend/screen_idle.c'),
                        '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.screen_idle_configure.argtypes = [C.POINTER(State), C.c_int, C.c_uint32]
        cls.lib.screen_idle_update.argtypes = [C.POINTER(State), C.c_bool, C.c_uint32]
        cls.lib.screen_idle_activity.argtypes = [C.POINTER(State), C.c_uint32]
        cls.lib.screen_idle_activity.restype = C.c_bool
        cls.lib.screen_idle_off.argtypes = [C.POINTER(State)]

    def setUp(self):
        self.s = State()

    def configure(self, minutes, now=100):
        self.lib.screen_idle_configure(C.byref(self.s), minutes, now)

    def update(self, now, playing=False):
        self.lib.screen_idle_update(C.byref(self.s), playing, now)

    def activity(self, now):
        return self.lib.screen_idle_activity(C.byref(self.s), now)

    def test_disabled_default_and_invalid_settings(self):
        for minutes in [0, -1, 1, 29, 31, 59, 61, 256]:
            self.configure(minutes)
            self.update(100_000_000)
            self.assertFalse(self.s.off)

    def test_exact_thresholds(self):
        for minutes in [30, 60]:
            self.s = State()
            self.configure(minutes)
            self.update(100 + minutes * 60_000 - 1)
            self.assertFalse(self.s.off)
            self.update(100 + minutes * 60_000)
            self.assertTrue(self.s.off)

    def test_activity_restarts_the_whole_interval_and_consumes_wake(self):
        self.configure(30)
        self.assertFalse(self.activity(1_700_000))
        self.update(1_800_100)
        self.assertFalse(self.s.off)
        self.update(3_500_000)
        self.assertTrue(self.s.off)
        self.assertTrue(self.activity(3_500_001))
        self.assertFalse(self.s.off)
        self.assertFalse(self.activity(3_500_002))

    def test_playing_cancels_countdown_and_pause_starts_fresh(self):
        self.configure(30)
        self.update(100_000_000, playing=True)
        self.assertFalse(self.s.off)
        self.update(100_100_000)
        self.update(101_899_999)
        self.assertFalse(self.s.off)
        self.update(101_900_000)
        self.assertTrue(self.s.off)
        self.update(101_900_001, playing=True)
        self.assertFalse(self.s.off)

    def test_manual_blank_stays_off_during_playback_until_input(self):
        self.configure(0)
        self.lib.screen_idle_off(C.byref(self.s))
        self.update(500, playing=True)
        self.update(1500, playing=True)
        self.assertTrue(self.s.off)
        self.assertTrue(self.activity(1600))
        self.assertFalse(self.s.manual)

    def test_monotonic_clock_wrap(self):
        start = 0xfffffff0
        self.configure(30, start)
        self.update((start + 1_799_999) & 0xffffffff)
        self.assertFalse(self.s.off)
        self.update((start + 1_800_000) & 0xffffffff)
        self.assertTrue(self.s.off)

    def test_changing_timeout_restarts_countdown(self):
        self.configure(30)
        self.configure(60, 1_799_000)
        self.update(1_800_100)
        self.assertFalse(self.s.off)
        self.update(5_399_000)
        self.assertTrue(self.s.off)


if __name__ == '__main__':
    unittest.main()
