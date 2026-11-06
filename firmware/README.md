# IAP2 Tab5 firmware

ESP-IDF firmware for the M5Stack Tab5 (ESP32-P4). It reuses the same
`app/ui` and `app/backend` code as the desktop simulator.

## Toolchain

- ESP-IDF **v5.4.2** (tested)
- Python 3.12 (ESP-IDF's install script creates its own virtualenv)
- Target: `esp32p4`, chip revision v1.x

This Tab5 reports an ESP32-P4 **v1.3**. ESP-IDF 5.5+ defaults to the v3.x
(ECO5) memory map and its rev<3 compatibility mode does not link reliably,
so the project stays on IDF 5.4.

The registry's `espressif/usb` component uses HAL FIFO APIs that only exist
in IDF 5.5, despite declaring IDF >=5.4. `components/usb` is a copy of the
USB host library shipped with IDF 5.4.2 and is selected through
`override_path` in `main/idf_component.yml`.

On this machine ESP-IDF is installed at `/home/ops/esp/esp-idf`:

```bash
export PATH="/home/ops/.local/share/uv/python/cpython-3.12-linux-aarch64-gnu/bin:$PATH"
. /home/ops/esp/esp-idf/export.sh
```

## Build

```bash
cd firmware
idf.py set-target esp32p4   # first time only
python3 ../tools/gen_iap2_identity.py   # MFi key is not committed
idf.py build
```

The mock library under `assets/mock` is **not** part of the hardware image.
The Tab5 loads its media library from `/sdcard/library.bin`; Now Playing
metadata and artwork come from iAP2. The desktop simulator is the only build
that uses `assets/mock`.

`firmware/main/iap2_identity.c` contains the Apple Accessories private key
and is intentionally excluded from git. Generate it locally from the MFi
identity files (default `../iap2cert/certificate.p7b` and
`../iap2cert/identity.pk8`) with `tools/gen_iap2_identity.py`.

## Flash / monitor

The Tab5 USB-C port exposes the ESP32-P4 USB Serial/JTAG. Auto-reset into
download mode normally works; if it does not, hold **BOOT**, tap **RESET**,
then release **BOOT**.

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Exit the monitor with `Ctrl+]`.

## Partition table

| Name | Type | Offset | Size |
| --- | --- | --- | --- |
| nvs | data/nvs | 0x9000 | 24 KB |
| otadata | data/ota | 0xf000 | 8 KB |
| phy_init | data/phy | 0x11000 | 4 KB |
| factory | app | 0x20000 | 3.5 MB |
| ota_0 | app/ota_0 | 0x3a0000 | 3.5 MB |
| ota_1 | app/ota_1 | 0x720000 | 3.5 MB |
| storage | data/spiffs | 0xaa0000 | 1 MB (pairing record; formatted on first boot) |

The two OTA slots are used by the web updater. After changing this table, do a
full `idf.py flash` once (not `app-flash`); subsequent code-only updates can use
`app-flash` or the web updater.

## SD card configuration

The card is mounted at `/sdcard` (FAT32 recommended). Ready-to-copy examples
live in `../docs/sd/`. Two files are used:

`/sdcard/config.json`:

```json
{
  "screen_flip": 0,
  "wifi_mode": 0,
  "sound_enabled": true,
  "volume": 65,
  "ap_ssid": "Tab5-Debug",
  "clock_12h": false,
  "sort_field": 0,
  "sort_desc": true,
  "menubar_left": true,
  "touch_enabled": true,
  "np_bg_mode": 2,
  "brightness": 70,
  "usb_mode": 0
}
```

- `screen_flip`: `0` or `180` (landscape upside down)
- `wifi_mode`: `0` station, `1` open AP
- `np_bg_mode`: `0` theme, `1` blurred cover, `2` cover gradient
- `sort_field`: `0` add date, `1` name, `2` year
- `usb_mode`: `0` host, `1` OTG (currently label only)

Missing or invalid files fall back to built-in defaults. The firmware writes
the file back after every settings change (one write per second at most while
the knob is being turned).

`/sdcard/wifi.txt` (only for station mode):

```
MyHomeWiFi
my-password
```

First non-empty line is the SSID, second is the password (empty/absent for an
open network). If the file is missing while `wifi_mode` is station, firmware
starts the open AP fallback so the web UI remains reachable.

## Web UI and OTA

Wi-Fi starts in AP mode when no station credentials are available:

- SSID: `Tab5-Debug` (open, no password)
- Address: <http://192.168.4.1>

The page shows uptime/heap/SD/Wi-Fi state, edits the config above, reboots, and
accepts a firmware `.bin` for OTA. `POST /api/config` and `POST /api/ota` are
the useful endpoints for scripts.

**C6 firmware note:** the Tab5 factory ESP32-C6 runs ESP-Hosted 1.4.1, while
the host component here is 2.13.x. The AP works, but the host logs a version
mismatch and station mode may be unreliable. For production, flash the C6 with
an ESP-Hosted 2.13.x slave image. The Tab5 C6 reset is active-high on the
tested unit; the board preset is overridden in `sdkconfig.defaults`.

## Serial debug keys

While the USB HID knob/keypad is not connected, the USB Serial/JTAG console
accepts single-key commands:

| Key | Action |
| --- | --- |
| `w` / `s` | knob up / down |
| `Enter` | knob press |
| `b` | back |
| `p` / `n` | Page Up / Page Down (page the active list) |
| `[` / `]` | previous / next screen |
| `c` | context menu |
| `1`..`5` | jump to Now Playing / Albums / Artists / Songs / Playlists |
| `6` | back |
| `7` | play / pause |
| `m` | open Settings (debug) |
| `r` | toggle screen flip 0/180 (debug) |
| `h` | HTTP self-test of `/api/status` (debug) |
| `k` | benchmark full-screen redraw (debug) |
| `l` | benchmark one list page (debug) |
| `d` | scroll active list 240 px and benchmark (debug) |
| `a` | print usbmux/lockdownd/AFC status (debug) |
| `f` | re-run the AFC directory/stat/read probe (debug) |
| `u` | upload a lockdownd pair plist over serial, end with `EOF` |
| `y` | copy MediaLibrary.sqlitedb + WAL/SHM to `/sdcard/AFC` |
| `Y` | format the SD card (destructive) and mount it |
| `V` | verify `/sdcard/AFC/MLDB.*` by hashing the local files |
| `S` | remount the SD card and print AFC/library file sizes |
| `T` / `W` | dump `MLDB.SQL` / `MLDB.WAL` as base64 over the console |
| `Q` | upload `/sdcard/library.json` as base64, end with `EOF` |
| `z` | hash the media DB over AFC without SD |
| `J` | toggle iAP2 (USB config 2) / AFC (config 4), save to NVS, reboot |
| `x` | run the embedded MFi P-256 signing self-test |
| `o` | iAP2 play/pause |
| `,` / `.` | iAP2 previous / next track |

The same mapping is used by a USB HID keyboard/knob on the USB-A port. The
HID listener also accepts keypad `8/2/5` and keyboard volume up/down as
knob up/down/press. On Albums and Songs, `p` / `n` page the list first
instead of switching screens.

Send `g` to dump the current screen as **hex-encoded** RGB565 (debug use
only):

```
IAP2SHOTHEX <w> <h> <stride>
<two hex digits per byte, stride*h bytes total>
IAP2SHOT_END
```

Hex is used because the USB Serial/JTAG console is a terminal: a raw binary
stream gets `0x0a` expanded to `0x0d 0x0a`, which shifts image rows. The dump
also yields to the idle task periodically so the task watchdog stays happy.

## Notes

- The panel is native 720×1280 portrait; the firmware rotates the LVGL
  display 90° to the 1280×720 landscape layout used by the UI.
- The library arrays are placed in `.ext_ram.bss` (PSRAM); without this the
  ~300 KB static library overflows the ESP32-P4 internal RAM.
- The LVGL port task stack is raised to 32 KB. The default 7 KB stack
  overflows while drawing label glyphs on the first full-screen frame.
- LVGL examples and demos are disabled (`CONFIG_LV_BUILD_EXAMPLES/DEMOS=n`)
  to keep builds fast and the image small.
- Performance: 8 MB decoded-image cache + 128 image headers, a 10-row
  virtual list for Albums, and covers outside the viewport are not decoded.
- PPA is enabled in two places:
  - `CONFIG_LVGL_PORT_ENABLE_PPA=y` rotates each partial flush with the PPA
    SRM engine instead of `lv_draw_sw_rotate`.
  - `CONFIG_LV_USE_PPA=y` lets LVGL's PPA draw unit accelerate opaque solid
    fills (backgrounds, panels, list rows).
  The LVGL 9.6 PPA draw unit calls `esp_cache_get_line_size_by_addr()`, an
  API added in IDF 5.5. `main/lvgl_ppa_idf_compat.[ch]` backports it on
  IDF 5.4.2.
- PPA requires draw-buffer addresses and sizes aligned to the 128-byte L2
  cache line, so `CONFIG_LV_DRAW_BUF_ALIGN=128`.
- The display now has **two full-screen buffers in PSRAM** (720×1280 each,
  3.7 MB total) plus one full-screen PPA rotation buffer (1.8 MB). LVGL stays
  in partial mode, so a clock tick only renders its dirty rectangle, while a
  screen switch can render the whole frame in one pass.
- `components/esp_lvgl_port` is a local copy of the 2.9.0 component with one
  fix: the partial-refresh DSI path waits for the previous DMA copy before
  reusing the shared PPA rotation buffer. This makes double buffering safe.
- `CONFIG_LV_USE_PPA_IMG=y` lets the PPA draw simple 1:1 images (mesh
  backdrop, raw thumbnail images) in addition to solid fills.
- Album thumbnails on hardware come from the per-library packed cache
  (`/sdcard/lib/<short8>/thumbs.bin`); the simulator uses the generated
  `assets/mock/thumbs*.bin` packs. The Now Playing cover is decoded from the
  iAP2 FT image into a 360×360 RGB565 buffer; the simulator keeps the old
  mesh-background path.
- Measured on the Tab5 (full render + flush wait): Albums page 45 ms
  (was 420 ms), Settings/Playlists full redraw 87–90 ms, Albums/Artists/Songs
  90–102 ms, Now Playing 128 ms (was 2.39 s). A full-screen flush itself is
  about 13.7 ms.
- DSI tear-free/VSYNC is **not** enabled yet: the panel uses one DSI
  framebuffer and the PPA flush writes into it. Full-screen changes can tear.
  The next step would be a double-DSI-framebuffer custom flush.
- Touch drag scrolling is restored on scrollable lists. The Albums/Songs
  virtual pools now only refresh when the first visible row index changes, so
  a finger drag no longer re-renders all rows on every motion event. Page
  Up/Down still jumps one viewport, and a tap after scrolling opens/plays the
  row under the finger.
- The menu-bar clock is seeded from the build timestamp until the RX8130CE
  RTC driver is added.
- Audio prompt, Wi-Fi station/AP, SD config and the web UI are enabled.
  Camera is not. On hardware the UI shows only the connected device's library;
  with no device the library pages are empty and Now Playing shows No track.
  The mock library/player is simulator-only.
- After changing only C code, `idf.py app-flash` is enough; use full
  `idf.py flash` after changing assets or the partition table.
- Prompt sounds use the ES8388 speaker codec (880/1200/520 Hz short beeps on
  activate/click/back). Volume and on/off are in Settings and the web UI.
- The web updater writes to `ota_0`/`ota_1`. The factory slot remains the
  recovery image; reflash with `idf.py flash` to return to it.
- AFC/usbmux is implemented: config 4, usbmux v2, lockdownd
  QueryType/StartSession/TLS, `StartService com.apple.afc`, directory/stat
  and file read. See `docs/afc-bringup.md`.
- iAP2 is implemented on USB configuration 2 (HID interface, EP `0x83`).
  MFi authentication, identification, Now Playing metadata, HID
  play/pause/next/previous and FT artwork work. See `docs/iap2-bringup.md`.
- AFC and iAP2 are mutually exclusive. `J` toggles the mode in NVS and
  reboots. The USB HID keyboard/D100H driver runs in both modes.
- The shared UI is wired to iAP2 for Now Playing metadata, play/pause,
  next/previous, shuffle/repeat, battery and device name.
- The real media library is loaded from `/sdcard/library.bin` (20,688
  tracks / 2,096 albums / 1,067 artists from the AFC SQLite database).
  Album/artist/track selection maps to iAP2 `PlayMediaLibraryItems`.
- Japanese text uses M PLUS 1p Regular from `/sdcard/JPFONT.TTF` through
  LVGL TinyTTF; Inter remains the tabular numeric/clock face. See
  `docs/media-library-import.md`.
- The binary library and font can be uploaded over serial with `Q`/`B`;
  `T`/`W` dump the SQLite DB/WAL for the host-side converter.
- Album artwork is synced over AFC with `C` (first 64 albums) or `E`
  (all). It writes the original image plus `<id>.thm` (54x54) and
  `<id>.cov` (360x360) RGB565 caches under `/sdcard/covers/`. The UI keeps
  a 48-entry list thumbnail cache plus dedicated mini-player/detail
  thumbnails, and a single 360 cover descriptor, all 64-byte aligned for
  the PPA draw path. The SD card is mounted with
  long filenames disabled, so the short `.thm` / `.cov` extensions are
  required. See `docs/media-library-import.md`.
- TJPGD handles baseline JPEG only; progressive artwork falls back to the
  album color placeholder.
- iAP2 Now Playing covers work end-to-end: the album ID is resolved from
  the iAP2 title/artist metadata (with a unique-title fallback for tag
  differences), and the cached 360x360 cover is shown in the Now Playing
  widget. Verified with a paused iAP2 track and an AFC artwork sync; see
  `docs/screenshots/tab5-real-cover-iap2.png`.
- `# <id>` syncs one album's cover over AFC, which is useful when testing
  a specific iAP2 track without running the full library sync.
- **Settings > Apple File Conduit Sync** provides Pair/Connect, Quick sync,
  Full sync, switch-to-iAP2, cached album count, cache size,
  thumbnail/cover counts, USB mode and AFC/sync status. Quick/full sync store a one-shot NVS job,
  reboot to AFC, wait for the iPod, run the artwork sync, then switch back
  to iAP2 and reboot automatically. The page is at
  `docs/screenshots/tab5-settings-afc.png`.
- Original artwork files are removed once both the `.thm` and `.cov`
  caches exist; this keeps a full sync around 600 MB instead of several
  GB. Interrupted older runs leave originals that are cleaned as their
  albums are processed.
- The system has two boot modes:
  - **iAP2** (default): normal media UI, USB config 2.
  - **AFC Sync**: dedicated sync-only UI, USB config 4, no media controls.
    It shows connection state, cached count/size, progress, and Quick sync /
    Full sync / Exit to iAP2 buttons. See
    `docs/screenshots/tab5-afc-sync.png`.
- The AFC job worker stack is in PSRAM and is only created when a job is
  pending. This avoids exhausting internal DMA memory before usbmux/AFC
  starts; an earlier 24 KB internal stack caused AFC to remain
  `waiting for iPod` while the UI showed `Full running`.
- Touch/click stability:
  - The `feedback` task used a 4 KB stack and a 2.5 KB local tone buffer;
    clicking any control could trigger a stack-protection panic. The tone
    buffer is now static and the task stack is 8 KB.
  - LVGL input callbacks queue page rebuilds/screen switches through
    `lv_async_call`, so the clicked row is not deleted while its event is
    still being dispatched.
  - The Artists page now uses a 12-row pool instead of creating a row for
    all 1,067 artists; artist details are capped at 10 albums / 12 tracks
    per album / 72 total rows. Verified on hardware: Albums, album detail,
    Songs, song row, Artists, artist detail, Settings and all seven
    Settings sub-pages open without a watchdog or panic.
