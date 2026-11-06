#!/usr/bin/env python3
"""Convert an iPod MediaLibrary.sqlitedb into the Tab5 library formats.

Outputs:
  JSON  - compact library.json for the mock/small-library loader
  BIN   - fixed-record library.bin read directly into the Tab5's PSRAM arrays

The script understands both the older iTunes schema (artist/album tables with
title columns) and the newer iOS schema where titles live in item_extra and
artists are split between item_artist and album_artist.

Usage:
    python3 tools/media_db_to_library.py \\
        --db /tmp/opencode/ipod-db/MLDB.SQL \\
        --wal /tmp/opencode/ipod-db/MLDB.WAL \\
        --out /tmp/opencode/ipod-db/library.json \\
        --bin /tmp/opencode/ipod-db/library.bin
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
import struct
import sys
import tempfile
from pathlib import Path

SYSTEM_PLAYLISTS = {
    "music", "movies", "tv shows", "podcasts", "audiobooks", "purchased",
    "genius", "voice memos", "rentals", "home videos", "ringtones",
    "books", "compilations", "recently added", "recently played",
}

PALETTE = [
    0x2F6F8F, 0x8F4F2F, 0x4F8F2F, 0x6F2F8F, 0x8F2F5F, 0x2F8F7F,
    0x5F5F8F, 0x8F7F2F, 0x3F7F5F, 0x7F3F3F, 0x3F5F8F, 0x7F5F3F,
]

BIN_MAGIC = b"T5LIB002"
BIN_VERSION = 2
BIN_HEADER = struct.Struct("<8sIIIIIII")
BIN_ARTIST = struct.Struct("<Q64s")
BIN_ALBUM = struct.Struct("<QQIIII80s64s")
BIN_TRACK = struct.Struct("<QQQIHH96s64s64s")
BIN_PLAYLIST = struct.Struct("<Q64sI")
BIN_ARTWORK = struct.Struct("<I128s")


def table_columns(conn: sqlite3.Connection, table: str) -> list[str]:
    try:
        return [row[1] for row in conn.execute(f'PRAGMA table_info("{table}")')]
    except sqlite3.DatabaseError:
        return []


def pick(columns: list[str], *names: str) -> str | None:
    for name in names:
        if name in columns:
            return name
    return None


def color_for(text: str) -> int:
    digest = hashlib.sha1(text.encode("utf-8", "replace")).digest()
    return PALETTE[digest[0] % len(PALETTE)]


def snapshot(db_path: Path, wal_path: Path | None):
    tmp = tempfile.TemporaryDirectory(prefix="tab5-media-db-")
    db = Path(tmp.name) / "MediaLibrary.sqlitedb"
    db.write_bytes(db_path.read_bytes())
    if wal_path and wal_path.exists():
        Path(str(db) + "-wal").write_bytes(wal_path.read_bytes())
    conn = sqlite3.connect(str(db))
    try:
        conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        conn.execute("PRAGMA journal_mode=DELETE")
        conn.execute("PRAGMA quick_check").fetchone()
    except sqlite3.DatabaseError as exc:
        print(f"warning: checkpoint failed: {exc}", file=sys.stderr)
    return conn, tmp


def load_new_schema(conn: sqlite3.Connection):
    """iOS 10+ MediaLibrary schema."""
    album_artist = {}
    for row in conn.execute("SELECT album_artist_pid, album_artist FROM album_artist"):
        if row[0] is not None:
            album_artist[row[0]] = (row[1] or "").strip()
    item_artist = {}
    for row in conn.execute("SELECT item_artist_pid, item_artist FROM item_artist"):
        if row[0] is not None:
            item_artist[row[0]] = (row[1] or "").strip()

    albums = {}
    for row in conn.execute(
            "SELECT album_pid, album, album_artist_pid, album_year FROM album"):
        if row[0] is None:
            continue
        albums[row[0]] = {
            "title": (row[1] or "").strip(),
            "artist_pid": row[2],
            "year": int(row[3] or 0),
        }

    tracks = []
    rows = conn.execute("""
        SELECT i.item_pid, i.album_pid, i.album_artist_pid, i.item_artist_pid,
               i.track_number, i.disc_number,
               e.title, e.total_time_ms, e.year
        FROM item i JOIN item_extra e ON e.item_pid = i.item_pid
        WHERE i.in_my_library != 0 AND i.media_type = 8
          AND e.title IS NOT NULL AND e.title != ''
    """)
    for row in rows:
        title = (row["title"] or "").strip()
        if not title:
            continue
        artist_pid = row["album_artist_pid"]
        if artist_pid is None:
            artist_pid = row["item_artist_pid"]
        tracks.append({
            "pid": row["item_pid"],
            "title": title,
            "album_pid": row["album_pid"],
            "artist_pid": artist_pid,
            "artist": album_artist.get(artist_pid, ""),
            "track_artist": item_artist.get(row["item_artist_pid"], ""),
            "duration_sec": int(round((row["total_time_ms"] or 0) / 1000.0)),
            "track_no": int(row["track_number"] or 0),
            "disc": int(row["disc_number"] or 1),
            "year": int(row["year"] or 0),
        })

    playlists = []
    try:
        rows = conn.execute(
            "SELECT container_pid, name, filepath, is_hidden, "
            "is_ignorable_itunes_playlist, smart_is_folder, smart_is_dynamic, "
            "smart_is_genius FROM container")
        for row in rows:
            name = (row["name"] or "").strip()
            if not name or name.lower() in SYSTEM_PLAYLISTS:
                continue
            if row["is_hidden"] or row["is_ignorable_itunes_playlist"]:
                continue
            if row["smart_is_folder"] or row["smart_is_dynamic"] or row["smart_is_genius"]:
                continue
            filepath = row["filepath"] or ""
            if "OnTheGoPlaylist" in filepath:
                continue
            item_ids = [r[0] for r in conn.execute(
                "SELECT item_pid FROM container_item "
                "WHERE container_pid = ? ORDER BY position", (row["container_pid"],))]
            if item_ids:
                playlists.append({"name": name, "item_pids": item_ids})
    except sqlite3.DatabaseError:
        pass

    return album_artist, albums, tracks, playlists


def load_artwork_map(conn: sqlite3.Connection):
    """Return item/album artwork relative paths (iOS schema)."""
    if "best_artwork_token" not in {r[0] for r in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")}:
        return {}, {}, {}
    item_art = {}
    try:
        rows = conn.execute("""
            SELECT b.entity_pid AS pid, a.relative_path AS rel
            FROM best_artwork_token b
            JOIN artwork a ON a.artwork_token = b.available_artwork_token
            WHERE b.entity_type = 0 AND b.artwork_type = 1
              AND b.available_artwork_token IS NOT NULL
              AND b.available_artwork_token != ''
        """)
        for row in rows:
            rel = row["rel"]
            if isinstance(rel, bytes):
                rel = rel.decode("utf-8", "replace")
            if rel:
                item_art[int(row["pid"])] = rel
    except sqlite3.DatabaseError:
        pass

    items = {}
    try:
        for row in conn.execute("SELECT item_pid, album_pid FROM item"):
            items[int(row[0])] = int(row[1]) if row[1] is not None else None
    except sqlite3.DatabaseError:
        pass
    albums = {}
    try:
        for row in conn.execute(
                "SELECT album_pid, representative_item_pid FROM album"):
            albums[int(row[0])] = int(row[1]) if row[1] is not None else None
    except sqlite3.DatabaseError:
        pass

    album_art = {}
    for apid, rep in albums.items():
        if rep is not None and rep in item_art:
            album_art[apid] = item_art[rep]
            continue
        for pid, ap in items.items():
            if ap == apid and pid in item_art:
                album_art[apid] = item_art[pid]
                break
    return item_art, album_art, items


def load_old_schema(conn: sqlite3.Connection):
    artist_cols = table_columns(conn, "artist")
    album_cols = table_columns(conn, "album")
    item_cols = table_columns(conn, "item")
    playlist_cols = table_columns(conn, "playlist")
    playlist_item_cols = table_columns(conn, "playlist_item")

    artist_id_col = pick(artist_cols, "artist_pid", "pid", "id")
    artist_name_col = pick(artist_cols, "name", "artist", "title")
    artist_names = {}
    if artist_id_col and artist_name_col:
        for row in conn.execute("SELECT * FROM artist"):
            if row[artist_id_col] is not None:
                artist_names[row[artist_id_col]] = (row[artist_name_col] or "").strip()

    album_id_col = pick(album_cols, "album_pid", "pid", "id")
    album_title_col = pick(album_cols, "title", "name", "album")
    album_artist_col = pick(album_cols, "album_artist_pid", "artist_pid")
    album_year_col = pick(album_cols, "year", "album_year")
    albums = {}
    if album_id_col and album_title_col:
        for row in conn.execute("SELECT * FROM album"):
            if row[album_id_col] is not None:
                albums[row[album_id_col]] = {
                    "title": (row[album_title_col] or "").strip(),
                    "artist_pid": row[album_artist_col] if album_artist_col else None,
                    "year": int(row[album_year_col] or 0) if album_year_col else 0,
                }

    item_id_col = pick(item_cols, "item_pid", "pid", "id")
    item_title_col = pick(item_cols, "title", "name")
    item_album_col = pick(item_cols, "album_pid", "album_id")
    item_artist_col = pick(item_cols, "artist_pid", "artist_id")
    item_album_artist_col = pick(item_cols, "album_artist_pid")
    duration_col = pick(item_cols, "duration", "duration_ms", "length", "total_time")
    track_no_col = pick(item_cols, "track_number", "track", "track_no")
    year_col = pick(item_cols, "year")

    where = [f'"{item_title_col}" IS NOT NULL', f'"{item_title_col}" != ""']
    if "is_music" in item_cols:
        where.append('"is_music" != 0')
    for flag in ("is_movie", "is_podcast", "is_audiobook", "is_ringtone",
                 "is_voice_memo", "is_hidden"):
        if flag in item_cols:
            where.append(f'("{flag}" IS NULL OR "{flag}" = 0)')
    rows = list(conn.execute("SELECT * FROM item WHERE " + " AND ".join(where)))
    durations = [int(r[duration_col] or 0) for r in rows if duration_col]
    duration_is_ms = bool(durations) and max(durations) > 10000

    tracks = []
    for row in rows:
        title = (row[item_title_col] or "").strip()
        if not title:
            continue
        artist_pid = row[item_album_artist_col] if item_album_artist_col else (
            row[item_artist_col] if item_artist_col else None)
        duration = int(row[duration_col] or 0) if duration_col else 0
        if duration_is_ms:
            duration //= 1000
        tracks.append({
            "pid": row[item_id_col],
            "title": title,
            "album_pid": row[item_album_col] if item_album_col else None,
            "artist_pid": artist_pid,
            "artist": artist_names.get(artist_pid, ""),
            "track_artist": artist_names.get(
                row[item_artist_col] if item_artist_col else None, ""),
            "duration_sec": max(0, duration),
            "track_no": int(row[track_no_col] or 0) if track_no_col else 0,
            "disc": 1,
            "year": int(row[year_col] or 0) if year_col else 0,
        })

    playlists = []
    if "playlist" in (r[0] for r in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table'")) and \
       playlist_item_cols:
        pl_id = pick(playlist_cols, "playlist_pid", "pid", "id")
        pl_name = pick(playlist_cols, "name", "title")
        pl_hidden = pick(playlist_cols, "is_hidden", "hidden")
        pl_smart = pick(playlist_cols, "is_smart", "smart")
        pl_folder = pick(playlist_cols, "is_folder", "folder")
        pi_pl = pick(playlist_item_cols, "playlist_pid", "playlist_id")
        pi_item = pick(playlist_item_cols, "item_pid", "item_id")
        pi_order = pick(playlist_item_cols, "position", "order", "track_order")
        if pl_id and pl_name and pi_pl and pi_item:
            for row in conn.execute("SELECT * FROM playlist"):
                name = (row[pl_name] or "").strip()
                if not name or name.lower() in SYSTEM_PLAYLISTS:
                    continue
                if pl_hidden and row[pl_hidden]:
                    continue
                if pl_smart and row[pl_smart]:
                    continue
                if pl_folder and row[pl_folder]:
                    continue
                order = f' ORDER BY "{pi_order}"' if pi_order else " ORDER BY rowid"
                item_ids = [r[0] for r in conn.execute(
                    f'SELECT * FROM playlist_item WHERE "{pi_pl}" = ?' + order,
                    (row[pl_id],))]
                if item_ids:
                    playlists.append({"name": name, "item_pids": item_ids})

    return artist_names, albums, tracks, playlists


def build_library(conn: sqlite3.Connection, args):
    tables = {r[0] for r in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    if "item_extra" in tables and "item_artist" in tables and \
       "album_artist" in tables:
        print("schema: iOS MediaLibrary (item_extra/item_artist/album_artist)")
        artist_names, albums, tracks, playlists = load_new_schema(conn)
        item_art, album_art, item_album = load_artwork_map(conn)
    else:
        print("schema: legacy iTunes (item/artist/album)")
        artist_names, albums, tracks, playlists = load_old_schema(conn)
        item_art, album_art, item_album = {}, {}, {}

    # Group tracks by album PID, creating synthetic albums when needed.
    groups: dict[object, list[dict]] = {}
    synthetic: dict[object, dict] = {}
    for track in tracks:
        apid = track["album_pid"]
        if apid is not None and apid in albums:
            key = ("pid", apid)
        else:
            title = albums.get(apid, {}).get("title") if apid is not None else ""
            title = title or "Unknown Album"
            key = ("name", track["artist"], title)
            synthetic[key] = {"title": title,
                              "artist_pid": track["artist_pid"],
                              "year": track["year"]}
        groups.setdefault(key, []).append(track)

    album_entries = []
    for key, group in groups.items():
        if key[0] == "pid":
            info = albums[key[1]]
            title = info["title"] or "Unknown Album"
            artist_pid = info["artist_pid"]
            year = info["year"]
        else:
            info = synthetic[key]
            title = info["title"]
            artist_pid = info["artist_pid"]
            year = info["year"]
        artist_name = artist_names.get(artist_pid, "")
        if not artist_name:
            artist_name = group[0]["artist"] or "Unknown Artist"
        artwork = None
        if key[0] == "pid":
            artwork = album_art.get(key[1])
        if not artwork:
            for track in group:
                artwork = item_art.get(track["pid"])
                if artwork:
                    break
        album_entries.append({
            "key": key, "title": title, "artist": artist_name,
            "artist_pid": artist_pid, "year": year,
            "track_count": len(group), "tracks": group,
            "artwork": artwork,
        })
    album_entries.sort(key=lambda a: (a["title"].lower(), a["artist"].lower()))
    if len(album_entries) > args.max_albums:
        print(f"warning: truncating albums {len(album_entries)} -> {args.max_albums}")
        album_entries = album_entries[:args.max_albums]

    # Primary artists are album artists referenced by the selected albums.
    artist_pids = set()
    for album in album_entries:
        if album["artist_pid"] is not None:
            artist_pids.add(album["artist_pid"])
        for track in album["tracks"]:
            if track["artist_pid"] is not None:
                artist_pids.add(track["artist_pid"])
    artist_entries = sorted(
        [(pid, artist_names.get(pid, "") or "Unknown Artist")
         for pid in artist_pids],
        key=lambda a: a[1].lower())
    if len(artist_entries) > args.max_artists:
        print(f"warning: truncating artists {len(artist_entries)} -> {args.max_artists}")
        artist_entries = artist_entries[:args.max_artists]
    artist_seq = {pid: i + 1 for i, (pid, _) in enumerate(artist_entries)}

    album_seq = {album["key"]: i + 1 for i, album in enumerate(album_entries)}

    track_entries = []
    track_pid_by_id = {}
    for album in album_entries:
        ordered = sorted(
            album["tracks"],
            key=lambda t: (t["disc"], t["track_no"], t["title"].lower()))
        for track in ordered:
            if len(track_entries) >= args.max_tracks:
                break
            track_entries.append({
                # The SQLite item_pid is also the iAP2 persistent media item
                # ID, so the UI can send PlayMediaLibraryItems (0x4C07)
                # directly for this track.
                "id": int(track["pid"]),
                "album_id": album_seq[album["key"]],
                "artist_id": artist_seq.get(track["artist_pid"], 0),
                "title": track["title"],
                "artist": track["track_artist"] or track["artist"] or album["artist"],
                "album": album["title"],
                "duration_sec": max(0, track["duration_sec"]),
                "track_no": track["track_no"],
                "year": track["year"] or album["year"],
            })
            track_pid_by_id[track["pid"]] = track_entries[-1]["id"]

    playlist_entries = []
    for pl in playlists:
        ids = [track_pid_by_id[pid] for pid in pl["item_pids"]
               if pid in track_pid_by_id]
        if ids:
            playlist_entries.append({
                "id": len(playlist_entries) + 1,
                "name": pl["name"],
                "track_ids": ids[:args.max_playlist_tracks],
            })
        if len(playlist_entries) >= args.max_playlists:
            break

    albums_out = []
    artwork_entries = []
    for i, album in enumerate(album_entries):
        art = album.get("artwork")
        albums_out.append({
            "id": i + 1,
            "title": album["title"],
            "artist_id": artist_seq.get(album["artist_pid"], 0),
            "artist": album["artist"],
            "year": album["year"],
            "track_count": album["track_count"],
            "color": color_for(album["title"] + album["artist"]),
            "cover": "",
            "cover_index": (i + 1 + 1000) if art else 100,
        })
        if art:
            artwork_entries.append((i + 1, art))
    artists_out = [{"id": i + 1, "name": name}
                   for i, (_, name) in enumerate(artist_entries)]

    print(f"library: {len(albums_out)} albums, {len(track_entries)} tracks, "
          f"{len(artists_out)} artists, {len(playlist_entries)} playlists, "
          f"{len(artwork_entries)} artwork entries")
    return artists_out, albums_out, track_entries, playlist_entries, artwork_entries


def write_json(path: Path, artists, albums, tracks, playlists):
    data = {"artists": artists, "albums": albums, "tracks": tracks,
            "playlists": playlists}
    path.write_text(json.dumps(data, ensure_ascii=False,
                               separators=(",", ":")), encoding="utf-8")
    print(f"wrote {path}: {path.stat().st_size} bytes")


def write_bin(path: Path, artists, albums, tracks, playlists, artwork):
    with path.open("wb") as f:
        f.write(BIN_HEADER.pack(BIN_MAGIC, BIN_VERSION, len(artists),
                                len(albums), len(tracks), len(playlists),
                                len(artwork), 0))
        for artist in artists:
            f.write(BIN_ARTIST.pack(artist["id"],
                                    artist["name"].encode("utf-8")[:63]))
        for album in albums:
            f.write(BIN_ALBUM.pack(
                album["id"], album["artist_id"], album["year"],
                album["track_count"], album["color"], album["cover_index"],
                album["title"].encode("utf-8")[:79],
                album["artist"].encode("utf-8")[:63]))
        for track in tracks:
            f.write(BIN_TRACK.pack(
                track["id"], track["album_id"], track["artist_id"],
                track["duration_sec"], track["track_no"], track["year"],
                track["title"].encode("utf-8")[:95],
                track["artist"].encode("utf-8")[:63],
                track["album"].encode("utf-8")[:63]))
        for playlist in playlists:
            ids = playlist["track_ids"]
            f.write(BIN_PLAYLIST.pack(
                playlist["id"], playlist["name"].encode("utf-8")[:63],
                len(ids)))
            for track_id in ids:
                f.write(struct.pack("<Q", track_id))
        prefix = "/iTunes_Control/iTunes/Artwork/Originals/"
        for album_id, rel in artwork:
            remote = prefix + rel
            f.write(BIN_ARTWORK.pack(album_id,
                                     remote.encode("utf-8")[:127]))
    print(f"wrote {path}: {path.stat().st_size} bytes")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", required=True, type=Path)
    ap.add_argument("--wal", type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--bin", type=Path)
    ap.add_argument("--max-albums", type=int, default=4096)
    ap.add_argument("--max-tracks", type=int, default=24000)
    ap.add_argument("--max-artists", type=int, default=2048)
    ap.add_argument("--max-playlists", type=int, default=32)
    ap.add_argument("--max-playlist-tracks", type=int, default=1024)
    args = ap.parse_args()

    conn, _tmp = snapshot(args.db, args.wal)
    conn.row_factory = sqlite3.Row
    artists, albums, tracks, playlists, artwork = build_library(conn, args)
    if args.out:
        write_json(args.out, artists, albums, tracks, playlists)
    if args.bin:
        write_bin(args.bin, artists, albums, tracks, playlists, artwork)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
