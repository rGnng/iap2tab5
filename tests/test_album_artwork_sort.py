"""Run the firmware album sort against AFC metadata membership."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_album_view import function

ROOT = Path(__file__).resolve().parents[1]
BASE = 1000


class AlbumArtworkSortTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT / 'app/backend/mock_player.c').read_text()
        prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "player.h"
#define ESP_PLATFORM 1
#define MAX_ALBUMS 4096
static player_album_t albums[MAX_ALBUMS];
static int album_count, album_sort_field, cache_reads;
static bool real_library=true, live_album_ready;
static bool album_sort_desc, cached[MAX_ALBUMS+1];
static uint32_t album_seen_seq_unlocked(uint64_t id) { return (uint32_t)id; }
static void live_take(void) {}
static void live_give(void) {}
static void album_seen_lock_take(void) {}
static void album_seen_lock_give(void) {}
'''
        functions = '\n'.join(function(source, name) for name in
                              ('album_artwork_pending', 'album_compare'))
        sort = function(source.replace('\nvoid player_sort_albums(', '\nstatic void player_sort_albums('),
                        'player_sort_albums').removeprefix('static ')
        driver = r'''
void reset(void) {
    album_count=cache_reads=0; live_album_ready=false; memset(albums,0,sizeof(albums));
    memset(cached,0,sizeof(cached));
}
void add(uint64_t id, uint16_t cover, uint32_t date, const char *title, uint16_t year) {
    assert(album_count<MAX_ALBUMS);
    player_album_t *a=&albums[album_count++];
    *a=(player_album_t){.id=id,.album_pid=id,.cover_index=cover,.date_added=date,.year=year};
    snprintf(a->title,sizeof(a->title),"%s",title);
}
void set_cached(uint32_t id,int on) { assert(id<=MAX_ALBUMS); cached[id]=on;
    for(int i=0;i<album_count;i++) if(albums[i].id==id) albums[i].afc_synced=on;
}
uint64_t at(int index) { return index>=0 && index<album_count?albums[index].id:0; }
int reads(void) { (void)live_album_ready; return cache_reads; }
'''
        work = Path(cls.tmp.name)
        (work / 'sort.c').write_text(prelude + functions + sort + driver)
        subprocess.run(['cc', '-std=c11', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT / 'app/backend'), str(work / 'sort.c'),
                        '-o', str(work / 'sort.so')], check=True)
        cls.lib = ctypes.CDLL(str(work / 'sort.so'))
        cls.lib.add.argtypes = [ctypes.c_uint64, ctypes.c_uint16, ctypes.c_uint32,
                                ctypes.c_char_p, ctypes.c_uint16]
        cls.lib.at.argtypes = [ctypes.c_int]
        cls.lib.at.restype = ctypes.c_uint64
        cls.lib.player_sort_albums.argtypes = [ctypes.c_int, ctypes.c_bool]

    def setUp(self):
        self.lib.reset()

    def add(self, id, date, title, *, cached=False, cover=None, year=2020):
        cover = BASE + id if cover is None else cover
        self.lib.add(id, cover, date, title.encode(), year)
        self.lib.set_cached(id, cached)

    def order(self, field=0, desc=True, count=4):
        self.lib.player_sort_albums(field, desc)
        return [self.lib.at(i) for i in range(count)]

    def test_afc_pending_album_precedes_dated_album(self):
        self.add(1, 300, 'Cached new', cached=True)
        self.add(2, 100, 'Pending old')
        self.assertEqual(self.order(count=2), [2, 1])

    def test_selected_sort_applies_inside_both_groups_in_each_direction(self):
        self.add(1, 400, 'Alpha', cached=True, year=2024)
        self.add(2, 100, 'Delta', year=2010)
        self.add(3, 300, 'Charlie', cached=True, year=2020)
        self.add(4, 200, 'Bravo', year=2015)
        expected = {
            (0, True): [4, 2, 1, 3], (0, False): [2, 4, 3, 1],
            (1, True): [2, 4, 3, 1], (1, False): [4, 2, 1, 3],
            (2, True): [4, 2, 1, 3], (2, False): [2, 4, 3, 1],
        }
        for (field, desc), ids in expected.items():
            with self.subTest(field=field, desc=desc):
                self.assertEqual(self.order(field, desc), ids)

    def test_pending_metadata_does_not_require_downloadable_artwork(self):
        self.add(1, 500, 'No artwork', cover=100)
        self.add(2, 400, 'Cached', cached=True)
        self.add(3, 100, 'Pending')
        self.assertEqual(self.order(count=3), [3, 1, 2])

    def test_afc_import_reclassifies_metadata_membership(self):
        self.add(1, 300, 'Cached new', cached=True)
        self.add(2, 100, 'Pending old')
        self.assertEqual(self.order(count=2), [2, 1])
        self.lib.set_cached(2, True)
        self.assertEqual(self.order(count=2), [1, 2])
        self.lib.set_cached(1, False)
        self.assertEqual(self.order(count=2), [1, 2])

    def test_cover_slot_is_stable_across_sorting_and_need_not_equal_album_id(self):
        self.add(1, 100, 'Pending', cover=BASE+4096)
        self.add(4096, 300, 'Cached', cover=BASE+1, cached=True)
        self.assertEqual(self.order(count=2), [1, 4096])
        self.assertEqual(self.order(1, False, count=2), [1, 4096])

    def test_sort_never_reads_the_thumbnail_bitmap(self):
        for id in range(1, 1001):
            self.add(id, id, f'Album {id:04}', cached=id % 3 != 0)
        ids = self.order(count=1000)
        self.assertEqual(ids, sorted(range(3, 1001, 3), reverse=True) +
                         sorted((i for i in range(1, 1001) if i % 3), reverse=True))
        self.assertEqual(self.lib.reads(), 0)

    def test_missing_entire_cache_and_empty_or_single_catalog_remain_ordered(self):
        self.assertEqual(self.order(count=0), [])
        self.add(1, 100, 'One')
        self.assertEqual(self.order(count=1), [1])
        self.add(2, 200, 'Two')
        self.assertEqual(self.order(count=2), [2, 1])


if __name__ == '__main__':
    unittest.main()
