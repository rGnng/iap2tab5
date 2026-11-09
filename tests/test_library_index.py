"""Exercise production membership getters against a linear-order oracle."""
import ctypes
import json
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LibraryIndexTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler unavailable')
        cls.tmp = tempfile.TemporaryDirectory()
        path = Path(cls.tmp.name)
        source = r'''
#include "app/backend/mock_player.c"
#include <time.h>
void reset(void) { track_count=album_count=artist_count=0; membership_index_ready=false; }
void add(uint64_t album, uint64_t artist) {
    player_track_t *t=&tracks[track_count];
    memset(t,0,sizeof(*t));t->id=track_count+1;t->album_id=album;t->artist_id=artist;
    t->track_no=MAX_TRACKS-track_count;
    snprintf(t->title,sizeof(t->title),"Track %d",track_count++);
}
void build(void) { membership_index_build(); }
void indexed(int on) { membership_index_ready=on; }
int count(uint64_t id,int artist) {
    return artist?player_artist_song_count(id):player_album_track_count(id);
}
uint64_t get(uint64_t id,int n,int artist) {
    player_track_t t;
    bool ok=artist?player_artist_song_get(id,n,&t):player_album_track_get(id,n,&t);
    return ok?t.id:0;
}
int null_get(uint64_t id,int artist) {
    return artist?player_artist_song_get(id,0,NULL):player_album_track_get(id,0,NULL);
}
void sort_catalog(void) {
    album_count=3;
    for(int i=0;i<3;i++) { albums[i].id=i+1; snprintf(albums[i].title,80,"%d",3-i); }
    player_sort_albums(1,false);
}
static uint64_t checksum;
double benchmark(int use_index,int artist,int pages) {
    membership_index_ready=use_index;checksum=0;
    clock_t start=clock();
    for(int p=0;p<pages;p++) {
        uint64_t id=1+p%200;
        int n=count(id,artist);checksum+=n;
        for(int i=0;i<n && i<16;i++) checksum+=get(id,i,artist);
    }
    return (double)(clock()-start)/CLOCKS_PER_SEC;
}
uint64_t bench_checksum(void) { return checksum; }
'''
        (path / 'index.c').write_text(source)
        subprocess.run(['cc', '-std=c11', '-O2', '-shared', '-fPIC',
                        '-I', str(ROOT), '-I', str(ROOT / 'third_party/cjson'),
                        f'-DMOCK_ASSET_DIR="{path}"', str(path / 'index.c'),
                        str(ROOT / 'third_party/cjson/cJSON.c'), str(ROOT / 'app/backend/media_index.c'), '-lm',
                        '-o', str(path / 'index.so')], check=True, capture_output=True)
        cls.lib = ctypes.CDLL(str(path / 'index.so'))
        cls.lib.add.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
        cls.lib.count.argtypes = [ctypes.c_uint64, ctypes.c_int]
        cls.lib.get.argtypes = [ctypes.c_uint64, ctypes.c_int, ctypes.c_int]
        cls.lib.get.restype = ctypes.c_uint64
        cls.lib.null_get.argtypes = [ctypes.c_uint64, ctypes.c_int]
        cls.lib.benchmark.restype = ctypes.c_double
        cls.lib.bench_checksum.restype = ctypes.c_uint64

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.lib.reset()

    def load(self, pairs):
        self.lib.reset()
        for album, artist in pairs:
            self.lib.add(album, artist)

    def check_members(self, pairs, parents=None):
        for kind in (0, 1):
            expected = {}
            for i, pair in enumerate(pairs):
                expected.setdefault(pair[kind], []).append(i + 1)
            for parent in parents if parents is not None else set(expected) | {987654321}:
                ids = expected.get(parent, [])
                self.assertEqual(self.lib.count(parent, kind), len(ids))
                self.assertEqual([self.lib.get(parent, n, kind) for n in range(len(ids))], ids)
                for n in (-1, len(ids), 2**31 - 1):
                    self.assertEqual(self.lib.get(parent, n, kind), 0)
                self.assertEqual(self.lib.null_get(parent, kind), 0)

    def test_interleaved_members_and_all_64_bit_parent_ids(self):
        rng = random.Random(12073)
        parents = [0, 2**64-1, 2**63, 1, 8] + [rng.getrandbits(64) for _ in range(80)]
        pairs = [(rng.choice(parents), rng.choice(parents)) for _ in range(1200)]
        self.load(pairs)
        self.check_members(pairs)  # The previous scan behavior is the oracle too.
        self.lib.build()
        self.check_members(pairs)

    def test_capacity_and_original_order_not_track_number(self):
        pairs = [(i % 1301, i % 513) for i in range(24000)]
        self.load(pairs)
        self.lib.build()
        self.check_members(pairs)

    def test_empty_single_and_rebuilt_smaller_library(self):
        for pairs in ([(1, 2)] * 100, [(3, 4)], []):
            self.load(pairs)
            self.lib.build()
            self.check_members(pairs, [0, 1, 2, 3, 4, 2**64-1])

    def test_album_catalog_sort_and_missing_catalog_rows(self):
        pairs = [(1, 99), (3, 99), (1, 5), (2, 5), (7000, 2)]
        self.load(pairs)
        self.lib.build()
        self.lib.sort_catalog()
        self.check_members(pairs)

    def test_player_init_builds_and_reloads_membership(self):
        fixture = Path(self.tmp.name) / 'library.json'
        for parents in ([9, 1, 9], [77], []):
            fixture.write_text(json.dumps({'tracks': [
                {'id': i+1, 'album_id': p, 'artist_id': p, 'title': str(i)}
                for i, p in enumerate(parents)]}))
            self.lib.player_init()
            self.check_members([(p, p) for p in parents], [1, 9, 77])

    def test_benchmark_preserves_results(self):
        self.load([((i*73) % 1200 + 1, (i*31) % 600 + 1) for i in range(24000)])
        self.lib.build()
        for kind, name in ((0, 'album'), (1, 'artist')):
            linear = self.lib.benchmark(0, kind, 500)
            expected = self.lib.bench_checksum()
            indexed = self.lib.benchmark(1, kind, 500)
            self.assertEqual(self.lib.bench_checksum(), expected)
            print(f'\nHost {name}: 500 pages, 24k tracks, scan={linear*1000:.2f} ms, '
                  f'index={indexed*1000:.2f} ms')


if __name__ == '__main__':
    unittest.main()
