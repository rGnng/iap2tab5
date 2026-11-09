"""Production PID lookup and deferred queue fill, exercised without hardware."""
import ctypes
from pathlib import Path
import random
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


class QueuePopupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        player = (ROOT / 'app/backend/mock_player.c').read_text()
        ui = (ROOT / 'app/ui/ui.c').read_text()
        lookup = player[player.index('#define TRACK_ID_SLOTS'):player.index('/* iAP2 tags', player.index('#define TRACK_ID_SLOTS'))]
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MAX_TRACKS 24000
#define MOCK_LARGE_BSS
#define LV_UNUSED(x) (void)(x)
#define UI_PERF_LOG(...) do { if(0) printf(__VA_ARGS__); } while(0)
#define NP_QUEUE_ROW_H 56
#define SCR_NOW_PLAYING 0
#define LV_OPA_TRANSP 0
static bool g_ui_trace;
static bool song_order_ready;
typedef struct { uint64_t id; char metadata[320]; } player_track_t;
static player_track_t tracks[MAX_TRACKS];
static int track_count;
'''
        boundary = r'''
typedef struct { int height, index; bool hidden; } lv_obj_t;
typedef struct { bool paused; } lv_timer_t;
typedef int lv_event_t;
typedef struct { lv_obj_t *obj; } focus_t;
static focus_t g_focus[128];
static lv_obj_t list, loading, spacer, rows[128];
static lv_timer_t timer;
static lv_obj_t *np_queue_list, *np_queue_loading, *np_queue_spacer;
static lv_timer_t *np_queue_fill_timer;
static bool np_queue_visible, np_queue_wait_frame, np_toggle_frame_pending;
static int g_current_screen, g_focus_count, np_queue_focus_start;
static int np_queue_fill_next, np_queue_fill_end, np_queue_fill_current;
static uint32_t np_queue_fill_rev, np_queue_fill_start_us;
static uint32_t np_queue_lookup_us, np_queue_widget_us, np_toggle_start_us;
static uint32_t tick, rev, row_cost;
static int current, row_calls, rebuilds, focused;
static bool fail_rows;
static int visited[128];
static uint32_t perf_now(void) { return tick; }
static void perf_log_us(const char *n, uint32_t t) { (void)n; (void)t; }
static uint32_t player_queue_revision(void) { return rev; }
static int player_queue_index(void) { return current; }
static void lv_timer_pause(lv_timer_t *t) { t->paused=true; }
static void lv_timer_resume(lv_timer_t *t) { t->paused=false; }
static void lv_timer_reset(lv_timer_t *t) { (void)t; }
static void ui_rebuild_page(void) { rebuilds++; }
static void lv_obj_set_height(lv_obj_t *o, int h) { o->height=h; }
static void lv_obj_set_hidden(lv_obj_t *o, bool h) { o->hidden=h; }
static void lv_obj_move_to_index(lv_obj_t *o, int i) { o->index=i; }
static int lv_obj_get_index(lv_obj_t *o) { return o->index; }
static void lv_label_set_text(lv_obj_t *o, const char *s) { (void)o;(void)s; }
static void focus_select(int i) { focused=i; }
static void np_queue_row(lv_obj_t *p, int i) {
    (void)p; visited[row_calls++]=i; tick+=row_cost;
    if(!fail_rows) { g_focus[g_focus_count].obj=&rows[g_focus_count];g_focus_count++; }
}
'''
        exports = r'''
void reset(void) {
    memset(track_id_slots,0,sizeof(track_id_slots));track_id_index_ready=false;track_count=0;
    g_current_screen=0;g_focus_count=np_queue_focus_start=0;np_queue_list=&list;
    np_queue_loading=&loading;np_queue_spacer=&spacer;np_queue_fill_timer=&timer;
    np_queue_visible=np_queue_wait_frame=true;np_toggle_frame_pending=false;
    np_queue_fill_current=current=7;np_queue_fill_next=8;np_queue_fill_end=22;
    np_queue_fill_rev=rev=1;tick=0;row_cost=100;timer.paused=true;
    row_calls=rebuilds=0;focused=-1;fail_rows=false;loading.hidden=false;spacer.hidden=false;
}
void add(uint64_t id) { tracks[track_count++].id=id; }
void build(void) { track_id_index_build(); }
int lookup_id(uint64_t id) { return track_index_by_id(id); }
int lookup_ptr(uint64_t id) { const player_track_t *p=track_by_id(id);return p?(int)(p-tracks):-1; }
void frame(void) { np_display_refresh_cb(NULL); }
void fill(void) { if(!timer.paused) np_queue_fill_cb(&timer); }
int state(int n) {
    switch(n) {case 0:return timer.paused;case 1:return row_calls;case 2:return g_focus_count;
    case 3:return focused;case 4:return spacer.height;case 5:return rebuilds;
    case 6:return loading.hidden;case 7:return spacer.hidden;default:return -1;}
}
int visit(int i) { return visited[i]; }
void cost(unsigned n) { row_cost=n; }
void change(int n) {
    switch(n) {case 0:rev++;break;case 1:current++;break;case 2:np_queue_visible=false;break;
    case 3:g_current_screen=1;break;case 4:np_queue_list=NULL;break;case 5:fail_rows=true;break;}
}
'''
        constants = '\n'.join(re.findall(r'^#define NP_QUEUE_FILL_(?:BATCH|BUDGET_US) .*$', ui, re.M))
        source = prelude + constants + '\n' + lookup + boundary
        source += function(ui, 'np_display_refresh_cb') + '\n' + function(ui, 'np_queue_fill_cb')
        source += exports
        c = Path(cls.tmp.name) / 'queue.c'
        so = Path(cls.tmp.name) / 'queue.so'
        c.write_text(source)
        result = subprocess.run(['cc', '-std=c11', '-O2', '-shared', '-fPIC', '-Wall', '-Werror',
                                 str(c), '-o', str(so)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        cls.lib = ctypes.CDLL(str(so))
        cls.lib.add.argtypes = [ctypes.c_uint64]
        cls.lib.lookup_id.argtypes = [ctypes.c_uint64]
        cls.lib.lookup_ptr.argtypes = [ctypes.c_uint64]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.lib.reset()

    def test_full_library_lookup_and_missing_ids(self):
        ids = random.Random(17).sample(range(1 << 63, (1 << 63) + 1000000), 24000)
        for key in ids:
            self.lib.add(key)
        self.lib.build()
        for i, key in enumerate(ids):
            self.assertEqual(self.lib.lookup_id(key), i)
        self.assertEqual(self.lib.lookup_id(0), -1)
        self.assertEqual(self.lib.lookup_id((1 << 64) - 1), -1)
        self.assertEqual(self.lib.lookup_ptr(ids[-1]), 23999)

    def test_empty_index_and_duplicate_first_match(self):
        self.lib.build()
        self.assertEqual(self.lib.lookup_id(0), -1)
        self.lib.reset()
        for key in (0, 2**64-1, 9, 9):
            self.lib.add(key)
        self.assertEqual(self.lib.lookup_id(9), 2)  # pre-index fallback
        self.lib.build()
        self.assertEqual(self.lib.lookup_id(0), 0)
        self.assertEqual(self.lib.lookup_id(2**64-1), 1)
        self.assertEqual(self.lib.lookup_id(9), 2)

    def test_fill_waits_for_first_frame_and_runs_in_batches(self):
        self.lib.fill()
        self.assertEqual(self.lib.state(1), 0)
        self.lib.frame()
        self.lib.fill()
        self.assertEqual(self.lib.state(1), 4)
        self.assertEqual(self.lib.state(3), 0)
        self.assertEqual(self.lib.state(4), 10*56)
        self.assertTrue(self.lib.state(6))
        for _ in range(6):
            self.lib.fill()
        self.assertEqual([self.lib.visit(i) for i in range(14)], list(range(8, 22)))
        self.assertTrue(self.lib.state(0))
        self.assertTrue(self.lib.state(7))
        self.assertEqual(self.lib.state(4), 0)

    def test_one_expensive_row_yields_at_time_budget(self):
        self.lib.frame()
        self.lib.cost(8500)
        self.lib.fill()
        self.assertEqual(self.lib.state(1), 1)

    def test_queue_change_restarts_before_binding_stale_rows(self):
        for change in (0, 1):
            with self.subTest(change=change):
                self.lib.reset();self.lib.frame();self.lib.change(change);self.lib.fill()
                self.assertEqual(self.lib.state(1), 0)
                self.assertEqual(self.lib.state(5), 1)
                self.assertTrue(self.lib.state(0))

    def test_close_switch_and_deleted_page_cancel_fill(self):
        for change in (2, 3, 4):
            with self.subTest(change=change):
                self.lib.reset();self.lib.frame();self.lib.change(change);self.lib.fill()
                self.assertEqual(self.lib.state(1), 0)
                self.assertEqual(self.lib.state(5), 0)
                self.assertTrue(self.lib.state(0))

    def test_unresolvable_rows_finish_without_fake_focus(self):
        self.lib.change(5);self.lib.frame()
        for _ in range(7):
            self.lib.fill()
        self.assertEqual(self.lib.state(1), 14)
        self.assertEqual(self.lib.state(2), 0)
        self.assertEqual(self.lib.state(3), -1)
        self.assertTrue(self.lib.state(0))
