"""A missing SD card must not create a cache-revision retry loop."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_album_view import function

ROOT=Path(__file__).resolve().parents[1]

class LibraryWithoutSDTests(unittest.TestCase):
    def test_cover_refresh_skips_io_without_sd_and_updates_when_ready(self):
        source=(ROOT/'app/backend/live_catalog.inc').read_text()
        prelude=r'''
#include <stdbool.h>
#include <stdint.h>
#define ESP_PLATFORM 1
#define PLAYER_REAL_COVER_BASE 1000
static struct { uint64_t album_pid; uint16_t cover_index; } albums[2];
static int album_count=2,reads;
static bool ready;
static bool app_config_sd_ready(void) {return ready;}
static uint32_t thumb_cache_slot(uint64_t pid,bool create) {
    (void)create;reads++;return (uint32_t)pid;
}
'''
        driver=r'''
void refresh(int sd) {
    ready=sd;reads=0;
    for(int i=0;i<2;i++) {albums[i].album_pid=i+1;albums[i].cover_index=1400+i;}
    live_covers();
}
int value(int index) {return index<0?reads:albums[index].cover_index;}
'''
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'sd.c').write_text(prelude+function(source,'live_covers')+driver)
            subprocess.run(['cc','-shared','-fPIC',str(root/'sd.c'),'-o',str(root/'sd.so')],check=True)
            library=ctypes.CDLL(str(root/'sd.so'))
            library.refresh(0)
            self.assertEqual([library.value(i) for i in (-1,0,1)],[0,1400,1401])
            library.refresh(1)
            self.assertEqual([library.value(i) for i in (-1,0,1)],[2,1001,1002])
