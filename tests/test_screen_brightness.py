"""Configured brightness stays visible; intentional screen blanking is separate."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ScreenBrightnessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        path = Path(cls.tmp.name)
        (path / 'brightness.c').write_text(
            '#include "screen_brightness.h"\n'
            'int normalize(int value) { return screen_brightness_normalize(value); }\n')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-I', str(ROOT / 'app/backend'),
                        str(path / 'brightness.c'), '-o', str(path / 'brightness.so')], check=True)
        cls.lib = ctypes.CDLL(str(path / 'brightness.so'))
        cls.lib.normalize.argtypes = [ctypes.c_int]

    def test_zero_and_negative_settings_recover_to_low(self):
        for value in (0, -1, -100, -(2**31)):
            self.assertEqual(self.lib.normalize(value), 45)

    def test_custom_positive_levels_and_presets_are_preserved(self):
        for value in range(1, 101):
            self.assertEqual(self.lib.normalize(value), value)

    def test_oversized_settings_are_capped_at_full_brightness(self):
        for value in (101, 256, 2**31 - 1):
            self.assertEqual(self.lib.normalize(value), 100)


if __name__ == '__main__':
    unittest.main()
