# Importing the real media library into the Tab5 UI

The UI library pages load a compact binary library. The mock build ships
`/spiffs/library.json`; the real imported library lives at
`/sdcard/library.bin` (v2, with an artwork manifest) and takes precedence.
A JSON fallback is kept for older images.

## 1. Pull the AFC database

The database copied by the AFC path is:

```text
/sdcard/AFC/MLDB.SQL      50552832 bytes
/sdcard/AFC/MLDB.WAL        358472 bytes
```

The firmware can stream these files over the USB serial console as base64
(command `T` for the DB, `W` for the WAL). On this machine:

```bash
cd /home/ops/iap2tab5
python3 - <<'PY'
# see /tmp/opencode/pull_db.py used during bring-up, or:
PY
```

The reference receiver used during bring-up is `/tmp/opencode/pull_db.py`.
It sends `T` / `W` and writes `/tmp/opencode/ipod-db/MLDB.SQL` and
`MLDB.WAL`. The measured throughput is about 300 KB/s of base64, so the
50 MB database takes roughly 3-4 minutes.

If the SD card is available on the host directly, copy the two files from
the card instead.

## 2. Convert to library.bin

```bash
python3 tools/media_db_to_library.py \
    --db /tmp/opencode/ipod-db/MLDB.SQL \
    --wal /tmp/opencode/ipod-db/MLDB.WAL \
    --out /tmp/opencode/ipod-db/library.json \
    --bin /tmp/opencode/ipod-db/library.bin
```

The converter handles both the legacy iTunes schema and the newer iOS
schema used by this iPod (`item_extra` holds titles/durations, artists are
split between `item_artist` and `album_artist`, playlists live in
`container` / `container_item`). It snapshots the database, checkpoints the
WAL, and emits sequential small IDs plus the fields the firmware expects.

The real library on this iPod:

```text
2,096 albums / 20,688 tracks / 1,067 album artists / 0 user playlists
library.bin  6,017,764 bytes  (v2, 2,090 artwork entries)
library.json 4,022,473 bytes
```

```text
artists[]  id, name
albums[]   id, title, artist_id, artist, year, track_count, color,
           cover, cover_index
tracks[]   id, album_id, artist_id, title, artist, album, duration_sec,
           track_no, year
playlists[] id, name, track_ids[]
```

Current firmware limits: 4096 albums, 24000 tracks, 2048 artists, 32
playlists, 1024 tracks per playlist. Albums with real artwork use
`cover_index = album_id + 1000` (`PLAYER_REAL_COVER_BASE`); albums without
artwork use `cover_index: 100`, which the UI treats as "no thumbnail" and
draws the album color placeholder.

The v2 header is:

```text
T5LIB002  8 bytes
version / artists / albums / tracks / playlists / artwork / reserved
          <8sIIIIIII, 36 bytes total
records   artists, albums, tracks, playlists, then artwork entries
artwork   <I128s: album_id + remote AFC path
```

## 3. Push library.bin to the Tab5

```bash
python3 tools/iap2_library_push.py /tmp/opencode/ipod-db/library.bin /dev/ttyACM0 Q
```

The firmware command `Q` writes `/sdcard/library.bin` (streamed base64, no
large RAM buffer) and prints `IAP2UP_OK <bytes>`. Reboot the Tab5 after the
upload; `player_init()` calls `app_config_sd_ensure()` and loads
`/sdcard/library.bin` directly into the PSRAM arrays, falling back to
`/sdcard/library.json` and then `/spiffs/library.json`.

Serial commands used by this workflow:

| key | action |
| --- | --- |
| `S` | remount SD and print the AFC/library file sizes |
| `T` | dump `MLDB.SQL` as base64 over the console |
| `W` | dump `MLDB.WAL` as base64 over the console |
| `Q` | receive base64 `library.bin`, terminate with `EOF` |
| `B` | receive base64 `/sdcard/JPFONT.TTF`, terminate with `EOF` |
| `C` | AFC artwork sync for the first 64 albums |
| `E` | AFC artwork sync for every album with an artwork manifest entry |
| `# <id>` | AFC artwork sync for one library album ID (line input) |

## 4. Album artwork cache

Artwork is fetched over AFC while the iPod is connected and the firmware is
in AFC mode (config 4). The manifest is generated from the iOS tables
`best_artwork_token` / `artwork`; the remote path is stored in the v2
`library.bin` artwork section.

The 54x54 thumbnails live in one packed file per library:

```text
thumbs.bin   64-byte header + one fixed 5,832-byte slot per album ID
thumbs.idx   bitmap; bit N set means slot N is valid
```

The pack is preallocated to `64 + album_count * 5832` bytes. This removes
the FAT overhead of creating and deleting ~2,000 tiny `.thm` files; the UI
reads a slot by fixed offset and keeps its 48-slot PSRAM cache. Legacy
`<id>.thm` files are imported into the pack once on the first sync and then
deleted.

### Slow cache log

A full sync writes `/sdcard/lib/<short8>/slow.log` (truncated at the
start of each full sync). Entries are added for albums that take at least
1,000 ms or fail:

```text
time album=123 total=8420ms result=failed image=3000x3000 bytes=... kind=sw 1/8 read=120ms decode=8200ms write=15ms "Album" / "Artist"
```

`read/decode/write` split the one-pass time. Use the log to find oversized
or undecodable artwork and replace/remove it in the media library. The
serial console command `v` prints the same file as plain text.

The 360x360 `<id>.cov` Now Playing cover is not generated by AFC. Now
Playing artwork is delivered through the iAP2 File Transfer session (file
type 2, `NowPlayingArtworkData`), decoded to 360x360 RGB565 in PSRAM and
shown directly. The latest cover is cached per library as:

```text
/sdcard/lib/<short8>/np.raw   360x360 RGB565
/sdcard/lib/<short8>/np.txt   album + artist + persistent ID
```

The cache is loaded at boot and used only when the reported album matches;
a zero-length type 2 Setup (device reports no artwork) removes it. Old
`.cov` files from earlier builds may remain on the card but are ignored by
the AFC cache statistics.

A full sync has three phases:

```text
1  database sync       copy/refresh the MediaLibrary snapshot
2  album art cache     read each original over AFC into PSRAM, decode it,
                       write the 54x54 slot; no original is written to SD
3  convert thumbnail   fallback for non-JPEG/progressive/oversized artwork
                       that could not be handled in the one-pass step
```

The one-pass path uses the ESP32-P4 hardware JPEG decoder when the RGB565
output fits in PSRAM (currently up to 6 MB), otherwise scaled TJPGD (1/2,
1/4 or 1/8). Only the thumbnail is kept; the original is freed immediately.

The SD card uses FATFS with long filenames disabled, so only 8.3 names are
valid. Keep the short extensions `.thm`, `.cov`, `.jpg`, `.wbp`, `.gif`;
`.thumb` / `.cover` / `.webp` fail with `EINVAL`.

Sync commands:

```text
C         first 64 albums (fast validation; skips already downloaded originals)
E         all 2,090 albums (large and long; old originals are removed)
# <id>    one album by library ID, e.g. #919
```

The same operations are available in **Settings > Apple File Conduit Sync**:

```text
Pair / Connect       switch to AFC / reconnect
Quick sync           first 64 albums, then automatically back to iAP2
Full sync            every album, then automatically back to iAP2
Cached albums        count / total
Cache size           thumbnails + covers + leftover originals
Thumbnails/Covers    file counts and sizes
USB mode / Sync      current mode and job state
Switch to iAP2       reboot back to iAP2 without syncing
```

Quick/full sync queue a one-shot job in NVS, reboot to AFC config 4, run
the sync, then set NVS back to iAP2 config 2 and reboot. The settings page
shows `Sync: Quick running` / `Full running` while it is in progress.

The two boot modes are:

```text
iAP2 (default)   normal media UI, USB config 2
AFC Sync         dedicated sync-only screen, USB config 4
```

AFC Sync has no Now Playing / Albums / Songs / media controls. It shows the
iPod/AFC connection, cached album count and size, a progress bar with
attempted/copied/failed counts, and only these buttons:

```text
Quick sync
Full sync
Exit to iAP2
```

Once a sync finishes it automatically switches back to iAP2 and reboots.
The screenshot is `docs/screenshots/tab5-afc-sync.png`.

The AFC job worker stack is allocated in PSRAM and is only created when a
job is pending; otherwise the USB DMA pool can be exhausted on boot and
AFC never becomes ready.

After both the 54x54 thumbnail and the 360x360 cover are written, the
downloaded original is removed. A full sync therefore stays around
600 MB of covers rather than several GB; leftovers from interrupted older
runs are cleaned when their album is processed.

`# <id>` is useful while testing iAP2: pause the iPod, switch to AFC,
sync the album reported by `a`, then switch back to iAP2. The paused track
keeps the album stable across the mode change.

The 54x54 thumbnails are cached in the UI in a 48-slot PSRAM cache, with
dedicated slots for the mini player and the album detail header. Legacy
360x360 covers are loaded as a 64-byte aligned RGB565 descriptor. Both are
handed to LVGL as raw images, so covers are never rescaled from a partially
decoded JPEG. PPA requires the 64-byte alignment; unaligned caches render
black.

### SDMMC write alignment

On ESP32-P4 the SDMMC DMA path requires PSRAM buffers to be aligned to the
L2 cache line, **128 bytes**. A 4-byte or 64-byte aligned buffer silently
falls back to single 512-byte block writes, which caps SD write throughput
at about 1 MB/s. The AFC receive buffer, thumbnail/cover buffers and file
writes must therefore use 128-byte aligned allocations and avoid stdio
copying through an unaligned `FILE` buffer. With the aligned path the same
card writes at roughly 5-7 MB/s.

### iAP2 album lookup

iAP2 only sends text metadata (title/artist/album). `iap2_album_id()`
first matches album title + artist, then falls back to a unique album
title. This handles small tag differences such as
`Bubble Records` (iAP2) vs `BubbleRecords` (SQLite). Verified with the
paused iAP2 track `まっしろわーるど(甘口remix)` / `NICONICO Works vol.1`;
the Now Playing screen showed the 360x360 cover
(`docs/screenshots/tab5-real-cover-iap2.png`).

### JPEG limitation

The built-in TJPGD decoder only supports baseline JPEG. A few iTunes
artwork files are progressive; those fail with `jd_prepare error: 8` and
keep the album color placeholder. In the first 8-album test, 7 decoded and
1 was progressive. libjpeg-turbo (which supports progressive) is not
currently part of the ESP-IDF build.

## 5. What still needs work

- The library pages are browse-only in iAP2 mode; selecting a track/album
  does not yet send an iAP2 play command.
- Progressive JPEG artwork is skipped (placeholder); see above.
- The iAP2 queue list (and therefore a real Now Playing queue panel) is not
  parsed yet.
- `library.bin` is loaded synchronously during UI init; with the real
  20k-track library the boot takes several extra seconds. This can be moved
  to a background task later.

## Japanese font

The UI uses **M PLUS 1p Regular** for proportional text (Latin + Japanese)
and keeps Inter for tabular numbers/clocks. The TTF is loaded once from
`/sdcard/JPFONT.TTF` into PSRAM and shared by the TinyTTF fonts created for
sizes 16/18/20/22/24/28/32/36/40.

```bash
curl -L -o MPLUS1p-Regular.ttf \
  https://raw.githubusercontent.com/google/fonts/main/ofl/mplus1p/MPLUS1p-Regular.ttf
python3 tools/iap2_library_push.py MPLUS1p-Regular.ttf /dev/ttyACM0 B
```

The font is licensed under the SIL Open Font License 1.1; see
`tools/fonts/MPLUS1p-OFL.txt`.

Verified on the Tab5: the Albums page shows `風神 Rhapsody`,
`飛翔、蒼の最果て`, `魂の語りに導かれて`, `魔法α`, `鳥船遺跡 ~ Trojan Green
Asteroid`; the album detail shows `運命 Disaster`, `キラキラ雨`, `鴉少女`.

## Notes

- The SD card mount can fail on the first boot attempt; `app_config_sd_ensure()`
  (serial `S`, or `player_init`) releases the stale SD LDO and remounts.
- Loading a large `library.json` uses PSRAM; the cJSON tree and arrays are
  allocated outside internal RAM.
