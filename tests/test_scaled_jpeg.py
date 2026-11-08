"""Decode real JPEGs through the production thumbnail sampler at every scale."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_album_view import function
ROOT=Path(__file__).resolve().parents[1]

class ScaledJPEGTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name)
        source=(ROOT/'firmware/main/lockdown_client.c').read_text()
        prelude=r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "scaled_tjpgd.h"
#define ART_THUMB_SIZE 54
#define ART_COVER_SIZE 360
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define ESP_LOGI(...) ((void)0)
#define heap_caps_free free
static uint32_t s_last_bytes;
static void *heap_caps_malloc(size_t n,int caps) {(void)caps;return malloc(n);}
static void *heap_caps_aligned_alloc(size_t a,size_t n,int caps) {
    (void)caps;void *p=NULL;return posix_memalign(&p,a,n)?NULL:p;
}
static int64_t esp_timer_get_time(void) {return 0;}
static void vTaskDelay(int t) {(void)t;}
static void art_set_info(int w,int h,const char *kind) {(void)w;(void)h;(void)kind;}
typedef struct {int x1,y1,x2,y2;} lv_area_t;
typedef enum {LV_COLOR_FORMAT_RGB565,LV_COLOR_FORMAT_RGB888,LV_COLOR_FORMAT_ARGB8888,LV_COLOR_FORMAT_XRGB8888} lv_color_format_t;
'''
        code=prelude+function(source,'art_sample_rect')
        start=source.index('typedef struct {\n    FILE * f;')
        end=source.index('} tjpgd_ctx_t;',start)+len('} tjpgd_ctx_t;')
        code+=source[start:end]+'\nstatic int64_t s_tjpgd_yield_us;\n'
        for name in ('tjpgd_input','tjpgd_yield_if_needed','tjpgd_output','tjpgd_decode_sampled','tjpgd_decode_sampled_mem'):
            code+=function(source,name)
        code+=r'''
int decode_mem(const void *data,size_t len,uint8_t *thumb,int target) {
    return tjpgd_decode_sampled_mem(data,len,thumb,NULL,target);
}
int decode_file(const char *path,uint8_t *thumb,int target) {
    return tjpgd_decode_sampled(path,thumb,NULL,target);
}
'''
        (p/'sample.c').write_text(code)
        subprocess.run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'firmware/main'),str(p/'sample.c'),str(ROOT/'firmware/main/scaled_tjpgd.c'),'-o',str(p/'sample.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'sample.so'))
        cls.lib.decode_mem.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_int]
        cls.lib.decode_file.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_int]

    def decode(self,name,target,file=False):
        path=ROOT/'tests/fixtures'/name;data=path.read_bytes();thumb=(ctypes.c_uint16*(54*54))(*([0]*2916))
        result=self.lib.decode_file(str(path).encode(),thumb,target) if file else self.lib.decode_mem(data,len(data),thumb,target)
        self.assertEqual(result,1)
        self.assertTrue(all(thumb),'no unfilled thumbnail pixels')
        return list(thumb)

    @staticmethod
    def rgb(pixel):
        return ((pixel>>11)*255//31,((pixel>>5)&63)*255//63,(pixel&31)*255//31)

    def test_all_scales_preserve_colors_orientation_and_odd_edges(self):
        for target in (54,100,200,600): # 1/8, 1/4, 1/2, and 1/1
            with self.subTest(target=target):
                pixels=self.decode('jpeg-quadrants.jpg',target)
                self.assertEqual(pixels,self.decode('jpeg-quadrants.jpg',target,file=True))
                for x,y,expected in ((10,10,(220,24,32)),(44,10,(24,220,32)),(10,44,(24,32,220)),(44,44,(220,220,24))):
                    for got,want in zip(self.rgb(pixels[y*54+x]),expected):self.assertLess(abs(got-want),18)

    def test_grayscale_at_one_eighth_scale(self):
        pixels=self.decode('jpeg-gray.jpg',54)
        for p in pixels:
            rgb=self.rgb(p);self.assertLess(max(rgb)-min(rgb),13)

    def test_truncated_data_fails_without_reporting_complete_image(self):
        data=(ROOT/'tests/fixtures/jpeg-quadrants.jpg').read_bytes()[:100]
        thumb=(ctypes.c_uint16*2916)()
        self.assertEqual(self.lib.decode_mem(data,len(data),thumb,54),0)
