"""Exercise the production album row/timer functions with a fake clock and I/O.

The LVGL boundary is stubbed so regressions in viewport reconciliation, idle
scheduling and read budgets can run without hardware or the desktop simulator.
Firmware screenshots provide the separate rendering check.
"""
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
        raise AssertionError('Production function missing: ' + name)
    depth, pos = 1, match.end()
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "thumb_cache_format.h"
#define LV_UNUSED(x) (void)(x)
#define UI_PERF_LOG(...) do { if(0) printf(__VA_ARGS__); } while(0)
#define COL_PANEL 0
#define COL_FOCUS 48
#define COL_ACCENT_DARK 40
#define COL_ACCENT 200
#define LV_OPA_COVER 255
#define LV_OPA_TRANSP 0
#define LV_BORDER_SIDE_LEFT 1
#define LV_ANIM_OFF 0
#define SCR_ALBUMS 1
#define REAL_THUMB_SIZE 54
#define N 100

typedef struct {
    int top, height, bg_opa, border_width;
    bool hidden, scrolling;
    const void *src;
} lv_obj_t;
typedef struct lv_timer_t {
    void (*cb)(struct lv_timer_t *); uint32_t period, last; bool paused;
} lv_timer_t;
typedef int lv_event_t;
typedef struct { uint64_t id; char title[32], artist[32]; uint32_t color; } player_album_t;
static lv_obj_t list, rows[10], covers[10], titles[10], artists[10], shuffle_row;
static lv_obj_t *albums_left_list, *albums_rows[10], *albums_row_covers[10];
static lv_obj_t *albums_shuffle_row;
static lv_obj_t *albums_row_titles[10], *albums_row_artists[10];
static uint64_t albums_row_album_ids[10];
static int albums_row_indices[10];
static bool albums_row_visible[10], albums_row_selected[10], albums_row_cover_pending[10];
static uint8_t albums_row_cover_attempts[10];
static uint32_t albums_row_cover_attempt_tick[10];
static int albums_selected, albums_first_index, albums_view_top, albums_view_h;
static int albums_left_scroll_y;
static bool albums_detail_open;
static int g_current_screen, count;
static bool g_ui_trace, albums_refresh_pending;
static uint32_t tick, albums_last_scroll_tick, read_cost, spent_us;
static lv_timer_t *albums_refresh_timer, *albums_cover_timer, timers[2];
static int timer_count, text_writes, read_calls[N], fail_left[N];
static bool cached[N], missing[N];
static uint32_t lv_tick_get(void) { return tick; }
static uint32_t lv_tick_elaps(uint32_t t) { return tick - t; }
static uint32_t perf_now(void) { return tick * 1000 + spent_us; }
static void perf_log_us(const char *s, uint32_t t) { (void)s; (void)t; }
static int player_album_count(void) { return count; }
static bool player_album_get(int i, player_album_t *a) {
    if(i < 0 || i >= count) return false;
    *a = (player_album_t){.id = (uint64_t)i + 1};
    snprintf(a->title, sizeof(a->title), "album %d", i);
    return true;
}
static void lv_obj_set_hidden(lv_obj_t *o, bool b) { o->hidden = b; }
static void lv_obj_set_pos(lv_obj_t *o, int x, int y) { (void)x; o->top = y; }
static void lv_obj_set_user_data(lv_obj_t *o, void *p) { (void)o; (void)p; }
static void lv_label_set_text(lv_obj_t *o, const char *s) { (void)o; (void)s; text_writes++; }
static int lv_color_hex(int c) { return c; }
static void lv_obj_set_style_bg_color(lv_obj_t *o, int c, int part) { (void)o; (void)c; (void)part; }
static void lv_obj_set_style_bg_opa(lv_obj_t *o, int v, int part) { (void)part; o->bg_opa = v; }
static void lv_obj_set_style_border_width(lv_obj_t *o, int v, int part) { (void)part; o->border_width = v; }
static void lv_obj_set_style_border_side(lv_obj_t *o, int v, int part) { (void)o; (void)v; (void)part; }
static void lv_obj_set_style_border_color(lv_obj_t *o, int v, int part) { (void)o; (void)v; (void)part; }
static void theme_mark_selected(lv_obj_t *o, bool selected) { (void)o; (void)selected; }
static void lv_image_set_src(lv_obj_t *o, const void *s) { o->src = s; }
static const void *lv_image_get_src(lv_obj_t *o) { return o->src; }
static int lv_obj_get_scroll_top(lv_obj_t *o) { return o->top; }
static int lv_obj_get_scroll_y(lv_obj_t *o) { return o->top; }
static int lv_obj_get_height(lv_obj_t *o) { return o->height; }
static bool lv_obj_is_scrolling(lv_obj_t *o) { return o->scrolling; }
static void albums_scroll_cb(lv_event_t *event);
static void lv_obj_scroll_to_y(lv_obj_t *o, int y, int anim) {
    (void)anim;
    o->top = y;
    albums_scroll_cb(NULL);
}
static const void *thumb_for_album_cached(const player_album_t *a) {
    return cached[a->id-1] ? (const void *)(uintptr_t)a->id : NULL;
}
static bool album_thumb_missing(const player_album_t *a) { return missing[a->id-1]; }
static const void *thumb_for_album(const player_album_t *a, int size) {
    (void)size;
    int i = a->id-1;
    read_calls[i]++;
    spent_us += read_cost;
    if(fail_left[i] > 0) { fail_left[i]--; return NULL; }
    if(missing[i]) return NULL;
    cached[i] = true;
    return (const void *)(uintptr_t)a->id;
}
static lv_timer_t *lv_timer_create(void (*cb)(lv_timer_t *), uint32_t period, void *p) {
    (void)p; assert(timer_count < 2);
    lv_timer_t *t = &timers[timer_count++];
    *t = (lv_timer_t){.cb=cb, .period=period, .last=tick};
    return t;
}
static void lv_timer_pause(lv_timer_t *t) { t->paused = true; }
static void lv_timer_resume(lv_timer_t *t) { t->paused = false; }
static void lv_timer_reset(lv_timer_t *t) { t->last = tick; }
static void lv_timer_set_period(lv_timer_t *t, uint32_t p) { assert(p); t->period = p; }

/* The actual pack reader is tested separately from the mock thumbnail I/O. */
typedef struct { char path[128]; int albums; } thumb_load_arg_t;
static uint8_t *s_thumb_pack_ram;
static size_t s_thumb_pack_ram_size;
static bool s_thumb_pack_ram_ready, s_thumb_pack_ready;
static uint32_t s_thumb_pack_count;
static uint8_t s_thumb_valid[512];
static FILE *s_thumb_pack_file;
static char s_artwork_dir[64];
static void *ui_alloc_aligned(size_t n) { return malloc(n); }
static int64_t esp_timer_get_time(void) { return perf_now(); }
static void vTaskDelete(void *p) { (void)p; }
'''

DRIVER = r'''
void setup(int n) {
    count = n; tick = 1000; spent_us = 0; read_cost = 0; text_writes = 0;
    timer_count = 0; albums_refresh_timer = albums_cover_timer = NULL;
    albums_refresh_pending = false; g_current_screen = SCR_ALBUMS;
    albums_first_index = -1; albums_selected = 0; albums_last_scroll_tick = tick;
    albums_left_scroll_y = 70; albums_detail_open = false;
    shuffle_row = (lv_obj_t){0}; albums_shuffle_row = &shuffle_row;
    memset(cached, 0, sizeof(cached)); memset(missing, 0, sizeof(missing));
    memset(read_calls, 0, sizeof(read_calls)); memset(fail_left, 0, sizeof(fail_left));
    memset(covers, 0, sizeof(covers)); memset(albums_row_album_ids, 0, sizeof(albums_row_album_ids));
    memset(albums_row_visible, 0, sizeof(albums_row_visible));
    memset(albums_row_selected, 0, sizeof(albums_row_selected));
    memset(albums_row_cover_pending, 0, sizeof(albums_row_cover_pending));
    memset(albums_row_cover_attempts, 0, sizeof(albums_row_cover_attempts));
    list = (lv_obj_t){.top=70, .height=560}; albums_left_list = &list;
    for(int i=0; i<10; i++) {
        albums_row_indices[i] = -1; albums_rows[i] = &rows[i];
        albums_row_covers[i] = &covers[i]; albums_row_titles[i] = &titles[i];
        albums_row_artists[i] = &artists[i];
    }
    albums_refresh_rows(); albums_update_selection(); albums_schedule_covers();
}
void advance(int ms) {
    while(ms-- > 0) {
        tick++;
        for(int i=0; i<timer_count; i++) {
            lv_timer_t *t = &timers[i];
            if(!t->paused && lv_tick_elaps(t->last) >= t->period) {
                t->last = tick; t->cb(t);
            }
        }
    }
}
void scroll_to(int top) { list.top = top; albums_scroll_cb(NULL); }
void touch_scroll(int active) { list.scrolling = active != 0; albums_scroll_cb(NULL); }
void leave_page(void) {
    albums_save_position(); albums_cancel_work();
    albums_left_list = NULL; g_current_screen = 0;
}
void enter_page(void) {
    albums_enter(); g_current_screen = SCR_ALBUMS;
    albums_left_list = &list; list.top = albums_left_scroll_y;
    albums_refresh_rows(); albums_update_selection(); albums_schedule_covers();
}
void choose(int index) { albums_selected = index; albums_scroll_to_selected(); }
void set_selection(int index) { albums_selected = index; }
int selection(void) { return albums_selected; }
int scroll_top(void) { return list.top; }
int focused(int index) {
    if(index < 0) return shuffle_row.border_width == 3 && shuffle_row.bg_opa == 255;
    for(int i=0; i<10; i++) if(albums_row_indices[i] == index)
        return rows[i].border_width == 3 && rows[i].bg_opa == 255;
    return 0;
}
void fail_reads(int index, int n) { fail_left[index] = n; }
void mark_missing(int index) { missing[index] = true; }
void set_read_cost(int us) { read_cost = us; }
int reads(int index) {
    if(index >= 0) return read_calls[index];
    int sum = 0; for(int i=0; i<N; i++) sum += read_calls[i]; return sum;
}
int labels(void) { return text_writes; }
int loaded(int index) {
    for(int i=0; i<10; i++) if(albums_row_indices[i] == index)
        return covers[i].src == (const void *)(uintptr_t)(index+1);
    return 0;
}
int visible_blank(void) {
    int n=0;
    for(int i=0; i<10; i++)
        if(albums_row_visible[i] && albums_row_indices[i]>=0 && !covers[i].src) n++;
    return n;
}
int active_timers(void) {
    int n=0; for(int i=0; i<timer_count; i++) n += !timers[i].paused; return n;
}
int load_pack(const char *path, int albums) {
    free(s_thumb_pack_ram); s_thumb_pack_ram = NULL;
    s_thumb_pack_ram_ready = false; s_thumb_pack_ram_size = 0;
    s_thumb_pack_count = albums; s_thumb_pack_ready = true;
    memset(s_thumb_valid, 0xff, sizeof(s_thumb_valid));
    thumb_load_arg_t *a = malloc(sizeof(*a));
    snprintf(a->path, sizeof(a->path), "%s", path); a->albums = albums;
    thumb_pack_load_task(a);
    return s_thumb_pack_ram_ready;
}
int read_pack(int id, uint8_t *data) { return real_thumb_read(id, data); }
'''


class AlbumViewTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc')
        if not cc:
            raise RuntimeError('A C compiler is required')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = (ROOT / 'app/ui/ui.c').read_text()
        constants = '\n'.join(re.findall(r'^#define ALBUMS_.*$', source, re.M))
        functions = '\n\n'.join(function(source, name) for name in (
            'albums_row_top', 'albums_first_for_top', 'albums_save_position',
            'albums_apply_row_style', 'albums_apply_shuffle_row_style',
            'albums_update_selection', 'albums_fill_row',
            'albums_refresh_rows', 'albums_refresh_timer_cb', 'albums_schedule_refresh',
            'albums_cover_timer_cb', 'albums_schedule_covers', 'albums_cancel_work',
            'albums_scroll_cb', 'albums_scroll_to_selected', 'albums_enter',
            'thumb_pack_load_task', 'real_thumb_read'))
        unit = work / 'albums.c'
        unit.write_text(PRELUDE + constants + '\n' + functions + '\n' + DRIVER)
        lib = work / 'albums.so'
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-I' + str(ROOT / 'app/ui'), str(unit), '-o', str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.load_pack.argtypes = [ctypes.c_char_p, ctypes.c_int]
        cls.lib.read_pack.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint8)]

    def setUp(self):
        self.lib.setup(60)

    def test_retained_overscan_row_loads_after_stop_without_enter(self):
        self.lib.advance(250)
        self.assertEqual(self.lib.reads(-1), 8)
        self.assertFalse(self.lib.loaded(8))
        labels = self.lib.labels()
        self.lib.scroll_to(140)  # Same ten-row pool; row 8 becomes visible.
        self.lib.advance(139)
        self.assertFalse(self.lib.loaded(8))
        self.lib.advance(100)
        self.assertTrue(self.lib.loaded(8))
        self.assertEqual(self.lib.visible_blank(), 0)
        self.assertEqual(self.lib.labels(), labels)  # No retained text rebuild.
        self.assertEqual(self.lib.reads(-1), 9)

    def test_first_up_reveals_and_highlights_shuffle_in_one_step(self):
        self.assertTrue(self.lib.focused(0))
        self.lib.choose(-1)
        self.assertEqual(self.lib.scroll_top(), 0)
        self.assertTrue(self.lib.focused(-1))
        self.assertFalse(self.lib.focused(0))
        self.lib.advance(300)
        self.assertEqual(self.lib.selection(), -1)
        self.assertTrue(self.lib.focused(-1))
        self.lib.choose(-2)  # Repeated Up clamps to Shuffle All.
        self.assertEqual(self.lib.selection(), -1)
        self.assertTrue(self.lib.focused(-1))
        self.lib.choose(0)
        self.assertTrue(self.lib.focused(0))
        self.assertFalse(self.lib.focused(-1))

    def test_returning_from_minus_item_selects_first_album(self):
        for minus_item in (-1, -2):
            self.lib.setup(60)
            self.lib.choose(-1)
            self.lib.set_selection(minus_item)
            self.lib.leave_page()
            self.lib.enter_page()
            self.assertEqual(self.lib.selection(), 0)
            self.assertEqual(self.lib.scroll_top(), 70)
            self.assertTrue(self.lib.focused(0))
            self.assertFalse(self.lib.focused(-1))

    def test_returning_from_regular_album_keeps_selection_and_position(self):
        self.lib.choose(15)
        top = self.lib.scroll_top()
        self.assertGreater(top, 70)
        self.lib.leave_page()
        self.lib.enter_page()
        self.assertEqual(self.lib.selection(), 15)
        self.assertEqual(self.lib.scroll_top(), top)
        self.assertTrue(self.lib.focused(15))

    def test_continuous_scroll_updates_rows_without_reads(self):
        for top in (140, 490, 840, 1190):
            self.lib.scroll_to(top)
            self.lib.advance(80)
            self.assertEqual(self.lib.reads(-1), 0)
        self.assertGreater(self.lib.labels(), 20)
        self.lib.advance(250)
        self.assertEqual(self.lib.visible_blank(), 0)
        self.assertEqual(self.lib.reads(-1), 8)
        self.assertEqual(self.lib.reads(0), 0)  # Obsolete viewport work dropped.

    def test_partial_row_enters_without_pool_index_change(self):
        self.lib.advance(250)
        self.lib.scroll_to(100)
        self.lib.advance(250)
        self.assertTrue(self.lib.loaded(8))
        self.assertEqual(self.lib.visible_blank(), 0)

    def test_cached_reverse_scroll_needs_no_reads(self):
        self.lib.advance(250)
        self.lib.scroll_to(140)
        self.lib.advance(250)
        calls = self.lib.reads(-1)
        self.lib.scroll_to(70)
        self.lib.advance(16)
        self.assertEqual(self.lib.visible_blank(), 0)
        self.lib.advance(300)
        self.assertEqual(self.lib.reads(-1), calls)

    def test_loads_are_batched_and_slow_reads_yield(self):
        self.lib.advance(139)
        self.assertEqual(self.lib.reads(-1), 0)
        self.lib.advance(1)
        self.assertEqual(self.lib.reads(-1), 2)
        self.lib.advance(16)
        self.assertEqual(self.lib.reads(-1), 4)
        self.lib.setup(60)
        self.lib.set_read_cost(5000)
        self.lib.advance(140)
        self.assertEqual(self.lib.reads(-1), 1)
        self.lib.advance(16)
        self.assertEqual(self.lib.reads(-1), 2)

    def test_touch_inertia_delays_artwork_until_scroll_end(self):
        self.lib.touch_scroll(1)
        self.lib.advance(400)
        self.assertEqual(self.lib.reads(-1), 0)
        self.lib.touch_scroll(0)
        self.lib.advance(250)
        self.assertEqual(self.lib.visible_blank(), 0)

    def test_temporary_read_failure_retries_but_missing_art_does_not_spin(self):
        self.lib.fail_reads(0, 2)
        self.lib.fail_reads(1, 100)
        self.lib.advance(1000)
        self.assertTrue(self.lib.loaded(0))
        self.assertEqual(self.lib.reads(0), 3)
        self.assertEqual(self.lib.reads(1), 3)
        self.assertEqual(self.lib.active_timers(), 0)
        self.lib.advance(5000)
        self.assertEqual(self.lib.reads(1), 3)
        self.lib.mark_missing(8)
        self.lib.scroll_to(140)
        self.lib.advance(1000)
        self.assertEqual(self.lib.reads(8), 0)

    def test_leaving_page_cancels_pending_reads(self):
        self.lib.advance(140)
        calls = self.lib.reads(-1)
        self.lib.leave_page()
        self.lib.advance(1000)
        self.assertEqual(self.lib.reads(-1), calls)
        self.assertEqual(self.lib.active_timers(), 0)

    def test_psram_pack_includes_header_and_last_thumbnail(self):
        path = Path(self.temp.name) / 'thumbs.bin'
        one = 54 * 54 * 2
        path.write_bytes(bytes(64) + b'\x11' * one + b'\x22' * one)
        self.assertTrue(self.lib.load_pack(str(path).encode(), 2))
        data = (ctypes.c_uint8 * one)()
        for album_id, value in ((1, 0x11), (2, 0x22)):
            self.assertTrue(self.lib.read_pack(album_id, data))
            self.assertEqual(bytes(data), bytes([value]) * one)


if __name__ == '__main__':
    unittest.main()
