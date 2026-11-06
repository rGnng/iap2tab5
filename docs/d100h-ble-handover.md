# D100H BLE remote: handover and driver-analysis brief

Status: **blocked on missing vendor key events**. Everything up to and including
BLE scan, connect, pairing/bonding, GATT discovery, CCCD subscription, and the
encrypted keep-alive works. The D100H never sends a vendor notification on either
of the two known channels, so no button or knob events reach the UI.

This document is written for a driver/protocol analysis session. It records the
hardware, the code paths, the exact GATT layout of the test unit, the protocol
references, the experiments already run, the current hypotheses, and the next
captures that should settle the question.

---

## 1. Hardware and software

| Item | Value |
| --- | --- |
| Host MCU | M5Stack Tab5, ESP32-P4, chip revision v1.3 |
| Co-processor | On-board ESP32-C6-MINI-1U, connected over 4-bit SDIO |
| C6 SDIO pins | CMD 18, CLK 19, D0 20, D1 21, D2 22, D3 23 (C6 side) |
| P4 SDIO pins | CMD 13, CLK 12, D0 11, D1 10, D2 9, D3 8 |
| C6 reset | P4 GPIO15, active high |
| Host framework | ESP-IDF **5.4.2** |
| ESP-Hosted host | `espressif/esp_hosted` **2.13.1** (P4 side) |
| C6 firmware | M5Stack factory **ESP-Hosted 1.4.1**; reports `0.0.0` over the 2.x RPC |
| BLE host | NimBLE in host-only mode, HCI over ESP-Hosted VHCI |
| Remote | Ulanzi D100H, BLE name `Ulanzi Dial`, test unit address `CA:63:45:7D:8C:B9` |

### Important C6 version mismatch

The factory C6 firmware is ESP-Hosted 1.4.1. The P4 host is 2.13.1. At boot the
hosted transport reports:

```text
transport: capabilities: 0xd
transport:   * WLAN
transport:     - HCI over SDIO
transport:     - BLE only
transport: Version mismatch: Host [2.13.0] > Co-proc [0.0.0] ==> Upgrade co-proc to avoid RPC timeouts
```

`esp_hosted_bt_controller_init()` and `esp_hosted_bt_controller_enable()` both
return `ESP_FAIL`; the current code treats that as non-fatal and lets NimBLE use
raw VHCI. NimBLE does sync (`NimBLE host synced`) and BLE scan/connect/GATT work,
so VHCI carries HCI packets. The missing controller lifecycle RPC may still
affect notification delivery, connection parameters, or coexistence.

A matching **ESP-Hosted 2.13.1 C6 slave firmware** has been built and is in
`tools/c6_firmware/`:

```text
bootloader.bin
partition-table.bin
ota_data_initial.bin
network_adapter.bin   (0x11f3b0 bytes, version 2.13.1)
SHA256SUMS
```

The factory 1.4.1 firmware has no in-band OTA path (ESP-Hosted slave OTA was
added in 2.6.0), so the first update requires the internal C6 USB-TTL header.
See `tools/c6_firmware/README.md` for the pinout, the P4-bootloader step, and
`flash_c6.sh`. This one-time wired flash is still **pending**.

---

## 2. What is implemented

### 2.1 Files

| File | Role |
| --- | --- |
| `app/backend/ble_remote.c` | NimBLE central, scan, connect, pairing, NVS paired list, GATT discovery, HID report discovery, keep-alive, key decode, auto-reconnect |
| `app/backend/ble_remote.h` | Public API used by UI and main |
| `app/ui/ui.c` | Bluetooth Remote page, menu entry, key routing, context/long-press handling |
| `app/ui/icons.h`, `app/ui/fonts/bootstrap_icons_*.c` | Bluetooth icon U+F682 |
| `firmware/main/ulanzi_d100h.c/h` | TEA decode for BLE frames and D100H ID → `ui_key_t` mapping |
| `firmware/main/usb_hid_input.c/h` | `usb_hid_input_post_key()` to inject BLE keys into the existing HID queue |
| `firmware/main/main.c` | Calls `ble_remote_start()` after Wi-Fi/ESP-Hosted is up |
| `firmware/main/CMakeLists.txt`, root `CMakeLists.txt` | Build wiring for firmware and simulator |
| `firmware/sdkconfig.defaults` | NimBLE host-only + hosted VHCI config |
| `tools/c6_firmware/` | 2.13.1 C6 slave firmware, build script, wired flash script |
| `tools/gen_icons.sh` | Bluetooth icon added to the subset |

### 2.2 UI (temporary interaction, as requested)

Menu: Now Playing, Albums, Artists, Songs, Playlists, **Bluetooth Remote**,
Settings.

Bluetooth Remote page:

- `Pair New Devices` row: starts a 15 s scan (turns into `Stop Scan` while
  scanning).
- Nearby devices list while scanning; selecting a row connects.
- Paired devices list otherwise; short press connects, long press unpairs.
- Status text: `Idle`, `Starting scan...`, `Scanning...`, `Connecting...`,
  `Securing...`, `Discovering D100H...`, `Subscribing...`,
  `Connected: <name>`, `Disconnected`, or an error.
- `CONTEXT` on the page stops a scan or unpairs the focused paired device.
- `BACK` stops a scan.

Known UI limitation: when the 15 s scan finishes, the page falls back to the
paired list and the nearby results disappear. A user must select a result during
the scan. This is a UX issue, not the driver blocker.

### 2.3 Pairing / bonding / reconnect

- `ble_gap_security_initiate()` is called immediately after `CONNECT`.
- Service discovery starts only after `BLE_GAP_EVENT_ENC_CHANGE` with
  `status == 0`.
- SM config: Just Works, bonding enabled, Secure Connections enabled, NVS bond
  store via `ble_store_config_init()`.
- Test unit pairs and encrypts successfully:

  ```text
  ble_remote: encryption change: status=0
  ```

- The bond survives P4 reboot: reconnect encrypts without pairing again.
- Auto-reconnect: after a disconnect, the heartbeat task retries the first
  paired device every 3 s (15 s connect timeout). This recovers after the D100H
  sleeps or is power-cycled.
- Paired list is also stored in NVS namespace `ble_remote`, key `paired`, as
  `addr,addr_type,name\n`. This is separate from the NimBLE bond store.

### 2.4 Button mapping (already written, untested over BLE)

`firmware/main/ulanzi_d100h.c:ulanzi_d100h_to_ui()` maps:

| D100H ID | Position | UI key |
| --- | --- | --- |
| 0x01 | top-left | `UI_KEY_BACK` |
| 0x02 | top-middle | `UI_KEY_7` (play/pause) |
| 0x03 | top-right | `UI_KEY_CONTEXT` short / `UI_KEY_STAR` long (handled in `ble_remote.c`) |
| 0x04 | upper-left side | `UI_KEY_SCREEN_PREV` |
| 0x05 | upper-right side | `UI_KEY_PREV_TRACK` |
| 0x06 | lower-left side | `UI_KEY_SCREEN_NEXT` |
| 0x07 | knob click | `UI_KEY_ENTER` |
| 0x08 | lower-right side | `UI_KEY_NEXT_TRACK` |
| 0x09 / 0x0A | free CCW / CW | `UI_KEY_UP` / `UI_KEY_DOWN` |
| 0x0B / 0x0C | knob-held CCW / CW | `UI_KEY_UP` / `UI_KEY_DOWN` |

Top-right long press is measured from the down event to the up event; >= 600 ms
is Star, otherwise Context.

---

## 3. Protocol background

Two independent reverse-engineering documents cover the D100H. They disagree
about which GATT channel carries vendor events, which is central to this bug.

### 3.1 `kriomant/ulanzi-d100h` — vendor service 0x1971

Source: <https://github.com/kriomant/ulanzi-d100h/blob/main/PROTOCOL.md>

- Device name in capture: `Ulanzi Dial`; firmware shown by Studio: `1.3.8.1`.
- Vendor service: `00001971-0000-1000-8000-00805f9b34fb`
- Write without response: `0000fd01-0000-1000-8000-00805f9b34fb`
- Notify: `0000fd00-0000-1000-8000-00805f9b34fb`
- BLE values are **20 bytes**.
- The vendor packet is encrypted with original TEA (32 rounds, little-endian
  words, delta `0x9e3779b9`, key bytes
  `ca ba a5 ca 6d 8a 2a bc ba 9e 5a ca ca 8b b8 9b`).
- For 20 bytes, encrypt/decrypt bytes 0–7 and 8–15; bytes 16–19 are unchanged.
- Heartbeat plaintext `06 01 23 00 01` + zeros, ciphertext:

  ```text
  24 56 9e f2 28 e1 45 a1 38 90 c4 99 a3 60 aa ad 00 00 00 00
  ```

- Host writes the heartbeat **immediately after subscribing** and every 1000 ms.
- Vendor mode suppresses factory HID keyboard/media actions and lasts about
  3 seconds after the last heartbeat.
- An independent client confirmed that subscription + heartbeat alone is enough
  on firmware 1.3.8.1, including after a power cycle. No Ulanzi Studio startup
  commands were required.
- Control events decrypt to `8b 10 ID STATE` + 16 zero bytes; ID 01–0C as in the
  mapping table above.
- The same doc also lists optional startup queries (MAC, flash ID, serial,
  firmware version, haptics, sleep timer), but states they are not required on
  the tested firmware.

### 3.2 `ftl/hamdial` — HID report 0xFB

Source: <https://github.com/ftl/hamdial/blob/master/doc/protocol.md>

- The dial has a HID service `0x1812` with seven Report characteristics
  (`0x2A4D`).
- Report ID **0xFB**, type input and output. The report reference descriptor
  identifies the report ID; handles differ per unit.
- Frames are **64 bytes**: bytes 0–7 are the encrypted command, bytes 8–15 are
  the constant token `38 90 c4 99 a3 60 aa ad`, bytes 16–63 are unused.
- The keep-alive command is the same first 16 bytes as the 20-byte BLE
  heartbeat, padded to 64 bytes:

  ```text
  24 56 9e f2 28 e1 45 a1 38 90 c4 99 a3 60 aa ad 00 ... 00
  ```

- The host writes this to the HID **output** report 0xFB each ~1.02 s. No other
  command is necessary to enter vendor mode. The dial returns to standard HID
  mode if keep-alives stop.
- Events arrive on the HID **input** report 0xFB, same `8b 10 ID STATE` format
  after decrypting the first 8 bytes.
- The vendor service 0x1971 is explicitly described here as "function not
  known"; this implementation does not use it.

### 3.3 `rGnng/UlanziD100HProtocol`

Source: <https://github.com/rGnng/UlanziD100HProtocol>

Consolidates both findings, includes a portable C++ decoder, verifies the TEA
vectors, and states that BLE vendor mode is entered by subscription +
heartbeat. It also notes that USB is a different transport and that the wired
D100H is `FFF1:00DE`, vendor usage page `0xFFFE`, 64-byte reports.

### 3.4 Verified ciphertext vectors

| Meaning | 20-byte ciphertext |
| --- | --- |
| Heartbeat | `24569ef228e145a13890c499a360aaad00000000` |
| Top-left down | `8e8f5e16ec67281e3890c499a360aaad00000000` |
| Top-left up | `028db3660224d0c73890c499a360aaad00000000` |
| Heartbeat reply | `21f45eb71c3587ab3890c499a360aaad00000000` |

The repeated block `38 90 c4 99 a3 60 aa ad` is the encryption of eight zero
bytes. The current `ulanzi_d100h_decode_ble()` checks exactly this format.

---

## 4. Actual GATT layout of the test unit

Service discovery on `CA:63:45:7D:8C:B9` produced:

| Service | Handles |
| --- | --- |
| `0x1800` Generic Access | 1–7 |
| `0x180A` Device Information | 8–26 |
| `0x180F` Battery | 27–30 |
| `0x1812` HID | 31–74 |
| `0x1971` vendor | 75–80 |

### 4.1 HID service characteristics

| UUID | Value handle | Properties | Report reference |
| --- | --- | --- | --- |
| `0x2A4A` HID Information | 33 | read | — |
| `0x2A4B` Report Map | 35 | read | — |
| `0x2A4C` HID Control Point | 37 | write-no-rsp | — |
| `0x2A4E` Protocol Mode | 39 | read + write-no-rsp | — |
| `0x2A22` Boot Keyboard Input | 41 | read + notify | — |
| `0x2A32` Boot Keyboard Output | 44 | read + write + write-no-rsp | — |
| `0x2A33` Boot Mouse Input | 46 | read + notify | — |
| `0x2A4D` Report | 50 | read + notify | ID 0x01, input |
| `0x2A4D` Report | 54 | read + write + write-no-rsp | ID 0x01, output |
| `0x2A4D` Report | 57 | read + notify | ID 0x03, input |
| `0x2A4D` Report | 61 | read + notify | ID 0x02, input |
| `0x2A4D` Report | 65 | read + notify | ID 0xFC, input |
| `0x2A4D` Report | 69 | read + notify | **ID 0xFB, input** |
| `0x2A4D` Report | 73 | read + write + write-no-rsp | **ID 0xFB, output** |

CCCDs found: 42, 47, 51 (owner 50), 58 (owner 57), 62 (owner 61), 66 (owner 65),
70 (owner 69). There is no CCCD for the output-only report 54 or 73.

### 4.2 Report Reference descriptors

| Descriptor handle | Report ID | Type | Owning report characteristic |
| --- | --- | --- | --- |
| 48 | 0x00 | 0x0A | before the first 0x2A4D, ignored |
| 52 | 0x01 | 0x01 input | 50 |
| 55 | 0x01 | 0x02 output | 54 |
| 59 | 0x03 | 0x01 input | 57 |
| 63 | 0x02 | 0x01 input | 61 |
| 67 | 0xFC | 0x01 input | 65 |
| 71 | **0xFB** | **0x01 input** | **69** |
| 74 | **0xFB** | **0x02 output** | **73** |

### 4.3 Vendor service 0x1971

Earlier discovery found:

| UUID | Value handle | Properties |
| --- | --- | --- |
| `0xFD00` notify | 77 | read + notify |
| `0xFD01` write | 80 | read + write + write-no-rsp |

CCCD for `0xFD00` is handle 78.

### 4.4 NimBLE API gotcha

`ble_gattc_disc_all_dscs()` callback's `chr_val_handle` argument was **always 31**
on this stack/unit, regardless of the descriptor. The code therefore computes
the owning report characteristic from the descriptor handle and the sorted list
of Report value handles:

```c
static uint16_t report_chr_owner(uint16_t handle)
{
    uint16_t owner = 0;
    for(int i = 0; i < s_hid_report_count; i++) {
        if(s_hid_report_chrs[i] <= handle) owner = s_hid_report_chrs[i];
        else break;
    }
    return owner;
}
```

This is why the first implementation set both input and output to handle 31 and
subscribed to the wrong CCCD. That is fixed.

---

## 5. Current runtime behavior

### 5.1 Boot / hosted

```text
W (17360) ble_remote: BT controller init RPC failed: ESP_FAIL (trying VHCI)
W (22360) ble_remote: BT controller enable RPC failed: ESP_FAIL (trying VHCI)
I (22361) ble_remote: nimble_port_init ok
I (22362) ble_remote: NimBLE host task running
I (22363) ble_remote: BLE central started
I (22375) ble_remote: NimBLE host synced
```

### 5.2 Scan / connect

Scan finds the D100H reliably:

```text
I (26424) ble_remote: found Ulanzi Dial rssi=-49
I (26658) ble_remote: found Ulanzi Dial rssi=-48
```

Connect and encryption:

```text
I (22882) ble_remote: encryption change: status=0
I (23182) NimBLE: start_handle=31 end_handle=74
...
I (25532) ble_remote: HID report 0xfb: input=69 output=73
I (25532) NimBLE: att_handle=39 len=1     # Protocol Mode = 0x01 (no-rsp)
I (25532) NimBLE: att_handle=37 len=1     # HID Control Point = 0x01 (no-rsp)
I (25533) NimBLE: att_handle=70 len=2     # CCCD input report 0xFB = 01 00
I (25682) NimBLE: att_handle=73 len=64    # keep-alive to output 0xFB
I (25682) ble_remote: connected to Ulanzi Dial (CA:63:45:7D:8C:B9) via HID
```

Keep-alives continue every second:

```text
I (26363) NimBLE: att_handle=73 len=64
I (27363) NimBLE: att_handle=73 len=64
...
```

### 5.3 The blocker

There is **no `BLE_GAP_EVENT_NOTIFY_RX` at all** on input handle 69. There is no
heartbeat reply (`06 01 23 10 ...`) either, even though the D100H protocol says
the controller replies to the heartbeat. There are also no notifications on the
vendor service's `0xFD00` (handle 77) when that channel was tried earlier.

The user has:

- unplugged the D100H's USB-C cable (it has a 1000 mAh battery),
- confirmed the bottom device-switch slot is the Tab5 slot,
- power-cycled the D100H,
- pressed all buttons / rotated the knob while the Tab5 was connected.

No notifications, no UI actions.

### 5.4 Disconnect behavior

Without interaction the D100H sometimes drops the link after 60–90 s:

```text
W (91306) ble_remote: disconnect: reason=0x208
```

`0x208` is HCI `0x08` (connection timeout). The current firmware auto-reconnects
after 3 s when the device advertises again. A deliberate local terminate uses
reason `0x216` / HCI 0x13.

### 5.5 Experiments already run

| Experiment | Result |
| --- | --- |
| Scan and list D100H | Works, RSSI −50 to −70 |
| Connect without bonding | Works, GATT discovery works |
| Pair/bond Just Works | Works, `ENC_CHANGE status=0`, bond survives reboot |
| Vendor service `0x1971` + 20-byte heartbeat | Subscription succeeds; **no notifications** |
| HID report 0xFB: subscribe CCCD 70, write 64-byte keep-alive to 73 | Writes succeed (no-rsp); **no notifications** |
| Protocol Mode 0x2A4E = 0x01 (Report Protocol) | Write-no-rsp sent; no effect |
| HID Control Point 0x2A4C = 0x01 (Exit Suspend) | Write-no-rsp sent; no effect |
| Keep-alive with **write with response** | Device does not respond; after ~20 s attempts fail with `BLE_HS_ENOMEM` (6). Reverted to write-no-rsp |
| Disconnect/reconnect, NVS paired list | Works; auto-reconnect works |
| User unplugged USB and power-cycled D100H | Still no notifications |

---

## 6. Hypotheses for the driver-analysis model

### H1. Vendor mode needs a different trigger or initialization sequence

The public docs say keep-alive alone is sufficient on firmware 1.3.8.1. This
unit may be a newer firmware, or Ulanzi Studio may send seven startup commands
that are "not necessary" on the captured unit but are necessary here. The most
valuable next artifact is a **known-good capture** of Ulanzi Studio connecting to
this exact D100H and the first button press. Capture every ATT write (handle,
opcode, value) and notification. Compare with:

- 0x1971/0xFD01 20-byte heartbeat,
- HID output 0xFB 64-byte keep-alive (and whether a report ID byte is included),
- Protocol Mode / HID Control Point writes,
- startup queries (`01 01 FA 01`, `01 04 0B 01`, `01 01 0B 01`, `01 01 02 01`,
  `01 06 40 01`, `01 01 42 01`, `01 04 04 01`),
- haptics/sleep writes,
- any write to 0x2A4D report 0xFC or to 0x1971 FD01.

### H2. Wrong channel / report ID on this unit

The unit exposes five input reports: 0x01, 0x02, 0x03, 0xFC, 0xFB. We only
subscribe to 0xFB. The next diagnostic should subscribe to **all** input report
CCCDs (51, 58, 62, 66, 70) and log every notification while pressing keys. This
will show whether the device is sending:

- standard HID reports (default mode) on 0x01/0x02/0x03,
- vendor reports on 0xFC,
- vendor reports on 0xFB (expected),
- nothing at all.

If standard HID reports arrive, the BLE link and CCCD path are proven; the
problem is vendor-mode activation. If nothing arrives on any report, suspect
authentication, controller firmware, or a device state issue.

### H3. Authenticated (MITM) pairing required

The link is encrypted with Just Works (`sm_mitm = 0`), so it is **not
authenticated**. Some vendor channels require an authenticated link. Check
`ble_gap_conn_desc.sec_state.authenticated`. Try:

```c
ble_hs_cfg.sm_mitm = 1;
ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY; // or KEYBOARD_ONLY
```

The D100H has a screen and a device-switch button; it may display a passkey.
Implement a serial-console passkey entry and `ble_sm_inject_io()` /
`ble_gap_passkey_reply()` if needed. This is a strong candidate because the
vendor channel may be deliberately protected while standard HID is not.

### H4. C6 1.4.1 / VHCI notification delivery

Although scan, connect, and ATT writes work, the old C6 firmware is outside the
2.x host protocol and the BT controller RPC is missing. Notification delivery
could be affected by controller-side HCI event handling, connection parameter
negotiation, or SDIO/VHCI buffering. The clean next step is the one-time wired
flash of the 2.13.1 C6 firmware in `tools/c6_firmware/`, then retest before
changing more host code.

### H5. CCCD / GATT caching state

We write CCCD 70 = `01 00` and see the ATT write go out, but we never read it
back. Add a read of handle 70 after subscribing and log the value. Also check
whether NimBLE GATT caching (if enabled) is returning stale handles/CCCD state.
Try clearing the bond (`ble_store_clear()`), rebooting, and pairing from scratch.

### H6. Unit/firmware difference

The protocol was captured on firmware 1.3.8.1. The user's unit may be newer and
may gate vendor mode behind a firmware flag, an "Online Mode" selection, or a
Ulanzi Studio session. The BLE name `Ulanzi Dial` matches the capture, but the
vendor mode behavior may differ.

### H7. USB/wired priority is still latched

The D100H is dual-mode; wired takes priority. The user unplugged USB and
power-cycled, but confirm:

- the device is actually off/on (not just screen blank),
- the bottom switch is on the Tab5 slot,
- the status LED is off (connected) or flashing (pairing), not showing another
  slot,
- the device is not still connected to a computer over USB for charging/data.

Charging from a dumb charger should be fine, but a data-capable USB host may
still take priority.

### H8. HID Report Protocol state not actually applied

Protocol Mode 0x2A4E is write-no-rsp only; a write-with-response returns ATT
0x03 (Write Not Permitted). We send 0x01 (Report Protocol) with no-rsp and
immediately continue. Read handle 39 back to confirm it is 0x01. If it stays
0x00 (Boot Protocol), the device will not use report 0xFB.

---

## 7. Recommended next steps

### 7.1 Capture a known-good session first

This is the highest-value action. On a host with a working Bluetooth stack:

**Linux**

```bash
sudo btmon -w d100h.btsnoop
# pair/connect with the OS, then run Ulanzi Studio or the d100h.py listener
python3 d100h.py ble CA:63:45:7D:8C:B9
```

Inspect the btsnoop with Wireshark (`btatt` dissector). Record every
`Write Command` / `Write Request` and every `Handle Value Notification` from
connection to first button press. Pay special attention to the first 2 seconds
after connect and to any write that is not the 20-byte heartbeat.

**macOS**

Use PacketLogger (Additional Tools for Xcode) or Wireshark with a BLE sniffer.
Pair through System Settings, quit Ulanzi Studio, and use the
`d100h_macos.py ble` helper. Capture both Ulanzi Studio and the standalone
listener.

### 7.2 Add an all-reports diagnostic in `ble_remote.c`

In `finish_hid_discovery()`, subscribe to every CCCD in `s_hid_cccd_handle[]`
(51, 58, 62, 66, 70) instead of only the 0xFB CCCD. In
`BLE_GAP_EVENT_NOTIFY_RX`, log the attribute handle and raw bytes for every
notification, not just handle 69. Press keys and compare which report ID
responds. This is a small, reversible change and will immediately distinguish
"no notifications at all" from "wrong report ID".

### 7.3 Read back state

After subscribing, issue GATT reads for:

- CCCD 70 (expect `01 00`),
- Protocol Mode 39 (expect `01`),
- Report Reference descriptors (already read during discovery),
- `ble_gap_conn_desc` security state (encrypted / authenticated / bonded).

Log the results.

### 7.4 Try MITM / passkey

If the all-reports diagnostic still shows nothing, switch SM to MITM and
implement passkey entry. Check the D100H screen during pairing; if it shows a
6-digit code, that confirms MITM is required.

### 7.5 Upgrade the C6

Flash `tools/c6_firmware/network_adapter.bin` (and the bootloader/partition
images) through the internal C6 header. This removes the version mismatch and
restores the BT controller lifecycle RPC. Retest scan/connect/notify before
changing the host code further.

### 7.6 Fallback: standard HID media keys

If vendor mode cannot be recovered, the D100H's default HID mode still sends
consumer control reports. Subscribe to input report 0x03 (CCCD 58) and parse the
3-byte consumer reports (`report id 0x02`, usage little-endian). The default
offline mapping is documented in
`brendanwelsh/ulanzi-d100h-homebrew` (`docs/offline-protocol.md`):

- dial CW/CCW: Volume Up / Volume Down,
- dial press: Mute (may be gated on external power),
- three top buttons: Previous / Play-Pause / Next,
- side buttons: Ctrl+C/V/Z/Y or similar keyboard usages.

This loses the requested per-button mapping but may be the only path if the
vendor channel is firmware-locked.

---

## 8. Build / flash / test

### 8.1 Host firmware

```bash
source /path/to/esp-idf/export.sh        # ESP-IDF 5.4.2
cd firmware
idf.py build
idf.py -p /dev/ttyACM0 -b 460800 app-flash
```

The MFi identity file `firmware/main/iap2_identity.c` is generated locally with
`tools/gen_iap2_identity.py` and is gitignored. It is not needed for BLE-only
testing unless the full app links it.

### 8.2 Simulator

```bash
cd /home/ops/iap2tab5
cmake --build build -j
./build/tab5_sim
```

The simulator uses `assets/mock`; BLE is stubbed out (`ble_remote_is_supported()
== false`).

### 8.3 Serial console

115200 8N1 on `/dev/ttyACM0`. Keys:

| Key | Action |
| --- | --- |
| `w` / `s` | up / down |
| `\r` | enter |
| `b` | back |
| `[` / `]` | previous / next screen |
| `c` | context menu |
| `1`..`7` | jump to screen / media keys |
| `g` | dump a 1280x720 RGB565 screenshot as hex (`IAP2SHOTHEX`) |

The Bluetooth page is screen index 5. A quick manual test:

```text
] ] ] ] ]   -> Bluetooth Remote
\r          -> Pair New Devices / stop scan
s, \r       -> focus first nearby device, connect
```

### 8.4 C6 firmware

Read `tools/c6_firmware/README.md`. Short version:

```bash
# Put the P4 in bootloader so it stops driving C6 reset:
esptool.py -p /dev/ttyACM0 --before default_reset --after no_reset run

# Connect a 3.3V USB-UART to the internal C6 header (TX/RX/DTR/RTS/GND,
# do not connect 3V3), then:
tools/c6_firmware/flash_c6.sh /dev/ttyUSB0
```

Power-cycle the Tab5 afterwards. Expected boot log: a 2.13.x C6 version and no
`BT controller init RPC failed`.

### 8.5 C6 firmware rebuild

```bash
source /path/to/esp-idf/export.sh
tools/c6_firmware/build_c6_firmware.sh
```

It copies the slave project out of
`firmware/managed_components/espressif__esp_hosted/`, appends
`CONFIG_ESP32P4_TAB5_C6_BOARD=y`, builds for `esp32c6`, and copies the four
images into `tools/c6_firmware/`.

---

## 9. Code map / where to look

### `app/backend/ble_remote.c`

- `start_discovery()`: discovers all services, stores HID service 0x1812 range.
- `on_hid_chr()`: stores HID Report characteristics (0x2A4D), Protocol Mode
  (0x2A4E), HID Control Point (0x2A4C).
- `on_hid_dsc()`: maps CCCDs / Report Reference descriptors to the owning report
  characteristic using handle ranges (`report_chr_owner()`).
- `on_report_ref_read()`: reads the 0x2908 value; finds report ID 0xFB input and
  output.
- `finish_hid_discovery()`: writes Protocol Mode = 0x01, HID Control Point =
  0x01, then subscribes to CCCD 70.
- `on_write()`: after the CCCD write succeeds, marks connected, saves the pair,
  sends the first 64-byte keep-alive.
- `gap_event()`: scan, connect, security, discovery, disconnect, notifications.
  Notification handler is at `BLE_GAP_EVENT_NOTIFY_RX`.
- `heartbeat_task()`: 1 s keep-alive + auto-reconnect.
- `ble_remote_scan_start()`, `ble_remote_connect()`, `ble_remote_unpair()`:
  public API.

### `app/ui/ui.c`

- `ble_create()`: page layout, pair row, nearby/paired lists, focus.
- `ble_tick()`: rebuilds when `ble_remote_revision()` changes.
- `ble_pair_action()`: toggles scan.
- `ble_row_action()`: connect a scan/paired row.
- `ble_long_async()` / `ble_long_cb()`: long-press unpair.
- `ble_key()` / `ble_back()`: context/back handling.
- `ui_handle_key()`: the context key is offered to the current page before
  opening the global context menu.

### `firmware/main/ulanzi_d100h.c`

- `tea_decrypt_block()`: TEA block decrypt.
- `ulanzi_d100h_decode_ble()`: validates the 20-byte `8b 10 ID STATE` frame.
- `ulanzi_d100h_to_ui()`: ID → `ui_key_t` mapping; top-right handled by caller.

### `firmware/main/usb_hid_input.c`

- `usb_hid_input_post_key()`: posts a `ui_key_t` into the existing USB HID key
  queue so BLE keys take the same path as wired keys.

---

## 10. Repository / isolation notes

This work is isolated from the other session investigating iPod charging. The
working tree contains uncommitted charging changes in:

```text
firmware/main/iap2_host.c
firmware/main/iap2_host.h
firmware/main/serial_input.c
firmware/main/web_server.c
docs/iap2-bringup.md
firmware/README.md
```

Those are **not** part of this handover and must not be committed or overwritten
by the BLE work. The BLE commit contains only:

```text
app/backend/ble_remote.c
app/backend/ble_remote.h
app/ui/ui.c
app/ui/icons.h
app/ui/fonts/bootstrap_icons_*.c
assets/fonts/bootstrap-icons-codepoints.txt
CMakeLists.txt
firmware/main/CMakeLists.txt
firmware/main/main.c
firmware/main/ulanzi_d100h.c
firmware/main/ulanzi_d100h.h
firmware/main/usb_hid_input.c
firmware/main/usb_hid_input.h
firmware/sdkconfig.defaults
tools/gen_icons.sh
tools/c6_firmware/
docs/d100h-ble-handover.md
```

`firmware/sdkconfig` is gitignored; the BT/NimBLE/Hosted VHCI options live in
`firmware/sdkconfig.defaults`.

---

## 11. One-paragraph summary for a driver model

The Tab5's P4 runs NimBLE as a BLE central over ESP-Hosted VHCI to the on-board
C6. It scans, connects, Just-Works bonds, and discovers the D100H's GATT table.
The D100H exposes both the documented vendor service 0x1971 (20-byte encrypted
frames on FD00/FD01) and a HID service 0x1812 with report 0xFB input/output
(64-byte encrypted frames). The host subscribes to the notify CCCD, writes the
correct TEA-encrypted keep-alive (20 bytes to FD01, or 64 bytes to report 0xFB
output), and sends it every second. The link is encrypted, the CCCD write
succeeds, the keep-alive write succeeds, but the device never sends any
notification on either channel — not even the expected heartbeat reply. The
factory C6 firmware is 1.4.1 and does not implement the 2.x BT controller RPC,
so a matching 2.13.1 C6 image is ready for a one-time wired flash. The next
diagnostic is to subscribe to all HID input reports (0x01/0x02/0x03/0xFC/0xFB)
and capture a known-good Ulanzi Studio session to see the exact vendor-mode
handshake, then test MITM/passkey and the C6 upgrade.
