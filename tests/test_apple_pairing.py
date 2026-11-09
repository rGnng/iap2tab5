"""Apple trust exchanges run production C against a fake lockdownd peer."""
import ctypes
from pathlib import Path
import shutil
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
typedef int esp_err_t;
typedef unsigned TickType_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_RESPONSE -2
#define ESP_ERR_INVALID_STATE -3
#define ESP_ERR_NO_MEM -4
#define ESP_ERR_TIMEOUT -5
#define LOCKDOWN_PORT 62078
#define pdMS_TO_TICKS(x) (x)
typedef struct {char *request_xml,*record_xml;} apple_pairing_identity_t;
static void *s_conn=(void*)1;
static int scenario, rounds, saved, busy, trusted, key_calls;
static char message[96];
static unsigned ticks;
static bool usbmux_host_is_ready(void){return scenario != 7 || rounds == 0;}
static unsigned xTaskGetTickCount(void){return ticks;}
static void vTaskDelay(unsigned n){ticks+=n;}
static void pair_state(bool b,bool t,const char *s){busy=b;trusted=t;snprintf(message,96,"%s",s);}
static void usbmux_conn_close(void *p){(void)p;}
static int usbmux_connect(int port,void **p,int timeout){(void)port;(void)timeout;*p=(void*)1;return 0;}
static int query_type(void){return 0;}
static int pair_public_key(uint8_t **p,size_t *n){
    *p=(uint8_t*)strdup(scenario==8 && key_calls ? "changed" : "device!");*n=7;key_calls++;return 0;
}
static int apple_pairing_generate(const uint8_t*p,size_t n,apple_pairing_identity_t*out){
    (void)p;(void)n;out->request_xml=strdup("public certificates only");
    out->record_xml=strdup("<plist><dict><key>PrivateKey</key><data>secret</data></dict></plist>");return 0;
}
static void apple_pairing_free(apple_pairing_identity_t*p){free(p->request_xml);free(p->record_xml);}
static int send_plist(const char*p){assert(!strstr(p,"secret"));assert(!strcmp(p,"public certificates only"));return 0;}
static int recv_plist(char**p){
    rounds++;
    const char *s="<plist><dict><key>Request</key><string>Pair</string><key>EscrowBag</key><data>YWJj</data></dict></plist>";
    if(scenario==1 && rounds<3)s="<key>Error</key><string>PairingDialogResponsePending</string>";
    if(scenario==2)s="<key>Error</key><string>UserDeniedPairing</string>";
    if(scenario==3)s="<key>Error</key><string>PasswordProtected</string>";
    if(scenario==4)s="<key>Request</key><string>GetValue</string>";
    if(scenario==5)s="<key>Error</key><string>InvalidPairRecord</string>";
    if(scenario==7)return ESP_FAIL;
    *p=strdup(s);return 0;
}
static int lockdown_client_set_pair_record(const char*p,size_t n){
    assert(n==strlen(p));assert(strstr(p,"<data>secret</data>"));
    assert(strstr(p,"<key>EscrowBag</key><data>YWJj</data>"));
    if(scenario==6)return ESP_FAIL;saved++;return 0;
}
'''
DRIVER = r'''
int run(int s){scenario=s;rounds=saved=busy=trusted=key_calls=ticks=0;return pair_with_apple();}
int get_saved(void){return saved;}
int get_busy(void){return busy;}
int get_trusted(void){return trusted;}
int get_rounds(void){return rounds;}
const char *get_message(void){return message;}
'''

class PairRequestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler required')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        source = (ROOT / 'firmware/main/lockdown_client.c').read_text()
        request = function(source.replace('\nesp_err_t lockdown_client_request_pair(',
                                          '\nstatic esp_err_t lockdown_client_request_pair('),
                           'lockdown_client_request_pair').removeprefix('static ')
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -1
#define portMAX_DELAY 0
static void *s_pair_lock;
static bool usb_ready, afc_ready, iap2_mode, s_art_active, s_pair_requested;
static struct { bool busy,trusted; char message[96]; } s_pair_status;
static int locked;
static bool usbmux_host_is_ready(void) { return usb_ready; }
static bool afc_client_is_ready(void) { return afc_ready; }
static bool app_config_iap2_mode(void) { return iap2_mode; }
static void xSemaphoreTake(void *p,int timeout) { (void)p;(void)timeout;assert(!locked);locked=1; }
static void xSemaphoreGive(void *p) { (void)p;assert(locked);locked=0; }
'''
        driver = r'''
int run(int scenario) {
    s_pair_lock=(void*)1;usb_ready=true;afc_ready=iap2_mode=s_art_active=s_pair_requested=false;
    s_pair_status.busy=s_pair_status.trusted=false;locked=0;
    switch(scenario) {
    case 1:afc_ready=true;break;case 2:s_pair_status.trusted=true;break;
    case 3:s_pair_status.busy=true;break;case 4:usb_ready=false;break;
    case 5:iap2_mode=true;break;case 6:s_art_active=true;break;case 7:s_pair_lock=NULL;break;
    }
    int result=lockdown_client_request_pair();assert(!locked);return result;
}
int requested(void) { return s_pair_requested; }
'''
        (work / 'request.c').write_text(prelude + request + driver)
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(work / 'request.c'),
                        '-o', str(work / 'request.so')], check=True)
        cls.lib = ctypes.CDLL(str(work / 'request.so'))

    def test_new_untrusted_device_can_request_pairing(self):
        self.assertEqual(self.lib.run(0), 0)
        self.assertTrue(self.lib.requested())

    def test_trusted_connected_or_unavailable_device_cannot_restart_pairing(self):
        for scenario in range(1, 8):
            with self.subTest(scenario=scenario):
                self.assertNotEqual(self.lib.run(scenario), 0)
                self.assertFalse(self.lib.requested())


class AppleTrustTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'): raise unittest.SkipTest('C compiler required')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name);source=(ROOT/'firmware/main/lockdown_client.c').read_text()
        (p/'trust.c').write_text(PRELUDE+function(source,'plist_get_string')+function(source,'pair_with_apple')+DRIVER)
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                        '-fsanitize=undefined',str(p/'trust.c'),'-o',str(p/'trust.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'trust.so'));cls.lib.get_message.restype=ctypes.c_char_p

    def test_success_saves_private_record_and_escrow_locally(self):
        self.assertEqual(self.lib.run(0),0)
        self.assertEqual(self.lib.get_saved(),1)
        self.assertTrue(self.lib.get_trusted());self.assertFalse(self.lib.get_busy())

    def test_pending_trust_retries_same_public_request(self):
        self.assertEqual(self.lib.run(1),0);self.assertEqual(self.lib.get_rounds(),3)
        self.assertEqual(self.lib.get_saved(),1)

    def test_denied_does_not_replace_existing_pair(self):
        self.assertNotEqual(self.lib.run(2),0);self.assertEqual(self.lib.get_saved(),0)
        self.assertIn(b'declined',self.lib.get_message());self.assertFalse(self.lib.get_busy())

    def test_locked_device_times_out_and_allows_retry(self):
        self.assertNotEqual(self.lib.run(3),0);self.assertEqual(self.lib.get_saved(),0)
        self.assertIn(b'timed out',self.lib.get_message());self.assertFalse(self.lib.get_busy())

    def test_bad_response_or_pairing_error_is_not_success(self):
        for scenario in (4,5):
            self.assertNotEqual(self.lib.run(scenario),0);self.assertEqual(self.lib.get_saved(),0)
            self.assertFalse(self.lib.get_trusted())

    def test_storage_failure_never_claims_saved_trust(self):
        self.assertNotEqual(self.lib.run(6),0);self.assertEqual(self.lib.get_saved(),0)
        self.assertFalse(self.lib.get_trusted());self.assertIn(b'saving failed',self.lib.get_message())

    def test_disconnect_stops_pairing_without_save(self):
        self.assertNotEqual(self.lib.run(7),0);self.assertEqual(self.lib.get_saved(),0)
        self.assertFalse(self.lib.get_busy());self.assertIn(b'disconnected',self.lib.get_message())

    def test_device_changed_during_key_generation_is_rejected(self):
        self.assertNotEqual(self.lib.run(8),0);self.assertEqual(self.lib.get_saved(),0)
        self.assertEqual(self.lib.get_rounds(),0);self.assertIn(b'changed',self.lib.get_message())

class AppleIdentityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not Path('/usr/include/mbedtls/x509_crt.h').exists():
            raise unittest.SkipTest('libmbedtls-dev needed for certificate validation')
        try:
            from cryptography.hazmat.primitives.asymmetric import rsa
            from cryptography.hazmat.primitives import serialization
        except ImportError:
            raise unittest.SkipTest('cryptography needed for certificate validation')
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        p=Path(cls.tmp.name);(p/'freertos').mkdir()
        (p/'freertos/FreeRTOS.h').write_text('')
        (p/'freertos/task.h').write_text('static inline void vTaskDelay(int ticks){(void)ticks;}\n')
        (p/'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM -2\n#define ESP_ERR_INVALID_ARG -3\n')
        (p/'compat.h').write_text(r'''
#include "mbedtls/version.h"
#include "mbedtls/x509_crt.h"
#if MBEDTLS_VERSION_MAJOR < 3
/* Same serial encoding through mbedTLS 2's API for host-only tests. */
static int mbedtls_x509write_crt_set_serial_raw(mbedtls_x509write_cert*c,const unsigned char*b,size_t n){
    mbedtls_mpi m;mbedtls_mpi_init(&m);int r=mbedtls_mpi_read_binary(&m,b,n);
    if(r==0)r=mbedtls_x509write_crt_set_serial(c,&m);mbedtls_mpi_free(&m);return r;
}
#endif
''')
        subprocess.run(['cc','-shared','-fPIC','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
            '-I'+str(p),'-include',str(p/'compat.h'),str(ROOT/'firmware/main/apple_pairing.c'),
            '-lmbedtls','-lmbedx509','-lmbedcrypto','-o',str(p/'identity.so')],check=True)
        cls.lib=ctypes.CDLL(str(p/'identity.so'))
        class Identity(ctypes.Structure):
            _fields_=[('request',ctypes.c_void_p),('record',ctypes.c_void_p)]
        cls.Identity=Identity
        cls.lib.apple_pairing_generate.argtypes=[ctypes.c_char_p,ctypes.c_size_t,ctypes.POINTER(Identity)]
        cls.lib.apple_pairing_free.argtypes=[ctypes.POINTER(Identity)]
        cls.device=rsa.generate_private_key(public_exponent=65537,key_size=2048)
        cls.public=cls.device.public_key().public_bytes(serialization.Encoding.PEM,serialization.PublicFormat.PKCS1)
        cls.request,cls.record=cls.generate()

    @classmethod
    def generate(cls):
        import plistlib
        identity=cls.Identity()
        result=cls.lib.apple_pairing_generate(cls.public,len(cls.public),ctypes.byref(identity))
        if result: raise AssertionError(f'identity generation failed: {result}')
        try:
            return plistlib.loads(ctypes.string_at(identity.request)),plistlib.loads(ctypes.string_at(identity.record))
        finally: cls.lib.apple_pairing_free(ctypes.byref(identity))

    def test_only_public_credentials_go_to_apple(self):
        self.assertEqual(self.request['Request'],'Pair')
        self.assertEqual(self.request['ProtocolVersion'],'2')
        self.assertNotIn('RootPrivateKey',self.request['PairRecord'])
        self.assertNotIn('HostPrivateKey',self.request['PairRecord'])
        self.assertIn('RootPrivateKey',self.record)
        self.assertEqual(self.request['PairRecord']['HostID'],self.record['HostID'])

    def test_certificates_match_keys_and_verify_against_root(self):
        from cryptography import x509
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import padding
        root=x509.load_pem_x509_certificate(self.record['RootCertificate'])
        for name in ('Root','Host','Device'):
            cert=x509.load_pem_x509_certificate(self.record[name+'Certificate'])
            root.public_key().verify(cert.signature,cert.tbs_certificate_bytes,padding.PKCS1v15(),cert.signature_hash_algorithm)
            self.assertEqual(len(cert.subject),0)
            if name!='Device':
                key=serialization.load_pem_private_key(self.record[name+'PrivateKey'],password=None)
                self.assertEqual(cert.public_key().public_numbers(),key.public_key().public_numbers())
            else: self.assertEqual(cert.public_key().public_numbers(),self.device.public_key().public_numbers())
        self.assertTrue(root.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)

    def test_new_pairing_uses_fresh_host_identity(self):
        _,record=self.generate()
        self.assertNotEqual(record['HostID'],self.record['HostID'])
        self.assertNotEqual(record['RootCertificate'],self.record['RootCertificate'])

    def test_invalid_device_key_produces_no_partial_identity(self):
        identity=self.Identity()
        self.assertNotEqual(self.lib.apple_pairing_generate(b'invalid',7,ctypes.byref(identity)),0)
        self.assertFalse(identity.request);self.assertFalse(identity.record)
