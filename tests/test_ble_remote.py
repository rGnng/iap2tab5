"""Exercise production ESP BLE callbacks against fake GAP/GATT and a clock."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BleRemoteTests(unittest.TestCase):
    def test_lifecycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work/'lvgl.h').write_text('typedef struct lv_indev_t lv_indev_t;\n')
            source = (ROOT/'app/backend/ble_remote.c').read_text()
            source = source.split('#else /* !ESP_PLATFORM: simulator stub */')[0]
            source = source.replace('#if defined(ESP_PLATFORM)', '')
            source = re.sub(r'^#include .*$', '', source, flags=re.M)
            (work/'backend_under_test.c').write_text(source)
            binary = work/'ble_test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra',
                            '-Wno-unused-parameter', '-Wno-unused-function',
                            '-Wno-unused-variable', '-Wno-unused-value',
                            '-I'+str(work), '-I'+str(ROOT/'app/backend'),
                            '-I'+str(ROOT/'app/ui'), '-I'+str(ROOT/'firmware/main'),
                            str(ROOT/'tests/ble_remote_harness.c'),
                            str(ROOT/'firmware/main/ulanzi_d100h.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    unittest.main()
