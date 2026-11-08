"""Exercise the production tab transition callbacks with a fake display/timer."""
import ctypes
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^static [^\n]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(name)
    depth, pos = 1, match.end()
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


class TabLoadingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        ui = (ROOT / 'app/ui/ui.c').read_text()
        header = (ROOT / 'app/ui/ui.h').read_text()
        keys = re.search(r'typedef enum \{.*?\} ui_key_t;', header, re.S).group()
        screens = re.search(r'typedef enum \{.*?\} screen_id_t;', ui, re.S).group()
        limit = re.search(r'^#define PAGE_LOADING_KEYS .*$', ui, re.M).group()
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define LV_UNUSED(x) (void)(x)
#define LV_PCT(x) (x)
#define MINI_NOW_H 72
#define SCREEN_H 720
#define UI_PERF_LOG(...) do { if(0) printf(__VA_ARGS__); } while(0)
'''
        boundary = r'''
typedef int lv_obj_t;
typedef int lv_event_t;
typedef struct { bool paused; int resets; } lv_timer_t;
typedef struct { void (*create)(lv_obj_t *); void (*enter)(void);void (*same_key)(void); } page_t;
static page_t g_pages[SCR_COUNT];
static lv_obj_t page,mini;
static lv_obj_t *g_page=&page,*g_mini=&mini;
static bool g_page_loading,g_page_wait_frame,g_tab_frame_pending,g_tab_content_frame_pending;
static bool g_afc_sync_mode,g_ctx_menu_open,g_ui_trace,fail_timer,create_detail;
static bool g_key_feedback_scope,g_key_feedback_sent;
static int silent_dispatches;
static screen_id_t g_current_screen,g_page_loading_screen,g_ble_return_screen;
static lv_timer_t *g_page_load_timer,*g_page_detail_timer;
static lv_timer_t timer,detail;
static uint32_t g_tab_start_us,tick;
static ui_key_t g_page_loading_keys[PAGE_LOADING_KEYS];
static int g_page_loading_key_count;
static int builds[SCR_COUNT],enters[SCR_COUNT],same_calls,shells,cleans,mini_updates;
static int received[PAGE_LOADING_KEYS],received_count;
static uint32_t perf_now(void) { return ++tick; }
static uint32_t lv_tick_get(void) { return tick/1000; }
static void lv_timer_pause(lv_timer_t *t) { t->paused=true; }
static void lv_timer_reset(lv_timer_t *t) { t->resets++; }
static void lv_timer_resume(lv_timer_t *t) { t->paused=false; }
static lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *),int ms,void *p) {
    (void)cb;(void)ms;(void)p;return fail_timer?NULL:&timer;
}
static void ui_mini_update(void) { mini_updates++; }
static void lv_obj_set_hidden(lv_obj_t *o,bool h) { (void)o;(void)h; }
static void lv_obj_set_pos(lv_obj_t *o,int x,int y) { (void)o;(void)x;(void)y; }
static void lv_obj_set_size(lv_obj_t *o,int x,int y) { (void)o;(void)x;(void)y; }
static void lv_obj_clean(lv_obj_t *o) { (void)o;cleans++; }
static void ui_page_cancel_load(void);
static void ui_show_screen(screen_id_t id);
static void ui_page_teardown(void) { ui_page_cancel_load(); }
static void ui_page_loading_shell(void) { shells++; }
static void ui_exit_edit(void) {}
static void focus_clear(void) {}
static void ui_set_hint(const char *s) { (void)s; }
static void ui_menu_update(void) {}
static void context_menu_close(void) { g_ctx_menu_open=false; }
static void ble_remote_scan_stop(void) {}
static void ui_dispatch_key(ui_key_t key) {
    received[received_count++]=key;
    if(g_key_feedback_scope && g_key_feedback_sent)silent_dispatches++;
}
static void create(lv_obj_t *o) {
    (void)o;builds[g_current_screen]++;
    if(create_detail) { g_page_detail_timer=&detail;detail.paused=false; }
}
static void enter(void) { enters[g_current_screen]++; }
static void same(void) { same_calls++; }
'''
        exports = r'''
void reset(void) {
    memset(builds,0,sizeof(builds));memset(enters,0,sizeof(enters));
    g_current_screen=SCR_NOW_PLAYING;g_ble_return_screen=g_page_loading_screen=SCR_COUNT;
    g_page_loading=g_page_wait_frame=g_tab_frame_pending=g_tab_content_frame_pending=false;
    g_afc_sync_mode=g_ctx_menu_open=g_ui_trace=fail_timer=create_detail=false;
    g_key_feedback_scope=g_key_feedback_sent=false;silent_dispatches=0;
    g_page_load_timer=g_page_detail_timer=NULL;
    timer=(lv_timer_t){.paused=true};detail=(lv_timer_t){.paused=true};
    g_page_loading_key_count=received_count=same_calls=shells=cleans=mini_updates=0;
    for(int i=0;i<SCR_COUNT;i++)g_pages[i]=(page_t){create,enter,same};
}
void show(int id) { ui_show_screen((screen_id_t)id); }
void frame(void) { ui_tab_refresh_cb(NULL); }
void run(void) { if(!timer.paused)ui_page_load_cb(&timer); }
void cancel(void) { ui_page_cancel_load(); }
void rebuild(void) { ui_rebuild_page(); }
int defer(int key) { return ui_page_defer_key((ui_key_t)key); }
int built(int id) { return builds[id]; }
int entered(int id) { return enters[id]; }
int key_at(int i) { return received[i]; }
void option(int n) { if(n==0)fail_timer=true;else if(n==1)create_detail=true;else g_afc_sync_mode=true; }
int state(int n) {
    switch(n) {case 0:return g_page_loading;case 1:return timer.paused;case 2:return g_page_wait_frame;
    case 3:return shells;case 4:return g_page_loading_key_count;case 5:return received_count;
    case 6:return detail.paused;case 7:return mini_updates;case 8:return same_calls;
    case 9:return g_current_screen;case 10:return silent_dispatches;
    case 11:return g_key_feedback_scope;case 12:return g_key_feedback_sent;default:return -1;}
}
'''
        names = ('ui_layout_page_area', 'ui_page_cancel_load', 'ui_page_load_cb',
                 'ui_page_defer_key', 'ui_tab_refresh_cb', 'ui_show_screen', 'ui_rebuild_page')
        source = prelude+keys+'\n'+screens+'\n'+limit+'\n'+boundary
        source += '\n'.join(function(ui, n) for n in names)+exports
        c, so = Path(cls.tmp.name)/'tab.c', Path(cls.tmp.name)/'tab.so'
        c.write_text(source)
        result = subprocess.run(['cc','-std=c11','-O2','-shared','-fPIC','-Wall','-Werror',
                                 str(c),'-o',str(so)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.lib=ctypes.CDLL(str(so))
        body = keys[keys.index('{')+1:keys.index('}')]
        cls.keys = [v.strip().split('=')[0].strip() for v in body.split(',') if v.strip()]

    def setUp(self):
        self.lib.reset()

    def key(self, name):
        return self.keys.index('UI_KEY_'+name)

    def test_shell_paints_before_page_and_mini_artwork_work(self):
        self.lib.show(1)
        self.assertTrue(self.lib.state(0));self.assertTrue(self.lib.state(1))
        self.assertEqual(self.lib.built(1),0)
        self.assertEqual(self.lib.state(7),0)
        self.lib.run()
        self.assertEqual(self.lib.built(1),0)
        self.lib.frame();self.lib.run()
        self.assertEqual(self.lib.built(1),1)
        self.assertFalse(self.lib.state(0));self.assertTrue(self.lib.state(1))
        self.assertEqual(self.lib.state(7),1)
        self.assertEqual(self.lib.entered(1),1)

    def test_rapid_tabs_cancel_previous_build_and_queued_input(self):
        self.lib.show(1);self.lib.defer(self.key('ENTER'));self.lib.frame()
        self.lib.show(2);self.lib.run()
        self.assertEqual(self.lib.built(1),0);self.assertEqual(self.lib.built(2),0)
        self.assertEqual(self.lib.state(4),0)
        self.lib.frame();self.lib.run()
        self.assertEqual(self.lib.built(2),1);self.assertEqual(self.lib.state(5),0)

    def test_rotary_and_enter_are_replayed_in_order_after_build(self):
        self.lib.show(1)
        values=[self.key(n) for n in ('DOWN','DOWN','ENTER','UP')]
        for key in values:self.assertTrue(self.lib.defer(key))
        self.assertEqual(self.lib.state(5),0)
        self.lib.frame();self.lib.run()
        self.assertEqual([self.lib.key_at(i) for i in range(4)],values)
        self.assertEqual(self.lib.state(4),0)

    def test_deferred_keys_do_not_repeat_the_arrival_feedback(self):
        self.lib.show(1);self.lib.defer(self.key('ENTER'))
        self.lib.frame();self.lib.run()
        self.assertEqual(self.lib.state(10),1)
        self.assertFalse(self.lib.state(11));self.assertFalse(self.lib.state(12))

    def test_preview_waits_for_content_frame(self):
        self.lib.option(1);self.lib.show(1);self.lib.frame();self.lib.run()
        self.assertTrue(self.lib.state(6))
        self.lib.frame()
        self.assertFalse(self.lib.state(6))

    def test_switch_before_content_frame_cancels_preview(self):
        self.lib.option(1);self.lib.show(1);self.lib.frame();self.lib.run()
        self.lib.show(2)
        self.assertTrue(self.lib.state(6))
        self.lib.frame()
        self.assertTrue(self.lib.state(6))

    def test_media_and_tab_navigation_are_not_queued(self):
        self.lib.show(1)
        for key in ('7','NEXT_TRACK','PREV_TRACK','SCREEN_NEXT','SCREEN_PREV','3','BACK'):
            self.assertFalse(self.lib.defer(self.key(key)), key)
        self.assertEqual(self.lib.state(4),0)

    def test_same_tab_while_loading_does_not_touch_unbuilt_widgets(self):
        self.lib.show(1);self.lib.show(1)
        self.assertEqual(self.lib.state(8),0);self.assertEqual(self.lib.state(3),1)
        self.lib.frame();self.lib.run();self.lib.show(1)
        self.assertEqual(self.lib.state(8),1)

    def test_theme_rebuild_while_loading_preserves_input_and_frame_gate(self):
        self.lib.show(1);self.lib.defer(self.key('DOWN'));self.lib.frame();self.lib.rebuild()
        self.assertEqual(self.lib.state(4),1);self.assertTrue(self.lib.state(1))
        self.lib.run();self.assertEqual(self.lib.built(1),0)
        self.lib.frame();self.lib.run()
        self.assertEqual(self.lib.built(1),1);self.assertEqual(self.lib.state(5),1)

    def test_cancel_and_timer_allocation_failure(self):
        self.lib.show(1);self.lib.frame();self.lib.cancel();self.lib.run()
        self.assertEqual(self.lib.built(1),0);self.assertFalse(self.lib.state(0))
        self.lib.reset();self.lib.option(0);self.lib.show(1)
        self.assertEqual(self.lib.built(1),1);self.assertFalse(self.lib.state(0))

    def test_invalid_and_afc_tabs_are_ignored(self):
        self.lib.show(-1);self.lib.show(99)
        self.assertEqual(self.lib.state(3),0)
        self.lib.option(2);self.lib.show(1)
        self.assertEqual(self.lib.state(3),0)

    def test_now_playing_opens_directly_without_a_loading_shell(self):
        self.lib.show(1)
        shells=self.lib.state(3)
        self.lib.show(0)
        self.assertEqual(self.lib.built(0),1)
        self.assertEqual(self.lib.state(3),shells)
        self.assertFalse(self.lib.state(0));self.assertTrue(self.lib.state(1))
        self.lib.frame();self.lib.run()
        self.assertEqual(self.lib.built(1),0)

    def test_deferred_input_is_bounded(self):
        self.lib.show(1)
        for _ in range(200):self.lib.defer(self.key('DOWN'))
        self.assertEqual(self.lib.state(4),64)
        self.lib.frame();self.lib.run()
        self.assertEqual(self.lib.state(5),64)
