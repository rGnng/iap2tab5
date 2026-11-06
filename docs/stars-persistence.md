# Star persistence (`stars.csv`)

The Tab5 keeps its Starred state locally. It never writes ratings back to the
iPod; the file exists so the user can apply the same choices manually in
iTunes, and so a later firmware/session can inspect what was chosen.

## Location

```
/sdcard/lib/<short8>/stars.csv
```

`<short8>` is the last 8 hex characters of the media-library UUID. For the
current library it is `4954524C`, so the file is:

```
/sdcard/lib/4954524C/stars.csv
```

The file is FAT 8.3-safe (`stars.csv`, temporary `stars.tmp`), because the
firmware mounts FATFS with long filenames disabled.

## Retrieval

- Remove the SD card and copy the file from the path above.
- Or, from the USB serial console, send `@`. The firmware prints
  `IAP2FILE /sdcard/.../stars.csv <size>` followed by base64 lines and
  `IAP2FILE_END ...`.
- Or run the helper, which does the same and writes the decoded file:

  ```sh
  python3 tools/fetch_stars.py /dev/ttyACM0 stars.csv
  ```

`!` is a debug command that toggles the current track's star, useful when the
D100H is not connected. `$` clears every local star and deletes `stars.csv`
(debug/reset).

## Columns

```text
updated,starred,library_id,track_id,album_id,artist_id,
title,artist,album,track_no,year,duration_sec,duration,device
```

| Column | Meaning |
| --- | --- |
| `updated` | Last change, local time, `YYYY-MM-DD HH:MM:SS` (or `boot+Ns` if the clock is not set). |
| `starred` | `1` = currently starred, `0` = explicitly unstarred. || `library_id` | Full iAP2/AFC media-library UUID. |
| `track_id` | 16-hex-digit iTunes persistent ID (the SQLite `item_pid`). |
| `album_id`, `artist_id` | Internal library IDs, useful for cross-referencing `library.bin`. |
| `title`, `artist`, `album` | Tags as stored in `library.bin`. `artist` is the album artist. |
| `track_no` | Track number within the album. |
| `year` | Album year. |
| `duration_sec` | Track length in seconds. |
| `duration` | Same length as `M:SS`, for quick visual matching. |
| `device` | iPod device name reported at star time. |

Text fields are quoted; embedded quotes are doubled and embedded CR/LF are
replaced with spaces. The current implementation keeps one row per track that
has ever been toggled. The last row for a `track_id` is authoritative; rows
with `starred=0` mean "unstar this track".

## Boot behaviour

The preview firmware seeds its clock from the firmware build timestamp; the
board's RX8130CE RTC is not read yet. Treat `updated` as an approximate
wall-clock label and rely on the row order/`starred` value for state. On a
production build with a real RTC or NTP this becomes accurate without a format
change.

`player_init()` loads `library.bin`, then reads `stars.csv` and applies every
row to `track_starred[]` (`1` sets, `0` clears). The local Starred playlist is
derived from that array, so it is correct immediately after boot.

When a star is toggled, the firmware rewrites the file through `stars.tmp`,
`fsync()`s it, removes the old file and renames the temporary file. If power is
lost between the remove and the rename, the next boot reads `stars.tmp` as a
fallback.

## Applying manually in iTunes

1. Enable **View → View Options → Persistent ID** in iTunes.
2. Match the 16-hex `track_id` (or use title/artist/album/track number).
3. Star or unstar the matching tracks.

The persistent ID is the reliable key. `artist` comes from the album-artist
field in `library.bin`; a track with a featured artist can therefore show the
album artist rather than the track artist.

## Limitations

- The iPod's own star/rating database is not modified. iAP2 library-rating
  write-back is not implemented.
- Star state is per media-library UUID. Switching libraries uses a different
  directory and a different `stars.csv`.
- If the SD card is absent, stars still change in RAM but are not persisted.
