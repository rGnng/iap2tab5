"""Verify production artwork copies cannot downgrade a sharper UI/cache image."""
import ctypes as C
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_album_view import function

ROOT = Path(__file__).resolve().parents[1]


class Quality(C.Structure):
    _fields_ = [('width', C.c_uint16), ('height', C.c_uint16)]


class ArtworkQualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        prelude = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include "artwork_quality.h"
#include "artwork_refresh.h"
#define ART_BYTES (360*360*2)
#define portMAX_DELAY 0
#define pdTRUE 1
static unsigned char data[ART_BYTES];
static unsigned char *s_art_cover=data;
static bool s_art_valid;
static void *s_art_lock=(void*)1;
static void *s_state_lock=(void*)2;
static unsigned held_locks, unavailable_locks, refresh_calls;
static bool s_identified=true, s_art_decoding, s_playing;
static void *s_art_pending;
static atomic_bool s_art_transferring;
static uint64_t s_track_key;
static unsigned s_play_status,s_elapsed_ms,s_duration_ms,s_revision;
static char s_title[128],s_artist[128],s_album[128];
static int64_t clock_us;
static int64_t esp_timer_get_time(void) { return clock_us; }
#define ESP_LOGI(...) ((void)0)
static void iap2_host_refresh_now_playing(void) { refresh_calls++; }
static artwork_quality_t s_art_quality;
static artwork_refresh_t s_art_refresh;
static artwork_refresh_snapshot_t s_art_refresh_snapshot;
static artwork_refresh_t refresh_state;
static artwork_refresh_snapshot_t snapshot_state;
void snapshot_begin(uint32_t now) { artwork_refresh_snapshot_begin(&snapshot_state,now); }
bool snapshot_hold(uint32_t now,bool empty,bool identity) {
    return artwork_refresh_snapshot_hold(&snapshot_state,now,empty,identity);
}
bool snapshot_expire(uint32_t now) { return artwork_refresh_snapshot_expire(&snapshot_state,now); }
void refresh_reset(void) { refresh_state=(artwork_refresh_t){0}; }
bool refresh(uint64_t key,uint32_t now,bool busy,unsigned edge) {
    return artwork_refresh_needed(&refresh_state,key,now,busy,edge);
}
static int xSemaphoreTake(void *s,int wait) {
    (void)wait;unsigned bit=s==s_art_lock?1:2;
    if(unavailable_locks&bit)return 0;
    assert(!(held_locks&bit));held_locks|=bit;return 1;
}
static void xSemaphoreGive(void *s) {
    unsigned bit=s==s_art_lock?1:2;assert(held_locks&bit);held_locks&=~bit;
}
void setup(unsigned width,unsigned height,bool valid) {
    s_art_quality=(artwork_quality_t){width,height};s_art_valid=valid;
    memset(data,0x5a,sizeof(data));
}
'''
        source = (ROOT/'firmware/main/iap2_host.c').read_text()
        source = source.replace('\nbool iap2_host_artwork_copy(',
                                '\nstatic bool iap2_host_artwork_copy(')
        driver = r'''
void poll_reset(void) {
    s_art_refresh=(artwork_refresh_t){0};s_art_refresh_snapshot=(artwork_refresh_snapshot_t){0};
    refresh_calls=held_locks=unavailable_locks=0;
}
unsigned poll_run(uint64_t key,uint32_t now,unsigned edge,unsigned busy,unsigned locks) {
    s_track_key=key;clock_us=(int64_t)now*1000;
    s_art_quality=(artwork_quality_t){edge,edge};s_art_valid=edge!=0;
    s_art_decoding=(busy&1)!=0;s_art_pending=(busy&2)?data:NULL;
    atomic_store(&s_art_transferring,(busy&4)!=0);unavailable_locks=locks;
    artwork_refresh_poll();assert(!held_locks);return refresh_calls;
}
'''
        code = (prelude + function(source, 'iap2_host_artwork_copy').removeprefix('static ') +
                function(source, 'artwork_refresh_poll') + driver)
        (work/'quality.c').write_text(code)
        subprocess.run(['cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'app/backend'), str(work/'quality.c'),
                        '-o', str(work/'quality.so')], check=True)
        cls.lib = C.CDLL(str(work/'quality.so'))
        cls.lib.setup.argtypes = [C.c_uint, C.c_uint, C.c_bool]
        cls.lib.iap2_host_artwork_copy.argtypes = [C.c_void_p, C.c_size_t, Quality, C.POINTER(Quality)]
        cls.lib.iap2_host_artwork_copy.restype = C.c_bool
        cls.lib.refresh.argtypes = [C.c_uint64, C.c_uint32, C.c_bool, C.c_uint]
        cls.lib.refresh.restype = C.c_bool
        cls.lib.snapshot_begin.argtypes = [C.c_uint32]
        cls.lib.snapshot_hold.argtypes = [C.c_uint32, C.c_bool, C.c_bool]
        cls.lib.snapshot_hold.restype = C.c_bool
        cls.lib.snapshot_expire.argtypes = [C.c_uint32]
        cls.lib.snapshot_expire.restype = C.c_bool
        cls.lib.poll_run.argtypes = [C.c_uint64, C.c_uint32, C.c_uint, C.c_uint, C.c_uint]
        cls.lib.poll_run.restype = C.c_uint

    def setUp(self):
        self.lib.refresh_reset()

    def test_usb_poll_wires_recovery_without_waiting_on_locks_or_transfers(self):
        self.lib.poll_reset()
        poll = self.lib.poll_run
        self.assertEqual(poll(10,100000,120,0,0),0)
        self.assertEqual(poll(10,102000,120,0,0),1)
        self.assertEqual(poll(0,102100,0,0,0),1)
        self.assertEqual(poll(10,105000,120,0,0),1)
        self.assertEqual(poll(11,106000,0,0,0),1)
        for now,locks in ((108000,1),(108100,2)):
            self.assertEqual(poll(11,now,0,0,locks),1)
        for now,busy in ((108200,1),(108300,2),(108400,4)):
            self.assertEqual(poll(11,now,0,busy,0),1)
        self.assertEqual(poll(11,108500,0,0,0),2)
        self.assertEqual(poll(12,109000,600,0,0),2)
        self.assertEqual(poll(12,111000,600,0,0),2)

    def test_missing_or_small_artwork_refreshes_once_after_settling(self):
        for edge in (0,120,200):
            self.lib.refresh_reset()
            self.assertFalse(self.lib.refresh(10,100,False,edge))
            self.assertFalse(self.lib.refresh(10,2099,False,edge))
            self.assertTrue(self.lib.refresh(10,2100,False,edge))
            self.assertFalse(self.lib.refresh(10,3000,False,edge))
            self.assertFalse(self.lib.refresh(10,60000,False,edge))

    def test_transient_empty_snapshot_is_held_until_real_identity_arrives(self):
        self.lib.snapshot_begin(100)
        self.assertTrue(self.lib.snapshot_hold(110,True,False))
        self.assertFalse(self.lib.snapshot_hold(120,False,False))  # playback-only update
        self.assertFalse(self.lib.snapshot_hold(150,False,True))
        self.assertFalse(self.lib.snapshot_expire(2000))
        self.assertFalse(self.lib.snapshot_hold(2100,True,False))

    def test_genuine_no_track_state_is_not_hidden_indefinitely(self):
        self.lib.snapshot_begin(100)
        self.assertTrue(self.lib.snapshot_hold(110,True,False))
        self.assertFalse(self.lib.snapshot_hold(150,False,False))
        self.assertFalse(self.lib.snapshot_expire(1099))
        self.assertTrue(self.lib.snapshot_expire(1100))
        self.assertFalse(self.lib.snapshot_expire(1200))
        self.lib.snapshot_begin(2000)
        self.assertFalse(self.lib.snapshot_hold(3000,True,False))
        self.assertFalse(self.lib.snapshot_expire(3100))  # empty already applied normally

    def test_refresh_pid_zero_does_not_rearm_the_same_track(self):
        self.lib.refresh(10,100,False,120)
        self.assertTrue(self.lib.refresh(10,2100,False,120))
        self.assertFalse(self.lib.refresh(0,2200,False,0))
        self.assertFalse(self.lib.refresh(10,2300,False,0))
        self.assertFalse(self.lib.refresh(10,6000,False,120))

    def test_scrolling_tracks_debounces_and_later_high_resolution_cancels_retry(self):
        self.lib.refresh(10,100,False,0)
        self.lib.refresh(11,1000,False,120)
        self.assertFalse(self.lib.refresh(11,2999,False,120))
        self.assertFalse(self.lib.refresh(11,3000,False,600))
        self.assertFalse(self.lib.refresh(11,6000,False,120))
        self.assertFalse(self.lib.refresh(12,6500,False,0))
        self.assertTrue(self.lib.refresh(12,8500,False,0))

    def test_active_transfer_or_decoder_defers_refresh(self):
        self.lib.refresh(10,100,False,0)
        self.assertFalse(self.lib.refresh(10,2100,True,0))
        self.assertFalse(self.lib.refresh(10,2400,True,0))
        self.assertTrue(self.lib.refresh(10,2500,False,0))

    def test_settling_timer_handles_uptime_wrap(self):
        self.assertFalse(self.lib.refresh(10,0xffffff00,False,0))
        self.assertFalse(self.lib.refresh(10,1000,False,0))
        self.assertTrue(self.lib.refresh(10,1800,False,0))

    def copy(self, source, minimum, valid=True, length=360*360*2):
        self.lib.setup(*source, valid)
        pixels = (C.c_uint8*(360*360*2))(*([0xa5]*(360*360*2)))
        actual = Quality(999,999)
        result = self.lib.iap2_host_artwork_copy(pixels, length, Quality(*minimum), C.byref(actual))
        return result, bytes(pixels), (actual.width,actual.height)

    def test_thumbnail_cannot_overwrite_a_sharper_cover(self):
        ok,pixels,actual = self.copy((120,120),(600,600))
        self.assertFalse(ok)
        self.assertEqual(actual,(120,120))
        self.assertEqual(pixels,bytes([0xa5])*len(pixels))

    def test_upgrades_and_same_size_updates_are_copied(self):
        for source,minimum in (((600,600),(120,120)),((500,500),(500,500)),((120,120),(0,0))):
            ok,pixels,actual = self.copy(source,minimum)
            self.assertTrue(ok)
            self.assertEqual(actual,source)
            self.assertEqual(pixels,bytes([0x5a])*len(pixels))

    def test_shorter_edge_limits_quality_for_non_square_sources(self):
        ok,_,_ = self.copy((600,120),(200,200))
        self.assertFalse(ok)
        ok,_,_ = self.copy((500,490),(120,120))
        self.assertTrue(ok)

    def test_missing_or_short_copy_preserves_the_destination(self):
        for valid,length in ((False,360*360*2),(True,360*360*2-1)):
            ok,pixels,actual = self.copy((600,600),(0,0),valid,length)
            self.assertFalse(ok)
            self.assertEqual(actual,(0,0))
            self.assertEqual(pixels,bytes([0xa5])*len(pixels))


if __name__ == '__main__':
    unittest.main()
