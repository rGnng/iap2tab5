# iAP2 bring-up

The Tab5 now runs a working iAP2 link over the iPod's USB HID interface. AFC
and iAP2 are separate USB configurations and are mutually exclusive.

## Mode switching

The mode is stored in NVS (`usbcfg` / `iap2`) so it works even when the SD
card is missing or unreadable.

- serial `J`: toggle AFC (config 4) / iAP2 (config 2), save, reboot
- web config checkbox `iAP2 mode`: sets the same NVS key; reboot to apply
- `/api/status` exposes `iap2`, `iap2_identified`, `iap2_mode`

```text
AFC:  USB config 4 -> PTP + ff/fe/02 usbmux + ff/fd/01 NCM
iAP2: USB config 2 -> audio control + audio streaming + HID (interface 2)
```

In iAP2 mode the firmware does not start the usbmux/AFC path or the USB HID
keyboard driver. Restore AFC with `J` and a reboot.

## USB HID transport

- interface 2, class `03/00/00`, interrupt IN endpoint `0x83`
- HID report descriptor: 208 bytes, vendor page `0xFF00`
- input reports 1..12: 5, 9, 13, 17, 25, 49, 95, 193, 257, 385, 513, 767 bytes
- output reports 13..21: 5, 9, 13, 17, 25, 49, 95, 193, 255 bytes
- each HID report is `[report_id][LCB][payload...]`; the LCB has
  `CONTINUATION=1` and `MORE_TO_FOLLOW=2`
- the working SET_REPORT form is `bmRequestType=0x21`, `bRequest=0x09`,
  `wValue=(0x02<<8)|report_id`, `wIndex=interface 2`, and the report data
  **includes** the report ID. `SET_IDLE`/`SET_PROTOCOL` are stalled by this
  iPod and are not sent.
- the IN transfer runs in 64-byte packets; reports larger than 64 bytes are
  reassembled from the `[report_id][LCB]` header before feeding the link layer
  using their exact descriptor sizes, including the trailing two bytes of
  258/386/514-byte reports. Stream writes are all-or-nothing; overflow causes
  controlled link recovery instead of silently dropping bytes.

## Link layer

Detection marker (host -> device, repeated every second):

```text
FF 55 02 00 EE 10
```

Link packet header (big-endian):

```text
FF 5A | length:2 | control:1 | seq:1 | ack:1 | session:1 | checksum:1
```

`checksum` is the two's complement of the sum of the first 8 header bytes.
Payload bytes are followed by the same checksum over the payload.

- control bits: `SYN=0x80`, `ACK=0x40`, `EAK=0x20`, `RST=0x10`
- control session id 10, EA session 11, FT session 12
- the host sends SYN with its LSP (max_outstanding 1, max_len 4096,
  retransmit 4000 ms, ack timeout 500 ms, max cumulative ACKs 1, 3 sessions), adopts the device LSP,
  then sends an ACK
- data packets are stop-and-wait with ACK and 4 retries
- TX fragmentation respects the peer's maximum packet length and retransmit
  timeout. A failed write retains its sequence and payload for retry.
- ACK/EAK-only packets do not consume RX sequence numbers. Duplicates are
  acknowledged without being delivered twice. Missing or unqueued data is
  never cumulatively acknowledged.
- Validated payloads enter a bounded 32-entry queue before ACK; a separate
  worker handles session parsing and storage. Reset generations discard stale
  queued work. USB completions wake the link task directly, with bounded parser
  work and periodic yields during continuous traffic.
- The parser validates headers before trusting packet lengths, scans using a
  cursor, and compacts once per batch. Reset during dispatch cannot underflow
  the receive length. Truncated frames expire so they cannot block reception
  indefinitely. Timed-out control transfers retain their DMA buffer until the
  completion callback is observed.
- the device ACKs each data packet and sends its own control-session CSM
  frames on session 10

## MFi authentication

The identity from `../iap2cert` is embedded in firmware:

```text
certificate.p7b  607 bytes  PKCS#7 SignedData, sent as 0xAA01 param 0
identity.pk8      67 bytes  converted to SEC1 DER (121 bytes) for mbedTLS
```

The certificate is an Apple Accessories P-256 certificate; the private key
matches it. `iap2_auth.c` signs the 32-byte iAP2 challenge with
**ECDSA P-256 over SHA-256(prehashed)** and emits raw `r||s` (64 bytes), as
the iAP2 0xAA03 parameter expects.

CSM authentication flow:

```text
device -> 0xAA00 RequestAuthenticationCertificate
host   -> 0xAA01 param 0 = PKCS#7 certificate
device -> 0xAA02 param 0 = 32-byte challenge
host   -> 0xAA03 param 0 = 64-byte raw ECDSA signature
device -> 0xAA05 AuthenticationSucceeded
```

The serial `x` command runs an embedded sign+verify self-test.

## Identification and control

```text
device -> 0x1D00 StartIdentification
host   -> 0x1D01 identification parameters
device -> 0x1D02 IdentificationAccepted
```

The firmware then requests Now Playing (0x5000), media library information
(0x4C00), power updates (0xAE00) and starts the HID component (0x6800).

### Power and charging

The Tab5 is a USB host and provides VBUS to the iPod. The iAP2
identification therefore declares `PowerProvidingCapability = Advanced (2)`
and `MaximumCurrentDrawnFromDevice = 0`. `StartPowerUpdates` requests
parameters 0, 1, 4, 5 and 6; parameter 2 (`AccessoryPowerMode`) is omitted
because the accessory does not draw power from the device.

Immediately after identification the firmware sends
`PowerSourceUpdate (0xAE03)` with:

- param 0 `AvailableCurrentForDevice` = 1000 mA (one of the spec's allowed
  values; the Tab5 USB-A switch is limited to ~1.3 A)
- param 1 `DeviceBatteryShouldChargeIfPowerIsPresent` = true

The first version declared `PowerProvidingCapability = None` and
`MaximumCurrentDrawnFromDevice = 20`, so the iPod treated the Tab5 as a
load, reported `AccessoryPowerMode = Low Power`, and did not charge. It now
reports `IsExternalChargerConnected = true` and `BatteryChargingState =
Charging` (`0xAE01` params 4 and 5). The firmware also sends Apple's USB
power-capability vendor request (0x40/0x40, 500/500 = 1000 mA) after
enumeration as a fallback for non-iAP2 power negotiation.

Verified on the connected iPod:

```text
authentication succeeded
identification accepted
now playing: status=playing
  title='next to you (MRM REMIX) [feat. Kanon Tachibana & Morimori Atsushi]'
  artist='Morimori Atsushi'
  album='FIRST:DREAMER'
media library information (164 bytes)
```

HID consumer-control bits: play 0, pause 1, next 2, previous 3, volume up 6,
volume down 7, mute 8, stop 9, fast-forward 10, rewind 11.

Each HID command must be sent as a pressed report **followed by an all-zero
release report**. Sending only the pressed report leaves the key held; the
iPod then treats bits 2/3 as scan/FF/REW instead of a next/previous track
tap.

Serial keys in iAP2 mode:

| key | action |
| --- | --- |
| `o` | play/pause (uses the last now-playing status) |
| `,` | previous track |
| `.` | next track |

`iap2_host_hid_command()` is the backend entry point for the UI.

## UI integration

The shared `player.h` API in `app/backend/mock_player.c` now dispatches to
iAP2 when `iap2_host_is_enabled()` is true (ESP builds only; the desktop
simulator keeps using the mock):

- Now Playing title/artist/album, play status, elapsed/duration, shuffle,
  repeat, battery and device name come from the iAP2 state
- the album cover uses a matching mock-library album if the real album title
  is also present there; otherwise it shows the placeholder
- play/pause, next/previous, shuffle/repeat and volume steps send HID
  consumer-control reports
- the progress bar/timestamps poll `player_get_snapshot()`; `ui_tick_cb()`
  also refreshes the menu play icon so the iAP2 state is visible without a
  change callback from the iAP2 task
- Settings -> Device shows `iPod touch`, battery and connection; Settings ->
  Backend shows `iAP2 (USB)` and `Connected`

The hardware build loads the real library from
`/sdcard/lib/<short8>/library.bin`; the mock library and boot demo playback
are simulator-only. Library counts are gated on the device connection, so
with no iPod the library pages are empty and Now Playing shows `No track`.
The Now Playing queue panel uses the device's parsed queue list and starts
after the current track. There is no position/total counter. Displayed songs
are numbered consecutively from 01, while actions retain the original device
queue index. At the end of the list it shows `No upcoming songs`.

Opening the panel uses the queue already in RAM; it does not request it from
iOS. The panel completes its first display refresh before a timer binds rows
in batches (at most four rows, with an 8 ms row-binding budget before yielding;
one slow row and layout work can exceed it). A tail spacer reserves the scroll extent.
Closing/switching pages cancels the timer; a changed queue restarts binding.
Track PID lookups use a 128 KiB index instead of repeatedly scanning the
20,688 metadata records. Trace mode logs panel construction, first refresh,
row-batch work and completion separately.

On the connected device (20,688 tracks, 14 upcoming rows, three toggles during
startup library synchronization), first refresh fell from 304–338 ms to
117–138 ms. Blocking panel construction fell from 196–202 ms to 19–20 ms;
metadata lookups for all 14 rows fell from 106–108 ms to under 1 ms. These are
first-panel timings: the rows populate after that refresh, rather than all
being present at once. Validation includes 96 host tests, simulator checks in
all three themes, OTA, and two boots confirming the Japanese font loads and
roughly 10 MiB of PSRAM remains free.

The Japanese font loads before the optional whole-pack thumbnail preload.
Only `main` binds that pack, after `thumb_cache_open()`; binding it in `ui_init()`
as well could start two 12 MB preloads while the first was still in flight.
Preloading now leaves at least 4 MiB of PSRAM for rendering, decoder and USB
work. If the pack does not fit, existing SD and per-cover caches serve the
artwork. Failed font reads release their allocation and log the read/allocation
failure rather than retaining a partial buffer.

### Queue editing investigation (2026-11-08)

The checked [Accessory Interface Specification R32](https://www.scribd.com/document/863231068/Accessory-Interface-Specification-R32-No-Watermark),
sections 95.12 and 95.13, documents queue reads and `SetNowPlayingInformation`
(`0x5003`) for seeking, selecting a queue index, or requesting a queue window.
It does not document a single-item insert, move, or remove command. The
[Nocturne iAP2 implementation](https://github.com/usenocturne/nocturne/blob/main/crates/iap2/src/csm/now_playing.rs)
uses the same three parameters. This does not establish that no newer or
undocumented extension exists; no working seamless edit mechanism has been
verified on this iPod.

Replaying a revised list with `PlayMediaLibraryItems` (`0x4C07`) and restoring
the current item/time is a possible fallback, but may interrupt playback.
The current sender also limits lists to 244 IDs per 2048-byte frame, and the
reader retains at most 512 IDs; a rebuild must not silently truncate a queue.
Queue-changing menu actions remain unimplemented pending a verified method
that preserves uninterrupted playback.

iOS has a separate [MediaPlayer `prepend` API](https://developer.apple.com/documentation/mediaplayer/mpmusicplayercontroller/prepend(_:))
that inserts items immediately after the current song. It requires code on
the iOS device; it is not an iAP2 command. An on-device helper would be a
separate approach and has not been implemented or tested here.

Now Playing artwork comes from the iAP2 File Transfer session (type 2). The
receiver keeps one buffer per transfer ID, so the queue file (type 3) and the
artwork (type 2) can be interleaved; it sends Start on Setup and Success on
LastData. The artwork is decoded to a 360x360 RGB565 buffer and the UI keeps
the previous cover across track changes until a new image or a zero-length
type 2 Setup arrives. The decoded 360x360 cover is cached per library
(`np.raw` + `np.txt`) and reloaded at boot; an album change or a zero-length
type 2 Setup drops it.

Observed UI snapshot after the integration:

```text
ui: snapshot: real=1 status=1 title='GLORY:ROAD (long ver.)'
  artist='Morimori Atsushi' album='FIRST:DREAMER'
  elapsed=5 dur=283 queue=0/1 bat=100 dev='iPod touch'
```

Pressing `7` (the UI play/pause key) sent a HID play report and the device
status changed to playing; the next `a` snapshot showed `status=1`.

## Known limitations

- The SD card currently fails to mount after the format/reboot cycle
  (`sdmmc_host_wait_for_event` timeout). Reseat the card and use `Y` to
  reformat if needed. The iAP2 mode is in NVS, so it does not depend on SD.
- AFC and iAP2 cannot run at the same time; switching requires a reboot.
- The USB HID keyboard driver is not started in iAP2 mode.
- iAP2 next/previous HID reports are accepted by the device, but track
  changes can take a moment and the now-playing update may lag.
- Full media-library transfer over iAP2 FT (session 12) is not implemented;
  use AFC config 4 for the SQLite/WAL/SHM copy.

## Security

`iap2_identity.c` contains the Apple Accessories private key. The firmware
image therefore contains that key; do not distribute the binary or commit
the identity to a public repository.
