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

Enter AFC mode, connect the Apple device to USB-A, and choose **Pair / Trust
Apple**. Unlock the Apple device, tap **Trust**, and enter its passcode if asked.
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

## Sync screen

The square layout follows Gray, Aqua, and Windows 95. Connection and Apple
trust are on the left; imported album/track counts, cached covers, and sync
progress are on the right. The footer shows SD usage and **Exit**.

- **Quick sync:** validates the imported library and caches the first 64 albums.
- **Full sync:** copies the database snapshot and caches all imported album art.
- Existing covers are reused. Copying a database snapshot does not rebuild the
  browsable `library.bin`; the desktop import step is still required for new
  tracks or albums.
- Sync requires AFC access and an SD card. Pairing, repeat sync, and leaving AFC
  are disabled while an operation runs. Results stay visible when sync finishes;
  **Exit** restarts in iAP2 mode. Failed database copies stop the job.

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

The copy streams 32 KB AFC reads and prints a SHA-256 digest for each file.
The serial command `V` re-reads the local files and prints their hashes for a
byte-for-byte check.

Verified on the inserted card:

```text
/sdcard/AFC/MLDB.SQL  50552832  sha256=d93caf337529244f5f234ee7d3249bb5bd39f33ab21a9eeb1d5c0406caadbe14
/sdcard/AFC/MLDB.WAL    358472  sha256=8d2ac8b963955f93d2f384cd81e8f942087e4666dd69faf2a64ca81542a13863
/sdcard/AFC/MLDB.SHM     32768  sha256=757f0b1581fda8d4fc692973ce6a8106fa191013a709ec424ec079f88b5ac77c
```

The card used here writes at roughly 0.4 MB/s, so the 50 MB database takes
about two minutes. `Y` formats the card (destroying all data) and mounts it;
the first boot mount after inserting an unformatted card fails, and an
explicit `Y` is needed.

After `StartService`, the firmware closes the lockdownd connection and keeps
only AFC open. The device otherwise resets the idle lockdownd connection
after about 30 seconds of a long copy, which also takes AFC down.

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

