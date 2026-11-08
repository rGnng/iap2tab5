"""Exercise pair-record replacement with real files and injected I/O failures."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_album_view import function
ROOT=Path(__file__).resolve().parents[1]

class PairRecordTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):raise unittest.SkipTest('C compiler required')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name);cls.path=p/'pair.plist'
        prelude=r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_NO_MEM -4
#define MAX_PAIR_LEN 262144
#define MALLOC_CAP_SPIRAM 1
#define portMAX_DELAY 0
#define ESP_LOGI(...) ((void)0)
static void *s_pair_lock=(void*)1;
static char *s_pair_ram;
static size_t s_pair_len;
static int failure,renames;
static void xSemaphoreTake(void*p,int n){(void)p;(void)n;}
static void xSemaphoreGive(void*p){(void)p;}
static void *heap_caps_malloc(size_t n,int caps){(void)caps;return malloc(n);}
static void set_status(const char*s){(void)s;}
static FILE *io_open(const char*p,const char*m){return failure==1?NULL:fopen(p,m);}
static size_t io_write(const void*p,size_t z,size_t n,FILE*f){return failure==2?0:fwrite(p,z,n,f);}
static int io_close(FILE*f){int r=fclose(f);return failure==3?-1:r;}
static int io_rename(const char*a,const char*b){renames++;return (failure==4 && renames==1)||(failure==5 && renames==2)?-1:rename(a,b);}
#define fopen io_open
#define fwrite io_write
#define fclose io_close
#define rename io_rename
'''
        driver=r'''
void reset(int f){failure=f;renames=0;free(s_pair_ram);s_pair_ram=strdup("previous");s_pair_len=8;}
const char *memory_record(void){return s_pair_ram;}
'''
        source=(ROOT/'firmware/main/lockdown_client.c').read_text()
        (p/'record.c').write_text('#define PAIR_PATH_SPIFS "'+str(cls.path)+'"\n'+prelude+function(source.replace('esp_err_t lockdown_client_set_pair_record', 'static esp_err_t lockdown_client_set_pair_record'),'lockdown_client_set_pair_record').replace('static esp_err_t lockdown_client_set_pair_record','esp_err_t lockdown_client_set_pair_record')+driver)
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(p/'record.c'),'-o',str(p/'record.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'record.so'))
        cls.lib.lockdown_client_set_pair_record.argtypes=[ctypes.c_char_p,ctypes.c_size_t]
        cls.lib.memory_record.restype=ctypes.c_char_p
        cls.record=b'<plist><dict><key>Fixture</key><string>'+b'x'*100+b'</string></dict></plist>'

    def setup_record(self,failure=0):
        self.path.write_bytes(b'previous');self.lib.reset(failure)
        for suffix in ('.bak','.tmp'):
            self.path.with_name(self.path.name+suffix).unlink(missing_ok=True)

    def test_success_publishes_record_and_keeps_previous_backup(self):
        self.setup_record()
        self.assertEqual(self.lib.lockdown_client_set_pair_record(self.record,len(self.record)),0)
        self.assertEqual(self.path.read_bytes(),self.record)
        self.assertEqual(self.path.with_suffix('.plist.bak').read_bytes(),b'previous')
        self.assertEqual(self.lib.memory_record(),self.record)

    def test_failed_write_or_rename_preserves_disk_and_ram_identity(self):
        for failure in range(1,6):
            with self.subTest(failure=failure):
                self.setup_record(failure)
                self.assertNotEqual(self.lib.lockdown_client_set_pair_record(self.record,len(self.record)),0)
                self.assertEqual(self.path.read_bytes(),b'previous')
                self.assertEqual(self.lib.memory_record(),b'previous')

    def test_first_pair_without_existing_file(self):
        self.setup_record();self.path.unlink()
        self.assertEqual(self.lib.lockdown_client_set_pair_record(self.record,len(self.record)),0)
        self.assertEqual(self.path.read_bytes(),self.record)

    def test_invalid_upload_does_not_overwrite_working_record(self):
        self.setup_record();data=b'not a plist'*20
        self.assertNotEqual(self.lib.lockdown_client_set_pair_record(data,len(data)),0)
        self.assertEqual(self.path.read_bytes(),b'previous')
