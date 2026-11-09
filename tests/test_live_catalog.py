"""Production live model: identity, reconciliation, recency and AFC membership."""
import ctypes as C
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]

class LiveCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work=tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        path=Path(cls.work.name)
        (path/'live.c').write_text(r'''
#include "app/backend/mock_player.c"
void reset(void) {
    track_count=album_count=artist_count=playlist_count=0;real_library=true;
    memset(tracks,0,sizeof(tracks));memset(albums,0,sizeof(albums));
    memset(live_tracks,0,sizeof(live_tracks));memset(album_seen,0,sizeof(album_seen));
    media_index_clear(&live_afc);live_in_batch=false;live_dirty=false;live_publish=false;
    album_seen_next_seq=1;live_batch=0;live_suspended=false;track_id_index_build();membership_index_build();
    live_album_ready=false;
}
void add(uint64_t pid,uint64_t album,const char *title) {
    player_media_item_t item={.pid=pid,.album_pid=album,
        .fields=(1u<<0)|(1u<<1)|(1u<<5)|(1u<<6)|(1u<<12)|(1u<<14)};
    snprintf(item.title,sizeof(item.title),"%s",title);
    snprintf(item.album,sizeof(item.album),"Album %llu",(unsigned long long)album);
    strcpy(item.artist,"Performer");strcpy(item.album_artist,"Album Artist");
    player_library_update(&item);
}
int tracks_count(void) { return track_count; }
uint64_t song_at(int index) {
    player_track_t track;return player_song_get(index,&track)?track.id:0;
}
uint64_t raw_track_at(int index) { return tracks[index].id; }
void year(uint64_t pid,uint16_t value) {
    player_media_item_t item={.pid=pid,.year=value};
    player_library_update(&item);
}
void rename_track(uint64_t pid,const char *title) {
    player_media_item_t item={.pid=pid,.fields=1u<<1};
    snprintf(item.title,sizeof(item.title),"%s",title);player_library_update(&item);
}
uint64_t queued_pid(int index) {
    player_track_t track;return player_queue_at(index,&track)?track.id:0;
}
uint32_t seen(uint64_t pid) { int i=track_index_by_id(pid);return i<0?0:live_tracks[i].discovered; }
uint32_t album_seen_seq(uint64_t pid) { return album_seen_seq_unlocked(pid); }
uint64_t album_at(int i) { return i>=0 && i<album_count?albums[i].id:0; }
uint32_t date(uint64_t pid) { int a=live_album(pid);return a<0?0:albums[a].date_added; }
void number(uint64_t pid,uint16_t disc,uint16_t track) {
    player_media_item_t item={.pid=pid,.fields=(1u<<7)|(1u<<9),.disc_no=disc,.track_no=track};
    player_library_update(&item);
}
uint64_t track_at(uint64_t album,int index) {
    player_track_t track;return player_album_track_get(album,index,&track)?track.id:0;
}
uint64_t play_at(uint64_t album,int index) {
    uint64_t ids[10];int count=ids_album_from(album,0,ids,10);
    return index>=0 && index<count?ids[index]:0;
}
int synced(uint64_t pid) { int a=live_album(pid);return a>=0 && albums[a].afc_synced; }
const char *album_artist(uint64_t pid) { int a=live_album(pid);return a>=0?albums[a].artist:""; }
const char *album_title(uint64_t pid) { int a=live_album(pid);return a>=0?albums[a].title:""; }
void empty_metadata(uint64_t pid) {
    player_media_item_t item={.pid=pid,.fields=(1u<<1)|(1u<<6)|(1u<<12)|(1u<<14)};
    player_library_update(&item);
}
void afc_title(uint64_t album,const char *title) {
    media_index_album_t *a=(media_index_album_t*)media_index_album(&live_afc,album);
    if(a) snprintf(a->title,sizeof(a->title),"%s",title);
    int index=live_album(album);if(index>=0) albums[index].title[0]=0;
    live_dates();
}
void performer(uint64_t pid) {
    player_media_item_t item={.pid=pid,.fields=1u<<12};
    strcpy(item.artist,"Guest performer");player_library_update(&item);
}
void afc(uint64_t track,uint64_t album,uint32_t date) {
    if(!live_afc.albums) {
        strcpy(live_afc.library_id,"test");
        live_afc.albums=calloc(4096,sizeof(*live_afc.albums));
        live_afc.tracks=calloc(24000,sizeof(*live_afc.tracks));
    }
    if(!media_index_album(&live_afc,album))
        live_afc.albums[live_afc.album_count++]=(media_index_album_t){.pid=album};
    live_afc.tracks[live_afc.track_count++]=(media_index_track_t){.pid=track,.album_pid=album,.date_added=date};
    media_index_sort(&live_afc);live_dates();
}
void cover(uint64_t album) { int a=live_album(album);if(a>=0) albums[a].cover_index=1234; }
void afc_year(uint64_t album,uint16_t year) {
    media_index_album_t *a=(media_index_album_t*)media_index_album(&live_afc,album);
    if(a) a->year=year;live_dates();
}
void afc_numbers(uint64_t pid,uint16_t disc,uint16_t number) {
    media_index_track_t *t=(media_index_track_t*)media_index_track(&live_afc,pid);
    if(t) {t->disc_no=disc;t->track_no=number;}
}
int save_load(const char *directory) {
    snprintf(live_directory,sizeof(live_directory),"%s",directory);strcpy(live_library,"test");
    char path[256];snprintf(path,sizeof(path),"%s/catalog.bin",directory);
    FILE *f=fopen(path,"wb");if(!f) return 0;
    live_header_t h={.tracks=track_count,.albums=album_count,.artists=artist_count,
        .batch=album_seen_next_seq,.track_size=sizeof(*tracks),.album_size=sizeof(*albums),
        .artist_size=sizeof(*artists),.seen_size=sizeof(*live_tracks)};
    memcpy(h.magic,"T5LIV001",8);strcpy(h.library,"test");
    h.crc=store_crc32(0,tracks,track_count*sizeof(*tracks));
    h.crc=store_crc32(h.crc,albums,album_count*sizeof(*albums));
    h.crc=store_crc32(h.crc,artists,artist_count*sizeof(*artists));
    h.crc=store_crc32(h.crc,live_tracks,track_count*sizeof(*live_tracks));
    fwrite(&h,1,sizeof(h),f);fwrite(tracks,sizeof(*tracks),track_count,f);
    fwrite(albums,sizeof(*albums),album_count,f);fwrite(artists,sizeof(*artists),artist_count,f);
    fwrite(live_tracks,sizeof(*live_tracks),track_count,f);fclose(f);
    track_count=album_count=artist_count=0;
    return live_load_one(path);
}
void reload(void) { live_load(); }
''')
        library=path/'live.so'
        subprocess.run(['cc','-O2','-shared','-fPIC','-D_POSIX_C_SOURCE=200809L',
                        '-I'+str(ROOT),'-I'+str(ROOT/'third_party/cjson'),
                        str(path/'live.c'),str(ROOT/'app/backend/media_index.c'),
                        str(ROOT/'third_party/cjson/cJSON.c'),'-lm','-o',str(library)],check=True)
        cls.lib=C.CDLL(str(library))
        for name in ('seen','album_seen_seq','date','synced','cover','player_library_delete'):
            getattr(cls.lib,name).argtypes=[C.c_uint64]
        cls.lib.add.argtypes=[C.c_uint64,C.c_uint64,C.c_char_p]
        cls.lib.afc.argtypes=[C.c_uint64,C.c_uint64,C.c_uint32]
        cls.lib.afc_year.argtypes=[C.c_uint64,C.c_uint16]
        cls.lib.afc_numbers.argtypes=[C.c_uint64,C.c_uint16,C.c_uint16]
        cls.lib.number.argtypes=[C.c_uint64,C.c_uint16,C.c_uint16]
        for name in ('track_at','play_at'):
            getattr(cls.lib,name).argtypes=[C.c_uint64,C.c_int]
            getattr(cls.lib,name).restype=C.c_uint64
        cls.lib.album_at.restype=C.c_uint64
        cls.lib.player_sort_albums.argtypes=[C.c_int,C.c_bool]
        cls.lib.player_sort_songs.argtypes=[C.c_int,C.c_bool]
        cls.lib.song_at.restype=C.c_uint64
        cls.lib.raw_track_at.restype=C.c_uint64
        cls.lib.queued_pid.restype=C.c_uint64
        cls.lib.year.argtypes=[C.c_uint64,C.c_uint16]
        cls.lib.rename_track.argtypes=[C.c_uint64,C.c_char_p]
        cls.lib.player_play_song.argtypes=[C.c_uint64]
        cls.lib.player_song_index.argtypes=[C.c_uint64]
        cls.lib.save_load.argtypes=[C.c_char_p]
        cls.lib.player_library_cached_revision.argtypes=[C.c_char_p,C.c_char_p,C.c_size_t]
        cls.lib.player_library_cached_revision.restype=C.c_bool
        cls.lib.performer.argtypes=[C.c_uint64]
        cls.lib.album_artist.argtypes=[C.c_uint64]
        cls.lib.album_artist.restype=C.c_char_p
        cls.lib.album_title.argtypes=[C.c_uint64]
        cls.lib.album_title.restype=C.c_char_p
        cls.lib.empty_metadata.argtypes=[C.c_uint64]
        cls.lib.afc_title.argtypes=[C.c_uint64,C.c_char_p]

    def setUp(self):
        self.lib.reset()

    def add(self,pid,album,title=b'Song'):
        self.lib.add(pid,album,title)

    def finish(self):
        self.lib.player_library_complete()

    def test_new_live_album_has_tracks_before_afc_and_stable_identity(self):
        self.add(2**64-1,2**64-2)
        self.finish()
        self.assertEqual(self.lib.tracks_count(),1)
        self.assertEqual(self.lib.album_at(0),2**64-2)
        self.assertFalse(self.lib.synced(2**64-2))

    def test_reconnect_and_full_reset_preserve_discovery(self):
        self.add(1,10);self.finish()
        original=self.lib.seen(1)
        self.lib.player_library_reset();self.add(1,10);self.finish()
        self.assertEqual(self.lib.seen(1),original)
        self.assertEqual(self.lib.album_seen_seq(10),original)

    def test_new_track_makes_existing_album_recent_and_pending(self):
        self.add(1,10);self.add(2,20);self.finish()
        self.lib.afc(1,10,2000);self.lib.afc(2,20,3000)
        self.assertTrue(self.lib.synced(10))
        self.add(3,10);self.finish()
        self.assertFalse(self.lib.synced(10))
        self.assertGreater(self.lib.album_seen_seq(10),self.lib.album_seen_seq(20))
        self.lib.player_sort_albums(0,True)
        self.assertEqual(self.lib.album_at(0),10)

    def test_cover_capture_does_not_change_priority_afc_merges_dates(self):
        self.add(1,10);self.add(2,20);self.finish()
        self.lib.afc(2,20,3000)
        self.lib.cover(10)
        self.lib.player_sort_albums(0,True)
        self.assertEqual(self.lib.album_at(0),10)
        self.assertFalse(self.lib.synced(10))
        self.lib.afc(1,10,1000)
        self.lib.player_sort_albums(0,True)
        self.assertTrue(self.lib.synced(10))
        self.assertEqual(self.lib.date(10),1000)
        self.assertEqual(self.lib.album_at(0),20)

    def test_afc_year_backfill_controls_year_sort_for_live_albums(self):
        self.add(1,10);self.add(2,20);self.finish()
        self.lib.afc(1,10,1000);self.lib.afc(2,20,2000)
        self.lib.afc_year(10,2025);self.lib.afc_year(20,2024)
        self.lib.player_sort_albums(2,True)
        self.assertEqual(self.lib.album_at(0),10)

    def test_song_date_order_uses_each_tracks_date(self):
        self.add(1,10,b'First');self.add(2,10,b'Second');self.add(3,20,b'Third');self.finish()
        self.lib.afc(1,10,100);self.lib.afc(2,10,300);self.lib.afc(3,20,200)
        self.lib.player_sort_songs(0,True)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[2,3,1])
        self.lib.player_sort_songs(0,False)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[1,3,2])

    def test_undated_song_discovery_is_stable_across_reconnect(self):
        self.add(1,10,b'Dated');self.finish();self.lib.afc(1,10,100)
        self.add(2,20,b'Older new');self.finish()
        self.add(3,30,b'Newest');self.finish()
        self.lib.player_sort_songs(0,True)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[3,2,1])
        self.lib.player_library_reset()
        self.add(1,10,b'Dated');self.add(2,20,b'Older new');self.add(3,30,b'Newest');self.finish()
        self.lib.player_sort_songs(0,True)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[3,2,1])
        self.lib.player_sort_songs(0,False)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[1,2,3])

    def test_song_name_order_preserves_storage_memberships_and_album_order(self):
        self.add(1,20,b'Beta');self.add(2,10,b'Alpha');self.add(3,20,b'Gamma');self.finish()
        self.lib.player_sort_albums(1,False)
        album_order=[self.lib.album_at(i) for i in range(2)]
        self.lib.player_sort_songs(1,True)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[3,1,2])
        self.assertEqual([self.lib.raw_track_at(i) for i in range(3)],[1,2,3])
        self.assertEqual([self.lib.track_at(20,i) for i in range(2)],[1,3])
        self.assertEqual([self.lib.album_at(i) for i in range(2)],album_order)
        self.assertEqual(self.lib.player_song_index(1),1)
        self.lib.player_play_song(1)
        self.assertEqual([self.lib.queued_pid(i) for i in range(3)],[1,2,3])

    def test_song_year_order_falls_back_to_afc_album_year(self):
        self.add(1,10,b'Old');self.lib.year(1,2020)
        self.add(2,20,b'New');self.lib.year(2,2024)
        self.add(3,30,b'Album year');self.finish()
        self.lib.afc(3,30,100);self.lib.afc_year(30,2025)
        self.lib.player_sort_songs(2,True)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[3,2,1])

    def test_song_order_refreshes_after_rename_and_deletion(self):
        self.add(1,10,b'Zulu');self.add(2,20,b'Beta');self.add(3,30,b'Charlie');self.finish()
        self.lib.player_sort_songs(1,False)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[2,3,1])
        self.lib.rename_track(1,b'Alpha');self.finish()
        self.lib.player_sort_songs(1,False)
        self.assertEqual([self.lib.song_at(i) for i in range(3)],[1,2,3])
        self.lib.player_library_delete(2);self.finish()
        self.lib.player_sort_songs(1,False)
        self.assertEqual([self.lib.song_at(i) for i in range(2)],[1,3])
        self.assertEqual(self.lib.player_song_index(2),-1)

    def test_incremental_delete_and_full_reconcile(self):
        self.add(1,10);self.add(2,20);self.add(3,20);self.finish()
        self.lib.player_library_delete(1);self.finish()
        self.assertEqual(self.lib.tracks_count(),2)
        self.lib.player_library_reset();self.add(3,20);self.finish()
        self.assertEqual(self.lib.tracks_count(),1)
        self.assertEqual(self.lib.album_at(0),20)

    def test_snapshot_round_trip_keeps_identity_and_discovery(self):
        self.add(1,10);self.finish()
        self.add(2,20);self.finish()
        original=self.lib.seen(2)
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.lib.save_load(directory.encode()))
        self.assertEqual(self.lib.tracks_count(),2)
        self.assertEqual(self.lib.seen(2),original)

    def test_album_tracks_and_playback_share_disc_track_order(self):
        self.add(3,10);self.lib.number(3,2,1)
        self.add(2,10);self.lib.number(2,1,2)
        self.add(1,10);self.lib.number(1,1,1)
        self.finish()
        self.assertEqual([self.lib.track_at(10,i) for i in range(3)],[1,2,3])
        self.assertEqual([self.lib.play_at(10,i) for i in range(3)],[1,2,3])

    def test_afc_disc_numbers_correct_device_metadata(self):
        self.add(1,10);self.lib.number(1,1,1)
        self.add(2,10);self.lib.number(2,1,2);self.finish()
        self.lib.afc(1,10,1000);self.lib.afc(2,10,1000)
        self.lib.afc_numbers(1,2,1);self.lib.afc_numbers(2,1,1);self.finish()
        self.assertEqual([self.lib.track_at(10,i) for i in range(2)],[2,1])

    def test_partial_performer_update_keeps_album_artist(self):
        self.add(1,10);self.finish()
        self.lib.performer(1);self.finish()
        self.assertEqual(self.lib.album_artist(10),b'Album Artist')

    def test_empty_updates_preserve_labels_and_afc_repairs_saved_empty_names(self):
        self.add(1,10);self.finish()
        self.lib.empty_metadata(1);self.finish()
        self.assertEqual(self.lib.album_title(10),b'Album 10')
        self.assertEqual(self.lib.album_artist(10),b'Album Artist')
        self.lib.afc(1,10,1000)
        self.lib.afc_title(10,b'Recovered album')
        self.assertEqual(self.lib.album_title(10),b'Recovered album')

    def test_cached_revision_is_bound_to_snapshot_crc_and_library(self):
        self.add(1,10);self.finish()
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.lib.save_load(directory.encode()))
            root=Path(directory)
            crc=struct.unpack_from('<I',(root/'catalog.bin').read_bytes(),152)[0]
            remote=struct.pack('<8s128s128sI',b'T5REV001',b'test',b'57',crc)
            (root/'catalog.rev').write_bytes(remote+struct.pack('<I',zlib.crc32(remote)))
            self.lib.reload()
            value=C.create_string_buffer(128)
            self.assertTrue(self.lib.player_library_cached_revision(b'test-MPB-15.7.8',value,len(value)))
            self.assertEqual(value.value,b'57')
            self.assertFalse(self.lib.player_library_cached_revision(b'foreign',value,len(value)))

    def test_manual_import_discards_only_superseded_catalog_and_pauses_writes(self):
        self.add(1,10);self.finish()
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.lib.save_load(directory.encode()))
            root=Path(directory)
            for name in ('library.bin','thumbs.bin','thumbs.map','afc_index.bin'):
                (root/name).write_bytes(b'preserved')
            self.lib.player_library_discard_snapshot()
            self.assertFalse((root/'catalog.bin').exists())
            for name in ('library.bin','thumbs.bin','thumbs.map','afc_index.bin'):
                self.assertEqual((root/name).read_bytes(),b'preserved')
            self.add(2,20);self.finish()
            self.assertEqual(self.lib.tracks_count(),1)
