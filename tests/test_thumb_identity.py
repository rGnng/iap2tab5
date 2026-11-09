"""Production cache migration, append, recovery and cross-library isolation."""
import ctypes as C
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
THUMB=54*54*2

class ThumbIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        path=Path(cls.work.name)
        headers={
            'esp_err.h': '''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_NO_MEM -4
''',
            'esp_log.h': '#define ESP_LOGE(...) ((void)0)\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n',
            'esp_heap_caps.h': '''#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
static inline void *heap_caps_malloc(size_t n,int caps) {(void)caps;return malloc(n);}
static inline size_t heap_caps_get_free_size(int caps) {(void)caps;return 0;}
''',
            'esp_timer.h': '#pragma once\n',
            'freertos/FreeRTOS.h': '''#pragma once
#define portMAX_DELAY 0
#define pdPASS 1
''',
            'freertos/semphr.h': '''#pragma once
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    pthread_mutex_t *m=malloc(sizeof(*m));pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m,&attr);pthread_mutexattr_destroy(&attr);return m;
}
static inline void xSemaphoreTakeRecursive(SemaphoreHandle_t m,int delay) {(void)delay;pthread_mutex_lock(m);}
static inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m) {pthread_mutex_unlock(m);}
''',
            'freertos/task.h': '''#pragma once
static inline void vTaskDelete(void *p) {(void)p;}
static inline int xTaskCreateWithCaps(void (*fn)(void*),const char *name,int stack,void *arg,
    int priority,void *handle,int caps) {
    (void)fn;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;(void)caps;return 0;
}
''',
            'app_config.h': '''#pragma once
#include <stdbool.h>
bool app_config_sd_ready(void);
const char *app_config_library_id(void);
const char *app_config_library_dir(void);
const char *app_config_artwork_dir(void);
''',
        }
        for name,contents in headers.items():
            dest=path/name;dest.parent.mkdir(exist_ok=True,parents=True);dest.write_text(contents)
        (path/'thumb_cache.c').write_text((ROOT/'firmware/main/thumb_cache.c').read_text())
        (path/'driver.c').write_text(r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "thumb_cache.h"
static char library[128],directory[256],covers[256];
bool app_config_sd_ready(void) {return true;}
const char *app_config_library_id(void) {return library;}
const char *app_config_library_dir(void) {return directory;}
const char *app_config_artwork_dir(void) {return covers;}
uint64_t player_legacy_album_pid(uint32_t slot) {return slot<=2?1000+slot:0;}
void setup(const char *dir,const char *id) {
    thumb_cache_close();snprintf(library,sizeof(library),"%s",id);
    snprintf(directory,sizeof(directory),"%s",dir);snprintf(covers,sizeof(covers),"%s/covers",dir);
}
''')
        subprocess.run(['cc','-shared','-fPIC','-D_POSIX_C_SOURCE=200809L',
                        '-I'+str(path),'-I'+str(ROOT/'firmware/main'),
                        '-I'+str(ROOT/'app/backend'),'-I'+str(ROOT/'app/ui'),
                        str(path/'thumb_cache.c'),str(path/'driver.c'),'-pthread',
                        '-o',str(path/'cache.so')],check=True)
        cls.lib=C.CDLL(str(path/'cache.so'))
        cls.lib.setup.argtypes=[C.c_char_p,C.c_char_p]
        cls.lib.thumb_cache_slot.argtypes=[C.c_uint64,C.c_bool]
        cls.lib.thumb_cache_matches.argtypes=[C.c_uint32,C.c_uint64]
        cls.lib.thumb_cache_version.argtypes=[C.c_uint32,C.c_uint64]
        cls.lib.thumb_cache_capture.argtypes=[C.c_char_p,C.c_uint64,C.c_void_p]
        cls.lib.thumb_cache_write.argtypes=[C.c_uint32,C.c_void_p]
        cls.lib.thumb_cache_replace.argtypes=[C.c_uint64,C.c_uint64,C.c_void_p]
        cls.lib.thumb_cache_read.argtypes=[C.c_uint32,C.c_void_p]

    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(self.lib.thumb_cache_close)
        self.root=Path(self.tmp.name)
        self.lib.setup(str(self.root).encode(),b'A')

    def test_legacy_slots_migrate_and_album_count_changes_keep_images(self):
        header=struct.pack('<8sIIII',b'T5THM001',1,2,THUMB,0).ljust(64,b'\0')
        (self.root/'thumbs.bin').write_bytes(header+b'\x11'*THUMB+b'\x22'*THUMB)
        (self.root/'thumbs.idx').write_bytes(b'\x03')
        self.assertEqual(self.lib.thumb_cache_open(2),0)
        self.assertEqual(self.lib.thumb_cache_slot(1001,False),1)
        self.assertEqual(self.lib.thumb_cache_slot(1002,False),2)
        slot=self.lib.thumb_cache_slot(999999,True)
        self.assertEqual(slot,3)
        self.assertEqual(self.lib.thumb_cache_open(3000),0)
        out=(C.c_uint8*THUMB)()
        self.assertTrue(self.lib.thumb_cache_read(1,out))
        self.assertEqual(bytes(out),b'\x11'*THUMB)
        self.lib.thumb_cache_close()
        self.assertEqual(self.lib.thumb_cache_open(1),0)
        self.assertEqual(self.lib.thumb_cache_slot(999999,False),3)
        self.assertTrue(self.lib.thumb_cache_read(2,out))
        self.assertEqual(bytes(out),b'\x22'*THUMB)

    def test_capture_is_idempotent_and_first_afc_import_binds_version(self):
        self.assertEqual(self.lib.thumb_cache_open(0),0)
        image=(C.c_uint8*(360*360*2))(*([0x18]*(360*360*2)))
        self.assertTrue(self.lib.thumb_cache_capture(b'A',2**64-1,image))
        slot=self.lib.thumb_cache_slot(2**64-1,False)
        self.assertTrue(slot)
        self.assertTrue(self.lib.thumb_cache_matches(slot,123))
        self.assertFalse(self.lib.thumb_cache_matches(slot,456))
        self.assertTrue(self.lib.thumb_cache_has(slot))
        self.assertFalse(self.lib.thumb_cache_capture(b'B',7,image))
        self.assertEqual(self.lib.thumb_cache_slot(7,False),0)

    def test_foreign_namespace_is_rejected_and_backup_map_recovers(self):
        self.assertEqual(self.lib.thumb_cache_open(0),0)
        first=self.lib.thumb_cache_slot(71,True)
        self.lib.thumb_cache_slot(72,True)
        self.lib.thumb_cache_close()
        (self.root/'thumbs.map').write_bytes(b'partial')
        self.assertEqual(self.lib.thumb_cache_open(0),0)
        self.assertEqual(self.lib.thumb_cache_slot(71,False),first)
        self.lib.setup(str(self.root).encode(),b'B')
        self.assertNotEqual(self.lib.thumb_cache_open(0),0)
        self.assertTrue((self.root/'thumbs.bin').exists())

    def test_append_many_slots_never_reassigns_old_pid(self):
        self.assertEqual(self.lib.thumb_cache_open(0),0)
        slots={}
        for pid in range(1,140):
            slots[pid]=self.lib.thumb_cache_slot(pid,True)
        self.assertEqual(len(set(slots.values())),139)
        self.lib.thumb_cache_close()
        self.assertEqual(self.lib.thumb_cache_open(1),0)
        for pid,slot in slots.items():
            self.assertEqual(self.lib.thumb_cache_slot(pid,False),slot)

    def test_replacement_keeps_old_pixels_until_map_publication(self):
        self.assertEqual(self.lib.thumb_cache_open(0),0)
        first=self.lib.thumb_cache_slot(777,True)
        old=(C.c_uint8*THUMB)(*([17]*THUMB))
        new=(C.c_uint8*THUMB)(*([34]*THUMB))
        self.assertEqual(self.lib.thumb_cache_write(first,old),0)
        self.assertEqual(self.lib.thumb_cache_replace(777,123,new),0)
        second=self.lib.thumb_cache_slot(777,False)
        self.assertNotEqual(first,second)
        out=(C.c_uint8*THUMB)()
        self.assertTrue(self.lib.thumb_cache_read(first,out))
        self.assertEqual(bytes(out),bytes(old))
        self.assertTrue(self.lib.thumb_cache_read(second,out))
        self.assertEqual(bytes(out),bytes(new))
        self.assertEqual(self.lib.thumb_cache_count(),1)
        self.lib.thumb_cache_close()
        self.assertEqual(self.lib.thumb_cache_open(0),0)
        self.assertEqual(self.lib.thumb_cache_slot(777,False),second)
        self.assertTrue(self.lib.thumb_cache_matches(second,123))

    def test_concurrent_reads_and_writes_never_publish_torn_pixels(self):
        self.assertEqual(self.lib.thumb_cache_open(0),0)
        slot=self.lib.thumb_cache_slot(123,True)
        image=(C.c_uint8*THUMB)(*([17]*THUMB))
        self.assertEqual(self.lib.thumb_cache_write(slot,image),0)
        def writer():
            for value in (17,34)*20:
                data=(C.c_uint8*THUMB)(*([value]*THUMB))
                self.assertEqual(self.lib.thumb_cache_write(slot,data),0)
        def reader():
            output=(C.c_uint8*THUMB)()
            for _ in range(80):
                self.assertTrue(self.lib.thumb_cache_read(slot,output))
                self.assertIn(bytes(output),(b'\x11'*THUMB,b'\x22'*THUMB))
        with ThreadPoolExecutor(max_workers=3) as executor:
            for future in [executor.submit(writer),executor.submit(reader),executor.submit(reader)]:
                future.result()
