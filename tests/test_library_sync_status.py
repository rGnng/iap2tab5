"""Exercise the actual iAP2 subscription and MediaLibraryUpdate lifecycle."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

from test_album_view import function

ROOT = Path(__file__).resolve().parents[1]


def param(pid, value=b''):
    return struct.pack('>HH', len(value) + 4, pid) + value


class LibrarySyncStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT / 'firmware/main/iap2_host.c').read_text()
        start = source.index('        case 0x4C04: {')
        case = source[start:source.index('        case 0x6801:', start)]
        prelude = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "player.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef struct { uint8_t *p; size_t len, cap; } pb_t;
static char s_library_sub_id[128], s_library_id[128], s_library_revision[128];
static bool s_library_subscribed, s_library_resynced;
static atomic_bool s_library_syncing;
static int s_library_items, requests;
static struct { uint32_t library_messages, library_items; bool library_complete; } s_stats;
static void csm_send(uint16_t id, const pb_t *b) { (void)id; (void)b; requests++; }
void player_library_update(const player_media_item_t *item) { (void)item; }
void player_library_delete(uint64_t pid) { (void)pid; }
void player_library_reset(void) { }
void player_library_complete(void) { }
void player_library_complete_revision(const char *revision) { (void)revision; }
'''
        names = ('pb_raw', 'pb_u16be', 'pb_param', 'pb_str', 'pb_void', 'be64',
                 'be32', 'next_param', 'send_library_updates_request')
        harness = prelude + '\n'.join(function(source, name) for name in names)
        harness += '\nvoid update(const uint8_t *body, size_t len) { switch(0x4C04) {\n' + case + '\n} }\n'
        harness += r'''
void reset(void) {
    memset(&s_stats, 0, sizeof(s_stats));
    s_library_subscribed=s_library_resynced=false;
    atomic_store(&s_library_syncing,false);
    s_library_revision[0]=s_library_id[0]='\0';
    strcpy(s_library_sub_id,"test-library"); s_library_items=requests=0;
}
void request(void) { send_library_updates_request(); }
int value(int which) {
    switch(which) {
    case 0: return atomic_load(&s_library_syncing);
    case 1: return s_stats.library_complete;
    case 2: return requests;
    default: return s_stats.library_items;
    }
}
'''
        work = Path(cls.tmp.name)
        (work / 'sync.c').write_text(harness)
        subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I'+str(ROOT/'app/backend'), str(work / 'sync.c'), '-o', str(work / 'sync.so')], check=True)
        cls.lib = ctypes.CDLL(str(work / 'sync.so'))
        cls.lib.update.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    def setUp(self):
        self.lib.reset()

    def update(self, message):
        self.lib.update(message, len(message))

    def test_request_starts_sync_and_completion_stops_it(self):
        self.assertEqual(self.lib.value(0), 0)
        self.lib.request()
        self.assertEqual(self.lib.value(0), 1)
        self.update(param(7, b'\x64'))
        self.assertEqual(self.lib.value(0), 0)
        self.assertEqual(self.lib.value(1), 1)

    def test_delete_phase_resubscription_keeps_sync_active(self):
        self.lib.request()
        self.update(param(1, b'revision-one') + param(6) + param(7, b'\x5a'))
        self.assertEqual(self.lib.value(2), 2)
        self.assertEqual(self.lib.value(0), 1)
        self.assertEqual(self.lib.value(1), 0)
        self.update(param(7, b'\x64'))
        self.assertEqual(self.lib.value(0), 0)

    def test_later_incremental_update_restarts_sync(self):
        self.lib.request()
        self.update(param(7, b'\x64'))
        self.update(param(2, param(5, struct.pack('>Q', 123))) + param(7, b'\x32'))
        self.assertEqual(self.lib.value(0), 1)
        self.assertEqual(self.lib.value(1), 0)
        self.assertEqual(self.lib.value(3), 1)
        self.update(param(7, b'\x64'))
        self.assertEqual(self.lib.value(0), 0)


if __name__ == '__main__':
    unittest.main()
