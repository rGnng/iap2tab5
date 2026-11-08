"""Verify optimized I/O keeps checksums, AFC framing and JPEG detection correct."""
import ctypes
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from test_album_view import function
ROOT = Path(__file__).resolve().parents[1]

class SyncIOTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.tmp.cleanup)
        p = Path(cls.tmp.name)
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <mbedtls/sha256.h>
#define mbedtls_sha256_starts mbedtls_sha256_starts_ret
#define mbedtls_sha256_update mbedtls_sha256_update_ret
#define mbedtls_sha256_finish mbedtls_sha256_finish_ret
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_INVALID_RESPONSE -4
#define ESP_ERR_INVALID_SIZE -5
#define ESP_ERR_NO_MEM -6
#define ESP_ERR_TIMEOUT -7
#define ESP_ERR_INVALID_ARG -8
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static int alloc_fail, read_fail;
static size_t max_read, allocation_size;
static uint64_t remote_size;
static void *heap_caps_aligned_alloc(size_t alignment, size_t size, int caps) {
    (void)caps; void *p = NULL; allocation_size=size;
    if(alloc_fail || posix_memalign(&p, alignment, size)) return NULL;
    return p;
}
#define heap_caps_free free
static ssize_t fake_read(int fd, void *p, size_t n) {
    if(n > max_read) max_read=n;
    return read_fail ? -1 : read(fd,p,n);
}
#define read fake_read
void setup_io(int a,int r) { alloc_fail=a;read_fail=r;max_read=0; }
size_t largest_read(void) { return max_read; }
void remote_file_size(uint64_t n) { remote_size=n; allocation_size=0; }
size_t allocated_size(void) { return allocation_size; }
static esp_err_t afc_client_stat(const char *p,uint64_t *n,char *t,size_t z) {
    (void)p;(void)t;(void)z;*n=remote_size;return ESP_OK;
}
#define AFC_MEMORY_FILE_LIMIT (8*1024*1024)
#define AFC_FOPEN_RDONLY 1
#define AFC_OP_FILE_OPEN 13
#define AFC_OP_FILE_OPEN_RES 14
#define AFC_OP_FILE_READ 15
#define AFC_OP_FILE_CLOSE 20
#define AFC_OP_DATA 2
#define AFC_MAGIC "CFA6LPAA"
#define AFC_MAGIC_LEN 8
#define AFC_HDR_LEN 40
#define AFC_RECV_TIMEOUT 15000
static void *s_conn=(void*)1;
static uint64_t s_packet_num;
static uint8_t transmitted[4096], response[40];
static int tx_count, tx_bytes;
static bool usbmux_conn_is_open(void *c) { return c!=NULL; }
static int usbmux_conn_send(void *c,const void *p,size_t n) {
    (void)c; if(tx_bytes+n>sizeof(transmitted)) return -1;
    memcpy(transmitted+tx_bytes,p,n); tx_bytes+=n; tx_count++; return n;
}
static int recv_exact(uint8_t *p,size_t n,uint32_t t) {
    (void)t; if(n!=40)return ESP_FAIL; memcpy(p,response,40);return ESP_OK;
}
static int usbmux_conn_recv(void *c,void *p,size_t n,uint32_t t) {
    (void)c;(void)p;(void)n;(void)t;return -1;
}
static void set_status(const char *s) { (void)s; }
int sends(void) { return tx_count; }
const void *sent(void) { return transmitted; }
'''
        config=(ROOT/'firmware/main/app_config.c').read_text()
        afc=(ROOT/'firmware/main/afc_client.c').read_text()
        lock=(ROOT/'firmware/main/lockdown_client.c').read_text()
        code=prelude
        for source,name,rettype in ((config,'app_config_hash_file','esp_err_t'),(afc,'rd_le64','uint64_t'),(afc,'wr_le64','void'),(afc,'afc_client_raw','esp_err_t'),(afc,'afc_client_read_all','esp_err_t'),(lock,'jpeg_file_is_progressive','bool')):
            # Export only public functions and the JPEG probe for ctypes.
            if name in ('app_config_hash_file','afc_client_raw','afc_client_read_all'):
                source=source.replace(rettype+' '+name, 'static '+rettype+' '+name)
            code+=function(source,name).replace('static '+rettype+' '+name,rettype+' '+name)
        code+=r'''
void setup_packet(void) {
    s_packet_num=0;tx_count=tx_bytes=0;memset(response,0,sizeof(response));
    memcpy(response,AFC_MAGIC,8);wr_le64(response+8,40);wr_le64(response+16,40);wr_le64(response+32,2);
}
'''
        (p/'io.c').write_text(code)
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror',str(p/'io.c'),'-lmbedcrypto','-o',str(p/'io.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'io.so'))
        cls.lib.app_config_hash_file.argtypes=[ctypes.c_char_p,ctypes.POINTER(ctypes.c_uint64),ctypes.c_void_p,ctypes.c_size_t]
        cls.lib.jpeg_file_is_progressive.argtypes=[ctypes.c_char_p];cls.lib.jpeg_file_is_progressive.restype=ctypes.c_bool
        cls.lib.afc_client_raw.argtypes=[ctypes.c_uint64,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_void_p]
        cls.lib.sent.restype=ctypes.c_void_p
        cls.lib.remote_file_size.argtypes=[ctypes.c_uint64]
        cls.lib.afc_client_read_all.argtypes=[ctypes.c_char_p,ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_size_t)]

    def test_hash_matches_sha256_across_chunk_boundaries_and_low_memory(self):
        path=Path(self.tmp.name)/'input'
        for alloc_fail in (0,1):
            for length in (0,1,4095,4096,65535,65536,65537,180231):
                with self.subTest(alloc_fail=alloc_fail,length=length):
                    data=(bytes(range(256))*705)[:length];path.write_bytes(data)
                    self.lib.setup_io(alloc_fail,0)
                    size=ctypes.c_uint64();digest=ctypes.create_string_buffer(65)
                    self.assertEqual(self.lib.app_config_hash_file(str(path).encode(),ctypes.byref(size),digest,65),0)
                    self.assertEqual(size.value,len(data));self.assertEqual(digest.value.decode(),hashlib.sha256(data).hexdigest())
                    self.assertEqual(self.lib.largest_read(),4096 if alloc_fail else 65536)

    def test_hash_read_failure_is_not_success(self):
        path=Path(self.tmp.name)/'read-error';path.write_bytes(b'contents')
        self.lib.setup_io(0,1)
        self.assertNotEqual(self.lib.app_config_hash_file(str(path).encode(),None,None,0),0)
        self.lib.setup_io(0,0)
        self.assertNotEqual(self.lib.app_config_hash_file(b'/nonexistent-fixture',None,None,0),0)

    def test_request_combines_small_payloads_and_preserves_large_payloads(self):
        for length in (0,16,512,513,2048):
            with self.subTest(length=length):
                payload=(bytes(range(256))*8)[:length];self.lib.setup_packet()
                self.assertEqual(self.lib.afc_client_raw(15,payload,length,None,None,None),0)
                packet=ctypes.string_at(self.lib.sent(),40+length)
                self.assertEqual(packet,b'CFA6LPAA'+struct.pack('<QQQQ',40+length,40+length,0,15)+payload)
                self.assertEqual(self.lib.sends(),1 if length<=512 else 2)

    def test_oversized_artwork_rejected_before_allocation_or_remote_open(self):
        for size in (8*1024*1024+1, 2**40):
            self.lib.remote_file_size(size);self.lib.setup_packet()
            data=ctypes.c_void_p(123);length=ctypes.c_size_t(123)
            self.assertEqual(self.lib.afc_client_read_all(b'/large.jpg',ctypes.byref(data),ctypes.byref(length)),-5)
            self.assertIsNone(data.value);self.assertEqual(length.value,0)
            self.assertEqual(self.lib.allocated_size(),0);self.assertEqual(self.lib.sends(),0)

    def test_fragmented_ram_requests_sd_fallback_before_opening_remote_file(self):
        self.lib.remote_file_size(2*1024*1024);self.lib.setup_packet();self.lib.setup_io(1,0)
        data=ctypes.c_void_p();length=ctypes.c_size_t()
        self.assertEqual(self.lib.afc_client_read_all(b'/cover.jpg',ctypes.byref(data),ctypes.byref(length)),-5)
        self.assertEqual(self.lib.sends(),0);self.assertIsNone(data.value)
        self.lib.setup_io(0,0)

    def test_progressive_marker_parser_skips_metadata_and_rejects_malformed(self):
        frame=lambda marker:b'\xff'+bytes([marker])+b'\x00\x11\x08\x01\xf4\x01\xf4\x03'+b'\x01\x11\x00\x02\x11\x01\x03\x11\x01'
        metadata=b'\xff\xe1'+struct.pack('>H',60002)+b'\xff\xc2'*30000
        cases=[(b'\xff\xd8'+frame(0xc2),True),(b'\xff\xd8'+metadata+frame(0xc2),True),
               (b'\xff\xd8'+metadata+frame(0xc0),False),(b'\xff\xd8\xff\xff'+frame(0xc2)[1:],True),
               (b'',False),(b'PNG data',False),(b'\xff\xd8\xff\xc2',False),
               (b'\xff\xd8\xff\xe1\x00\x01',False),(b'\xff\xd8\xff\xe1\xff\xff',False),
               (b'\xff\xd8\xff\xda'+frame(0xc2),False)]
        path=Path(self.tmp.name)/'image.jpg'
        for data,expected in cases:
            path.write_bytes(data)
            self.assertEqual(self.lib.jpeg_file_is_progressive(str(path).encode()),expected)
