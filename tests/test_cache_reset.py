"""Run the production boot-only artwork reset against a temporary library."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_album_view import function

ROOT = Path(__file__).resolve().parents[1]

class CacheResetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        p = Path(cls.tmp.name)
        source = (ROOT / 'firmware/main/thumb_cache.c').read_text()
        prelude = r'''
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_LOGI(...) ((void)0)
static FILE *s_pack, *s_idx;
static uint8_t *s_valid;
static char s_pack_path[128], s_idx_path[128], library[128], covers[128];
static bool ready;
static bool app_config_sd_ready(void) { return ready; }
static const char *app_config_library_dir(void) { return library; }
static const char *app_config_artwork_dir(void) { return covers; }
void setup(const char *l, const char *c, int mounted, int opened) {
    snprintf(library, sizeof(library), "%s", l);
    snprintf(covers, sizeof(covers), "%s", c);
    ready = mounted; s_valid = opened ? (uint8_t*)1 : NULL;
}
'''
        code = prelude + function(source, 'make_paths') + function(source, 'is_cached_cover')
        code += function(source.replace('esp_err_t thumb_cache_reset', 'static esp_err_t thumb_cache_reset'), 'thumb_cache_reset').replace('static esp_err_t thumb_cache_reset', 'esp_err_t thumb_cache_reset')
        (p / 'reset.c').write_text(code)
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', str(p/'reset.c'), '-o', str(p/'reset.so')], check=True)
        cls.lib = ctypes.CDLL(str(p/'reset.so'))
        cls.lib.setup.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]

    def setUp(self):
        self.files = tempfile.TemporaryDirectory()
        self.addCleanup(self.files.cleanup)
        self.library = Path(self.files.name)
        self.covers = self.library / 'covers'
        self.covers.mkdir()
        self.lib.setup(bytes(str(self.library), 'utf8'), bytes(str(self.covers), 'utf8'), 1, 0)

    def test_deletes_only_cache_and_preserves_library_and_other_files(self):
        removed = [self.library/'thumbs.bin', self.library/'thumbs.idx']
        removed += [self.covers / ('12' + ext) for ext in ('.jpg', '.PNG', '.thm', '.cov', '.art', '.webp')]
        kept = [self.library / name for name in ('library.bin', 'stars.csv', 'album_seen.bin', 'MediaLibrary.sqlitedb', 'pair.plist')]
        kept += [self.covers / name for name in ('notes.txt', '12.jpg.bak', '-1.jpg', 'x.jpg', '1.bin')]
        for path in removed + kept: path.write_bytes(b'keep contents intact')
        (self.covers/'99.jpg').mkdir()
        (self.covers/'99.jpg'/'1.jpg').write_bytes(b'nested')
        self.assertEqual(self.lib.thumb_cache_reset(), 0)
        for path in removed: self.assertFalse(path.exists(), path)
        for path in kept: self.assertEqual(path.read_bytes(), b'keep contents intact')
        self.assertEqual((self.covers/'99.jpg'/'1.jpg').read_bytes(), b'nested')
        self.assertEqual(self.lib.thumb_cache_reset(), 0)

    def test_requires_mounted_sd_and_closed_handles(self):
        pack = self.library/'thumbs.bin'; pack.write_bytes(b'old cache')
        for ready, opened in ((0, 0), (1, 1)):
            self.lib.setup(str(self.library).encode(), str(self.covers).encode(), ready, opened)
            self.assertNotEqual(self.lib.thumb_cache_reset(), 0)
            self.assertEqual(pack.read_bytes(), b'old cache')

    def test_missing_cover_directory_is_already_empty(self):
        self.covers.rmdir()
        self.assertEqual(self.lib.thumb_cache_reset(), 0)
