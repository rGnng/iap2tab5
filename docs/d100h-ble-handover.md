# D100H BLE remote: handover and driver-analysis brief

Status: **vendor mode working end-to-end**. Key and knob events are decoded
and mapped to the UI. The earlier "ACL-flow deadlock" conclusion was superseded
by section 13: the real blocker was a central-only NimBLE build with GATT server
support disabled, which discarded every incoming ATT notification. The original
`ec37af9` blocker and the earlier candidate patch are preserved below as
history.

Original status: **blocked on missing vendor key events**. Everything up to and including
BLE scan, connect, pairing/bonding, GATT discovery, CCCD subscription, and the
encrypted keep-alive works. The D100H never sends a vendor notification on either
of the two known channels, so no button or knob events reach the UI.

This document is written for a driver/protocol analysis session. It records the
hardware, the code paths, the exact GATT layout of the test unit, the protocol
references, the experiments already run, the current hypotheses, and the next
captures that should settle the question.

---

## 0. Driver-analysis findings after `ec37af9`

The local candidate patch addresses concrete framing/observability defects before
changing pairing policy or flashing the C6. It has **not** been built with
ESP-IDF or tested on a Tab5 in the analysis environment. The original logs and
experiments below are preserved as historical evidence, not proof of current
wire traffic or of the candidate fix working.

### 0.1 A logged 64-byte write may have been truncated to 20 bytes

The current HID keep-alive is 64 bytes, but `ec37af9` never requests an ATT MTU
exchange. A 64-byte ATT value requires MTU **at least 67** (three bytes for the
write/notification opcode and handle). The default ATT MTU is 23.

This is especially important with the **actual ESP-IDF 5.4.2 NimBLE revision**,
`0225775e49669cd4f070400908f73718bddce83b`:

- `ble_gattc_write_no_rsp()` logs the requested buffer length before ATT send.
- `ble_att_tx_with_conn()` then calls `ble_att_truncate_to_mtu()`.
- That function removes bytes past the negotiated MTU. With MTU 23, a requested
  64-byte value becomes a 20-byte value, without making the original `len=64`
  log a wire-level confirmation.

Sources (pinned to the IDF submodule, rather than a moving NimBLE branch):

- [IDF v5.4.2 NimBLE submodule](https://github.com/espressif/esp-idf/tree/v5.4.2/components/bt/host/nimble)
- [GATT logging before transmit](https://github.com/espressif/esp-nimble/blob/0225775e49669cd4f070400908f73718bddce83b/nimble/host/src/ble_gattc.c)
- [ATT send/truncation call](https://github.com/espressif/esp-nimble/blob/0225775e49669cd4f070400908f73718bddce83b/nimble/host/src/ble_att_cmd.c)
- [MTU truncation and default MTU selection](https://github.com/espressif/esp-nimble/blob/0225775e49669cd4f070400908f73718bddce83b/nimble/host/src/ble_att.c)

**Inference:** the HID trial may have sent only 20 of its required 64 bytes.
There is no negotiated-MTU measurement or wire capture in the supplied logs, so
this is a concrete missing precondition and plausible activation blocker, not a
proven explanation for every failed experiment. In particular, it does not
explain the earlier, already-20-byte FD01 trial by itself.

The candidate patch sets preferred MTU 247, exchanges MTU before HID discovery,
requires the actual result to be >=67, checks the size again before every HID
heartbeat, and logs failed sends. It does not split 64-byte commands into
multiple independent GATT writes or add a HID report ID to a characteristic
value. Neither would reproduce a single 64-byte HID report.

### 0.2 The HID notification decoder used the wrong framing

The `ec37af9` notification handler copies at most 20 bytes, overwrites the length
with that truncated length, and calls `ulanzi_d100h_decode_ble()`.
That function decrypts two blocks but leaves bytes 16..19 unchanged and demands
that they be zero. This is valid for the **20-byte FD00 vendor channel**, not a
general HID report 0xFB. If those bytes belong to another encrypted block or an
opaque tail in the 64-byte HID report, a valid control event is rejected.

The candidate patch preserves the full 64-byte value and routes it to the
existing `ulanzi_d100h_decode()`, which checks the control prefix and first
block padding without treating the unrelated HID tail as BLE padding. The BLE
vendor decoder now requires exactly 20 bytes to prevent accidental reuse with
larger frames. Full notification length and hex are logged before decoding.

This fixes a downstream decode defect. It cannot, by itself, explain the
complete absence of `BLE_GAP_EVENT_NOTIFY_RX` reported in the original log.

### 0.3 The current code does not implement the FD00/FD01 path

Despite the historical trials described below, `ec37af9` only discovers service
`1812` and report `FB`. The occurrence of `1971` in its advertising filter is not
vendor-service discovery. It contains no FD00 notification subscription or FD01
write handle. The two public documents describe two channels, not necessarily
contradictory protocols. The 20-byte vendor route needs a separate implementation
or separate experiment; changing only the payload length on the HID handle is
not equivalent.

### 0.4 Additional corrections and limits

- The old immediate and periodic heartbeat calls ignore their return values;
  the candidate patch logs failures and does not mark a failed initial send as
  connected. A successful write-without-response still is not a device
  acknowledgment. The connection log now says it is awaiting notifications.
- Discovery previously started even on failed encryption-change events. The
  patch stops that path and clears top-right hold timing across disconnects.
- The literal HID keep-alive contains the documented first 16 ciphertext bytes
  and a zero tail. It matches the `hamdial` HOGP recipe. Do not replace it with
  the fully encrypted USB heartbeat just because the USB path worked: transport
  equivalence has not been established by a BLE capture from this exact unit.
- User-run macOS **USB** tests confirmed the same dial family's `FFF1:00DE`,
  page `FFFE`, unnumbered 64-byte USB interface and decoded controls. They do
  not validate BLE notification delivery, Just Works/MITM requirements, or the
  Tab5's hosted controller. The D100H should not be assumed to have a screen
  or display a passkey; security changes require actual evidence.
- The C6 host/slave version mismatch remains real and unresolved. Do not call
  it the proven source of missing vendor events while the above host defects
  remain untested.

### 0.5 Validation and next hardware run

Host-only regression command:

```sh
python3 -m unittest discover -s tests -v
```

The four tests compile the production C codec using the host C compiler with
`-Wall -Wextra -Werror`. They cover captured BLE vectors, an assembled 64-byte
HID frame with different tails, the old truncation/decoder failure, strict BLE
length checking, and the wired heartbeat vector. Assembled HID fixtures are not
represented as raw captures from this BLE unit. These tests do not compile or
exercise the ESP-platform NimBLE backend.

For the next Tab5 build/run, expect:

1. `ATT MTU exchange: status=0 mtu=... (HID requires >=67)` before discovery.
2. Report `FB` selection and a successful CCCD write.
3. `heartbeat queued, awaiting notifications`; no heartbeat send errors.
4. `HID notify handle=... len=64 mtu=...`, then decoded controls or raw
   non-control packets (heartbeat replies are not key frames).

If MTU <67, stop diagnosing firmware locks: the HID value cannot fit that link.
If MTU >=67 but no notifications arrive, add all-input CCCD subscription/readback
and implement the independently specified `1971/FD00/FD01` route. A known-good
BLE capture from the same dial remains the best discriminator for that earlier
vendor-channel failure. The C6 firmware upgrade is a separate controlled step.

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

Two independent reverse-engineering documents cover distinct D100H GATT
channels. Their payload lengths and padding rules must be kept separate; the
original investigation treated channel choice as a possible source of the bug.

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

This is an unverified security hypothesis, not evidence that MITM is needed.
Do not assume the D100H can display a passkey. Inspect the actual peer pairing
request and negotiated security state before changing IO capabilities or
implementing passkey entry. Host framing defects in section 0 should be tested
first.

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

At `ec37af9` the Tab5 connects and bonds with the D100H but receives no vendor
notifications in the supplied log. The checked-in path selects HOGP report FB,
requests 64-byte writes without an MTU exchange, and truncates received HID
values to 20 bytes before using an FD00 decoder. ESP-IDF 5.4.2's NimBLE can
truncate writes to MTU-3 after logging the original length, so those logs do not
establish a successful 64-byte wire write. The local candidate patch negotiates
and requires MTU >=67, logs send errors, preserves 64-byte HID notifications,
and uses the HID decoder. Host codec tests pass; full ESP-IDF build and hardware
retest are pending. The earlier failed 20-byte FD01 trial is not explained by
the HID MTU defect, and that vendor-service path is absent from this commit.
If corrected HID framing still yields no notifications, collect CCCD readback,
all-input notifications and a known-good BLE capture from the same unit. The
C6 1.4.1 versus host 2.13.1 mismatch remains an independent unresolved issue;
a matching C6 image is ready, but neither that nor MITM is a proven root cause.

---

## 12. C6 2.13.1 upgrade and post-upgrade BLE findings (2026-11-06)

> **Superseded by section 13 (2026-11-06).** The ACL-pool failures below were
> caused by verbose per-notification logging blocking the NimBLE host task, not
> by the heartbeat itself. The actual reason no notifications reached the UI was
> that the host was built central-only (`MYNEWT_VAL(BLE_GATTS)=0`), so NimBLE's
> ATT dispatch table did not include the Notification/Indication opcodes.

This section records the hardware work after `ec37af9` and the driver analysis
of Ulanzi Studio 3.4.8. It supersedes the earlier "HID 0xFB uses 64-byte
frames" assumption for this specific D100H unit.

### 12.1 C6 firmware upgrade completed

The factory C6 (ESP-Hosted 1.4.1) was replaced with the matching ESP-Hosted
2.13.1 slave firmware from `tools/c6_firmware/`:

- `bootloader.bin` at `0x0`
- `partition-table.bin` at `0x8000`
- `ota_data_initial.bin` at `0xd000`
- `network_adapter.bin` at `0x10000`

The internal `J1 / C6_ISP` header is the only programming interface. **The boot
strap is IO9, not IO0**:

| J1 pin | Net | Programmer |
| --- | --- | --- |
| 1 | `WLAN_3.3V` | do not connect (Tab5 supplies power) |
| 2 | `RF_C6_TXD` | adapter RX |
| 3 | `RF_C6_RXD` | adapter TX |
| 4 | `RF_C6_RST` / EN | adapter RTS (or manual reset) |
| 5 | `RF_C6_IO9` / BOOT | adapter DTR (or manual GND) |
| 6 | GND | adapter GND |

The C6 UART is not routed to the P4; there is no P4 UART bridge. The P4 can
control C6 EN through GPIO15 but cannot control IO9. A one-time wired flash is
therefore required for the 1.4.1 -> 2.13.1 step. `tools/c6_firmware/README.md`
and `flash_c6.sh` now document IO9 and the J1 pinout.

The successful flash was done with:

```bash
python -m esptool --chip esp32c6 -p /dev/ttyUSB0 -b 115200 \
  --before no_reset --after no_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m \
  0x10000 tools/c6_firmware/network_adapter.bin
```

The bootloader/partition-table/ota_data had already been written and verified.
The practical workaround for the CH340's weak 3.3V and the P4 reset line was:
power the C6 from the programmer's 3.3V with the Tab5 fully off, or keep the P4
in bootloader; any reset during the large app write aborts the transfer.

Post-upgrade boot log:

```text
transport: Identified slave [esp32c6]
transport: got co-processor transport size: 1536
transport: co-processor transport size matches host transport size: 1536
transport: Base transport is set-up, TRANSPORT_TX_ACTIVE
```

There is no `Version mismatch` warning and the BT controller init/enable RPCs
now succeed. NimBLE initializes (`nimble_port_init ok`, `NimBLE host synced`).

### 12.2 Ulanzi Studio driver: the vendor-service path is the Studio path

Disassembly of `Ulanzi_Studio.dylib` from the Ulanzi Studio 3.4.8 macOS package
(SHA-256 `482f4827...`) confirms:

- `BleDevice` uses service **0x1971**, notify **FD00**, write **FD01**.
- It subscribes to FD00 with `setNotifyValue:YES` and writes to FD01 with
  `writeValue:forCharacteristic:type:` where type = 1 (write without response).
- For FD01 it takes the first 20 bytes of the message and calls
  `+[MessageHelper encodeMessage:]` (TEA), then writes the 20-byte value.
- `KehwinDevice` starts a background thread and sends
  `+[MessageHelper deviceHeartbeatMessage]` every ~1 second. The plaintext is
  `06 01 23 00 01` + zeros; the encrypted 20-byte heartbeat is
  `24569ef228e145a13890c499a360aaad00000000`.
- The driver also builds optional startup queries/settings (`setExpertMouseSWStatus`,
  `getDeviceActive`, `getDeviceStandbyStatus`, `getDeviceVersion`,
  `getDeviceHardwareVersion`, `getDeviceFlashId`, `getDeviceMacAddress`,
  `setDeviceDialMiniLedHeartbeat`). The independent protocol test showed the
  heartbeat alone is sufficient on firmware 1.3.8.1, but this unit may send
  HID reports until vendor mode is established.

The Studio does **not** use HID report 0xFB for the D100H custom events. The
`ftl/hamdial` HID report 0xFB route is an alternative client path, not the
Studio path.

### 12.3 The unit sends 20-byte notifications on both channels

With the vendor service and HID service both discovered, the unit emits
20-byte ATT notifications on:

- **HID report 0xFB input, value handle 69** (CCCD 70)
- **vendor FD00, value handle 77** (CCCD 78)

The ACL-level trace (HCI ACL, L2CAP CID 0x0004, ATT opcode 0x1b) showed:

```text
att=69 len=32   (20-byte payload)
att=77 len=32   (20-byte payload)
```

It also sends standard HID reports on handles 50/61/29 during the initial
HID-mode burst. The HID 0xFB notification is therefore **20 bytes**, not the
64-byte frame assumed by the earlier branch patch. `ulanzi_d100h_decode_ble()`
(which requires exactly 20 bytes) is the correct decoder for both channels.
The branch's HID path was subscribing to CCCD 70 but rejecting everything
because it required 64 bytes.

### 12.4 The remaining blocker: ACL pool exhaustion / host-task starvation

After the C6 upgrade, the failure changes shape but remains:

```text
vhci_drv: Rx: alloc_acl_from_ll failed
```

ACL failures appear only after the heartbeat is sent. With the heartbeat
disabled, failures are zero. With the heartbeat active, the D100H sends a
burst of 20-byte HID/vendor notifications plus standard HID reports; the host
ACL pool fills and the GATT notifications never reach
`BLE_GAP_EVENT_NOTIFY_RX`.

A NimBLE `ble_npl_callout` (`host alive`) confirmed that the host task itself
continues to run, but the ACL receive queue is not drained. The likely
mechanism is:

- a NimBLE GATT heartbeat write from a separate FreeRTOS task holds the NimBLE
  host lock while `esp_hosted_tx()` waits on the shared SDIO bus;
- the NimBLE host task needs the same lock (or is otherwise blocked) to process
  the incoming ACL queue;
- the queue fills, `ble_transport_alloc_acl_from_ll()` fails, and the
  notifications are dropped.

Things that did **not** fix it:

| Experiment | Result |
| --- | --- |
| Stop Wi-Fi after ESP-Hosted init | no improvement; heartbeat still fills ACL |
| Move heartbeat to a NimBLE callout (host task) | host task sends, but ACL still fills |
| Subscribe to HID 0xFB CCCD 70 | subscription succeeds; no notifications dispatched |
| Increase `CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT` to 64 | failures delayed, not eliminated |
| Increase host task stack to 8192 | no improvement |
| 256 transport ACL buffers | `nimble_port_init` fails with `ESP_FAIL` (memory) |
| Direct HCI ACL heartbeat via `esp_hosted_tx()` | packet bytes match NimBLE exactly, but bypassing controller flow control makes the D100H disconnect with reason `0x0c` |

The direct HCI packet was byte-identical at the HCI/L2CAP/ATT level:

```text
02 00 00 1b 00 17 00 04 00 52 50 00 ...
```

It still failed because it bypassed the host/controller ACL flow-control
accounting; the controller/C6 rejected it with HCI `0x0c` (Command Disallowed)
when the ACL buffers were not credited.

### 12.5 Current stable state

The currently flashed P4 build is stable and does not reboot:

- Wi-Fi manager starts normally (ESP-Hosted transport is initialized).
- C6 2.13.1 is identified; transport size 1536; no version mismatch.
- D100H `Ulanzi Dial` connects, encrypts, discovers service 0x1971/FD00/FD01,
  and the HID 0xFB CCCD (handle 70) is subscribed.
- NimBLE heartbeat is sent once per second from the heartbeat task.
- The ACL pool still shows occasional `alloc_acl_from_ll failed` under D100H
  notification load, and no key events reach the UI.

Do not disable Wi-Fi before `ble_remote_start()`: that leaves ESP-Hosted
uninitialized, the P4 resets the C6 via GPIO15, SDIO init fails, and the board
appears to reboot in a loop. If Wi-Fi must be disabled for a BLE-only test,
start Wi-Fi first, then stop it (or keep the transport initialized).

The test code used for the post-upgrade experiments (20-byte HID decode,
hardcoded HID CCCD 70, heartbeat task, host stack 8192, transport ACL 64) is
**uncommitted** in the worktree. This handover commit contains the conclusions
and the corrected C6 programming documentation, not the experimental code.

### 12.6 Recommended next paths

1. **Dedicated BLE module (recommended)**  
   Use an ESP32-C3/S3 (or similar) as the BLE central for the D100H, with a
   UART/USB bridge into the P4. This removes the C6/Wi-Fi/BLE SDIO coexistence
   and host-lock problem entirely. The D100H protocol is already known
   (0x1971/FD00/FD01, 20-byte TEA heartbeat), so the bridge firmware is small.

2. **Upgrade IDF to 5.5 + ESP-Hosted 3.x**  
   OrcSDR runs a Tab5 P4 at IDF 5.5.4 with C6 3.0.6 and reports normal Wi-Fi
   operation. A newer ESP-Hosted VHCI/NimBLE may fix the blocking TX / ACL
   flow-control behavior. This is a larger migration; the P4 rev<3 memory map
   must be configured explicitly.

3. **Patch the VHCI TX path for non-blocking/flow-controlled ACL sends**  
   The current `vhci_drv` calls `esp_hosted_tx()` while the NimBLE host lock is
   held. A flow-control-aware, non-blocking TX queue would allow the host task
   to drain RX. This is the most direct software fix but needs careful testing
   to avoid dropping heartbeat packets.

4. **BLE-only C6 mode**  
   Start Wi-Fi to initialize ESP-Hosted, then stop Wi-Fi and use a
   flow-controlled heartbeat. The current tests did not achieve reliable
   notifications even with Wi-Fi stopped, so this still depends on (2) or (3).

Until one of these is done, the D100H can connect and heartbeat but key events
will not reach the UI.


---

## 13. Root cause: GATT-server dispatch, and working vendor mode (2026-11-06)

This section supersedes the blocker described in section 12. Vendor mode is now
working on the Tab5; the D100H is no longer limited to HID-only operation.

### 13.1 Why no notification ever reached `BLE_GAP_EVENT_NOTIFY_RX`

The P4 NimBLE build was central-only:

```text
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n
# CONFIG_BT_NIMBLE_GATT_SERVER is not set
```

`esp_nimble_cfg.h` maps the missing GATT-server option to
`MYNEWT_VAL(BLE_GATTS)=0`. In NimBLE's ATT receive dispatch table
(`ble_att.c`), the Notification and Indication opcodes are compiled only when
`BLE_GATTS` is enabled:

```c
#if MYNEWT_VAL(BLE_GATTS)
    { BLE_ATT_OP_NOTIFY_REQ,   ble_att_svr_rx_notify },
    { BLE_ATT_OP_INDICATE_REQ, ble_att_svr_rx_indicate },
#endif
```

A central-only build therefore **silently discards every incoming ATT
Notification (0x1b)** before it can become `BLE_GAP_EVENT_NOTIFY_RX`. This
affected both the HID input reports and the vendor FD00 frames. The packets
were visible in the HCI ACL trace and were drained correctly by the host task,
but they never reached the application callback.

Fix:

```text
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_GATT_SERVER=y
CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=8192
CONFIG_BT_NIMBLE_ACL_BUF_COUNT=64
CONFIG_BT_NIMBLE_ACL_BUF_SIZE=255
CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT=64
```

The peripheral role/GATT server is only compiled in; the Tab5 does not
advertise or accept connections. It exists so the ATT notification dispatch is
present.

### 13.2 The ACL-pool failures were a logging artifact

The earlier `vhci_drv: Rx: alloc_acl_from_ll failed` storm was reproduced with
very verbose notification logging: a multi-line hex dump per report at 115200
baud blocked the NimBLE host task long enough to exhaust the ACL-from-LL pool.
With one compact log line per event (or no per-event log), the pool drains
normally. The 64-buffer configuration above is retained as headroom.

A direct HCI ACL heartbeat was also tried earlier; it was byte-identical to
NimBLE's packet but bypassed controller flow control and made the D100H
disconnect with reason `0x0c`. That workaround is not used. The final firmware
sends the heartbeat through the normal NimBLE GATT API.

### 13.3 Vendor mode: verified

The final firmware discovers service `0x1971`, subscribes to FD00 (value
handle 77, CCCD 78), and sends the 20-byte TEA heartbeat once per second from
the NimBLE host task's event queue. The D100H replies with 20-byte status
frames and, on user input, 20-byte event frames that decrypt to:

```text
8b 10 <id> <down> 00 ...
```

`ulanzi_d100h_decode_ble()` validates the frame and
`ulanzi_d100h_to_ui()` maps it to `ui_key_t`; the event is injected with
`usb_hid_input_post_key()`.

The D100H sends the same 20-byte event on both FD00 (handle 77) and HID report
0xFB (handle 69). The firmware treats FD00 as canonical and ignores handle 69
when the vendor handle is known, so a key press produces exactly one UI action.

Hardware verification:

```text
520 D100H events processed
0 alloc_acl_from_ll failures
0 disconnects
```

### 13.4 Current mapping

| Vendor ID | Physical key | UI key |
| --- | --- | --- |
| `0x01` | top-left | `UI_KEY_BACK` |
| `0x02` | top-middle | `UI_KEY_7` (play/pause) |
| `0x03` | top-right | short: `UI_KEY_CONTEXT`; long (>=600 ms): `UI_KEY_STAR` |
| `0x04` | upper-left | `UI_KEY_SCREEN_PREV` |
| `0x05` | upper-right | `UI_KEY_PREV_TRACK` |
| `0x06` | lower-left | `UI_KEY_SCREEN_NEXT` |
| `0x07` | knob click | `UI_KEY_ENTER` |
| `0x08` | lower-right | `UI_KEY_NEXT_TRACK` |
| `0x09` | rotate counter-clockwise | `UI_KEY_UP` |
| `0x0A` | rotate clockwise | `UI_KEY_DOWN` |
| `0x0B` / `0x0C` | held rotation | `UI_KEY_UP` / `UI_KEY_DOWN` |

### 13.5 HID-only fallback (also verified)

Before the GATT-server fix was understood, the standard HID path was verified
as a fallback. With the D100H in default HID mode (no heartbeat, no vendor
mode), the Tab5 receives:

| Physical key | HID report |
| --- | --- |
| top-left | Consumer `0xB6` (Scan Previous) |
| top-middle | Consumer `0xCD` (Play/Pause) |
| top-right | Consumer `0xB5` (Scan Next) |
| rotate left | Consumer `0xEA` (Volume Down) |
| rotate right | Consumer `0xE9` (Volume Up) |
| side buttons | Keyboard `Ctrl+C` / `Ctrl+V` / `Ctrl+Y` / `Ctrl+Z` |
| knob click | device-profile dependent (one unit sent `Cmd+Shift+Z`) |

The HID report handles observed on this unit are 50 (keyboard), 61 (consumer),
57 (5-byte report), 29 (1-byte status), and 69 (HID 0xFB, 20-byte vendor-style
frame). The fallback decoder is not used by the final firmware, but the raw
captures are useful if the vendor profile is ever reset incorrectly.
