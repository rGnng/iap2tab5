"""Run production display counters, trace paging and CSV against fake clocks."""
import csv
import ctypes as C
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise AssertionError(name)
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


class DisplayDiagTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        port = (ROOT / 'firmware/components/esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c').read_text()
        port_h = (ROOT / 'firmware/components/esp_lvgl_port/include/esp_lvgl_port_disp.h').read_text()
        diag = (ROOT / 'firmware/main/display_diag.c').read_text()
        diag_h = (ROOT / 'firmware/main/display_diag.h').read_text()
        web = (ROOT / 'firmware/main/web_server.c').read_text()
        port_type = re.search(r'typedef struct \{[^}]*\} lvgl_port_display_stats_t;', port_h, re.S).group()
        sample_type = re.search(r'typedef struct \{[^}]*\} display_diag_sample_t;', diag_h, re.S).group()
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define IRAM_ATTR
#define ESP_OK 0
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY 0
#define DISPLAY_DIAG_CAPACITY 8
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL_ISR(p) ((void)(p))
#define portEXIT_CRITICAL_ISR(p) ((void)(p))
#define ESP_LOGI(tag,...) ((void)(tag))
typedef int esp_err_t;
typedef int BaseType_t;
typedef void *esp_lcd_panel_handle_t;
typedef int esp_lcd_dpi_panel_event_data_t;
typedef void lv_display_t;
static uint64_t now;
static int draw_result, draws, flush_ready;
static int s_stats_lock, s_ring_lock;
static uint64_t esp_timer_get_time(void) { return now; }
static void *lv_display_get_driver_data(void *d) { return d; }
static void xSemaphoreTake(int p,int t) { (void)p;(void)t; }
static void xSemaphoreGive(int p) { (void)p; }
static void xSemaphoreGiveFromISR(void *p,int *yield) { (void)p;*yield=0; }
static void lv_disp_flush_ready(void *d) { (void)d;flush_ready++; }
static int esp_lcd_panel_draw_bitmap(void *p,int x1,int y1,int x2,int y2,const void *pixels) {
    (void)p;(void)x1;(void)y1;(void)x2;(void)y2;(void)pixels;draws++;return draw_result;
}
'''
        boundary = r'''
typedef struct { lvgl_port_display_stats_t stats; uint32_t copy_started_us; void *panel_handle,*trans_sem; } lvgl_port_display_ctx_t;
static lvgl_port_display_ctx_t ctx;
static display_diag_sample_t storage[DISPLAY_DIAG_CAPACITY],*s_ring;
static uint32_t s_next=1,s_count,s_marker,s_marker_ms;
'''
        production = '\n'.join([function(port, name) for name in [
            'lvgl_port_diag_vsync', 'lvgl_port_diag_draw', 'lvgl_port_flush_dpi_panel_ready_callback']]
            + [function(diag, name) for name in ['display_diag_latest','display_diag_read','display_diag_mark','display_diag_last_marker']]
            + [function(web, 'display_sample_csv')])
        # Production callback uses assert; include the real assertion macro.
        wrappers = r'''
void reset(void) {
    memset(&ctx,0,sizeof(ctx));memset(storage,0,sizeof(storage));
    s_ring=storage;s_next=1;s_count=s_marker=s_marker_ms=0;now=0;draws=flush_ready=draw_result=0;
}
void vsync(uint64_t t) { now=t;lvgl_port_diag_vsync(NULL,NULL,&ctx); }
uint32_t max_gap(void) { return ctx.stats.vsync_max_gap_us; }
uint32_t long_gaps(void) { return ctx.stats.vsync_gaps_over_25ms; }
uint32_t vsync_count(void) { return ctx.stats.vsync_count; }
void submit(uint64_t t,int error) { now=t;draw_result=error;lvgl_port_diag_draw(&ctx,0,0,10,10,NULL); }
void complete(uint64_t t) { now=t;lvgl_port_flush_dpi_panel_ready_callback(NULL,NULL,&ctx); }
uint32_t copies(void) { return ctx.stats.copy_completed; }
uint32_t copy_time(void) { return ctx.stats.copy_max_us; }
uint32_t errors(void) { return ctx.stats.draw_errors; }
void append(uint32_t t) {
    display_diag_sample_t s={.seq=s_next++,.uptime_ms=t,.pwm_duty=409,.pwm_hz=5000,.pwm_period=1024,.display=ctx.stats};
    storage[s.seq%DISPLAY_DIAG_CAPACITY]=s;if(s_count<DISPLAY_DIAG_CAPACITY)s_count++;
}
static display_diag_sample_t page[DISPLAY_DIAG_CAPACITY];
static uint32_t next,dropped;
size_t read_page(uint32_t since,size_t cap) { return display_diag_read(since,page,cap,&next,&dropped); }
uint32_t page_seq(int i) { return page[i].seq; }
uint32_t next_seq(void) { return next; }
uint32_t lost(void) { return dropped; }
uint32_t latest_seq(void) { display_diag_sample_t s;return display_diag_latest(&s)?s.seq:0; }
uint32_t mark_at(uint64_t t) { uint32_t ms;now=t;return display_diag_mark(&ms); }
uint32_t marker_time(void) { uint32_t id,ms;display_diag_last_marker(&id,&ms);return ms; }
char *csv_line(void) { static char b[512];display_sample_csv(b,sizeof(b),&storage[(s_next-1)%DISPLAY_DIAG_CAPACITY]);return b; }
'''
        work = Path(cls.tmp.name)
        (work / 'diag.c').write_text('#include <assert.h>\n' + prelude + port_type + sample_type + boundary + production + wrappers)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(work / 'diag.c'), '-o', str(work / 'diag.so')], check=True)
        cls.lib = C.CDLL(str(work / 'diag.so'))
        cls.lib.vsync.argtypes = [C.c_uint64]
        cls.lib.submit.argtypes = [C.c_uint64, C.c_int]
        cls.lib.complete.argtypes = [C.c_uint64]
        cls.lib.mark_at.argtypes = [C.c_uint64]
        cls.lib.append.argtypes = [C.c_uint32]
        cls.lib.read_page.argtypes = [C.c_uint32, C.c_size_t]
        cls.lib.csv_line.restype = C.c_char_p

    def setUp(self):
        self.lib.reset()

    def test_vsync_gaps_and_wrap(self):
        for t in [0xfffffff0, 0xfffffff0+17461, 0xfffffff0+34922]:
            self.lib.vsync(t)
        self.assertEqual(self.lib.vsync_count(), 3)
        self.assertEqual(self.lib.max_gap(), 17461)
        self.assertEqual(self.lib.long_gaps(), 0)
        self.lib.vsync(0xfffffff0+34922+50000)
        self.assertEqual(self.lib.max_gap(), 50000)
        self.assertEqual(self.lib.long_gaps(), 1)

    def test_dma_completion_and_errors(self):
        self.lib.submit(0xfffffff0, 0)
        self.lib.complete(0xfffffff0+1234)
        self.assertEqual(self.lib.copies(), 1)
        self.assertEqual(self.lib.copy_time(), 1234)
        self.lib.submit(0x100001000, -1)
        self.assertEqual(self.lib.errors(), 1)

    def test_empty_and_latest(self):
        self.assertEqual(self.lib.latest_seq(), 0)
        self.assertEqual(self.lib.read_page(0, 8), 0)
        self.lib.append(100)
        self.assertEqual(self.lib.latest_seq(), 1)

    def test_trace_wrap_paging_and_overwrite_count(self):
        for t in range(12):
            self.lib.append(t * 100)
        self.assertEqual(self.lib.read_page(0, 3), 3)
        self.assertEqual([self.lib.page_seq(i) for i in range(3)], [5,6,7])
        self.assertEqual(self.lib.next_seq(), 8)
        self.assertEqual(self.lib.lost(), 0)
        self.assertEqual(self.lib.read_page(1, 8), 8)
        self.assertEqual(self.lib.lost(), 4)
        self.assertEqual(self.lib.next_seq(), 13)
        self.assertEqual(self.lib.read_page(1000, 8), 0)
        self.assertEqual(self.lib.next_seq(), 13)

    def test_marker_is_monotonic_uptime(self):
        self.assertEqual(self.lib.mark_at(123456000), 1)
        self.assertEqual(self.lib.marker_time(), 123456)
        self.assertEqual(self.lib.mark_at(123457000), 2)

    def test_csv_has_expected_columns_and_values(self):
        self.lib.append(1234)
        row = next(csv.reader([self.lib.csv_line().decode()]))
        self.assertEqual(len(row), 23)
        self.assertEqual(row[:4], ['1','1234','409','5000'])
        self.assertEqual(row[-1], '1024')


if __name__ == '__main__':
    unittest.main()
