"""Read production AFC snapshots with committed, pending and corrupt WAL data."""
import ctypes as C
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]

class Album(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [('pid', C.c_uint64), ('date', C.c_uint32), ('version', C.c_uint64),
                ('title', C.c_char*80), ('artist', C.c_char*64), ('remote', C.c_char*160),
                ('year', C.c_uint16)]

class Track(C.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [('pid', C.c_uint64), ('album', C.c_uint64), ('date', C.c_uint32),
                ('disc',C.c_uint16),('track',C.c_uint16)]

class Index(C.Structure):
    _fields_ = [('library', C.c_char*128), ('albums_count', C.c_uint32),
                ('tracks_count', C.c_uint32), ('albums', C.POINTER(Album)),
                ('tracks', C.POINTER(Track))]

class MediaIndexTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.work.cleanup)
        sqlite = ROOT/'.deps/sqlite3'
        if not (sqlite/'sqlite3.c').exists():
            raise unittest.SkipTest('Run the firmware dependency configuration first')
        lib = Path(cls.work.name)/'index.so'
        subprocess.run(['cc', '-O2', '-shared', '-fPIC', '-D_POSIX_C_SOURCE=200809L',
                        '-DSQLITE_THREADSAFE=0', '-DSQLITE_OMIT_LOAD_EXTENSION',
                        '-DSQLITE_OS_OTHER=1','-DSQLITE_OMIT_WAL','-DSQLITE_OMIT_SHARED_CACHE',
                        '-DSQLITE_OMIT_DEPRECATED','-DSQLITE_DEFAULT_MEMSTATUS=0',
                        '-DSQLITE_OMIT_DESERIALIZE',
                        '-DSQLITE_TEMP_STORE=3','-DSQLITE_DEFAULT_CACHE_SIZE=-512',
                        '-DSQLITE_MAX_MMAP_SIZE=0',
                        '-I'+str(sqlite), '-I'+str(ROOT/'app/backend'),
                        str(ROOT/'firmware/main/afc_sqlite.c'),
                        str(ROOT/'app/backend/media_index.c'), str(sqlite/'sqlite3.c'),
                        str(ROOT/'firmware/components/sqlite3/sqlite_os.c'),
                        '-ldl', '-lm', '-o', str(lib)], check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.media_index_import.argtypes = [C.POINTER(Index), C.c_char_p, C.c_char_p,
                                               C.c_char_p, C.c_char_p, C.c_size_t]
        cls.lib.media_index_import.restype = C.c_bool
        cls.lib.media_index_save.argtypes = [C.POINTER(Index), C.c_char_p]
        cls.lib.media_index_save.restype = C.c_bool
        cls.lib.media_index_load.argtypes = [C.POINTER(Index), C.c_char_p, C.c_char_p]
        cls.lib.media_index_load.restype = C.c_bool
        cls.lib.media_index_clear.argtypes = [C.POINTER(Index)]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.db = self.root/'media.db'
        self.conn = sqlite3.connect(self.db)
        self.addCleanup(self.conn.close)
        self.conn.executescript("""
          CREATE TABLE _MLDatabaseProperties(key TEXT,value TEXT);
          INSERT INTO _MLDatabaseProperties VALUES('_UUID','LIBRARY-A');
          CREATE TABLE item(item_pid INTEGER PRIMARY KEY,album_pid INTEGER,date_added INTEGER,
                            in_my_library INTEGER,media_type INTEGER);
          CREATE TABLE item_extra(item_pid INTEGER,title TEXT);
          CREATE TABLE album(album_pid INTEGER PRIMARY KEY,album TEXT,album_artist_pid INTEGER,
                             representative_item_pid INTEGER);
          CREATE TABLE album_artist(album_artist_pid INTEGER,album_artist TEXT);
          INSERT INTO album_artist VALUES(3,'Artist');
          INSERT INTO album VALUES(-2,'Album',3,-1);
          INSERT INTO item VALUES(-1,-2,700000000,1,8);
          INSERT INTO item_extra VALUES(-1,'Song');
          CREATE TABLE best_artwork_token(entity_pid INTEGER,entity_type INTEGER,
                       artwork_type INTEGER,available_artwork_token TEXT);
          CREATE TABLE artwork(artwork_token TEXT,relative_path TEXT);
          INSERT INTO best_artwork_token VALUES(-1,0,1,'token');
          INSERT INTO artwork VALUES('token','AB/cover.jpg');
        """)
        self.index = Index()
        self.addCleanup(self.lib.media_index_clear, C.byref(self.index))

    def read(self, library=b'LIBRARY-A'):
        error = C.create_string_buffer(160)
        ok = self.lib.media_index_import(C.byref(self.index), bytes(str(self.db), 'utf8'),
                    bytes(str(self.db)+'-wal', 'utf8'), library, error, len(error))
        return ok, error.value

    def test_base_database_and_unsigned_pids(self):
        self.assertEqual(self.read(), (True, b''))
        self.assertEqual(self.index.albums_count, 1)
        self.assertEqual(self.index.tracks[0].pid, 2**64-1)
        a = self.index.albums[0]
        self.assertEqual((a.pid, a.date), (2**64-2, 1678307200))
        self.assertEqual(a.remote, b'/iTunes_Control/iTunes/Artwork/Originals/AB/cover.jpg')

    def test_committed_wal_used_without_checkpoint(self):
        self.conn.execute('PRAGMA journal_mode=WAL')
        self.conn.execute('PRAGMA wal_autocheckpoint=0')
        self.conn.execute("UPDATE item SET date_added=800000000")
        self.conn.execute("UPDATE album SET album='Changed in WAL'")
        self.conn.commit()
        self.assertEqual(self.read(), (True, b''))
        self.assertEqual(self.index.albums[0].title, b'Changed in WAL')
        self.assertEqual(self.index.albums[0].date, 1778307200)

    def test_album_date_uses_newest_active_music_track(self):
        self.conn.executescript("""
          INSERT INTO item VALUES(2,-2,800000000,1,8);
          INSERT INTO item_extra VALUES(2,'New song');
          INSERT INTO album VALUES(33,'Inactive album',3,3);
          INSERT INTO item VALUES(3,33,900000000,0,8);
          INSERT INTO item_extra VALUES(3,'Removed song');
        """)
        self.assertEqual(self.read(), (True,b''))
        self.assertEqual(self.index.tracks_count,2)
        self.assertEqual(self.index.albums_count,1)
        self.assertEqual(self.index.albums[0].date,1778307200)
        self.assertEqual(self.index.tracks[0].pid,2)

    def test_afc_album_year_round_trips(self):
        self.conn.executescript("ALTER TABLE album ADD COLUMN album_year INTEGER; UPDATE album SET album_year=2025;")
        self.assertEqual(self.read(),(True,b''))
        self.assertEqual(self.index.albums[0].year,2025)
        path=self.root/'years.bin'
        self.assertTrue(self.lib.media_index_save(C.byref(self.index),str(path).encode()))
        restored=Index();self.addCleanup(self.lib.media_index_clear,C.byref(restored))
        self.assertTrue(self.lib.media_index_load(C.byref(restored),str(path).encode(),b'LIBRARY-A'))
        self.assertEqual(restored.albums[0].year,2025)

    def test_afc_disc_and_track_numbers_are_imported(self):
        self.conn.executescript("ALTER TABLE item ADD COLUMN disc_number INTEGER; "
                               "ALTER TABLE item ADD COLUMN track_number INTEGER; "
                               "UPDATE item SET disc_number=2,track_number=7;")
        self.assertTrue(self.read()[0])
        self.assertEqual((self.index.tracks[0].disc,self.index.tracks[0].track),(2,7))

    def test_previous_index_format_migrates_without_losing_dates_or_artwork(self):
        self.assertTrue(self.read()[0])
        body=bytes(self.index.albums[0])[:-2]+bytes(self.index.tracks[0])[:-4]
        header=struct.pack('<8sIII128sI',b'T5AFC001',1,1,1,b'LIBRARY-A',zlib.crc32(body))
        path=self.root/'previous.bin';path.write_bytes(header+body)
        restored=Index();self.addCleanup(self.lib.media_index_clear,C.byref(restored))
        self.assertTrue(self.lib.media_index_load(C.byref(restored),str(path).encode(),b'LIBRARY-A'))
        self.assertEqual(restored.albums[0].date,self.index.albums[0].date)
        self.assertEqual(restored.albums[0].remote,self.index.albums[0].remote)
        self.assertEqual(restored.albums[0].year,0)

    def test_uncommitted_or_corrupt_tail_keeps_last_commit(self):
        self.conn.execute('PRAGMA journal_mode=WAL')
        self.conn.execute('PRAGMA wal_autocheckpoint=0')
        self.conn.execute('UPDATE item SET date_added=800000000')
        self.conn.commit()
        wal = Path(str(self.db)+'-wal')
        original = wal.read_bytes()
        wal.write_bytes(original + b'\xff'*512)
        self.assertTrue(self.read()[0])
        self.assertEqual(self.index.albums[0].date, 1778307200)

    def test_wrong_library_or_corrupt_wal_preserves_existing_index(self):
        self.assertTrue(self.read()[0])
        self.assertFalse(self.read(b'LIBRARY-B')[0])
        self.assertEqual(self.index.albums_count, 1)
        Path(str(self.db)+'-wal').write_bytes(b'bad header')
        self.assertFalse(self.read()[0])
        self.assertEqual(self.index.albums[0].pid, 2**64-2)

    def test_atomic_index_backup_and_namespace(self):
        self.assertTrue(self.read()[0])
        path = self.root/'afc_index.bin'
        self.assertTrue(self.lib.media_index_save(C.byref(self.index), str(path).encode()))
        self.assertTrue(self.lib.media_index_save(C.byref(self.index), str(path).encode()))
        path.write_bytes(b'truncated')
        restored = Index()
        self.addCleanup(self.lib.media_index_clear, C.byref(restored))
        self.assertFalse(self.lib.media_index_load(C.byref(restored), str(path).encode(), b'FOREIGN'))
        self.assertTrue(self.lib.media_index_load(C.byref(restored), str(path).encode(), b'LIBRARY-A'))
        self.assertEqual(restored.tracks_count, 1)
