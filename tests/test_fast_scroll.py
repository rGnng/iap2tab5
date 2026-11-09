"""Exercise production scroll speed policy against physical detents and time."""
import ctypes as C
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class State(C.Structure):
    _fields_ = [('ticks', C.c_uint32 * 128), ('last_ms', C.c_uint32),
                ('head', C.c_uint), ('count', C.c_uint), ('direction', C.c_int),
                ('rows', C.c_int), ('pages', C.c_int), ('trigger_window_ms', C.c_int),
                ('hold_ms',C.c_int), ('active', C.c_bool)]

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
            C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.c_int, C.POINTER(C.c_bool)]
        cls.lib.fast_scroll_feed.restype = C.c_int
        cls.lib.fast_scroll_expire.argtypes = [C.POINTER(State), C.c_uint32]
        cls.lib.fast_scroll_expire.restype = C.c_bool

    def setUp(self): self.s = State()
    def feed(self, now, count=1, rows=8, pages=2, direction=1, hold=150, trigger=550):
        entered = C.c_bool()
        steps = self.lib.fast_scroll_feed(C.byref(self.s), now, direction,
            count, rows, pages, trigger, hold, C.byref(entered))
        return steps, entered.value

    def test_each_threshold_uses_physical_notches_then_one_page_per_notch(self):
        for pages in range(1, 6):
            self.s = State()
            for i in range(pages * 8 - 1): self.assertEqual(self.feed(i*10,pages=pages),(1,False))
            self.assertEqual(self.feed(pages*8*10,pages=pages),(8,True))
            self.assertEqual(self.feed(pages*8*10+10,pages=pages),(8,False))

    def test_slow_and_sparse_input_never_accelerates(self):
        for gap in (160, 250, 500, 1200):
            self.s = State()
            for i in range(100): self.assertEqual(self.feed(i*gap),(1,False))

    def test_window_excludes_old_notches(self):
        for trigger in (550,800,1200):
            self.s=State()
            self.assertEqual(self.feed(0,15,trigger=trigger,hold=1500),(15,False))
            self.assertEqual(self.feed(trigger-1,trigger=trigger,hold=1500),(8,True))
            self.s=State()
            self.feed(0,15,trigger=trigger,hold=1500)
            self.assertEqual(self.feed(trigger,trigger=trigger,hold=1500),(1,False))
            self.assertFalse(self.s.active)
            self.assertEqual(self.s.count,1)

    def test_same_spin_only_triggers_with_a_long_enough_window(self):
        for trigger,expected in ((550,False),(800,True),(1200,True)):
            self.s=State()
            for i in range(16): self.feed(i*45,trigger=trigger)
            self.assertEqual(self.s.active,expected)

    def test_invalid_trigger_times_use_550_ms_default(self):
        for trigger in (0,-1,150,549,551,801,1201):
            self.s=State()
            self.feed(0,trigger=trigger)
            self.assertEqual(self.s.trigger_window_ms,550)

    def test_changing_trigger_window_discards_old_velocity(self):
        self.feed(0,15,trigger=1200)
        self.assertEqual(self.feed(10,trigger=550),(1,False))
        self.assertEqual(self.s.count,1)
        self.feed(20,15,trigger=550)
        self.assertTrue(self.s.active)
        self.assertEqual(self.feed(30,trigger=800),(1,False))
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
            self.assertEqual(self.s.hold_ms,300)
            self.assertTrue(self.lib.fast_scroll_expire(C.byref(self.s),400))

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
        for i in range(15):self.assertEqual(self.feed((start+i*35) % 2**32),(1,False))
        self.assertEqual(self.feed((start+525) % 2**32),(8,True))
        self.assertTrue(self.lib.fast_scroll_expire(C.byref(self.s),(start+675) % 2**32))

class FastScrollConfigTests(unittest.TestCase):
    """Load the production JSON parsing block, including old Off settings."""
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT/'firmware/main/app_config.c').read_text()
        start = source.index('    item = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_pages");')
        end = source.index('    item = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_hold_ms");', start)
        defaults = source[source.index('static void set_defaults(void)'):source.index('static void load_from_json')]
        save_start = source.index('    cJSON_AddBoolToObject(root, "fast_scroll_enabled",')
        save_end = source.index('    cJSON_AddNumberToObject(root, "brightness",',save_start)
        web = (ROOT/'firmware/main/web_server.c').read_text()
        web_start = web.index('    const cJSON * enabled = cJSON_GetObjectItemCaseSensitive(root, "fast_scroll_enabled");')
        web_end = web.index('    item = cJSON_GetObjectItemCaseSensitive(root, "screen_off_minutes");',web_start)
        (Path(cls.tmp.name)/'esp_err.h').write_text('typedef int esp_err_t;\n')
        harness = Path(cls.tmp.name)/'config.c'
        harness.write_text('''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "app_config.h"
#include "fast_scroll.h"
app_config_t g_app_config;
''' + defaults + '''
unsigned load_settings(const char *text) {
    set_defaults();
    cJSON *root=cJSON_Parse(text);const cJSON *item;
''' + source[start:end] + '''
    cJSON_Delete(root);
    return (g_app_config.fast_scroll_enabled ? 256 : 0) | g_app_config.fast_scroll_pages;
}
int trigger_time(void) { return g_app_config.fast_scroll_trigger_window_ms; }
const char *save_settings(void) {
    static char saved[2048];
    cJSON *root=cJSON_CreateObject();
''' + source[save_start:save_end] + '''
    char *text=cJSON_PrintUnformatted(root);
    snprintf(saved,sizeof(saved),"%s",text);
    cJSON_free(text);cJSON_Delete(root);return saved;
}
#define HTTPD_400_BAD_REQUEST 400
static int httpd_resp_send_err(void *req,int code,const char *message) {
    (void)req;(void)message;return code;
}
int request_settings(const char *text) {
    set_defaults();void *req=NULL;
    cJSON *root=cJSON_Parse(text);
''' + web[web_start:web_end] + '''
    cJSON_Delete(root);return 200;
}
''')
        lib = Path(cls.tmp.name)/'config.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        '-I'+cls.tmp.name,'-I'+str(ROOT/'firmware/main'),
                        '-I'+str(ROOT/'app/backend'),'-I'+str(ROOT/'third_party/cjson'),str(harness),
                        str(ROOT/'app/backend/fast_scroll.c'),
                        str(ROOT/'third_party/cjson/cJSON.c'),'-o',str(lib)],check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.load_settings.argtypes = [C.c_char_p]
        cls.lib.load_settings.restype = C.c_uint
        cls.lib.save_settings.restype = C.c_char_p
        cls.lib.request_settings.argtypes = [C.c_char_p]

    def load(self, **settings):
        result = self.lib.load_settings(json.dumps(settings).encode())
        return bool(result & 256), result & 255

    def test_missing_enable_flag_keeps_enabled_default_and_legacy_threshold(self):
        self.assertEqual(self.load(), (True,2))
        for pages in range(1,6):
            self.assertEqual(self.load(fast_scroll_pages=pages), (True,pages))

    def test_legacy_off_migrates_to_disabled_with_a_valid_threshold(self):
        self.assertEqual(self.load(fast_scroll_pages=0), (False,2))

    def test_explicit_enable_flag_preserves_independent_threshold(self):
        for enabled in (False,True):
            for pages in range(1,6):
                self.assertEqual(self.load(fast_scroll_enabled=enabled,fast_scroll_pages=pages),
                                 (enabled,pages))
        self.assertEqual(self.load(fast_scroll_enabled=True,fast_scroll_pages=0), (True,2))

    def test_invalid_saved_threshold_does_not_disable_fast_scroll(self):
        for pages in (-1,6,1.5,'2',False):
            self.assertEqual(self.load(fast_scroll_pages=pages), (True,2))

    def test_missing_or_invalid_saved_trigger_defaults_to_550(self):
        self.load()
        self.assertEqual(self.lib.trigger_time(),550)
        for value in (-1,0,300,551,1201,550.5,'800',True,None):
            self.load(fast_scroll_trigger_window_ms=value)
            self.assertEqual(self.lib.trigger_time(),550)

    def test_trigger_choice_survives_save_reload_while_disabled(self):
        for value in (550,800,1200):
            self.load(fast_scroll_enabled=False,fast_scroll_trigger_window_ms=value)
            saved=json.loads(self.lib.save_settings())
            self.assertEqual(saved['fast_scroll_trigger_window_ms'],value)
            self.assertEqual(self.load(**saved),(False,2))
            self.assertEqual(self.lib.trigger_time(),value)

    def test_web_config_accepts_only_offered_trigger_times(self):
        for value in (550,800,1200):
            body=json.dumps(dict(fast_scroll_trigger_window_ms=value)).encode()
            self.assertEqual(self.lib.request_settings(body),200)
            self.assertEqual(self.lib.trigger_time(),value)
        for value in (-1,0,300,551,1201,550.5,'800',True,None):
            body=json.dumps(dict(fast_scroll_trigger_window_ms=value)).encode()
            self.assertEqual(self.lib.request_settings(body),400)
            self.assertEqual(self.lib.trigger_time(),550)


if __name__ == '__main__': unittest.main()
