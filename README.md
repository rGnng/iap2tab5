# iAP2 Tab5

An iPod / iPhone media controller for the **M5Stack Tab5**, with a shared
**LVGL 9.6** interface for the ESP32-P4 board and an SDL2 desktop simulator.
The UI runs at 1280×720 and supports a rotary controller, USB keyboard, Ulanzi
D100H remote, and optional touch input.

On hardware, **iAP2** provides playback control, Now Playing metadata, and
artwork. **AFC** copies the device's media database and album artwork to an SD
card; a host-side Python tool converts the database into the library used by
the UI. The simulator uses a bundled mock library and needs no Apple device or
MFi identity.

## What it does

- Browse **Now Playing, Albums, Artists, Songs, Playlists, and Settings** with
  consistent knob/key navigation, virtualized lists, and a mini player.
- Control playback, shuffle, repeat, and seeking; start playback from library
  lists and save local stars per library.
- Show Now Playing artwork, album thumbnails, and an Up Next panel. When the
  device supplies a queue, its entries are matched against the imported library;
  otherwise the UI falls back to the list started on the Tab5 or the current track.
- Configure the device, upload libraries and firmware, inspect logs, inject
  keys, and capture screenshots through a Wi-Fi web interface.
- Preview the same UI on a desktop, including mock covers, keyboard/touch input,
  the optional KCN4M1 boot animation, and headless PNG screenshots.

## Run the desktop simulator

Run these commands from the repository root. On Debian / Ubuntu, install the
build tools and SDL2 development headers:

```bash
sudo apt install build-essential cmake ninja-build libsdl2-dev
cmake -S . -B build -G Ninja
cmake --build build -j
./build/tab5_sim
```

The first configure downloads the pinned **LVGL v9.6.0** release into
`build/_deps`. To use an existing checkout, configure with
`-DLVGL_SOURCE_DIR=/path/to/lvgl`. The `run` build target also starts the simulator:

```bash
cmake --build build --target run
```

The bundled library contains 24 artists, 100 albums, 562 tracks, and 6 playlists.
The mouse acts as the touchscreen. The simulator starts directly in the UI
unless a boot animation is requested.

### Screenshots and command-line options

```bash
# Open Albums with the menubar on the right.
./build/tab5_sim --screen 2 --menubar right

# Capture the UI without a display server.
mkdir -p build/screenshots
./build/tab5_sim --headless --screen 2 --keys enter \
    --screenshot build/screenshots/album.png --wait 1500

# Capture a fixed point in the boot animation.
./build/tab5_sim --headless --boot-freeze 3900 \
    --screenshot build/screenshots/boot.png --wait 300
```

| Option | Purpose |
| --- | --- |
| `--screen <1-6>` | Start on Now Playing, Albums, Artists, Songs, Playlists, or Settings |
| `--menubar <left\|right>` | Choose the menubar side |
| `--zoom <factor>` | Scale the window; default `1.0` |
| `--keys <list>` | Inject comma-separated keys, such as `2,enter` or `context` |
| `--screenshot <path>` | Save a PNG and exit |
| `--wait <ms>` | Delay before capture; default `1000` |
| `--headless` | Use SDL's offscreen video driver |
| `--boot` | Play the KCN4M1 boot animation |
| `--boot-freeze <ms>` | Show the boot animation at a fixed time offset |
| `--no-boot` | Skip the boot animation |
| `--help` | Print command-line help |

On Linux, screenshot mode automatically uses the offscreen driver when neither
`DISPLAY` nor `WAYLAND_DISPLAY` is set. Explicit screen, menubar, or key options
skip the normal boot animation; `--boot-freeze` can still select a boot frame.

## Controls

The simulator and hardware share the same logical actions:

| Input | Action |
| --- | --- |
| `Up` / `Down` | Move focus, like turning the knob |
| `Enter` | Activate; hold for 650 ms to open the context menu |
| `1`–`5` | Open Now Playing / Albums / Artists / Songs / Playlists |
| `6` or `Backspace` | Go back |
| `7` | Play / pause |
| `Page Up` / `Page Down` | Page the active list; Page Up toggles Up Next on Now Playing |
| `[` / `]` | Previous / next screen; `[` toggles Up Next on Now Playing |
| `C` | Open the context menu in the simulator |

On hardware, input can come from a USB HID keyboard or knob, the D100H over USB
or BLE, the serial console, or the web debug page. The serial equivalents
include `w` / `s` for knob movement, `b` for Back, and `c` for the context menu;
see the [full serial key map](firmware/README.md#serial-debug-keys).

The D100H supports tap-to-skip and hold-to-seek on its previous/next controls.
Its top-right key opens the context menu on a tap and toggles Star on a hold.
Pair it from **Settings → Bluetooth**, under **Search Devices**.

## Build and set up the Tab5

### Prerequisites

- M5Stack Tab5, an SD card, and an iAP2-capable iPod or iPhone.
- **ESP-IDF 5.5.6**, the version used by the current firmware dependency lock.
- An MFi P-256 certificate chain and private key for iAP2 authentication.
- A matching lockdownd pair record for AFC access to the Apple device.
- **ESP-Hosted 2.13.1** on the Tab5's ESP32-C6 for the Wi-Fi/BLE stack used here.

The MFi identity and lockdownd pair record serve separate purposes. Neither is
included in this repository. C6 firmware images and the initial wired update
procedure are in the [C6 firmware guide](tools/c6_firmware/README.md). A unit
running the factory 1.4.1 firmware needs that update for reliable Wi-Fi and BLE.

### Generate the identity, build, and flash

From the repository root, generate the identity **before configuring the
firmware**, because CMake includes the generated source file:

```bash
python3 tools/gen_iap2_identity.py \
    --cert /path/to/certificate.p7b \
    --key /path/to/identity.pk8

. "$IDF_PATH/export.sh"
cd firmware
idf.py set-target esp32p4  # first configure only
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

With no arguments, the identity generator reads `../iap2cert/certificate.p7b`
and `../iap2cert/identity.pk8`, relative to the repository root. It writes the
ignored `firmware/main/iap2_identity.c`.

Exit the monitor with `Ctrl+]`. If auto-reset fails, hold **BOOT**, tap **RESET**,
then release **BOOT**. After moving from IDF 5.4 to 5.5, clean the old build and
perform a full flash once. Later code-only updates can use `idf.py app-flash` or
the web updater; partition changes require a full flash.

**Generated firmware contains the MFi private key. Keep the identity and built
images private.** Lockdownd pair records also contain private keys and are stored
in the internal SPIFFS partition.

### SD card and Wi-Fi

The firmware mounts the card at `/sdcard`; FAT32 is recommended. Start with the
[SD configuration examples](docs/sd/). For station mode, create `wifi.txt` with
the SSID on the first line and password on the second:

```text
MyHomeWiFi
my-password
```

With no usable credentials, the default configuration starts an open
**Tab5-Debug** access point at **http://192.168.4.1**. Failed station connections
are retried. AP mode can also be selected explicitly in `config.json`.

Open the Tab5's address in a browser to configure settings, upload a library or
pair record, copy the iPod database, sync the clock, or install a firmware `.bin`
over the air. The `/debug` page adds live logs, key injection, and BMP screenshots.
Enable **Debug log to SD** to retain logs in `/sdcard/debug.log` across resets.
The web server has no authentication, so use it on a trusted network.

Typical card contents after setup and sync:

```text
/sdcard/
  config.json                   # UI, network, power, and USB settings
  wifi.txt                      # optional station credentials
  JPFONT.TTF                    # optional Japanese metadata font
  AFC/MLDB.SQL                   # copied media database
  AFC/MLDB.WAL                   # database write-ahead log, when present
  AFC/MLDB.SHM                   # database shared-memory file, when present
  lib/<short8>/library.bin       # converted media library
  lib/<short8>/thumbs.bin        # packed album thumbnails
  lib/<short8>/thumbs.idx        # thumbnail validity bitmap
  lib/<short8>/stars.csv         # local stars
  lib/<short8>/np.raw            # cached Now Playing artwork
  lib/<short8>/np.txt            # metadata identifying the cached artwork
```

`<short8>` is derived from the device's library identifier. The firmware manages
these directories and supports the older flat `/sdcard/library.bin` layout.
The hardware image uses the imported library; mock assets are simulator-only.

### USB ports and modes

Two independent settings control USB: **which ports act as hosts** and **which
Apple device protocol runs**. Both take effect at boot.

| Host setting | USB-A | USB-C |
| --- | --- | --- |
| `usb_mode: 0` — default | High-speed host for the Apple device or HID controller | Serial console and flashing |
| `usb_mode: 2` — Both A+C | High-speed host | Full-speed host through a powered hub/dock |

Choose the host setting in **Settings → USB Options** and reboot to apply it.
Dual-host mode needs a hub/dock that supplies 5 V upstream to the Tab5 and uses
Wi-Fi for debugging because the USB-C console is unavailable. Use mode `0` when
connecting USB-C to a PC for the serial console. The old mode `1` falls back to
mode `0`.

For simultaneous iPod and wired D100H use, the documented setup puts the iPod
on USB-A and the D100H behind a powered USB-C hub. The current USB host stack
cannot handle full-/low-speed devices behind a high-speed hub on USB-A; see
[USB modes and hub limitations](docs/usb-modes.md).

| Apple device mode | Purpose |
| --- | --- |
| **iAP2**, USB configuration 2 | Normal media UI, playback control, metadata, and Now Playing artwork |
| **AFC Sync**, USB configuration 4 | Database copying and album thumbnail sync |

These protocol modes are mutually exclusive. Switching between them saves the
selection in NVS and reboots. Quick/full sync jobs return to iAP2 automatically
when finished.

## Import a media library

Library conversion runs on a host computer. Copying the database or running an
artwork sync on the Tab5 does not rebuild `library.bin`.

1. Upload the device's lockdownd pair record through the web UI and enter
   **AFC Sync** mode. See [AFC pairing and bring-up](docs/afc-bringup.md) for
   pair-record setup.
2. Use **Copy iPod database** in the web UI to copy the database to
   `/sdcard/AFC/`. Copy `MLDB.SQL` and, if present, `MLDB.WAL` from the card to a
   host directory such as `/tmp/ipod-db/`.
3. From the repository root, convert the snapshot with Python 3:

   ```bash
   python3 tools/media_db_to_library.py \
       --db /tmp/ipod-db/MLDB.SQL \
       --wal /tmp/ipod-db/MLDB.WAL \
       --bin /tmp/ipod-db/library.bin \
       --out /tmp/ipod-db/library.json
   ```

   Omit `--wal` if no WAL was copied. The JSON output is optional.
4. Upload `library.bin` using the web UI, or send it directly to the Tab5's
   address (the example uses its default AP):

   ```bash
   curl --data-binary @/tmp/ipod-db/library.bin \
       http://192.168.4.1/api/library
   ```

   The upload replaces the active library file and reboots the device.
5. Run **Quick sync** for the first 64 albums or **Full sync** for the library's
   artwork, then return to iAP2 playback. Album thumbnails are cached on SD;
   large Now Playing artwork arrives separately over iAP2.

Repeat the conversion and upload after changing the device's library. See the
[media library import guide](docs/media-library-import.md) for serial transfers,
the binary format, fonts, and artwork cache details.

## Development

The shared UI calls the backend contract in
[`app/backend/player.h`](app/backend/player.h). The simulator supplies mock data;
ESP builds route the same API to the imported library and iAP2 host.

| Path | Contents |
| --- | --- |
| [`app/ui/`](app/ui/) | Screens, focus navigation, boot animation, and generated fonts |
| [`app/backend/`](app/backend/) | Player API, mock data/loading, hardware dispatch, and BLE remote state |
| [`sim/`](sim/) | SDL2 entry point, keyboard input, command-line options, and PNG capture |
| [`firmware/main/`](firmware/main/) | ESP-IDF app, iAP2, AFC/usbmux/lockdownd, USB input, SD storage, and web server |
| [`firmware/components/`](firmware/components/) | Local LVGL port and USB HID component fixes |
| [`assets/`](assets/) | Mock library/covers, source fonts, web page, and UI artwork |
| [`tools/`](tools/) | Library conversion, asset generators, identity generation, and C6 firmware |
| [`tests/`](tests/) | Host-side D100H protocol and gesture tests |

### Checks

The D100H tests compile the production codec with the host C compiler and run
without ESP-IDF:

```bash
python3 -m unittest discover -s tests -v
```

They cover BLE/HID framing, captured protocol vectors, heartbeat encoding, and
top-right tap/hold handling. Simulator screenshots are useful for reviewing UI
changes; hardware timing, USB behavior, and BLE still need a Tab5.

### Regenerate assets

Generated assets are checked in, so these tools are only needed when changing
their sources. Run them from the repository root:

| Command | Purpose | Additional dependency |
| --- | --- | --- |
| `python3 tools/gen_mock_library.py` | Rebuild the deterministic mock library, covers, and thumbnails | Pillow |
| `python3 tools/gen_web_ui.py` | Embed `assets/web/index.html` in `firmware/main/web_index.h` | Python 3 |
| `python3 tools/gen_plug_art.py` | Embed the no-device illustration | Pillow |
| `./tools/gen_icons.sh` | Generate Bootstrap Icons font subsets | Node/npm (`lv_font_conv`) |
| `./tools/gen_inter_fonts.sh` | Generate proportional and tabular Inter fonts | Node/npm and Python `fonttools` |
| `./tools/gen_boot_font.sh` | Generate the VT323 boot font | Node/npm |

## Current limitations

- Full iAP2 media-library subscription is disabled by default; AFC import is the
  supported path. `IAP2TAB5_FULL_LIBRARY_SYNC` gates the experimental subscription.
- Up Next depends on the queue supplied by the device and matching tracks in the
  imported library. Its fallback may show only the current track.
- Stars are stored locally in `stars.csv` and are not written back to the iPod.
- Unsupported artwork can fall back to a placeholder; the software JPEG decoder
  supports baseline JPEG only.
- Hardware display output is not yet tear-free. The simulator also uses a
  different rendering path and does not predict Tab5 performance.

## Further documentation

| Guide | Topics |
| --- | --- |
| [Firmware](firmware/README.md) | Configuration schema, partitions, serial commands, and hardware notes |
| [UI design](docs/design.md) | Layout, navigation, focus model, and playback behavior |
| [iAP2 bring-up](docs/iap2-bringup.md) | Link layer, authentication, identification, control, and artwork |
| [AFC bring-up](docs/afc-bringup.md) | usbmux, lockdownd, TLS, pairing, and file access |
| [Media library import](docs/media-library-import.md) | Database conversion, binary format, fonts, and artwork |
| [Star persistence](docs/stars-persistence.md) | Per-library star storage and matching |
| [USB modes](docs/usb-modes.md) | Port roles, powered hubs, charging, and boot configuration |
| [D100H driver notes](docs/d100h-ble-handover.md) | BLE protocol analysis and bring-up history |
| [C6 firmware](tools/c6_firmware/README.md) | ESP-Hosted images and flashing instructions |

Some bring-up documents retain historical experiments and older configurations;
use the current source and build files when working on the drivers.

## Third-party code and licenses

The project uses LVGL, SDL2, ESP-IDF/esp-bsp/ESP-Hosted, cJSON, stb, Bootstrap
Icons, Inter, VT323, and optional M PLUS 1p fonts. Each retains its own license;
font licenses are included in [`assets/fonts/`](assets/fonts/) and
[`tools/fonts/`](tools/fonts/), and cJSON's license is in
[`third_party/cjson/LICENSE`](third_party/cjson/LICENSE). There is currently no
project-wide license file.
