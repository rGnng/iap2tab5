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
- the host sends SYN with its LSP (max_outgoing 30, max_len 65535,
  retransmit 4000 ms, ack timeout 500 ms, 3 sessions), adopts the device LSP,
  then sends an ACK
- data packets are stop-and-wait with ACK and 4 retries
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
The Now Playing queue panel shows a single-item queue (`1/1`) because the
iAP2 queue list has not been parsed yet.

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
