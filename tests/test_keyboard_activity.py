"""Keyboard idle activity must include unmapped keys, excluding held reports."""
import ctypes as C
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class KeyboardActivityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT / 'firmware/main/usb_hid_input.c').read_text()
        production = source[source.index('static void keyboard_report'):source.index('static void hid_interface_cb')]
        boundary = '''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define HID_KEYBOARD_KEY_MAX 6
#define D100H_INPUT_USB 0
#define KEY_EVENT_ACTIVITY 4
typedef int ui_key_t;
typedef struct { struct { uint8_t val; } modifier; uint8_t reserved; uint8_t key[6]; } hid_keyboard_input_report_boot_t;
static uint8_t s_prev_keys[6],s_prev_modifiers;
static int activities,keys;
static bool hid_key_to_ui(uint8_t code,ui_key_t *key) { *key=code; return code==0x28; }
static void usb_hid_input_post_key(ui_key_t key) { (void)key; keys++; }
static void post_gesture_event(int source,int kind,int id,bool down) {
    (void)source;(void)kind;(void)id;(void)down;activities++;
}
void reset(void) { memset(s_prev_keys,0,6);s_prev_modifiers=0;activities=keys=0; }
int activity_count(void) { return activities; }
int key_count(void) { return keys; }
'''
        work = Path(cls.tmp.name)
        (work / 'keyboard.c').write_text(boundary + production + '''
void report(uint8_t mods,uint8_t first,uint8_t second) {
    uint8_t data[8]={mods,0,first,second};keyboard_report(data,8);
}
''')
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared',
                        '-fPIC', str(work / 'keyboard.c'), '-o', str(work / 'keyboard.so')], check=True)
        cls.lib = C.CDLL(str(work / 'keyboard.so'))
        cls.lib.report.argtypes = [C.c_uint8, C.c_uint8, C.c_uint8]

    def setUp(self):
        self.lib.reset()

    def test_unmapped_key_wakes_without_held_reports_resetting_idle(self):
        self.lib.report(0, 4, 0)
        self.lib.report(0, 4, 0)
        self.assertEqual(self.lib.activity_count(), 1)
        self.assertEqual(self.lib.key_count(), 0)
        self.lib.report(0, 0, 0)
        self.lib.report(0, 4, 0)
        self.assertEqual(self.lib.activity_count(), 2)

    def test_modifier_only_presses_are_activity(self):
        self.lib.report(2, 0, 0)
        self.lib.report(2, 0, 0)
        self.assertEqual(self.lib.activity_count(), 1)
        self.lib.report(0, 0, 0)
        self.lib.report(2, 0, 0)
        self.assertEqual(self.lib.activity_count(), 2)

    def test_mapped_key_with_modifiers_is_one_event_for_wake_consumption(self):
        self.lib.report(2, 0x28, 4)
        self.assertEqual(self.lib.key_count(), 1)
        self.assertEqual(self.lib.activity_count(), 0)

    def test_empty_reports_do_not_keep_device_awake(self):
        for _ in range(10):
            self.lib.report(0, 0, 0)
        self.assertEqual(self.lib.activity_count(), 0)
        self.assertEqual(self.lib.key_count(), 0)


if __name__ == '__main__':
    unittest.main()
