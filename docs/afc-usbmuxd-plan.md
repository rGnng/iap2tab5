# AFC / usbmuxd on Tab5: implementation plan

Status: **not implemented yet**. The current preview firmware has the UI,
mock library, SD config, Wi-Fi web UI and USB HID host. AFC needs a separate
USB configuration and protocol stack.

## What is already in place

- ESP-IDF 5.5.6, USB Host stack and the USB-A port (`bsp_usb_host_start`).
- USB HID keyboard/knob on the USB-A port.
- microSD mount (`/sdcard`) and JSON config.
- Wi-Fi AP + HTTP server, which can drive an AFC pull later.
- 3.5 MB OTA slots and SD storage for downloaded files.

## What AFC needs

An iPod/iPhone talks usbmux over a **different USB configuration** than HID:

```text
USB-A (P4 high-speed host)
  └─ configuration 4 (PTP + Apple Mobile Device + Ethernet)
      └─ interface 1, class 0xFF / subclass 0xFE / protocol 0x02
          ├─ bulk OUT EP 0x04
          └─ bulk IN  EP 0x85
```

Configuration 2 is the iAP2/HID configuration (audio control, audio
streaming and a HID interface). The two configurations are mutually
exclusive; switching requires a USB-A power cycle / re-enumeration. The
current firmware must keep HID as the default so a keyboard still works.
iAP2 over HID needs Apple's MFi authentication coprocessor; the Tab5 does not
have one, so the link can only be brought up to the authentication request.

## Protocol layers

1. **USB host client**
   - Enumeration filter: Apple VID `0x05AC` → configuration 4.
   - Open device, claim mux interface 1, find bulk endpoints.
   - Reset/re-enumerate if the device is in the HID configuration.
2. **usbmux v2**
   - Header framing, version handshake, `Connect` to port 62078, TCP-like
     SYN/ACK and sequence/window handling.
   - Two simultaneous streams: lockdown and AFC.
3. **lockdown**
   - XML plist `QueryType`, `StartSession` with the host pair record.
   - Upgrade the stream to TLS (mbedTLS) using the pair record's
     `RootCertificate` / `RootPrivateKey`; do not verify the device certificate.
   - `StartService "com.apple.afc"` and read `Port`.
4. **AFC**
   - 40-byte header (`AFC_OP_*`), `FILE_OPEN`, `FILE_READ`, `FILE_CLOSE`.
   - Read in 128 KiB chunks and write to SD using a temporary file + rename.
5. **UI / web**
   - Progress and errors on the Tab5.
   - `POST /api/afc/pull` to start a pull, `GET /api/afc/status` to poll.
   - Target: `iTunes_Control/iTunes/MediaLibrary.sqlitedb` plus `-wal`/`-shm`.

The tested Linux host already has a pair record for the iPod:

```text
/var/lib/lockdown/00008150-000935823E22401C.plist
```

Copy it to the Tab5 SD card as `/sdcard/pair.plist` (never commit it).
The device must be unlocked and trusted, and the IPv6/USB config must be
switched back to HID after the pull.

## PoC order

1. Connect the iPod to the Tab5 USB-A port.
2. Add the enumeration filter and log VID/PID + config 4 + mux interface.
3. Implement the usbmux version handshake and connect to 62078.
4. Implement lockdown + TLS and StartService.
5. Pull a small file (for example `/iTunes_Control/iTunes/MediaLibrary.sqlitedb`
   metadata) to SD.
6. Pull the DB + WAL + SHM with progress; verify the file size and SHA-256
   against the Linux host copy.

## Open questions / blockers

- Need physical access to the iPod on the Tab5 USB-A port for bring-up.
- Need the pair record copied to the SD card.
- Need to decide whether the web config toggle may switch away from HID
  (which temporarily disables the keyboard/knob).
- The current C6 firmware is factory ESP-Hosted 1.4.1; Wi-Fi works for the web
  UI but should be upgraded to 2.13.x for production.

Estimated effort for a minimal pull path: 1,500–2,500 lines of C plus hardware
bring-up, matching the feasibility study in
`../iap2mediactrl/docs/tab5-afc-db-pull-feasibility.md`.
