# AFC / usbmux bring-up

## Working stack

- USB host enumeration filter selects configuration 4 for the iPod
  `05ac:12aa`.
- `usbmux_host.c` opens the device, claims interface 1 (class `0xff`,
  subclass `0xfe`, protocol `0x02`), finds the bulk endpoints (`0x04` /
  `0x85`), sends the usbmux version request and the v2 setup packet.
- The mux TCP layer implements SYN / SYN+ACK / ACK, per-connection sequence
  numbers and a USB transmit queue. Payloads are buffered per connection.
- lockdownd on port 62078:
  - `QueryType`
  - `StartSession` with the HostID/SystemBUID from the pair record
  - TLS 1.2 using the pair record's `RootCertificate` and `RootPrivateKey`
  - `StartService com.apple.afc`
- AFC:
  - `GET_DEVINFO`
  - `READ_DIR`
  - `GET_FILE_INFO` (`st_size`, `st_ifmt`)
  - `FILE_OPEN` / `FILE_READ` / `FILE_CLOSE`
  - full-file streaming to `/sdcard/AFC/` with SHA-256

Verified on the connected iPod (iOS 15.x):

```text
usbmux v2 active
TCP ...->62078 established
StartSession OK, SSL=1
TLS established: TLSv1.2 / TLS-ECDHE-RSA-WITH-AES-256-GCM-SHA384
service com.apple.afc on port 51547 (ssl=0)
/iTunes_Control/iTunes/MediaLibrary.sqlitedb: size=50552832
/iTunes_Control/iTunes/MediaLibrary.sqlitedb-wal: size=358472
/iTunes_Control/iTunes/MediaLibrary.sqlitedb-shm: size=32768
read 16 bytes from MediaLibrary.sqlitedb OK
```

## Pair and trust on the Tab5

Enter AFC mode, connect the Apple device to USB-A, and choose **Pair / Trust**. Unlock the Apple device, tap **Trust**, and enter its passcode if asked.
The Tab5 generates its own RSA host identity; no computer or exported pair
record is needed. The connection panel shows USB connection, approved trust,
and ready file access separately. Approval is polled for up to two minutes;
denial, timeout, disconnection, and save failures leave the previous record
intact and allow a retry. Keep the device connected while pairing runs.

Pairing runs on the lockdown worker, not the display task. The Pair request
contains certificates and host identifiers only; private keys stay on the Tab5.
After generation, the device public key is checked again to avoid sending an
identity for a device that was swapped while generating keys. A successful
reply's escrow bag is saved with the identity.

The web page has the same **Pair / Trust** action (`POST /api/trust`,
AFC mode required). `/api/status` includes `apple_pair_busy`, `apple_trusted`,
and `apple_pair_status` for progress. Apple approval always happens on the
Apple device itself.

Existing pair records remain supported through web upload (`POST /api/pair`),
serial upload (`u`, plist, then `EOF`), or `/sdcard/pair.plist`. The saved
`/spiffs/pair.plist` takes precedence over the SD copy so a newly approved
identity survives reboot. Saves use a temporary file and keep the previous
record as `/spiffs/pair.plist.bak`; that backup is also used if a reboot
interrupts replacement. Records contain private keys and must stay out of Git.
Code-only OTA preserves SPIFFS; a full flash can overwrite it.

Reconnecting the same device reuses the saved TLS certificates and host
identifiers. A different device needs its own Trust approval and pairing
record. The current firmware stores one active record, with the previous
record retained as a replacement-recovery backup; it does not automatically
select records by device ID. Pairing a different device therefore replaces
the active record. The embedded iAP2/MFi identity is separate and is reused.

## Sync screen

The square layout follows Gray, Aqua, and Windows 95. A narrow left column
shows one Device state, **Pair / Trust**, and SD usage, with **Exit** below it.
Pairing instructions appear only when needed. The wider right area contains
library counts and sync progress; the redundant USB/trust/file-access checklist
has been removed.

**Pair / Trust** is disabled once trust is approved or AFC is connected. It
remains available for an attached device that still needs trust approval.

- **Sync:** refreshes the database snapshot and incrementally caches missing
  album art. There is one sync action; no separate Quick/Full choice.
- The panel shows live status, progress, and the current album, without numbered
  stage labels. Artwork normally downloads, decodes, and writes in one pass.
- Existing covers are reused. Copying a database snapshot does not rebuild the
  browsable `library.bin`; the desktop import step is still required for new
  tracks or albums.
- Sync requires AFC access and an SD card. Pairing, repeat sync, and leaving AFC
  are disabled while an operation runs. Results stay visible when sync finishes;
  focus moves to **Exit** once at completion, and **Exit** restarts in iAP2 mode.
  Failed database copies stop the job.

## mbedTLS patch

Apple's lockdown certificates have an empty subject/issuer Name. mbedTLS 3.6
rejects the empty RDNSequence with `MBEDTLS_ERR_X509_INVALID_NAME +
MBEDTLS_ERR_ASN1_OUT_OF_DATA`. Apply the local ESP-IDF patch once:

```sh
cd /home/ops/esp/esp-idf
git apply /home/ops/iap2tab5/tools/mbedtls-empty-name.patch
```

The patch makes `mbedtls_x509_get_name()` accept an empty Name. This is
required both for the client certificate and for the device certificate sent
during the TLS handshake.

## SD card and media copy

The Tab5 SD card is mounted at `/sdcard`; FATFS long filenames are enabled,
but the copied AFC database files keep 8.3 names for compatibility:

```text
/sdcard/AFC/MLDB.SQL   <- /iTunes_Control/iTunes/MediaLibrary.sqlitedb
/sdcard/AFC/MLDB.WAL   <- ...-wal
/sdcard/AFC/MLDB.SHM   <- ...-shm
```

If no card was present at boot, `app_config_sd_ensure()` releases the stale
SD LDO and retries the mount on demand.

Trigger the copy:

- serial console: `y`
- web page: **Media library -> Copy to SD**
- HTTP: `POST /api/sync`

The copy streams 64 KB AFC reads and prints a SHA-256 digest for each file.
The serial command `V` re-reads the local files and prints their hashes for a
byte-for-byte check.

Verified on the inserted card:

```text
/sdcard/AFC/MLDB.SQL  50552832  sha256=d93caf337529244f5f234ee7d3249bb5bd39f33ab21a9eeb1d5c0406caadbe14
/sdcard/AFC/MLDB.WAL    358472  sha256=8d2ac8b963955f93d2f384cd81e8f942087e4666dd69faf2a64ca81542a13863
/sdcard/AFC/MLDB.SHM     32768  sha256=757f0b1581fda8d4fc692973ce6a8106fa191013a709ec424ec079f88b5ac77c
```

During initial bring-up the card wrote at roughly 0.4 MB/s, so the 50 MB
database took about two minutes. This is historical; see the sync performance
notes below for the current path. `Y` formats the card (destroying all data) and mounts it;
the first boot mount after inserting an unformatted card fails, and an
explicit `Y` is needed.

After `StartService`, the firmware closes the lockdownd connection and keeps
only AFC open. The device otherwise resets the idle lockdownd connection
after about 30 seconds of a long copy, which also takes AFC down.

## Sync performance and cache rebuild

`POST /api/afc/cache/reset` explicitly queues a reboot, clears the current
library's packed thumbnails and numeric cover files, and starts a fresh Sync.
It does not remove `library.bin`, stars, album-seen history, pairing, settings,
or the separate Now Playing cache. The one-shot request is consumed before
any deletion, and reset runs before UI/cache file handles open. An absent SD
card or an active sync/pairing operation rejects the request. This debug
operation has no additional button in the normal Sync screen.

`GET /api/status` exposes `afc_sync` progress. For complete benchmark logs,
advance `GET /api/log?since=...` using each response's `X-Log-Seq` header;
repeatedly requesting `since=0` returns the same first chunk, not the tail.

Research and implementation:

- The [LVGL 9.6 TJPGD configuration](https://github.com/lvgl/lvgl/blob/v9.6.0/src/libs/tjpgd/tjpgdcnf.h)
  disables scaling, and its decoder removes the pixel-reduction implementation.
  Our former requests for scaled decoding failed, then retried from SD at 1:1.
  AFC now uses a separate copy of [upstream TJpgDec](https://elm-chan.org/fsw/tjpgd/en/appnote.html),
  with actual 1/2, 1/4 and 1/8 decoding and RGB565 output. It keeps a 4 KiB work
  pool and samples directly into the thumbnail; it does not allocate a full
  software-decoded frame. Stock LVGL and managed dependencies are unchanged.
- SD verification uses 64 KiB, 128-byte-aligned reads, matching the P4 DMA path
  in [ESP-IDF's SDMMC driver](https://github.com/espressif/esp-idf/blob/v5.5.6/components/sdmmc/sdmmc_cmd.c).
  These use POSIX `read()`: [size-optimized newlib](https://github.com/espressif/newlib-esp32/blob/master/newlib/libc/stdio/fread.c)
  can turn unbuffered `fread()` into single-byte I/O. SHA-256 and byte-count verification still cover the
  complete database files.
  The old small-buffer path remains available on allocation failure.
- Small AFC requests combine the header and arguments in one USB/mux send.
  Reads remain 64 KiB and sequential. This reduces transfers without increasing
  the outstanding-data window or introducing concurrent requests on one AFC
  stream. [libimobiledevice's AFC client](https://github.com/libimobiledevice/libimobiledevice/blob/master/src/afc.c)
  also serializes request/reply access to a client; parallel workers would need
  explicit ownership and receive-flow-control work first.
- Compressed covers over 8 MiB, or unable to fit contiguous PSRAM, stream to a
  temporary SD file in bounded chunks,
  then use the scaled decoder with an aligned read buffer. A short/failed copy
  is never published as an available original.
- Both [P4 hardware JPEG](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32p4/api-reference/peripherals/jpeg.html)
  and TJpgDec support baseline JPEG. A marker probe identifies progressive
  originals without three full-file decoder attempts on every sync.
  Progressive files use the managed libjpeg-turbo software decoder, with scaled
  scanline output sampled into RGB565. Existing originals are retried from SD.
  AFC mode skips the optional whole-pack PSRAM preload to leave room for
  progressive coefficient buffers. Corrupt/truncated images never become valid.
- The thumbnail pack is preallocated with aligned bulk writes. Per-run logs
  report reused covers and aggregate read/decode/write time.

### Album identity, dates and live updates

The catalog is scoped to the full library UUID, with unsigned 64-bit album and
track persistent IDs. `library.bin` remains the legacy import source;
`catalog.bin` stores live metadata, per-track discovery batches and observation
times, using checksummed snapshots and a recoverable backup. `catalog.rev`
binds the saved iAP2 revision to the snapshot checksum, so subsequent boots can
request incremental updates. iAP2 updates are
queued to the UI thread; reset reconciles existing IDs before pruning unseen
tracks at completion. Reconnects preserve discovery order.

After copying SQL/WAL/SHM, Sync reads the database through an immutable SQLite
VFS that verifies WAL checksums and overlays only committed frames. It writes
`afc_index.bin` before fetching covers. Dates use the Apple-to-Unix epoch offset;
the album date is its newest track date. A track missing from this index keeps
its album in the first, AFC-pending group, even when a thumbnail is available.
Within that group, Date Added uses the newest newly discovered track batch.
The index also supplies album years and authoritative disc/track numbers;
the previous index format loads with those optional fields unset until Sync.

`thumbs.map` maps album PIDs to stable `thumbs.bin` slots, including the AFC
artwork version. Legacy slots and their numeric original filenames remain in
place. Album growth appends slots instead of rebuilding the pack. The artwork
worker pins library/album identity at transfer Setup and saves a thumbnail on
successful playback cover decoding. First AFC import binds its artwork version
and reuses that image; subsequent known version changes trigger replacement.
Thumbnail readers and writers share a lock and update any PSRAM preload.
Replacements write a new slot before publishing the PID map, preserving the
old image across an interrupted update.

Progressive JPEG coefficient buffers are bounded by available PSRAM. Larger
covers spill coefficients to temporary files on SD; the decoder removes them
on success or error, and boot cleanup recovers space after an interrupted
decode. The fallback also handles CMYK covers.

`GET /api/status` includes `catalog` counts and `afc_pending_albums`, separately
from thumbnail counts and AFC job progress. Arbitrary album artwork requests
over iAP2 are not implemented: playback supplies individual covers and AFC
supplies bulk artwork.

### Catalog and cover recovery validation (2026-11-09)

The connected iPod's refreshed database contained **2,109 albums / 20,829
tracks**. The initial live iAP2 reconciliation reached the same counts and
saved its revision. AFC recovered the formerly unsupported JPEGs and the
recently added albums' covers: **2,103 available covers cached, zero failures**.
Existing cache slots were retained throughout.

The first iAP2 subscription from revision zero included roughly seven minutes
of deletion history before item metadata arrived. Once its snapshot is saved,
subsequent subscriptions use that snapshot's revision.
The reconnect test subscribed at 15.083 s after boot and completed at 15.137 s:
three small update messages, zero item groups, unchanged catalog counts, and
no deletion-history replay.

### Device measurements (2026-11-08)

One connected iPod and SD card, Gray theme, USB-A plus USB-C, SD debug logging
enabled. The imported library has 2,096 albums / 20,688 tracks and 2,090 artwork
manifest entries. Both cold runs started with an empty artwork cache. Timings
use firmware log timestamps from sync start to completion, excluding cache
reset, reboot, and pack creation. Baseline: `ce9d86d-sync-base`; optimized:
`ce9d86d-sync-final3`.

| Measurement | Baseline | Optimized |
| --- | ---: | ---: |
| Fresh artwork sync, including database verification | 1,306.809 s (21:47) | 681.651 s (11:22) |
| Repeat sync with cached covers | 65.130 s | 21.036 s |
| Database copy plus full verification | about 36 s | about 18 s |
| Covers successfully cached after the cold run | 1,989 | 1,992 |
| Cold artwork download failures | 4 | 0 |

The fresh run is about 48% faster; repeat sync is about 68% faster. All three
SQL/WAL/SHM hashes matched the source in both runs. The optimized repeat reused
1,992 thumbnails and read zero artwork bytes over AFC. Pairing remained trusted.
The 98 remaining conversion failures are 96 progressive JPEGs and two files
whose sampling factors are unsupported (album IDs 99 and 1465). They are still
reported as failures, and their originals remain on SD.

The optimized cold pass read 1,285,418,273 artwork bytes. Its one-pass counters
recorded 174.574 s reading (including SD spooling where needed), 444.543 s
in decoding, and 16.080 s writing. The remaining-original pass took 23.550 s,
compared with 906.018 s before. Software JPEG decoding is now the main cost.
TJpgDec's larger Huffman lookup configuration is a candidate for a subsequent
benchmark with a larger work pool; its speedup has not been measured here.

Validation: 88 host tests, ESP-IDF firmware and desktop simulator builds, Gray /
Aqua / Windows 95 screen checks, OTA, cold and repeat device syncs. JPEG tests
exercise production memory/file callbacks at all four scales, check colors
and odd-sized image edges, and check truncated input. Other tests cover SHA-256
read boundaries/failure paths, AFC request framing, low-memory SD fallback,
and cache deletion scope.

## Debug keys

| Key | Action |
| --- | --- |
| `a` | print usbmux, lockdownd and AFC status |
| `f` | re-run the AFC directory/stat/read probe |
| `u` | upload a pair plist over serial, terminated by `EOF` |
| `y` | copy the media library database + WAL/SHM to `/sdcard/AFC` |

## iAP2

iAP2 is implemented in `iap2_host.c` using USB configuration 2 and the HID
interface (interface 2, endpoint `0x83`). The Apple Accessories P-256
identity from `../iap2cert` is embedded in `iap2_identity.c`; MFi
authentication and identification succeed on the connected iPod.

Mode selection is stored in NVS:

- serial `J` toggles AFC config 4 / iAP2 config 2 and reboots
- web checkbox `iAP2 mode`
- iAP2 mode: `o` play/pause, `,` previous, `.` next, `x` MFi self-test

See `docs/iap2-bringup.md` for the report format, link layer, authentication
messages and known limitations. AFC and iAP2 are mutually exclusive. The
`ff/fd/01` interface in configuration 4 is the NCM/Ethernet function, not
iAP2.

