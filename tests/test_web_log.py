"""Exercise log pagination with the HTTP server's borrowed header pointers."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WebLogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        source = (ROOT / 'firmware/main/web_server.c').read_text()
        start = source.index('static esp_err_t log_get(')
        end = source.index('\nstatic ', start + 1)
        boundary = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
typedef int esp_err_t;
typedef struct {
    const char *seq, *dropped;
    unsigned sent_seq, sent_dropped;
} httpd_req_t;
static uint32_t requested_since, reported_dropped;
static bool query_value(httpd_req_t *r,const char *key,char *value,size_t len) {
    (void)r;(void)key;snprintf(value,len,"%u",requested_since);return true;
}
static uint32_t debug_log_read_text(uint32_t since,char *buf,size_t cap,size_t *len,uint32_t *dropped) {
    (void)cap;*dropped=reported_dropped;*len=4;memcpy(buf,"log\n",4);return since+10;
}
static int httpd_resp_send_500(httpd_req_t *r) { (void)r;return -1; }
static void httpd_resp_set_type(httpd_req_t *r,const char *type) { (void)r;(void)type; }
static void httpd_resp_set_hdr(httpd_req_t *r,const char *key,const char *value) {
    /* ESP-IDF retains pointers until httpd_resp_send(), without copying. */
    if(!strcmp(key,"X-Log-Seq"))r->seq=value;
    if(!strcmp(key,"X-Log-Dropped"))r->dropped=value;
}
static int httpd_resp_send(httpd_req_t *r,const char *buf,ssize_t len) {
    if(len!=4 || memcmp(buf,"log\n",4))return -1;
    r->sent_seq=(unsigned)strtoul(r->seq,NULL,10);
    r->sent_dropped=r->dropped?(unsigned)strtoul(r->dropped,NULL,10):0;
    return 0;
}
'''
        exports = r'''
unsigned request(unsigned since,unsigned dropped,int which) {
    requested_since=since;reported_dropped=dropped;httpd_req_t r={0};
    if(log_get(&r))return UINT32_MAX;
    return which?r.sent_dropped:r.sent_seq;
}
'''
        (work / 'log.c').write_text(boundary + source[start:end] + exports)
        binary = work / 'log.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared',
                        '-fPIC', str(work / 'log.c'), '-o', str(binary)], check=True)
        cls.lib = ctypes.CDLL(str(binary))
        cls.lib.request.argtypes = [ctypes.c_uint] * 3
        cls.lib.request.restype = ctypes.c_uint

    def test_dropped_count_does_not_replace_next_sequence(self):
        self.assertEqual(self.lib.request(1000, 731, 0), 1010)
        self.assertEqual(self.lib.request(1000, 731, 1), 731)

    def test_next_sequence_without_dropped_lines(self):
        self.assertEqual(self.lib.request(1000, 0, 0), 1010)
        self.assertEqual(self.lib.request(1000, 0, 1), 0)


if __name__ == '__main__':
    unittest.main()
