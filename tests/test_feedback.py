"""Run the production feedback queue/task without audio hardware."""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^(?:static )?void ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    depth, pos = 1, match.end()
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos]


class FeedbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT/'firmware/main/feedback.c').read_text()
        ui = (ROOT/'app/ui/ui.h').read_text()
        ui_enum = re.search(r'typedef enum \{\s*UI_FEEDBACK_CLICK.*?\} ui_feedback_t;', ui, re.S).group()
        request = re.search(r'typedef struct \{\s*uint16_t frequency;.*?\} tone_request_t;', source, re.S).group()
        kinds = ['CLICK','ACTIVATE','BACK','FAST_SCROLL','FAST_SCROLL_EXIT','SCREEN_OFF','SCREEN_ON']
        harness = r'''
#include "feedback.h"
#include <setjmp.h>
#include <stddef.h>
#define pdTRUE 1
#define portMAX_DELAY 0
#define TAG "test"
#define ESP_LOGD(...) ((void)0)
'''+ui_enum+'\n'+request+r'''
static bool s_enabled, pending;
static uint8_t s_volume;
static void *s_queue=(void *)1;
static tone_request_t queued;
static int writes, hz[4], ms[4];
static jmp_buf stop;
static int xQueueSend(void *q,const void *p,unsigned wait)
{ (void)q;(void)wait;queued=*(const tone_request_t *)p;pending=true;return pdTRUE; }
static int xQueueReceive(void *q,void *p,unsigned wait)
{ (void)q;(void)wait;if(!pending)longjmp(stop,1);*(tone_request_t *)p=queued;pending=false;return pdTRUE; }
static void write_tone(uint16_t frequency,uint16_t duration)
{ hz[writes]=frequency;ms[writes++]=duration; }
'''+function(source,'feedback_task')+'\n'+function(source,'feedback_play')+r'''
void reset(void) { s_enabled=true;s_volume=50;pending=false;writes=0; }
void settings(bool enabled,int volume) { s_enabled=enabled;s_volume=volume; }
void play(int kind) {
    const ui_feedback_t kinds[]={'''+','.join('UI_FEEDBACK_'+k for k in kinds)+r'''};
    feedback_play((feedback_t)kinds[kind]);
}
void drain(void) { if(!setjmp(stop))feedback_task(0); }
int value(int field) { return field==0?writes:field==1?hz[0]:ms[0]; }
bool matching_ids(void) { return '''+' && '.join('(int)UI_FEEDBACK_'+k+'==(int)FEEDBACK_'+k for k in kinds)+r'''; }
'''
        c = Path(cls.tmp.name)/'feedback.c'
        lib = Path(cls.tmp.name)/'feedback.so'
        c.write_text(harness)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        '-I'+str(ROOT/'firmware/main'),str(c),'-o',str(lib)],check=True)
        cls.lib=ctypes.CDLL(str(lib))
        cls.lib.settings.argtypes=[ctypes.c_bool,ctypes.c_int]
        cls.lib.matching_ids.restype=ctypes.c_bool

    def setUp(self):
        self.lib.reset()

    def tone(self, kind):
        self.lib.reset();self.lib.play(kind);self.lib.drain()
        self.assertEqual(self.lib.value(0),1,'Each event must write exactly one tone')
        return self.lib.value(1),self.lib.value(2)

    def test_fast_scroll_uses_single_high_entry_and_low_exit(self):
        start,start_ms=self.tone(3);end,end_ms=self.tone(4)
        self.assertGreater(start,end)
        self.assertLessEqual(max(start_ms,end_ms),30)

    def test_screen_uses_single_low_off_and_high_wake(self):
        off,off_ms=self.tone(5);on,on_ms=self.tone(6)
        self.assertLess(off,on)
        self.assertLessEqual(max(off_ms,on_ms),80)

    def test_existing_key_feedback_stays_single_tone(self):
        for kind in range(3):self.tone(kind)

    def test_muted_feedback_is_silent_even_when_already_queued(self):
        for enabled,volume in [(False,50),(True,0)]:
            self.lib.reset();self.lib.settings(enabled,volume)
            self.lib.play(5);self.lib.drain();self.assertEqual(self.lib.value(0),0)
            self.lib.reset();self.lib.play(6);self.lib.settings(enabled,volume)
            self.lib.drain();self.assertEqual(self.lib.value(0),0)

    def test_ui_and_firmware_feedback_ids_agree(self):
        self.assertTrue(self.lib.matching_ids())


if __name__=='__main__':unittest.main()
