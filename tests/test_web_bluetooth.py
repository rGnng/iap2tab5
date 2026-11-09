"""Run production Bluetooth HTTP handlers with fake HTTP, BLE and SD boundaries."""
import ctypes as C
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
A = 'CA:63:45:81:8C:B9'
B = 'CA:63:45:81:8C:BA'

class WebBluetoothTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        source = (ROOT/'firmware/main/web_server.c').read_text()
        source = source[source.index('static cJSON * bluetooth_json'):source.index('static esp_err_t status_get')]
        boundary = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "ble_remote.h"
typedef int esp_err_t;
typedef int httpd_err_code_t;
#define ESP_OK 0
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_404_NOT_FOUND 404
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
typedef struct { const char *body; int content_len, offset, status; bool fail; char reply[8192]; } httpd_req_t;
static unsigned saves, applies, scans, stops, connects, forgets, enables;
static bool enabled, supported, scanning, paired, have_scan, valid_battery, connected, fail_connect, fail_save;
static char last_addr[18];
static struct { bool ble_enabled; } g_app_config;
static int64_t esp_timer_get_time(void) { return 100000000; }
static void apply(void) { applies++; }
static void (*s_apply_cb)(void) = apply;
static int app_config_save(void) { saves++;return fail_save ? -1 : 0; }
static int httpd_req_recv(httpd_req_t *r,char *out,int len) {
    if(r->fail) return -1;
    /* Exercise chunked reads instead of assuming one recv supplies the body. */
    if(len>7)len=7;memcpy(out,r->body+r->offset,len);r->offset+=len;return len;
}
static int httpd_resp_sendstr(httpd_req_t *r,const char *text) { snprintf(r->reply,sizeof(r->reply),"%s",text);return 0; }
static int httpd_resp_send_err(httpd_req_t *r,int status,const char *text) { r->status=status;return httpd_resp_sendstr(r,text); }
static int httpd_resp_send_500(httpd_req_t *r) { return httpd_resp_send_err(r,500,"allocation failed"); }
static void httpd_resp_set_type(httpd_req_t *r,const char *type) { (void)r;(void)type; }
static void httpd_resp_set_hdr(httpd_req_t *r,const char *key,const char *value) { (void)r;(void)key;(void)value; }
static void httpd_resp_set_status(httpd_req_t *r,const char *status) { r->status=atoi(status); }
void ble_remote_status(ble_remote_state_t *state,char *text,size_t len) {
    *state=!enabled ? BLE_REMOTE_OFF : scanning ? BLE_REMOTE_SCANNING : BLE_REMOTE_CONNECTED;
    snprintf(text,len,"%s",!enabled ? "Bluetooth off" : scanning ? "Scanning..." : "Connected");
}
bool ble_remote_enabled(void) { return enabled; }
bool ble_remote_is_supported(void) { return supported; }
uint32_t ble_remote_revision(void) { return 42; }
int ble_remote_paired_count(void) { return paired ? 1 : 0; }
int ble_remote_scan_count(void) { return have_scan ? 2 : 0; }
bool ble_remote_paired_get(int i,ble_remote_device_t *d) {
    if(!paired || i!=0)return false;
    *d=(ble_remote_device_t){.name="Ulanzi Dial",.addr="CA:63:45:81:8C:B9"};return true;
}
bool ble_remote_scan_get(int i,ble_remote_device_t *d) {
    if(!have_scan || i<0 || i>1)return false;
    *d=(ble_remote_device_t){.name="Nearby Dial",.addr="CA:63:45:81:8C:BA",.rssi=-46};
    if(i==0)strcpy(d->addr,"CA:63:45:81:8C:B9");
    return true;
}
void ble_remote_get_info(const char *addr,ble_remote_info_t *info) {
    (void)addr;*info=(ble_remote_info_t){.connected=connected,.battery_valid=valid_battery,
        .battery_percent=73,.charging=true,.charge_full=false,.sleep_disabled=connected};
}
void ble_remote_get_link_stats(ble_remote_link_stats_t *out) {
    *out=(ble_remote_link_stats_t){.notifications=12,.heartbeat_replies=10,.link_checks=2,
        .notification_age_ms=50,.heartbeat_age_ms=60,.last_disconnect_reason=0x216,
        .last_recovery="GATT link probe timed out"};
}
void ble_remote_set_enabled(bool value) { enables++;enabled=value; }
void ble_remote_scan_start(void) { scans++;scanning=true; }
void ble_remote_scan_stop(void) { stops++;scanning=false; }
bool ble_remote_connect(const char *addr) { connects++;snprintf(last_addr,sizeof(last_addr),"%s",addr);return !fail_connect; }
bool ble_remote_unpair(const char *addr) { forgets++;snprintf(last_addr,sizeof(last_addr),"%s",addr);paired=false;return true; }
'''
        exports = r'''
static httpd_req_t request;
void reset(void) {
    enabled=supported=paired=have_scan=valid_battery=connected=true;
    scanning=fail_connect=fail_save=false;g_app_config.ble_enabled=true;
    saves=applies=scans=stops=connects=forgets=enables=0;last_addr[0]=0;
}
int invoke(const char *body,int fail) {
    request=(httpd_req_t){.body=body,.content_len=body?(int)strlen(body):0,.status=200,.fail=fail};
    if(body)bluetooth_post(&request);else bluetooth_get(&request);
    return request.status;
}
const char *response(void) { return request.reply; }
unsigned count(int which) {
    unsigned values[]={saves,applies,scans,stops,connects,forgets,enables};return values[which];
}
void set_enabled(int value) { enabled=value; }
void set_battery_valid(int value) { valid_battery=value; }
void set_connect_failure(int value) { fail_connect=value; }
void set_save_failure(int value) { fail_save=value; }
int saved_enabled(void) { return g_app_config.ble_enabled; }
const char *address(void) { return last_addr; }
'''
        (work/'web_bt.c').write_text(boundary+source+exports)
        binary=work/'web_bt.so'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-shared','-fPIC','-I'+str(ROOT/'third_party/cjson'),'-I'+str(ROOT/'app/backend'),
            str(work/'web_bt.c'),str(ROOT/'third_party/cjson/cJSON.c'),'-o',str(binary)],check=True)
        cls.lib=C.CDLL(str(binary))
        cls.lib.invoke.argtypes=[C.c_char_p,C.c_int]
        cls.lib.response.restype=cls.lib.address.restype=C.c_char_p

    def setUp(self):
        self.lib.reset()

    def call(self,body=None,fail=False):
        wire=None if body is None else (body if isinstance(body,str) else json.dumps(body)).encode()
        status=self.lib.invoke(wire,fail)
        return status,self.lib.response().decode()

    def test_status_reports_battery_and_filters_paired_scan_results(self):
        status,text=self.call()
        self.assertEqual(status,200)
        bt=json.loads(text)
        self.assertTrue(bt['enabled'])
        self.assertTrue(bt['supported'])
        self.assertEqual(bt['revision'],42)
        self.assertEqual(bt['link']['responsive_link_checks'],2)
        self.assertEqual(bt['link']['last_disconnect_reason'],0x216)
        self.assertEqual(bt['uptime_s'],100)
        self.assertEqual(bt['devices'][0],dict(name='Ulanzi Dial',addr=A,connected=True,
            battery=73,charging=True,charge_full=False,sleep_disabled=True,
            standby_seconds=None,standby=None))
        self.assertEqual(bt['discovered'],[dict(name='Nearby Dial',addr=B,rssi=-46)])
        self.lib.set_battery_valid(False)
        bt=json.loads(self.call()[1])
        for key in ['battery','charging','charge_full']:
            self.assertIsNone(bt['devices'][0][key])

    def test_enable_is_boolean_applied_and_persisted(self):
        self.assertEqual(self.call(dict(action='enable',enabled=False))[0],200)
        self.assertFalse(json.loads(self.call()[1])['enabled'])
        self.assertFalse(self.lib.saved_enabled())
        self.assertEqual([self.lib.count(i) for i in [0,1,6]],[1,1,1])
        self.lib.set_save_failure(True)
        status,text=self.call(dict(action='enable',enabled=True))
        self.assertEqual(status,200)
        self.assertIn('could not be saved',json.loads(text)['message'])
        self.assertTrue(json.loads(text)['enabled'])

    def test_scan_stop_pair_connect_and_forget_use_expected_device(self):
        self.assertTrue(json.loads(self.call(dict(action='scan'))[1])['scanning'])
        self.assertFalse(json.loads(self.call(dict(action='stop_scan'))[1])['scanning'])
        self.assertEqual(self.call(dict(action='pair',addr=B))[0],200)
        self.assertEqual(self.lib.address().decode(),B)
        self.assertEqual(self.call(dict(action='connect',addr=A.lower()))[0],200)
        self.assertEqual(self.lib.count(4),2)
        self.assertEqual(self.call(dict(action='forget',addr=A))[0],200)
        self.assertEqual(json.loads(self.call()[1])['devices'],[])
        self.assertEqual([self.lib.count(i) for i in [2,3,5]],[1,1,1])

    def test_disabled_bluetooth_rejects_connection_but_can_forget(self):
        self.lib.set_enabled(False)
        for body in [dict(action='scan'),dict(action='stop_scan'),dict(action='pair',addr=B),dict(action='connect',addr=A)]:
            self.assertEqual(self.call(body)[0],409)
        self.assertEqual(self.lib.count(4),0)
        self.assertEqual(self.call(dict(action='forget',addr=A))[0],200)

    def test_bad_requests_do_not_mutate_bluetooth(self):
        for body in ['', 'x'*257, '{', '{} extra', '[]', 'null', dict(action=2),
                     dict(action='enable'),dict(action='enable',enabled='false'),
                     dict(action='connect'),dict(action='connect',addr=2),dict(action='connect',addr='x'),
                     dict(action='unknown')]:
            self.assertEqual(self.call(body)[0],400,body)
        self.assertEqual(self.call(dict(action='scan'),fail=True)[0],400)
        self.assertEqual(sum(self.lib.count(i) for i in range(7)),0)

    def test_unknown_devices_and_failed_connect_are_reported(self):
        for action,addr in [('pair','00:00:00:00:00:00'),('connect',B),('forget',B)]:
            self.assertEqual(self.call(dict(action=action,addr=addr))[0],404)
        self.assertEqual(self.lib.count(4),0)
        self.assertEqual(self.lib.count(5),0)
        self.lib.set_connect_failure(True)
        self.assertEqual(self.call(dict(action='connect',addr=A))[0],500)

if __name__=='__main__':
    unittest.main()
