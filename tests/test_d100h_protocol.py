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


class TopRightState(ctypes.Structure):
    _fields_ = [('down', ctypes.c_bool), ('fired', ctypes.c_bool),
                ('pressed_us', ctypes.c_int64)]


# Public gesture event/action enums from ulanzi_d100h.h.
DOWN, UP, POLL, CANCEL = range(4)
NONE, CONTEXT, STAR = range(3)


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
        cls.lib.ulanzi_d100h_encode_ble.argtypes = [
            ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t,
            ctypes.POINTER(ctypes.c_uint8)]
        cls.lib.ulanzi_d100h_encode_ble.restype = None
        cls.lib.ulanzi_top_right_update.argtypes = [
            ctypes.POINTER(TopRightState), ctypes.c_int, ctypes.c_int64]
        cls.lib.ulanzi_top_right_update.restype = ctypes.c_int
        cls.lib.ulanzi_top_right_deadline.argtypes = [ctypes.POINTER(TopRightState)]
        cls.lib.ulanzi_top_right_deadline.restype = ctypes.c_int64

    def gesture(self, state, event, now_us):
        return self.lib.ulanzi_top_right_update(ctypes.byref(state), event, now_us)

    def test_top_right_fires_at_threshold_without_release(self):
        state = TopRightState()
        self.assertEqual(self.gesture(state, DOWN, 1000000), NONE)
        self.assertEqual(self.lib.ulanzi_top_right_deadline(ctypes.byref(state)), 1600000)
        self.assertEqual(self.gesture(state, POLL, 1599999), NONE)
        self.assertEqual(self.gesture(state, POLL, 1600000), STAR)
        self.assertTrue(state.down)
        self.assertEqual(self.lib.ulanzi_top_right_deadline(ctypes.byref(state)), 0)
        self.assertEqual(self.gesture(state, POLL, 9000000), NONE)
        self.assertEqual(self.gesture(state, UP, 9000001), NONE)
        self.assertFalse(state.down)

    def test_top_right_short_tap_and_unmatched_release(self):
        state = TopRightState()
        self.assertEqual(self.gesture(state, UP, 0), NONE)
        self.assertEqual(self.gesture(state, DOWN, 0), NONE)
        self.assertEqual(self.gesture(state, UP, 599999), CONTEXT)
        self.assertEqual(self.gesture(state, POLL, 600000), NONE)
        self.assertEqual(self.gesture(state, UP, 700000), NONE)

    def test_top_right_repeat_down_does_not_restart_or_repeat(self):
        state = TopRightState()
        self.gesture(state, DOWN, 0)
        self.assertEqual(self.gesture(state, DOWN, 500000), NONE)
        self.assertEqual(self.gesture(state, POLL, 600000), STAR)
        self.assertEqual(self.gesture(state, DOWN, 900000), NONE)
        self.assertEqual(self.gesture(state, POLL, 1500000), NONE)
        self.assertEqual(self.gesture(state, UP, 1600000), NONE)

    def test_top_right_release_at_threshold_when_dispatch_is_delayed(self):
        for release_us in (600000, 800000):
            state = TopRightState()
            self.gesture(state, DOWN, 0)
            self.assertEqual(self.gesture(state, UP, release_us), STAR)
            self.assertEqual(self.gesture(state, POLL, 1000000), NONE)

    def test_top_right_cancel_and_separate_input_sources(self):
        usb, ble = TopRightState(), TopRightState()
        self.gesture(usb, DOWN, 0)
        self.gesture(ble, DOWN, 0)
        self.assertEqual(self.gesture(usb, CANCEL, 500000), NONE)
        self.assertEqual(self.gesture(usb, POLL, 600000), NONE)
        self.assertEqual(self.gesture(usb, UP, 700000), NONE)
        self.assertEqual(self.gesture(ble, POLL, 600000), STAR)
        self.gesture(usb, DOWN, 1000000)
        self.assertEqual(self.gesture(usb, POLL, 1600000), STAR)

    def test_top_right_next_hold_can_toggle_again(self):
        state = TopRightState()
        self.gesture(state, DOWN, 0)
        self.assertEqual(self.gesture(state, POLL, 600000), STAR)
        self.assertEqual(self.gesture(state, UP, 700000), NONE)
        self.gesture(state, DOWN, 800000)
        self.assertEqual(self.gesture(state, POLL, 1400000), STAR)
        self.assertEqual(self.gesture(state, UP, 1500000), NONE)

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

    def test_ble_heartbeat_vector(self):
        plain = bytes.fromhex('0601230001')
        src = (ctypes.c_uint8 * len(plain)).from_buffer_copy(plain)
        out = (ctypes.c_uint8 * 20)()
        self.lib.ulanzi_d100h_encode_ble(src, len(plain), out)
        self.assertEqual(bytes(out), bytes.fromhex(
            '24569ef228e145a1') + ZERO_BLOCK + bytes(4))

    def test_ble_encode_decode_roundtrip(self):
        for code, down in [(1, True), (7, True), (10, True), (3, False)]:
            plain = bytes([0x8b, 0x10, code, 1 if down else 0]) + bytes(16)
            src = (ctypes.c_uint8 * 20).from_buffer_copy(plain)
            out = (ctypes.c_uint8 * 20)()
            self.lib.ulanzi_d100h_encode_ble(src, 20, out)
            self.assertEqual(
                self.decode('ulanzi_d100h_decode_ble', bytes(out)),
                (True, code, down))

    def test_wired_heartbeat_vector(self):
        packet = (ctypes.c_uint8 * 64)()
        self.lib.ulanzi_d100h_heartbeat(packet)
        self.assertEqual(bytes(packet), bytes.fromhex('24569ef228e145a1') + ZERO_BLOCK * 7)


if __name__ == '__main__':
    unittest.main()
