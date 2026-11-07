# iAP2 Tab5

An iPod / iPhone media controller for the **M5Stack Tab5** (ESP32-P4), built around a shared LVGL 9.6 UI that runs both on the desktop and on the real hardware.

The Tab5 has a 5-inch 1280×720 MIPI-DSI panel, 16 MB flash, 32 MB PSRAM and an ESP32-C6 for Wi-Fi/BLE. It talks to the iPod over USB using **iAP2** for playback control and metadata, and **AFC** for importing the media library and album artwork. A rotary encoder plus seven keys (reported as standard USB HID keys) are the primary input; the touchscreen is optional.

This repository contains:

- a 1280×720 **SDL2 desktop simulator** with a generated mock library, boot sequence and screenshot support;
- a working **ESP-IDF firmware** for the Tab5 with iAP2, AFC, Wi-Fi, a web UI, OTA updates and a BLE remote;
- the shared UI, backend interface, fonts, tools and bring-up documentation.

![Tab5 UI overview](docs/screenshots/overview.png)

## Status at a glance

| Area | State |
| --- | --- |
| Desktop simulator | Working; mock library, keyboard/touch input, headless screenshots |
| Shared UI | 6 top-level screens, knob/key focus engine, optional touch |
| iAP2 over USB | Working; MFi authentication, identification, Now Playing, HID controls, artwork |
| AFC / usbmux | Working; pair record, lockdownd TLS, database/artwork copy |
| Real media library | Imported from the iPod's SQLite database into a compact `library.bin` |
| Album artwork | AFC thumbnail sync plus iAP2 Now Playing artwork, cached on SD |
| Remote input | USB HID knob/keypad, Ulanzi D100H over USB or BLE |
| Network | Wi-Fi station/AP, web configuration, live debug page, OTA |
| iAP2 queue | Not parsed yet; the Up Next panel reports a single-item queue |
| iAP2 media-library subscription | Disabled by default; the AFC import path is the supported one |

## Features

### UI

- Six top-level screens: **Now Playing**, **Albums**, **Artists**, **Songs**, **Playlists**, **Settings**.
- Bluetooth pairing lives in Settings → Bluetooth: opening the page starts a scan, new devices appear under **Search Devices** at the bottom, and selecting one pairs it. A dedicated **AFC Sync** boot mode is also available.
- Vertical menubar on the left or right, with a 72 px clock area and a 72 px device-battery area.
- Knob + key navigation with a custom focus engine; touch works as an optional pointer and can be disabled in Settings.
- Now Playing includes artwork, title/artist/album, elapsed time, shuffle/repeat/star, transport controls, an Up Next panel and three background modes: Theme, Blurred cover, Cover gradient.
- Long-press Enter opens a context menu with Go to album/artist, Star/Unstar, Shuffle and Repeat.
- Playing from a list queues what follows it: an album track plays through the end of its album, an artist or Songs track wraps around the list, and Shuffle on Albums, Artists and Playlists plays that list in random order, and Albums has a Shuffle All row above the first album. See `docs/design.md` §5.2.1.
- Albums, Artists, Playlists and Settings use a master/detail layout; Songs uses a virtualized list.
- A 72 px mini Now Playing bar appears on every screen except Now Playing.
- Album sort order can be add date, name or year, ascending or descending; albums without an AFC add date fall back to the order in which iAP2 first reported them.
- UI is grayscale, square-cornered and designed for a 5-inch 1280×720 panel.

### Input

The simulator and the firmware share the same logical key events:

| Input | Action |
| --- | --- |
| `Up` / `Down` | Move focus (rotary encoder) |
| `Enter` | Activate; hold 650 ms for the context menu |
| `1`–`5` | Jump to Now Playing / Albums / Artists / Songs / Playlists |
| `6` or `Backspace` | Back |
| `7` | Play / pause |
| `Page Up` / `Page Down` | Page the active list; on Now Playing, Page Up toggles Up Next |
| `[` / `]` | Previous / next screen (simulator and serial console) |
| `C` | Context menu (simulator/debug) |

On hardware, the same events can come from:

- a USB HID keyboard or rotary/keypad controller on the USB-A port;
- the Ulanzi D100H remote, either through its USB HID interface or over BLE via the ESP32-C6;
- the USB Serial/JTAG console (`w`, `s`, `Enter`, `b`, `1`–`7`, `c`, ...);
- the web debug page, which can inject keys remotely.

Previous/next on the D100H supports tap-to-skip and hold-to-rewind/fast-forward. Its top-right key opens the context menu on a short press and toggles Star on a long press.

### Device bridge

- **iAP2** runs on USB configuration 2: Apple MFi P-256 authentication, identification, Now Playing metadata, HID consumer controls and File Transfer artwork.
- **AFC** runs on USB configuration 4: usbmux v2, lockdownd `QueryType`/`StartSession`/TLS, `StartService com.apple.afc`, directory/stat/file reads, and streaming copy to SD.
- AFC needs a matching lockdownd pair record, uploaded through the web page or serial console. It is stored in the internal SPIFFS partition and is erased by a full flash.
- The two USB modes are mutually exclusive. The selected mode is stored in NVS; a reboot switches between them.
- The media library is read from `/sdcard/lib/<short8>/library.bin`, generated on a host from the iPod's `MediaLibrary.sqlitedb`.
- Album thumbnails are kept in a packed cache on SD; the Now Playing cover comes from iAP2 and can fall back to AFC artwork cached on the card.
- Local star state is stored in `/sdcard/lib/<short8>/stars.csv`; it is never written back to the iPod.
- Settings provides pairing/connection, Quick sync, Full sync, cached artwork counts and a one-click switch back to iAP2.

### Desktop simulator

- SDL2 window at 1280×720; the mouse acts as the touchscreen.
- The optional **KCN4M1** boot animation (Settings → Boot Screen, default OFF) plays a three-stage POST-style sequence at startup on the device. The simulator plays it only with `--boot`.
- Mock library with 24 artists, 100 albums, 562 tracks, 6 device playlists and generated abstract covers.
- Screenshot mode works without X11/Wayland by using SDL's offscreen driver.
- Command-line switches can select a screen, menubar side, injected key sequence, boot freeze time and window zoom.

## Quick start: desktop simulator

### Requirements

Debian / Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build libsdl2-dev
```

CMake downloads **LVGL v9.6.0** into `build/_deps` on the first configure. To use a local LVGL checkout instead:

```bash
cmake -S . -B build -G Ninja -DLVGL_SOURCE_DIR=/path/to/lvgl
```

### Build and run

```bash
cmake -S . -B build -G Ninja
cmake --build build -j
./build/tab5_sim
```

The `run` target is also available:

```bash
cmake --build build --target run
```

### Simulator options

```text
--zoom <factor>       Window zoom, e.g. 0.75 (default: 1.0)
--screenshot <path>   Render one frame to a PNG and exit
--wait <ms>           Wait before the screenshot (default: 1000)
--screen <n>          Start on a specific screen
--menubar <side>      Menubar position: left or right
--keys <list>         Inject keys, e.g. "2,enter" or "context"
--boot                Play the KCN4M1 boot animation
--no-boot             Skip the boot sequence (default)
--boot-freeze <ms>    Freeze the boot sequence at a time offset
--headless            Force SDL's offscreen video driver
--help                Show help
```

Examples:

```bash
# Start on Albums with the menubar on the right
./build/tab5_sim --no-boot --screen 2 --menubar right

# Headless screenshot (no display server required)
mkdir -p build/screenshots
./build/tab5_sim --no-boot --screenshot build/screenshots/ui.png --wait 1500

# Freeze the boot sequence at specific times
./build/tab5_sim --boot-freeze 1200 --screenshot build/screenshots/boot-post.png --wait 300
./build/tab5_sim --boot-freeze 3900 --screenshot build/screenshots/boot-app.png --wait 300
./build/tab5_sim --boot-freeze 5000 --screenshot build/screenshots/boot-display.png --wait 300
```

`--boot-freeze` only stops the animation for screenshots/debugging. Without `--boot` the simulator starts directly in the UI; with `--boot` the animation enters the UI after about 5.8 seconds.

### Mock library

The simulator reads `assets/mock/library.json`, the generated PNG covers and the prebuilt thumbnail packs. The hardware image does not include this data: on hardware the library comes from the SD card and the device.

```text
assets/mock/
  library.json                 # 24 artists / 100 albums / 562 tracks / 6 playlists
  covers/000.png ... 099.png   # 256×256 generated covers
  thumbs34.bin, thumbs54.bin, thumbs80.bin  # packed raw thumbnails
tools/gen_mock_library.py      # library/cover generator
```

Regenerate it with:

```bash
python3 tools/gen_mock_library.py
# optional:
python3 tools/gen_mock_library.py --albums 100 --seed 20261104
```

The generator uses a fixed seed by default, so the output is reproducible. Editing the mock data does not require a C rebuild. The generator needs Python 3 and Pillow.

### Fonts and icons

The checked-in LVGL font subsets are generated from the fonts in `assets/fonts/`:

```bash
./tools/gen_icons.sh          # Bootstrap Icons, 24/32/40/48 px
./tools/gen_web_ui.py         # embed assets/web/index.html
./tools/gen_plug_art.py       # embed assets/ui/plug-in.png as the no-iPod illustration
./tools/gen_inter_fonts.sh    # Inter, proportional + tabular digits
./tools/gen_boot_font.sh      # optional VT323 boot font
```

These scripts need Node/npm (for `lv_font_conv`); the Inter script also needs Python `fonttools`.

## Tab5 firmware

The firmware lives in [`firmware/`](firmware/) and reuses the same `app/ui` and `app/backend` code as the simulator. It is built with **ESP-IDF 5.5.6** and esp-bsp's `m5stack_tab5` board support.

### Toolchain and build

```bash
. $IDF_PATH/export.sh          # tested with ESP-IDF 5.5.6
cd firmware
idf.py set-target esp32p4      # first time only
python3 ../tools/gen_iap2_identity.py
idf.py build
```

The MFi identity is **not committed**: `tools/gen_iap2_identity.py` reads `../iap2cert/certificate.p7b` and `../iap2cert/identity.pk8` by default and writes the ignored `firmware/main/iap2_identity.c`. Pass `--cert` / `--key` to use another location.

After moving from IDF 5.4 to 5.5, run `idf.py fullclean` once and do one full `idf.py flash`; later code-only updates can use `app-flash`.

### Flash and monitor

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Exit the monitor with `Ctrl+]`. If auto-reset fails, hold **BOOT**, tap **RESET**, then release **BOOT**.

### SD card

The card is mounted at `/sdcard` (FAT32 recommended). A typical setup looks like this:

```text
/sdcard/
  config.json                         # UI, Wi-Fi and USB settings
  wifi.txt                            # station SSID/password (optional)
  JPFONT.TTF                          # optional Japanese font for metadata
  AFC/MLDB.SQL, MLDB.WAL, MLDB.SHM    # copied from the iPod
  lib/<short8>/library.bin            # imported media library
  lib/<short8>/thumbs.bin, thumbs.idx # packed 54×54 artwork cache
  lib/<short8>/stars.csv              # local star state
  lib/<short8>/np.raw, np.txt         # cached iAP2 Now Playing cover
  lib/<short8>/album_seen.bin         # iAP2 first-seen album order fallback
```

Ready-to-copy examples are in [`docs/sd/`](docs/sd/). Missing or invalid files fall back to built-in defaults. Settings are written back to `config.json` at most once per second while a value is being changed.

See [`firmware/README.md`](firmware/README.md) for the full `config.json` schema, partition table, serial commands and hardware notes.

### USB ports

| Port | Connection | Role |
| --- | --- | --- |
| USB-A (J10) | P4 high-speed OTG | iPod, USB HID keyboard/knob and D100H (normal host port) |
| USB-C (J8) | P4 full-speed PHY0 / USB Serial/JTAG | Console and flashing; optional FS host through a powered hub/dock |

- `usb_mode: 0` — USB-A high-speed host (default); USB-C stays a device / serial console.
- `usb_mode: 2` — USB-A HS & USB-C OTG (Both A+C): USB-A HS + USB-C FS dual host; verified with an iPod on USB-A and a Ulanzi D100H behind a powered USB-C hub.
- Settings → USB Options offers the same two modes as **USB-A HighSpeed Only** and **USB-A HS & USB-C OTG**.
- The old USB-C-FS-only mode (`1`) has been removed; existing configs fall back to USB-A HS.

A plain USB-C hub that only draws power from the host cannot power the Tab5. Use a hub/dock that supplies 5 V upstream. The runtime/boot behavior, compile-time constraints and Settings UI integration notes are in [`docs/usb-modes.md`](docs/usb-modes.md).

### Wi-Fi, web UI and OTA

If `/sdcard/wifi.txt` is missing or station mode fails, the firmware starts an open AP:

- SSID: `Tab5-Debug`
- Address: <http://192.168.4.1>

The main page is a single-page control panel (monospace, dark, square-cornered, inspired by `../iap2mediactrl`) with sections for Now Playing and transport keys, device status, charging, the media library (upload `library.bin`, copy the iPod database, upload the lockdown pair record), system settings (volume, brightness, USB mode, iAP2 mode, orientation, Wi-Fi), firmware OTA with a progress bar, and logs. It refreshes every 2 s. The HTML lives in `assets/web/index.html`; `tools/gen_web_ui.py` regenerates `firmware/main/web_index.h`, which the firmware embeds. The **Device clock** card shows the RTC-backed local time and has a **Sync browser time** button that writes the browser's UTC epoch to the system clock and the RX8130CE. The `/debug` page provides a live log, key injection, BMP screenshots, log-level control, USB host-mode switching, reboot and OTA upload. This is the main control path when USB-C is in host mode and the serial console is unavailable.

Enable **Debug log to SD** in the config card to append `ESP_LOG` output to `/sdcard/debug.log`. The log is written by a low-priority task and flushed after each line, so it survives a reset; download it from **Download SD debug log** on the main page or `GET /api/debuglog`. At 4 MB it rotates to `debug.old.log`. This is useful when a boot hang happens before the web server is reachable.

The web server has no authentication, and the fallback AP is open. Run it only on a trusted network.

### ESP32-C6 co-processor

The Tab5's ESP32-C6 provides Wi-Fi and BLE through ESP-Hosted. The host project uses **ESP-Hosted 2.13.1**, while many factory units still ship with **1.4.1**. Basic AP mode may work with the mismatch, but BLE and station mode are not reliable.

Prebuilt 2.13.1 C6 images and flashing instructions are in [`tools/c6_firmware/`](tools/c6_firmware/). The first update requires the internal `C6_ISP` header; later 2.x updates can use the in-band path.

The host enables the ESP-Hosted heartbeat (5 s interval, 30 s timeout) and logs the C6 firmware version at boot. If the C6 stops responding while the P4 UI keeps running, the watchdog logs the timeout and performs a hard reboot, which also resets the C6; no manual power cycle is needed.

### Media library import

The AFC path can copy the iPod's SQLite database and artwork to SD. Converting the database to `library.bin` is a host-side step:

```bash
python3 tools/media_db_to_library.py \
    --db /tmp/ipod-db/MLDB.SQL \
    --wal /tmp/ipod-db/MLDB.WAL \
    --out /tmp/ipod-db/library.json \
    --bin /tmp/ipod-db/library.bin
```

Upload the result over Wi-Fi or serial, or copy it directly to the card:

```bash
curl --data-binary @/tmp/ipod-db/library.bin \
     http://<tab5-ip>/api/library
```

The serial console also supports file transfer and debug commands. The complete workflow, including artwork sync and the `library.bin` format, is documented in [`docs/media-library-import.md`](docs/media-library-import.md).

### Security

`firmware/main/iap2_identity.c` contains the Apple Accessories private key once generated. The firmware image therefore contains that key: do not distribute the binary or commit the identity to a public repository. The pair record stored on the internal SPIFFS partition also contains private keys.

## Repository layout

```text
.
├── app/
│   ├── backend/                  # player interface, mock/iAP2 dispatch, BLE state
│   │   ├── player.h
│   │   ├── mock_player.c
│   │   └── ble_remote.c
│   └── ui/
│       ├── ui.c                  # all screens, focus engine, menubar
│       ├── boot.c                # boot sequence
│       ├── icons.h
│       └── fonts/                # Bootstrap Icons + Inter + VT323
├── sim/
│   ├── main.c                    # SDL2 display, keyboard mapping, CLI
│   └── screenshot.c              # PNG snapshots
├── firmware/
│   ├── main/                     # ESP-IDF app: iAP2, AFC, USB, web, BLE, config
│   ├── components/               # local esp_lvgl_port fix
│   ├── partitions.csv
│   └── README.md
├── tools/
│   ├── gen_mock_library.py
│   ├── media_db_to_library.py
│   ├── gen_iap2_identity.py
│   ├── gen_icons.sh
│   ├── gen_inter_fonts.sh
│   ├── c6_firmware/              # ESP-Hosted slave images and flashing docs
│   └── ...
├── tests/
│   └── test_d100h_protocol.py    # host-side D100H codec tests
├── docs/                         # design, bring-up notes, screenshots, SD examples
├── assets/                       # fonts and mock library
├── lv_conf.h
└── CMakeLists.txt
```

`app/ui/` depends only on LVGL and the `app/backend` interfaces. SDL2, screenshots and command-line handling stay in `sim/`; ESP-IDF, USB and SD stay in `firmware/`. That keeps the page code portable between the simulator and the board.

## Architecture

The backend contract is [`app/backend/player.h`](app/backend/player.h). It exposes library queries, Now Playing snapshots and playback commands. `mock_player.c` implements the simulator's player and, on ESP builds, dispatches the same API to the real library and iAP2 host.

On hardware:

- `iap2_host.c` handles the iAP2 link, authentication, identification, Now Playing, HID commands and artwork transfers.
- `usbmux_host.c`, `lockdown_client.c` and `afc_client.c` implement the AFC path.
- `thumb_cache.c` and `star_store.c` manage the on-SD artwork and star caches.
- `web_server.c` provides the configuration/OTA/debug UI.
- `ble_remote.c` and `ulanzi_d100h.c` handle the BLE remote and its protocol.

The UI is deliberately not tied to LVGL's default input group. A small focus engine keeps track of page-specific focus items, detail/list transitions and value editors, so the same button mapping works on every screen.

## Testing

The host-side D100H protocol tests compile the production codec with the system C compiler:

```bash
python3 -m unittest discover -s tests -v
```

They cover captured BLE vectors, HID framing, padding rejection and the encrypted heartbeat.

## Screenshots

UI mock:

- [Boot sequence](docs/screenshots/boot-sequence.png)
- [Now Playing](docs/screenshots/now-playing.png)
- [Now Playing + queue](docs/screenshots/now-playing-queue.png)
- [Now Playing context menu](docs/screenshots/now-playing-menu.png)
- [Albums](docs/screenshots/albums.png)
- [Album detail](docs/screenshots/album-detail.png)
- [Artists](docs/screenshots/artists.png)
- [Artist detail](docs/screenshots/artist-detail.png)
- [Songs](docs/screenshots/songs.png)
- [Settings](docs/screenshots/settings.png)
- [Settings — Clock](docs/screenshots/settings-time.png)
- [Settings — Now Playing background](docs/screenshots/settings-now-playing-background.png)
- [Menubar on the right](docs/screenshots/menubar-right.png)

Real Tab5 hardware:

- [Imported library with Japanese metadata](docs/screenshots/tab5-real-library-jp.png)
- [Real album covers](docs/screenshots/tab5-real-covers-albums.png)
- [iAP2 Now Playing artwork](docs/screenshots/tab5-real-cover-now-playing.png)
- [AFC sync settings](docs/screenshots/tab5-settings-afc.png)
- [Dedicated AFC sync screen](docs/screenshots/tab5-afc-sync.png)

## Known limitations

- **iAP2 and AFC are mutually exclusive.** Switching modes requires a reboot.
- **The iAP2 queue is not parsed yet.** On the device, Up Next shows the accessory-side list rather than the iPod's queue. Playback actions send the ordered list (albums, artists, playlists) or `PlayMediaLibrarySpecial` (songs) to the iPod, and normal playback control works.
- **The iAP2 media-library subscription is disabled by default.** The device's large delete-phase bursts are not yet reliable over the current link layer; use the AFC import path. The experimental path is enabled with `-DIAP2TAB5_FULL_LIBRARY_SYNC`.
- **Star ratings are local.** They are persisted in `stars.csv` but not written back to the iPod.
- **Some artwork falls back to a color placeholder.** TJPGD handles baseline JPEG; progressive or otherwise unsupported images may not decode.
- **The clock is backed by the RX8130CE RTC.** On a battery-empty board it is seeded from the build timestamp, then corrected by the web **Sync browser time** button or Settings → Date Time.
- **USB-C host mode needs a powered hub/dock**; dual-host mode is verified. Battery charging can be enabled/disabled from the web (`/api/charge`). See [`docs/usb-modes.md`](docs/usb-modes.md).
- **The C6 and P4 must use matching ESP-Hosted versions** for reliable Wi-Fi and BLE.
- **The firmware image contains the MFi private key**; do not publish built binaries.
- **The simulator is not a performance model.** It renders with LVGL software rendering, while the Tab5 uses PSRAM buffers, PPA and a MIPI-DSI panel.

## Documentation

- [`docs/design.md`](docs/design.md) — UI requirements, key map, focus model and layout specifications
- [`firmware/README.md`](firmware/README.md) — build, partitions, SD config, serial keys and hardware notes
- [`docs/iap2-bringup.md`](docs/iap2-bringup.md) — iAP2 link layer, MFi authentication, identification and control
- [`docs/afc-bringup.md`](docs/afc-bringup.md) — usbmux, lockdownd, AFC and pairing
- [`docs/media-library-import.md`](docs/media-library-import.md) — database conversion, `library.bin` and artwork cache
- [`docs/stars-persistence.md`](docs/stars-persistence.md) — `stars.csv` format and behaviour
- [`docs/d100h-ble-handover.md`](docs/d100h-ble-handover.md) — D100H BLE protocol analysis and driver notes
- [`docs/afc-usbmuxd-plan.md`](docs/afc-usbmuxd-plan.md) — AFC/usbmux implementation plan
- [`tools/c6_firmware/README.md`](tools/c6_firmware/README.md) — ESP32-C6 firmware and flashing

## Roadmap

- Parse the iAP2 queue and library update stream so Up Next and newly added media reflect the device.
- Harden the iAP2 link window/ACK handling and re-enable the full library subscription.
- Add optional NTP sync now that the RX8130CE RTC is wired up.
- Add tear-free DSI/VSYNC output on hardware.
- Expand context menus and queue actions across Albums, Artists and Songs.
- Continue CJK font coverage and metadata testing.

## Third-party components

- [LVGL](https://lvgl.io/) — MIT
- [SDL2](https://libsdl.org/) — zlib license
- [Bootstrap Icons](https://icons.getbootstrap.com/) — MIT
- [Inter](https://rsms.me/inter/) — SIL OFL 1.1
- [VT323](https://fonts.google.com/specimen/VT323) — SIL OFL 1.1
- [M PLUS 1p](https://fonts.google.com/specimen/M+PLUS+1p) — SIL OFL 1.1
- [cJSON](https://github.com/DaveGamble/cJSON) — MIT
- [stb](https://github.com/nothings/stb) — public domain / MIT
- [ESP-IDF](https://github.com/espressif/esp-idf), [esp-bsp](https://github.com/espressif/esp-bsp) and [ESP-Hosted](https://github.com/espressif/esp-hosted) — Apache-2.0

Each component keeps its own license. No project-wide license file is included yet.
