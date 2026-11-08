"""Run the production sidecar writer against real files and injected I/O races."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from test_album_view import function

ROOT = Path(__file__).resolve().parents[1]
PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ALBUM_SEEN_SLOTS 8
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef struct { uint64_t pid; uint32_t seq; } album_seen_t;
typedef struct __attribute__((packed)) { char magic[8]; uint32_t version,count; } album_seen_header_t;
typedef struct __attribute__((packed)) { uint64_t pid; uint32_t seq; } album_seen_record_t;
static album_seen_t album_seen[8];
static bool album_seen_dirty;
static uint32_t album_seen_revision;
static int64_t album_seen_dirty_since_us;
static int locked,inject;
static char path[512];
static void album_seen_lock_take(void){assert(!locked);locked=1;}
static void album_seen_lock_give(void){assert(locked);locked=0;}
static bool app_config_sd_ready(void){return true;}
static int64_t esp_timer_get_time(void){return 10000000;}
static void album_seen_path(char*p,size_t n){snprintf(p,n,"%s",path);}
static FILE *checked_open(const char*p,const char*m){
    assert(!locked);if(inject==2)return NULL;return fopen(p,m);
}
static size_t checked_write(const void*p,size_t z,size_t n,FILE*f){
    assert(!locked);
    if(inject==1){inject=0;album_seen[2]=(album_seen_t){333,3};album_seen_revision++;album_seen_dirty=true;}
    if(inject==3)return 0;
    return fwrite(p,z,n,f);
}
static int checked_close(FILE*f){assert(!locked);return fclose(f);}
static int checked_rename(const char*a,const char*b){assert(!locked);if(inject==4)return -1;return rename(a,b);}
#define fopen checked_open
#define fwrite checked_write
#define fclose checked_close
#define rename checked_rename
'''
DRIVER = r'''
void setup(const char*p,int i){
    snprintf(path,sizeof(path),"%s",p);inject=i;locked=0;album_seen_revision=1;album_seen_dirty=true;
    album_seen_dirty_since_us=0;memset(album_seen,0,sizeof(album_seen));
    album_seen[0]=(album_seen_t){111,1};album_seen[1]=(album_seen_t){222,2};
}
void save(void){album_seen_save(true);}
int dirty(void){return album_seen_dirty;}
void clear_failure(void){inject=0;}
'''


class AlbumSeenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc=shutil.which('cc')
        if not cc:raise unittest.SkipTest('C compiler unavailable')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name)
        source=(ROOT/'app/backend/mock_player.c').read_text()
        (p/'seen.c').write_text(PRELUDE+function(source,'album_seen_save')+DRIVER)
        subprocess.run([cc,'-shared','-fPIC','-Wall','-Wextra','-Werror',str(p/'seen.c'),'-o',str(p/'seen.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'seen.so'));cls.lib.setup.argtypes=[ctypes.c_char_p,ctypes.c_int]

    def setup_file(self,inject=0):
        work=tempfile.TemporaryDirectory();self.addCleanup(work.cleanup)
        path=Path(work.name)/'album_seen.bin'
        self.lib.setup(str(path).encode(),inject)
        return path

    def records(self,path):
        data=path.read_bytes();magic,version,count=struct.unpack('<8sII',data[:16])
        self.assertEqual((magic,version),(b'T5SEE001',1));self.assertEqual(len(data),16+12*count)
        return list(struct.iter_unpack('<QI',data[16:]))

    def test_save_snapshot_without_holding_album_lock_during_io(self):
        p=self.setup_file();self.lib.save()
        self.assertEqual(self.records(p),[(111,1),(222,2)]);self.assertFalse(self.lib.dirty())

    def test_album_arriving_during_flush_is_saved_on_next_pass(self):
        p=self.setup_file(1);self.lib.save()
        self.assertTrue(self.lib.dirty());self.assertEqual(len(self.records(p)),2)
        self.lib.save();self.assertFalse(self.lib.dirty())
        self.assertEqual(self.records(p),[(111,1),(222,2),(333,3)])

    def test_io_failure_keeps_unsaved_records_pending(self):
        for failure in (2,3,4):
            with self.subTest(failure=failure):
                p=self.setup_file(failure);self.lib.save();self.assertTrue(self.lib.dirty())
                self.lib.clear_failure();self.lib.save();self.assertFalse(self.lib.dirty())
                self.assertEqual(self.records(p),[(111,1),(222,2)])

if __name__=='__main__':unittest.main()
