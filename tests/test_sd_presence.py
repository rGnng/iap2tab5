"""Exercise production card-status polling and the live-removal mount latch."""
import ctypes as C
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^(?:bool|void) ' + name + r'\([^;]*?\)\s*\{', source, re.M)
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


class SDPresenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        work = Path(cls.tmp.name)
        production = (ROOT / 'firmware/main/app_config.c').read_text()
        boundary = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef struct { struct { char name[8]; } cid; unsigned real_freq_khz,log_bus_width;bool is_uhs1; } sdmmc_card_t;
static sdmmc_card_t card;
static const char *TAG="test";
static bool s_sd_ready,s_sd_lost,s_sd_probe_started,handle;
static unsigned s_sd_probe_failures;
static TickType_t s_sd_probe_tick,tick;
static int error,probes,mounts,unmounts;
static TickType_t xTaskGetTickCount(void) { return tick; }
static sdmmc_card_t *bsp_sdcard_get_handle(void) { return handle?&card:0; }
static int sdmmc_get_status(sdmmc_card_t *c) { (void)c;probes++;return error; }
static void bsp_sdcard_unmount(void) { unmounts++; }
static int sd_mount_default(void) { mounts++;return 0; }
static void app_config_sd_info_refresh(void) {}
static void library_migrate_legacy(void) {}
static void library_dirs_ensure(void) {}
'''
        exports = r'''
void reset(int mounted) {
 s_sd_ready=mounted;s_sd_lost=s_sd_probe_started=false;s_sd_probe_failures=0;
 s_sd_probe_tick=tick=0;handle=true;error=probes=mounts=unmounts=0;
}
void status(int err,int has_handle) { error=err;handle=has_handle; }
void poll_at(uint32_t now) { tick=now;app_config_sd_poll(); }
int lost(void) { return s_sd_lost; }
int probe_count(void) { return probes; }
int mount_count(void) { return mounts; }
int unmount_count(void) { return unmounts; }
'''
        source = boundary + '\n'.join(function(production, n) for n in
            ['app_config_sd_ready','app_config_sd_poll','app_config_sd_ensure']) + exports
        (work / 'sd.c').write_text(source)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        str(work / 'sd.c'), '-o', str(work / 'sd.so')], check=True)
        cls.lib = C.CDLL(str(work / 'sd.so'))
        cls.lib.poll_at.argtypes = [C.c_uint32]
        cls.lib.app_config_sd_ready.restype = C.c_bool
        cls.lib.app_config_sd_ensure.restype = C.c_bool

    def setUp(self):
        self.lib.reset(1)

    def test_healthy_card_probe_is_rate_limited(self):
        self.lib.poll_at(0);self.lib.poll_at(1999)
        self.assertEqual(self.lib.probe_count(), 1)
        self.lib.poll_at(2000)
        self.assertEqual(self.lib.probe_count(), 2)
        self.assertTrue(self.lib.app_config_sd_ready())

    def test_transient_failure_does_not_disable_card(self):
        self.lib.status(-1, 1);self.lib.poll_at(0)
        self.assertTrue(self.lib.app_config_sd_ready())
        self.lib.status(0, 1);self.lib.poll_at(2000)
        self.lib.status(-1, 1);self.lib.poll_at(4000)
        self.assertTrue(self.lib.app_config_sd_ready())
        self.assertFalse(self.lib.lost())

    def test_two_failures_latch_missing_without_unmounting_open_files(self):
        self.lib.status(-1, 1);self.lib.poll_at(0);self.lib.poll_at(2000)
        self.assertFalse(self.lib.app_config_sd_ready())
        self.assertTrue(self.lib.lost())
        self.assertEqual(self.lib.unmount_count(), 0)
        self.lib.status(0, 1);self.lib.poll_at(4000)
        self.assertFalse(self.lib.app_config_sd_ready())
        self.assertFalse(self.lib.app_config_sd_ensure())
        self.assertEqual(self.lib.mount_count(), 0)
        self.assertEqual(self.lib.probe_count(), 2)

    def test_missing_handle_is_unavailable(self):
        self.lib.status(0, 0);self.lib.poll_at(0);self.lib.poll_at(2000)
        self.assertTrue(self.lib.lost())
        self.assertFalse(self.lib.app_config_sd_ready())

    def test_missing_boot_card_can_still_use_existing_on_demand_mount(self):
        self.lib.reset(0);self.lib.poll_at(0)
        self.assertEqual(self.lib.probe_count(), 0)
        self.assertTrue(self.lib.app_config_sd_ensure())
        self.assertEqual(self.lib.mount_count(), 1)

    def test_poll_clock_wrap(self):
        start = 0xfffffc00
        self.lib.poll_at(start);self.lib.poll_at((start+1500)&0xffffffff)
        self.assertEqual(self.lib.probe_count(), 1)
        self.lib.poll_at((start+2000)&0xffffffff)
        self.assertEqual(self.lib.probe_count(), 2)


if __name__ == '__main__':
    unittest.main()
