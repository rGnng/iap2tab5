"""Compile the production D100H codec on the host; no ESP-IDF required.

Run: python3 -m unittest discover -s tests -v
The LVGL forward declaration replaces only the unrelated UI library dependency.
"""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ZERO_BLOCK = bytes.fromhex('3890c499a360aaad')


class D100HProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if compiler is None:
            raise RuntimeError('A C compiler is required to test the production codec')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        (work / 'lvgl.h').write_text('typedef struct lv_indev_t lv_indev_t;\n')
        library = work / 'd100h.so'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', '-I' + str(work),
                        '-I' + str(ROOT / 'app/ui'),
                        str(ROOT / 'firmware/main/ulanzi_d100h.c'),
                        '-o', str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        for name in ('ulanzi_d100h_decode', 'ulanzi_d100h_decode_ble'):
            decoder = getattr(cls.lib, name)
            decoder.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
                                ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_bool)]
            decoder.restype = ctypes.c_bool
        cls.lib.ulanzi_d100h_heartbeat.argtypes = [ctypes.POINTER(ctypes.c_uint8)]
        cls.lib.ulanzi_d100h_heartbeat.restype = None

    def decode(self, name, wire):
        data = (ctypes.c_uint8 * len(wire)).from_buffer_copy(wire)
        control, down = ctypes.c_uint8(), ctypes.c_bool()
        ok = getattr(self.lib, name)(data, len(wire), ctypes.byref(control), ctypes.byref(down))
        return ok, control.value, down.value

    def test_captured_ble_vectors(self):
        for code, down in [('8e8f5e16ec67281e', True), ('028db3660224d0c7', False)]:
            wire = bytes.fromhex(code) + ZERO_BLOCK + bytes(4)
            self.assertEqual(self.decode('ulanzi_d100h_decode_ble', wire), (True, 1, down))

    def test_hid_preserves_64_byte_frame(self):
        # Captured first-block code, with either encrypted zero padding or an
        # opaque HID tail. These assembled fixtures are not live BLE captures.
        code = bytes.fromhex('8e8f5e16ec67281e')
        for tail in (ZERO_BLOCK * 7, ZERO_BLOCK + bytes(48), bytes(range(56))):
            wire = code + tail
            self.assertEqual(self.decode('ulanzi_d100h_decode', wire), (True, 1, True))
        # Reproduces the old receive path: first 20 bytes of a fully encrypted
        # HID report must not be treated as a 20-byte FD00 notification.
        wire = code + ZERO_BLOCK * 7
        self.assertFalse(self.decode('ulanzi_d100h_decode_ble', wire[:20])[0])
        self.assertFalse(self.decode('ulanzi_d100h_decode_ble', wire)[0])

    def test_rejects_ble_trailing_bytes(self):
        wire = bytes.fromhex('8e8f5e16ec67281e') + ZERO_BLOCK + bytes(5)
        self.assertFalse(self.decode('ulanzi_d100h_decode_ble', wire)[0])

    def test_wired_heartbeat_vector(self):
        packet = (ctypes.c_uint8 * 64)()
        self.lib.ulanzi_d100h_heartbeat(packet)
        self.assertEqual(bytes(packet), bytes.fromhex('24569ef228e145a1') + ZERO_BLOCK * 7)


if __name__ == '__main__':
    unittest.main()
