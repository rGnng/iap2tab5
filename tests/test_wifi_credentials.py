"""Exercise the production SD/NVS Wi-Fi credential path with file and NVS faults."""
import ctypes as C
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WiFiCredentialsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        cls.path = work/'wifi.txt'
        production = (ROOT/'firmware/main/app_config.c').read_text()
        production = production[production.index('typedef struct {\n    char ssid[33];'):]
        boundary = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static bool sd_ready;
static unsigned commits,sets,closes;
static int failure;
static uint8_t saved[256],pending[256];
static size_t saved_size,pending_size;
static bool app_config_sd_ready(void) { return sd_ready; }
static int nvs_open(const char *name,int mode,nvs_handle_t *handle) {
    assert(strcmp(name,"wifi_backup")==0);
    if((failure==1 && mode==NVS_READWRITE) || failure==5) return ESP_FAIL;
    if(mode==NVS_READONLY && !saved_size) return ESP_ERR_NOT_FOUND;
    *handle=mode;return ESP_OK;
}
static int nvs_get_blob(int handle,const char *key,void *out,size_t *size) {
    (void)handle;assert(strcmp(key,"station")==0);
    if(!saved_size) return ESP_ERR_NOT_FOUND;
    if(*size<saved_size) return ESP_ERR_INVALID_SIZE;
    memcpy(out,saved,saved_size);*size=saved_size;return ESP_OK;
}
static int nvs_set_blob(int handle,const char *key,const void *data,size_t size) {
    assert(handle==NVS_READWRITE && strcmp(key,"station")==0);
    sets++;if(failure==2) return ESP_FAIL;
    assert(size<=sizeof(pending));memcpy(pending,data,size);pending_size=size;return ESP_OK;
}
static int nvs_commit(int handle) {
    assert(handle==NVS_READWRITE);commits++;
    if(failure==3) return ESP_FAIL;
    memcpy(saved,pending,pending_size);saved_size=pending_size;return ESP_OK;
}
static void nvs_close(int handle) { (void)handle;closes++; }
static int io_ferror(FILE *f) { return failure==4?1:ferror(f); }
#define ferror io_ferror
'''
        exports = r'''
void reset(void) { sd_ready=true;commits=sets=closes=0;failure=0;saved_size=pending_size=0; }
void set_sd(int ready) { sd_ready=ready; }
void set_failure(int value) { failure=value; }
unsigned commit_count(void) { return commits; }
unsigned set_count(void) { return sets; }
void corrupt(int kind) {
    memset(saved,0,sizeof(saved));saved_size=sizeof(wifi_credentials_t);
    strcpy((char*)saved,"saved-network");
    if(kind==0) saved_size--;
    if(kind==1) memset(saved,'x',sizeof(((wifi_credentials_t*)0)->ssid));
    if(kind==2) memset(saved+33,'x',65);
    if(kind==3) saved[0]=0;
    if(kind==4) saved_size++;
}
'''
        source = '#define APP_WIFI_CRED_PATH "'+str(cls.path)+'"\n'+boundary+production+exports
        (work/'credentials.c').write_text(source)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        str(work/'credentials.c'),'-o',str(work/'credentials.so')],check=True)
        cls.lib = C.CDLL(str(work/'credentials.so'))
        cls.lib.app_config_read_wifi_credentials.argtypes = [C.c_void_p,C.c_size_t,C.c_void_p,C.c_size_t]

    def setUp(self):
        self.lib.reset()
        self.path.unlink(missing_ok=True)

    def read(self, ssid_size=33, password_size=65):
        ssid=C.create_string_buffer(ssid_size)
        password=C.create_string_buffer(password_size)
        ssid.value=b'?'[:ssid_size-1];password.value=b'?'[:password_size-1]
        err=self.lib.app_config_read_wifi_credentials(ssid,ssid_size,password,password_size)
        return err,ssid.value,password.value

    def seed(self):
        self.path.write_bytes(b'Original network\nold-password\n')
        self.assertEqual(self.read(),(0,b'Original network',b'old-password'))

    def test_no_card_or_backup_returns_empty_outputs(self):
        self.lib.set_sd(0)
        err,ssid,password=self.read()
        self.assertNotEqual(err,0)
        self.assertEqual((ssid,password),(b'',b''))

    def test_sd_credentials_survive_an_unmounted_card(self):
        self.seed();self.lib.set_sd(0)
        self.path.write_bytes(b'Unreadable new network\nunused-password\n')
        self.assertEqual(self.read(),(0,b'Original network',b'old-password'))
        self.assertEqual(self.lib.commit_count(),1)

    def test_sd_file_overwrites_nvs_as_one_pair(self):
        self.seed()
        self.path.write_bytes(b'New network\r\nnew-password\r\n')
        self.assertEqual(self.read(),(0,b'New network',b'new-password'))
        self.lib.set_sd(0)
        self.assertEqual(self.read(),(0,b'New network',b'new-password'))
        self.assertEqual(self.lib.commit_count(),2)

    def test_unchanged_file_does_not_write_flash_again(self):
        self.seed()
        for _ in range(10):self.assertEqual(self.read()[0],0)
        self.assertEqual(self.lib.commit_count(),1)
        self.assertEqual(self.lib.set_count(),1)

    def test_missing_empty_or_overlong_file_keeps_previous_backup(self):
        self.seed();self.path.unlink()
        self.assertEqual(self.read(),(0,b'Original network',b'old-password'))
        for data in (b'',b'\r\n \n',b'x'*33+b'\npassword\n',b'New\n'+b'x'*65+b'\n'):
            with self.subTest(data_length=len(data)):
                self.path.write_bytes(data)
                self.assertEqual(self.read(),(0,b'Original network',b'old-password'))
        self.assertEqual(self.lib.commit_count(),1)

    def test_file_io_error_does_not_replace_backup(self):
        self.seed();self.path.write_bytes(b'New network\nnew-password\n')
        self.lib.set_failure(4)
        self.assertEqual(self.read(),(0,b'Original network',b'old-password'))
        self.assertEqual(self.lib.commit_count(),1)

    def test_nvs_failure_does_not_block_valid_sd_credentials(self):
        for failure in (1,2,3,5):
            with self.subTest(failure=failure):
                self.lib.reset();self.lib.set_failure(failure)
                self.path.write_bytes(b'Working network\nworking-password\n')
                self.assertEqual(self.read(),(0,b'Working network',b'working-password'))

    def test_corrupt_backup_is_rejected_without_exposing_partial_values(self):
        self.lib.set_sd(0)
        for kind in range(5):
            with self.subTest(kind=kind):
                self.lib.corrupt(kind)
                err,ssid,password=self.read()
                self.assertNotEqual(err,0)
                self.assertEqual((ssid,password),(b'',b''))

    def test_optional_or_short_output_still_saves_complete_credentials(self):
        self.path.write_bytes(b'Whole network name\nwhole-password\n')
        ssid=C.create_string_buffer(6)
        self.assertEqual(self.lib.app_config_read_wifi_credentials(ssid,6,None,0),0)
        self.assertEqual(ssid.value,b'Whole')
        self.assertEqual(self.read(6,6),(0,b'Whole',b'whole'))
        self.lib.set_sd(0)
        self.assertEqual(self.read(),(0,b'Whole network name',b'whole-password'))
        self.assertEqual(self.lib.commit_count(),1)

    def test_open_network_clears_old_password(self):
        self.seed();self.path.write_bytes(b'Open network\n\n')
        self.assertEqual(self.read(),(0,b'Open network',b''))
        self.lib.set_sd(0)
        self.assertEqual(self.read(),(0,b'Open network',b''))

    def test_utf8_ssid_and_full_length_key_are_preserved(self):
        ssid=('海'*10+'AB').encode()
        password=b'1'*64
        self.path.write_bytes(ssid+b'\n'+password+b'\n')
        self.assertEqual(self.read(),(0,ssid,password))
        self.lib.set_sd(0)
        self.assertEqual(self.read(),(0,ssid,password))

    def test_invalid_arguments_do_not_touch_nvs(self):
        self.path.write_bytes(b'Network\npassword\n')
        self.assertNotEqual(self.lib.app_config_read_wifi_credentials(None,33,None,0),0)
        buffer=C.create_string_buffer(33)
        self.assertNotEqual(self.lib.app_config_read_wifi_credentials(buffer,0,None,0),0)
        self.assertEqual(self.lib.set_count(),0)


if __name__ == '__main__':
    unittest.main()
