"""Run the actual queued D100H dispatch with a fake clock, display and UI."""
import ctypes as C
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class D100HDispatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):raise unittest.SkipTest('C compiler required')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name)
        (p/'lvgl.h').write_text('typedef struct lv_indev_t lv_indev_t;\n')
        source=(ROOT/'firmware/main/usb_hid_input.c').read_text()
        types=source[source.index('typedef enum {\n    KEY_EVENT_KEY,'):source.index('static QueueHandle_t s_device_events;')]
        state=source[source.index('static ulanzi_top_right_t s_top_right'):source.index('static uint8_t s_prev_keys')]
        dispatch=source[source.index('static uint16_t gesture_button_bit'):source.index('static void key_dispatch_task')]
        post=source[source.index('void usb_hid_input_post_key(ui_key_t'):source.index('void usb_hid_input_start(void)')]
        boundary=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "usb_hid_input.h"
#include "ulanzi_d100h.h"
typedef void *QueueHandle_t;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define ULANZI_TOP_RIGHT_ID 0x03
static QueueHandle_t s_key_events=(void*)1;
static int64_t clock_us;
static bool screen_on;
static int actions[64],offs,wakes,feedback,enter_after_up;
static uint32_t nav_first,nav_last;static int nav_count;
static bool bsp_display_lock(unsigned ms) { (void)ms;return true; }
static void bsp_display_unlock(void) {}
static int64_t esp_timer_get_time(void) { return clock_us; }
bool ui_user_activity(void) { bool woke=!screen_on;screen_on=true;if(woke)wakes++;return woke; }
bool ui_screen_is_on(void) { return screen_on; }
void ui_screen_off(void) { screen_on=false;offs++; }
void ui_handle_key(ui_key_t key) { if(key==UI_KEY_ENTER)enter_after_up=actions[UI_KEY_UP];actions[key]++; }
void ui_handle_key_repeat(ui_key_t key,int n) { actions[key]+=n; }
static uint32_t lv_tick_get(void) { return (uint32_t)(clock_us/1000); }
void ui_handle_key_repeat_timed(ui_key_t key,int n,uint32_t first,uint32_t last) { nav_first=first;nav_last=last;nav_count=n;actions[key]+=n; }
void ui_play_key_feedback(ui_key_t key) { (void)key;feedback++; }
static void iap2_host_hid_release(void) {}
static int xQueuePeek(void *q,void *data,int ticks) { (void)q;(void)data;(void)ticks;return 0; }
static int xQueueReceive(void *q,void *data,int ticks) { (void)q;(void)data;(void)ticks;return 0; }
'''
        queue=r'''
static key_event_t queued;
static bool queued_valid;
static int xQueueSend(void *q,const void *data,int ticks) {
 (void)q;(void)ticks;memcpy(&queued,data,sizeof(queued));queued_valid=true;return pdTRUE;
}
'''
        exports=r'''
void reset(int on) {
 memset(s_top_right,0,sizeof(s_top_right));memset(s_knob,0,sizeof(s_knob));
 memset(s_back,0,sizeof(s_back));memset(s_play,0,sizeof(s_play));memset(s_rotary,0,sizeof(s_rotary));
 memset(s_button_down,0,sizeof(s_button_down));memset(s_wake_consumed,0,sizeof(s_wake_consumed));
 memset(actions,0,sizeof(actions));offs=wakes=feedback=enter_after_up=0;nav_first=nav_last=nav_count=0;clock_us=0;screen_on=on;
 s_hold_pending=s_hold_sent=false;s_hold_sent_us=0;queued_valid=false;
}
void input(int source,int id,int down,int64_t arrival,int64_t dispatch_time) {
 clock_us=arrival;queued_valid=false;
 if(id==1) usb_hid_input_post_back(source,down);
 else if(id==2) usb_hid_input_post_play(source,down);
 else if(id==3) usb_hid_input_post_top_right(source,down);
 else if(id==4 || id==6) usb_hid_input_post_side(source,id,down);
 else if(id==7 || id==11 || id==12) usb_hid_input_post_knob(source,id,down);
 else if(id==9 || id==10) usb_hid_input_post_rotary(source,id,down);
 else if(id==0) usb_hid_input_cancel_gestures(source);
 clock_us=dispatch_time;if(queued_valid)dispatch_key_event(&queued);
}
void poll_at(int64_t now) {
 clock_us=now;
 for(int i=0;i<D100H_INPUT_COUNT;i++) {
  dispatch_rotary_poll(i,now);
  dispatch_play(i,ULANZI_PLAY_POLL,now);
  dispatch_top_right(i,ULANZI_TOP_RIGHT_POLL,now);
 }
}
void keyboard_play(void) { usb_hid_input_post_key(UI_KEY_7);dispatch_key_event(&queued); }
void keyboard_tab(void) { usb_hid_input_post_key(UI_KEY_SCREEN_NEXT);dispatch_key_event(&queued); }
int side_prev_count(void) { return actions[UI_KEY_SCREEN_PREV]; }
int side_next_count(void) { return actions[UI_KEY_SCREEN_NEXT]; }
int up_count(void) { return actions[UI_KEY_UP]; }
int down_count(void) { return actions[UI_KEY_DOWN]; }
int back_count(void) { return actions[UI_KEY_BACK]; }
int play_count(void) { return actions[UI_KEY_7]; }
int star_count(void) { return actions[UI_KEY_STAR]; }
int context_count(void) { return actions[UI_KEY_CONTEXT]; }
int enter_count(void) { return actions[UI_KEY_ENTER]; }
int enter_up_count(void) { return enter_after_up; }
int volume_count(void) { return actions[UI_KEY_VOLUME_UP]; }
uint32_t first_tick(void) { return nav_first; }
uint32_t last_tick(void) { return nav_last; }
int notch_count(void) { return nav_count; }
int off_count(void) { return offs; }
int wake_count(void) { return wakes; }
int is_on(void) { return screen_on; }
'''
        (p/'dispatch.c').write_text(boundary+types+state+queue+dispatch+post+exports)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
            '-I'+str(p),'-I'+str(ROOT/'app/ui'),'-I'+str(ROOT/'firmware/main'),
            str(p/'dispatch.c'),str(ROOT/'firmware/main/ulanzi_d100h.c'),
            '-o',str(p/'dispatch.so')],check=True)
        cls.lib=C.CDLL(str(p/'dispatch.so'))
        cls.lib.input.argtypes=[C.c_int,C.c_int,C.c_int,C.c_int64,C.c_int64]
        cls.lib.poll_at.argtypes=[C.c_int64]

    def setUp(self):self.lib.reset(1)
    def post(self,id,down,t=0,source=0,delayed=None):
        self.lib.input(source,id,down,t,t if delayed is None else delayed)

    def test_short_play_fires_only_on_up(self):
        self.post(2,True)
        self.assertEqual(self.lib.play_count(),0)
        self.post(2,False,100000)
        self.assertEqual(self.lib.play_count(),1)
        self.assertEqual(self.lib.off_count(),0)

    def test_side_buttons_fire_once_only_on_matching_release_for_all_sources(self):
        for source in range(3):
            for id,count in ((4,self.lib.side_prev_count),(6,self.lib.side_next_count)):
                with self.subTest(source=source,id=id):
                    self.lib.reset(1)
                    self.post(id,False,source=source)
                    self.post(id,True,100000,source=source)
                    self.post(id,True,200000,source=source)
                    self.lib.poll_at(3000000)
                    self.assertEqual(count(),0)
                    self.post(id,False,4000000,source=source)
                    self.post(id,False,4100000,source=source)
                    self.assertEqual(count(),1)

    def test_side_wake_press_is_consumed_through_release(self):
        for source in range(3):
            for id,count in ((4,self.lib.side_prev_count),(6,self.lib.side_next_count)):
                with self.subTest(source=source,id=id):
                    self.lib.reset(0)
                    self.post(id,True,source=source)
                    self.post(id,True,100000,source=source)
                    self.post(id,False,200000,source=source)
                    self.assertEqual((self.lib.wake_count(),count()),(1,0))
                    self.post(id,True,300000,source=source)
                    self.post(id,False,400000,source=source)
                    self.assertEqual(count(),1)

    def test_side_buttons_track_independent_presses(self):
        self.post(4,True);self.post(6,True)
        self.post(6,False,100000)
        self.assertEqual((self.lib.side_prev_count(),self.lib.side_next_count()),(0,1))
        self.post(4,False,200000)
        self.assertEqual((self.lib.side_prev_count(),self.lib.side_next_count()),(1,1))

    def test_disconnect_cancels_side_releases_only_for_its_source(self):
        for source in (0,1):
            for id in (4,6):self.post(id,True,source=source)
        self.post(0,False,100000,source=0)
        for id in (4,6):self.post(id,False,200000,source=0)
        self.assertEqual((self.lib.side_prev_count(),self.lib.side_next_count()),(0,0))
        for id in (4,6):self.post(id,False,300000,source=1)
        self.assertEqual((self.lib.side_prev_count(),self.lib.side_next_count()),(1,1))

    def test_side_release_after_screen_off_never_wakes_or_navigates(self):
        self.post(4,True);self.post(6,True)
        self.lib.ui_screen_off()
        self.post(4,False,100000);self.post(6,False,200000)
        self.assertFalse(self.lib.is_on())
        self.assertEqual((self.lib.wake_count(),self.lib.side_prev_count(),self.lib.side_next_count()),(0,0,0))

    def test_wake_elsewhere_consumes_already_held_side_releases(self):
        self.post(4,True,source=1);self.post(6,True,source=1)
        self.lib.ui_screen_off();self.post(1,True,100000,source=2)
        self.post(4,False,200000,source=1);self.post(6,False,200000,source=1)
        self.post(1,False,200000,source=2)
        self.assertTrue(self.lib.is_on())
        self.assertEqual((self.lib.side_prev_count(),self.lib.side_next_count(),self.lib.back_count()),(0,0,0))

    def test_keyboard_tab_still_acts_on_down(self):
        self.lib.keyboard_tab();self.assertEqual(self.lib.side_next_count(),1)

    def test_hold_blanks_at_threshold_and_release_does_not_navigate_or_wake(self):
        self.post(2,True)
        self.lib.poll_at(1999999);self.assertTrue(self.lib.is_on())
        self.lib.poll_at(2000000);self.assertFalse(self.lib.is_on())
        self.post(2,True,2500000);self.lib.poll_at(3000000)
        self.post(2,False,4000000)
        self.assertEqual(self.lib.off_count(),1)
        self.assertEqual(self.lib.play_count(),0)
        self.assertEqual(self.lib.wake_count(),0)
        self.assertFalse(self.lib.is_on())

    def test_play_wake_press_is_consumed_until_release(self):
        self.lib.reset(0)
        self.post(2,True);self.assertTrue(self.lib.is_on())
        self.post(2,True,1000000);self.lib.poll_at(3000000)
        self.post(2,False,4000000)
        self.assertEqual(self.lib.off_count(),0)
        self.assertEqual(self.lib.play_count(),0)
        self.post(2,True,5000000);self.post(2,False,5100000)
        self.assertEqual(self.lib.play_count(),1)

    def test_back_only_on_release_including_a_long_hold(self):
        self.post(1,True);self.post(1,True,1000000);self.lib.poll_at(3000000)
        self.assertEqual(self.lib.back_count(),0)
        self.post(1,False,5000000);self.post(1,False,6000000)
        self.assertEqual(self.lib.back_count(),1)

    def test_back_wake_press_never_toggles_on_release(self):
        self.lib.reset(0)
        self.post(1,True);self.post(1,True,1000000);self.post(1,False,2000000)
        self.assertEqual(self.lib.back_count(),0)
        self.assertEqual(self.lib.wake_count(),1)
        self.post(1,True,3000000);self.post(1,False,3100000)
        self.assertEqual(self.lib.back_count(),1)

    def test_cancel_discards_pending_play_and_back(self):
        self.post(2,True);self.post(1,True);self.post(0,False,1000000)
        self.lib.poll_at(3000000);self.post(2,False,4000000);self.post(1,False,4000000)
        self.assertEqual((self.lib.off_count(),self.lib.play_count(),self.lib.back_count()),(0,0,0))

    def test_release_does_not_wake_if_screen_was_blanked_after_down(self):
        for id in (1,2):
            with self.subTest(id=id):
                self.lib.reset(1);self.post(id,True)
                self.lib.ui_screen_off();self.post(id,False,100000)
                self.assertFalse(self.lib.is_on())
                self.assertEqual((self.lib.play_count(),self.lib.back_count(),self.lib.wake_count()),(0,0,0))

    def test_disconnect_cancels_only_its_source(self):
        for source in (0,1):self.post(1,True,source=source)
        self.post(0,False,source=0)
        for source in (0,1):self.post(1,False,100000,source=source)
        self.assertEqual(self.lib.back_count(),1)

    def test_play_can_wake_after_its_previous_hold(self):
        self.post(2,True);self.lib.poll_at(2000000);self.post(2,False,2100000)
        self.post(2,True,3000000);self.post(2,False,3100000)
        self.assertTrue(self.lib.is_on());self.assertEqual(self.lib.play_count(),0)
        self.assertEqual(self.lib.wake_count(),1)

    def test_wake_on_another_source_consumes_preexisting_back_release(self):
        self.post(1,True,source=1);self.lib.ui_screen_off()
        self.post(2,True,100000,source=2)
        self.post(1,False,200000,source=1);self.post(2,False,200000,source=2)
        self.assertTrue(self.lib.is_on())
        self.assertEqual((self.lib.back_count(),self.lib.play_count()),(0,0))

    def test_wake_elsewhere_cancels_an_older_play_hold(self):
        self.post(2,True,source=0);self.lib.ui_screen_off()
        self.post(1,True,1000000,source=1);self.lib.poll_at(3000000)
        self.post(2,False,3100000,source=0);self.post(1,False,3100000,source=1)
        self.assertTrue(self.lib.is_on())
        self.assertEqual((self.lib.off_count(),self.lib.back_count(),self.lib.play_count()),(1,0,0))

    def test_short_release_uses_arrival_time_when_dispatch_was_busy(self):
        self.post(2,True)
        self.post(2,False,100000,delayed=3000000)
        self.lib.poll_at(3000000)
        self.assertEqual(self.lib.play_count(),1);self.assertEqual(self.lib.off_count(),0)

    def test_existing_top_right_and_knob_gestures_remain_available(self):
        self.post(3,True);self.lib.poll_at(600000);self.post(3,False,700000)
        self.assertEqual(self.lib.star_count(),1);self.assertEqual(self.lib.context_count(),0)
        self.post(7,True,1000000);self.post(12,True,1100000);self.post(12,True,1200000)
        self.post(7,False,1300000)
        self.assertEqual(self.lib.volume_count(),2);self.assertEqual(self.lib.enter_count(),0)

    def test_delayed_rotary_dispatch_preserves_physical_arrival_time(self):
        self.post(10,True,123000,delayed=900000)
        self.assertEqual(self.lib.first_tick(),123)
        self.assertEqual(self.lib.last_tick(),123)
        self.assertEqual(self.lib.notch_count(),1)

    def test_rotary_glitch_does_not_reach_navigation(self):
        for id,t in [(10,0),(10,20000),(9,30000),(10,55000)]:self.post(id,True,t)
        self.lib.poll_at(200000)
        self.assertEqual((self.lib.up_count(),self.lib.down_count()),(0,3))

    def test_rotary_single_reverse_is_not_lost_when_turn_stops(self):
        self.post(10,True);self.post(10,True,20000);self.post(9,True,30000)
        self.lib.poll_at(149999);self.assertEqual(self.lib.up_count(),0)
        self.lib.poll_at(150000);self.assertEqual(self.lib.up_count(),1)

    def test_normal_rotary_reverse_noise_is_filtered_for_usb_and_ble(self):
        for source in (0,1):
            with self.subTest(source=source):
                self.lib.reset(1)
                self.post(10,True,source=source)
                self.post(9,True,200000,source=source,delayed=900000)
                self.post(10,True,225000,source=source,delayed=910000)
                self.lib.poll_at(1000000)
                self.assertEqual((self.lib.up_count(),self.lib.down_count()),(0,2))

    def test_normal_rotary_reverse_is_released_after_30_ms(self):
        self.post(10,True);self.post(9,True,200000)
        self.lib.poll_at(229999);self.assertEqual(self.lib.up_count(),0)
        self.lib.poll_at(230000);self.assertEqual(self.lib.up_count(),1)

    def test_normal_rotary_reverse_is_flushed_before_enter(self):
        self.post(10,True);self.post(9,True,200000)
        self.post(7,True,210000);self.post(7,False,215000)
        self.lib.poll_at(300000)
        self.assertEqual((self.lib.up_count(),self.lib.enter_count(),self.lib.enter_up_count()),(1,1,1))

    def test_normal_rotary_disconnect_cancels_only_its_source(self):
        for source in (0,1):
            self.post(10,True,source=source);self.post(9,True,200000,source=source)
        self.post(0,False,210000,source=0)
        self.lib.poll_at(230000)
        self.assertEqual((self.lib.up_count(),self.lib.down_count()),(1,2))

    def test_rotary_pending_reverse_is_flushed_before_knob_enter(self):
        self.post(10,True);self.post(10,True,20000);self.post(9,True,30000)
        self.post(7,True,40000);self.post(7,False,45000)
        self.assertEqual((self.lib.up_count(),self.lib.enter_count(),self.lib.enter_up_count()),(1,1,1))

    def test_rotary_disconnect_cancels_only_its_source(self):
        self.post(10,True);self.post(10,True,20000);self.post(9,True,30000)
        self.post(9,True,40000,source=1);self.post(0,False,45000)
        self.lib.poll_at(200000)
        self.assertEqual((self.lib.up_count(),self.lib.down_count()),(1,2))

    def test_rotary_wake_notch_does_not_reappear_after_filter_deadline(self):
        self.lib.reset(0);self.post(10,True);self.lib.poll_at(100000)
        self.assertEqual((self.lib.wake_count(),self.lib.down_count()),(1,0))
        self.post(9,True,120000)
        self.assertEqual(self.lib.up_count(),1)

    def test_captured_rotary_stream_reaches_dispatch_without_false_reverse_steps(self):
        expected=0;last=0
        for line in (ROOT/'tests/fixtures/d100h_fast_spin.csv').read_text().splitlines():
            if not line or line.startswith('#'):continue
            last,d=map(int,line.split(','));self.post(10 if d>0 else 9,True,last)
            expected+=d>0
        self.lib.poll_at(last+60000)
        self.assertEqual((self.lib.up_count(),self.lib.down_count()),(0,expected))

    def test_keyboard_play_still_acts_on_down(self):
        self.lib.keyboard_play();self.assertEqual(self.lib.play_count(),1)

    def test_all_sources_have_play_hold_and_release_back(self):
        for source in range(3):
            with self.subTest(source=source):
                self.lib.reset(1);self.post(1,True,source=source);self.post(1,False,10,source=source)
                self.assertEqual(self.lib.back_count(),1)
                self.post(2,True,100,source=source);self.lib.poll_at(2000100)
                self.assertEqual(self.lib.off_count(),1)

if __name__=='__main__':unittest.main()
