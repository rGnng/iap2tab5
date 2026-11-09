# IAP2 Tab5 firmware

ESP-IDF firmware for the M5Stack Tab5 (ESP32-P4). It reuses the same
`app/ui` and `app/backend` code as the desktop simulator.

## Toolchain

- ESP-IDF **v5.5.6** (tested)
- Python 3.12 (ESP-IDF's install script creates its own virtualenv)
- Target: `esp32p4`, chip revision v1.x

This Tab5 reports an ESP32-P4 **v1.3**. IDF 5.5 defaults to the v3.x (ECO5)
memory map, so the project explicitly selects the rev<3 mode:

```text
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_1=y
```

Without those options the bootloader is built for rev3.x and the ROM rejects
it when flashing. The registry `espressif/usb` component (currently 1.6.0)
provides the USB host stack and the HS/FS OTG controller selection; the old
vendored `components/usb` copy from IDF 5.4 has been removed.


The HID driver in `components/usb_host_hid` is a local copy of Espressif
`usb_host_hid` 1.2.1 with interrupt-OUT report support. The D100H needs its
64-byte heartbeat on that endpoint; SET_REPORT alone enumerates successfully
but does not enable its vendor keys on this unit. The driver copies each
report, allows one transfer in flight, and logs completion failures. The
2026-11-08 OTA test confirmed 64-byte transfers on endpoint `0x05`; the user
verified keypad play/pause, knob, Back, iPod control, and sound together.

The D100H top-right button opens Context on a short release. Holding it for
600 ms toggles Star once, with key feedback, while the button is still down;
releasing after a long press does nothing. USB and BLE use the same timer
handler, and disconnect cancels that input's pending gesture. For web debug,
`POST /api/keystate?k=topright&down=1` and `down=0` exercise the same handler
using a separate input state.

Hold the D100H knob and turn clockwise / counter-clockwise to raise / lower
iPod volume, one native relative step per detent. A plain click activates on
release; any held turn cancels that activation. USB and BLE share this gesture
handler, with separate state for each input source. Disconnect cancels a pending click.
Web debug can exercise the same path using `POST /api/keystate?k=knob&down=1`,
then `k=knobcw&down=1` or `k=knobccw&down=1`, followed by `k=knob&down=0`.

On this machine ESP-IDF is installed at `/home/ops/esp/esp-idf-5.5.6`:

```bash
export PATH="/home/ops/.local/share/uv/python/cpython-3.12-linux-aarch64-gnu/bin:$PATH"
. /home/ops/esp/esp-idf-5.5.6/export.sh
```

## USB ports and host modes

| Port | P4 connection | Role |
| --- | --- | --- |
| USB-A (J10) | `USB2_OTG` high-speed pins 49/50; VBUS switched by the BSP | USB host (iPod, HID keyboard) |
| USB-C (J8) | FSLS PHY0 (GPIO24/25); CC1/CC2 5.1k to GND; VBUS is `USB_5VIN` input | USB Serial/JTAG console by default; FS host when powered through a hub/dock |

The USB-C data lines are wired to **FSLS PHY0**, not the PHY that ESP-IDF
uses by default. `usb_host_core.c` therefore calls
`usb_wrap_ll_phy_select(&USB_WRAP, 0)` before `usb_host_install()` when the
FS host is selected. Without that call the FS host runs on PHY1
(GPIO26/27), which is not connected to USB-C.

GPIO26/27 are connected to the ES8388 as I2S DOUT/BCLK. Switching the FS
host to PHY0 also moves USB Serial/JTAG to PHY1, so its pads must be disabled
with `usb_serial_jtag_ll_phy_enable_pad(false)` **before** switching. Leaving
them enabled takes over the audio pins even though codec opens and tone
writes succeed. The USB-A-only mode retains the USB-C serial console; in
Both A+C mode use the web debug log instead. The sound fix was OTA tested
on 2026-11-08; the user confirmed audible feedback with Both A+C enabled.

The USB-C port itself is a sink/device port: the CC pins have 5.1k
pull-downs and VBUS is an input into the charger/system rail. A USB-C
hub/dock that supplies 5V to its upstream port can power the Tab5 while the
Tab5 uses USB-C as a host. This was verified with a Ulanzi D100H behind a USB
hub with the battery removed. A plain hub that only draws power from the host
will not power the Tab5.

`usb_mode` in `/sdcard/config.json`:

- `0`: USB-A HighSpeed Only (default); USB-C remains a device / serial console.
- `2`: USB-A HS & USB-C OTG (Both A+C): USB-A HS + USB-C FS dual host. Verified with an iPod on
  USB-A and a Ulanzi D100H behind a powered USB-C hub at the same time.

The two P4 OTG controllers can run as hosts simultaneously. The serial debug
commands `(` / `)` select USB-A HS / Both A+C and reboot.

See [`docs/usb-modes.md`](../docs/usb-modes.md) for the boot flow, controller
and PHY mapping, TT limitation, OTG device caveats, web interfaces, and the
Settings UI integration notes for `usb_mode`.

## Build

```bash
cd firmware
idf.py set-target esp32p4   # first time only
python3 ../tools/gen_iap2_identity.py   # MFi key is not committed
idf.py build
```

After switching from IDF 5.4 to 5.5, run `idf.py fullclean` once (or use a
fresh build directory); the old CMake cache is not compatible. Do a full
`idf.py flash` once after the major upgrade as well, because the 5.5
bootloader/partition images differ from 5.4. Later code-only updates can use
`app-flash` again.

The mock library under `assets/mock` is **not** part of the hardware image.
The Tab5 loads its media library from `/sdcard/library.bin`; Now Playing
metadata and artwork come from iAP2. The desktop simulator is the only build
that uses `assets/mock`.

`firmware/main/iap2_identity.c` contains the Apple Accessories private key
and is intentionally excluded from git. Generate it locally from the MFi
identity files (default `../iap2cert/certificate.p7b` and
`../iap2cert/identity.pk8`) with `tools/gen_iap2_identity.py`. Keys without
an embedded public point are supported: authentication passes the seeded
DRBG to mbedTLS when parsing the key so it can derive that point.

## Flash / monitor

The Tab5 USB-C port exposes the ESP32-P4 USB Serial/JTAG. Auto-reset into
download mode normally works. If it does not, connect USB-C to the computer,
hold the Tab5 power button until the green LED flashes, then release it.
Recheck the serial port after entering download mode. See the root README's
[detailed P4 flashing procedure](../README.md#flash-the-esp32-p4-application)
for tool installation, first flashing, OTA, and recovery.

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
  "debug_log": false,
  "volume": 65,
  "ap_ssid": "Tab5-Debug",
  "clock_12h": false,
  "sort_field": 0,
  "sort_desc": true,
  "menubar_left": true,
  "touch_enabled": true,
  "np_bg_mode": 2,
  "brightness": 70,
  "screen_off_minutes": 0,
  "fast_scroll_pages": 2,
  "fast_scroll_hold_ms": 150,
  "usb_mode": 0,
  "charge_enabled": true,
  "qc_high": false
}
```

- `screen_flip`: `0` or `180` (landscape upside down)
- `wifi_mode`: `0` station, `1` open AP
- `np_bg_mode`: `0` theme, `1` blurred cover, `2` cover gradient
- `fast_scroll_pages`: `0` off, `1`–`5` physical pages per 1200 ms to enter fast
  scroll (default `2`). Once active, each notch moves one visible page.
- `fast_scroll_hold_ms`: `150` (default), `300`, `500`, or `1500` ms without
  another scroll notch before returning to single-row precision.
- `screen_off_minutes`: `0` disabled (default), `30` or `60`. Only paused/stopped
  playback with no input counts as idle. Hold the menubar clock for two seconds
  to blank immediately; the first touch/key wakes without performing its action.
  This switches off the backlight while USB, playback, sync and Wi-Fi continue.
- `sort_field`: `0` add date, `1` name, `2` year
- `usb_mode`: `0` USB-A HS host (default), `2` USB-A HS + USB-C FS
  dual host (Both A+C, verified). The old USB-C-FS-only value `1` is
  normalised to `0`.
- `charge_enabled`: `true` enables the IP2326 charger (`CHG_EN`, E2.P7);
  can also be toggled at runtime from the web. Both charger control pins must
  use push-pull output mode: setting the output latch alone leaves them high-Z.
- `qc_high`: raw level of `E2.P5` / `nCHG_QC_EN`; debug/compatibility knob
  for the board's NTC/current divider, default `false` (nominal current).
  Startup and web changes disable charging, configure the divider and status
  input, then apply the requested charger enable state.

Missing or invalid files fall back to built-in defaults. The firmware writes
the file back after every settings change (one write per second at most while
the knob is being turned).

`/sdcard/wifi.txt` (only for station mode):

```
MyHomeWiFi
my-password
```

First non-empty line is the SSID, second is the password (empty/absent for an
open network). Valid SD credentials replace the NVS backup; unchanged values
do not write flash. If the file cannot be read, including when the SD card
fails to mount, station mode uses the NVS backup. With no usable credentials
in either location, firmware starts the open AP fallback.

## Web UI and OTA

Wi-Fi starts in AP mode when no station credentials are available:

- SSID: `Tab5-Debug` (open, no password)
- Address: <http://192.168.4.1>

The page shows uptime/heap/SD/Wi-Fi state, edits the config above, reboots, and
accepts a firmware `.bin` for OTA. `POST /api/config` and `POST /api/ota` are
the useful endpoints for scripts.

The debug page at `/debug` provides a live log, UI key injection, screenshots,
USB host mode switching, reboot and OTA upload. This is the main control path
when USB-C is in host mode and the USB Serial/JTAG console is unavailable.

Useful endpoints:

| Endpoint | Purpose |
| --- | --- |
| `GET /api/status` | JSON status (build time, USB mode, iAP2, heap, SD) |
| `GET /api/display` | actual backlight PWM registers, VSYNC/DMA/render counters and internal memory |
| `GET /api/display/trace?since=N` | CSV of up to 1024 recent samples at 100 ms intervals; `X-Diag-Next` resumes collection |
| `POST /api/display/mark` | timestamp an observed flicker in the live/SD log |
| `GET /api/config` / `POST /api/config` | read/write `/sdcard/config.json` |
| `GET /api/log?since=N` | log text after sequence N (`X-Log-Seq`) |
| `GET /api/log/stream?since=N` | live SSE log stream; resumes with `Last-Event-ID` |
| `POST /api/loglevel?tag=usb&level=4` | change a log tag level at runtime |
| `POST /api/key?k=enter` | named navigation/media action; legacy serial aliases also work |
| `POST /api/key?k=screenoff` / `k=wake` | blank/wake the display without changing saved brightness |
| `POST /api/keystate?k=back&down=1\|0` | D100H Back press/release; action fires on release |
| `POST /api/keystate?k=play&down=1\|0` | D100H Play/Pause on short release; hold 2 seconds for Screen Off |
| `POST /api/keystate?k=cancel` | cancel web keypad holds without emitting a click |
| `POST /api/sync` | same incremental library/artwork sync worker as the AFC screen |
| `GET /api/screenshot.bmp` | 1280x720 24-bit BMP snapshot |
| `POST /api/usbmode?mode=0\|1\|2` | select USB-A / USB-C / both and reboot |
| `GET /api/power` | INA226 voltage/current + `usb5v_en` / `chg_en` / `chg_stat` |
| `POST /api/charge?en=0\|1` | enable/disable the IP2326 charger and persist it |
| `POST /api/reboot` | reboot |
| `POST /api/ota` | raw firmware image upload; verified over WiFi |

The debug page's **Mark flicker + save** button marks the observation and
downloads the preceding display trace. The recorder uses a bounded PSRAM ring
and does not write samples to SD. PWM readings sample the hardware duty/frequency
registers; VSYNC gaps measure interrupt callback delivery. These do not measure
optical brightness, USB input voltage, or directly count LCD FIFO underruns.
The INA226 measures the battery connector, so its voltage is not a USB power
measurement when the battery has been removed. `lcd_enabled` in `/api/power`
reads the LCD enable pin separately.

`library_available` in `/api/status` reports the UI's actual library gate.
Albums, Artists, Songs and Playlists are disabled until the SD library is ready
and the iPod is identified; all touch, keyboard/keypad and debug navigation
uses the same check. An unavailable library returns to Now Playing, cancels
pending page loads and skips the disabled tabs when switching pages.
The application polls card status outside the display lock every two seconds;
two consecutive failures latch the SD card unavailable without unmounting open
files. Reinsert and reboot after removal to rebuild the library state.

Backlight control uses a fixed crystal clock and nominal 44.1 kHz, 9-bit PWM,
following [M5GFX's Tab5 configuration](https://github.com/m5stack/M5GFX/blob/master/extras/board_spec/boards/m5stack_tab5.json)
and [PWM implementation](https://github.com/m5stack/M5GFX/blob/master/src/lgfx/v1/platforms/esp32/Light_PWM.inl).
The actual frequency is quantized by the LEDC divider and appears in diagnostics.
Duty uses a complete 512-count period: 0% is constant low, 100% constant high.
Startup goes directly to the saved brightness; Screen Off and wake use the
same controller. If initialization fails, it restores the BSP timer/channel
configuration and reports the fallback in the log. CSV traces append a
`pwm_period` column; use this value to interpret duty counts.

Example OTA upload:

```bash
curl --data-binary @firmware/build55/iap2tab5.bin \
     http://10.0.0.181/api/ota
```

A 2.7 MB image uploads in roughly 10-20 seconds on the tested C6 1.4.1
firmware.

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
| `P` | re-send the iAP2 500 mA USB current budget after identification |
| `*` | print the USB-A power switch / IO-expander state |
| `&` | print the INA226 battery bus voltage/current |

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
  Albums updates row text/cached images at most once per 16 ms during touch
  scrolling. After 140 ms without scrolling, visible missing covers load in
  batches of at most two; a read exceeding the 4 ms budget yields before the
  next read. Retained rows are checked when they enter the viewport, so Enter
  is no longer needed to load their artwork. Failed reads retry at 250 ms,
  up to three attempts; known missing art and offscreen rows cause no reads.
  Page teardown cancels both row and artwork timers.
  `POST /api/key?k=scroll&dy=-70` exercises a one-row drag, and
  `POST /api/key?k=trace&en=1` enables timing/visible-cover diagnostics
  (`en=0` disables them).
  OTA verification on 2026-11-08 (`3e68d36-album-idle`, 2096 albums) filled
  all 8–9 visible covers without Enter after knob/scroll bursts and direction
  reversal. Row-binding work peaked at 1.812 ms in those bursts (excluding
  rendering); artwork batches took 5.2–13.4 ms and yielded after a slow read.
  The nine-cover cold viewport settled about 306 ms after the final scroll.
  Selection highlighting also includes the separate Shuffle All row when
  scrolling refreshes the pool. Negative-index actions are not restored after
  leaving Albums: returning selects item 1 and scrolls past the action row.
  Regular album selections retain their scroll position across tab changes.
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
- Touch drag scrolling is restored on scrollable lists. Albums reuses
  unchanged row text while checking visibility at frame intervals; Songs
  refreshes its virtual pool when the first visible row index changes. Page
  Up/Down still jumps one viewport, and a tap after scrolling opens/plays the
  row under the finger.
- The menu-bar clock is read from the RX8130CE RTC at boot. The web
  **Sync browser time** button and Settings → Clock write the system clock
  and the RTC; a board with an empty backup supply is seeded from the build
  timestamp.
- Audio prompt, Wi-Fi station/AP, SD config and the web UI are enabled.
  Camera is not. On hardware the UI shows only the connected device's library;
  with no device the library pages are empty and Now Playing shows No track.
  The mock library/player is simulator-only.
- After changing only C code, `idf.py app-flash` is enough; use full
  `idf.py flash` after changing assets or the partition table.
- Prompt sounds use the ES8388 speaker codec: 880 Hz for Enter, play/pause
  and star; 1200 Hz for tab changes, previous/next and context; a longer
  880 Hz / 65 ms tone for Back. Held previous/next sounds once when seeking
  begins. Rotary scrolling
  and list paging stay silent. Key Feedback mode (None / Touch / Touch + Key)
  and volume are available in Settings and the web UI. The shared dispatcher
  covers page-specific Enter/Back handlers and suppresses duplicate tones.
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
  the PPA draw path. FATFS long filenames are enabled, but the cache files
  keep the short `.thm` / `.cov` names for compatibility with older cards.
  See `docs/media-library-import.md`.
- TJPGD handles baseline JPEG only; progressive artwork falls back to the
  album color placeholder.
- iAP2 Now Playing covers work end-to-end: the album ID is resolved from
  the iAP2 title/artist metadata (with a unique-title fallback for tag
  differences), and the cached 360x360 cover is shown in the Now Playing
  widget. Verified with a paused iAP2 track and an AFC artwork sync; see
  `docs/screenshots/tab5-real-cover-iap2.png`.
- `# <id>` syncs one album's cover over AFC, which is useful when testing
  a specific iAP2 track without running the full library sync.
- **Settings > iOS Device** provides Pair/Connect, Quick sync,
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
