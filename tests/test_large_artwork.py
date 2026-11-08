"""Exercise large-artwork fallback without allocating whole compressed covers."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_album_view import function
ROOT=Path(__file__).resolve().parents[1]

class LargeArtworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name)
        prelude=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
static const uint8_t *contents;
static size_t size;
static int failure, marked;
static char dir[128];
static uint32_t s_last_bytes;
static const char *artwork_dir(void) { return dir; }
static void art_set_info(int w,int h,const char *kind) { (void)w;(void)h;(void)kind; }
static void thumb_cache_set_original(uint32_t id) { marked=id; }
static esp_err_t afc_client_stat(const char *path,uint64_t *expected,char *type,size_t len) {
    (void)path;(void)type;(void)len;*expected=size;return failure==1?ESP_FAIL:ESP_OK;
}
static esp_err_t afc_client_copy_file(const char *remote,const char *local,uint64_t expected,char *hash,size_t hash_len) {
    (void)remote;(void)expected;(void)hash;(void)hash_len;
    FILE *f=fopen(local,"wb");if(!f)return ESP_FAIL;
    size_t n=size-(failure==2?1:0);fwrite(contents,1,n,f);fclose(f);
    return failure==3?ESP_FAIL:ESP_OK;
}
void setup(const char *path,const void *data,size_t length,int fail) {
    snprintf(dir,sizeof(dir),"%s",path);contents=data;size=length;failure=fail;marked=0;
}
int original(void) { return marked; }
'''
        source=(ROOT/'firmware/main/lockdown_client.c').read_text()
        code=prelude+function(source,'artwork_ext_mem')+function(source,'artwork_download_large').replace('static bool artwork_download_large','bool artwork_download_large')
        (p/'large.c').write_text(code)
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(p/'large.c'),'-o',str(p/'large.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'large.so'));cls.lib.setup.argtypes=[ctypes.c_char_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_int]
        cls.lib.artwork_download_large.argtypes=[ctypes.c_uint64,ctypes.c_char_p];cls.lib.artwork_download_large.restype=ctypes.c_bool

    def setUp(self):
        self.files=tempfile.TemporaryDirectory();self.addCleanup(self.files.cleanup);self.dir=Path(self.files.name)

    def run_copy(self,data,failure=0):
        self.lib.setup(str(self.dir).encode(),data,len(data),failure)
        return self.lib.artwork_download_large(495,b'/artwork/image')

    def test_complete_image_published_for_conversion(self):
        data=b'\xff\xd8\xff'+b'x'*100
        self.assertTrue(self.run_copy(data));self.assertEqual(self.lib.original(),495)
        self.assertEqual((self.dir/'495.jpg').read_bytes(),data)
        self.assertFalse((self.dir/'495.art').exists())

    def test_failed_or_short_copy_is_not_marked_original(self):
        for failure in (1,2,3):
            self.assertFalse(self.run_copy(b'\xff\xd8\xff'+b'x'*100,failure))
            self.assertEqual(self.lib.original(),0)
            self.assertEqual(list(self.dir.iterdir()),[])

    def test_unknown_image_and_failed_rename_leave_no_partial_file(self):
        self.assertFalse(self.run_copy(b'not an image'))
        self.assertEqual(list(self.dir.iterdir()),[])
        (self.dir/'495.jpg').mkdir()
        self.assertFalse(self.run_copy(b'\xff\xd8\xff'+b'x'*100))
        self.assertEqual(self.lib.original(),0)
        self.assertFalse((self.dir/'495.art').exists())
