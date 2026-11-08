# Tab5 ESP32-C6 firmware (ESP-Hosted 2.13.1)

The Tab5's ESP32-C6 runs Espressif's ESP-Hosted **slave** firmware. The ESP32-P4
application is the **host** side. The factory Tab5 ships with M5Stack's
`ESP32C6-WiFi-SDIO-Interface-V1.4.1`, which speaks the ESP-Hosted **1.4.x**
protocol. This project uses `espressif/esp_hosted` **2.13.1**, so the C6 must be
updated to the matching 2.13.1 slave before Bluetooth and Wi-Fi are reliable:

- The factory 1.4.1 firmware does not implement the 2.x `bt_controller_init` /
  `bt_controller_enable` RPCs. The host falls back to raw VHCI, which works for
  basic HCI but leaves the controller lifecycle and coexistence unmanaged.
- AP+STA operation and BLE scanning/connection are not reliable on the 1.4.x
  protocol with a 2.13.x host.
- The 2.4 GHz coexistence between Wi-Fi and BLE is owned by the slave firmware;
  matching host and slave versions is required for the D100H BLE remote.

## Why a wired flash is required

ESP-Hosted added slave OTA over SDIO/SPI/UART in **2.6.0**. The factory firmware
is **1.4.1**, so there is no in-band update path from the P4. The C6 must be
flashed once through its internal programming header. After this first update,
future 2.x C6 firmware can be updated over SDIO using the ESP-Hosted OTA APIs.

## Firmware in this directory

Built from the `espressif/esp_hosted` **2.13.1** slave example with:

- `CONFIG_ESP32P4_TAB5_C6_BOARD=y`
- 4-bit SDIO slave interface (C6 pins: CMD 18, CLK 19, D0 20, D1 21, D2 22, D3 23)
- Bluetooth controller-only (`CONFIG_BT_ENABLED=y`, `CONFIG_BT_CONTROLLER_ONLY=y`)
- 4 MB flash, two OTA app slots

| Offset | Image |
| --- | --- |
| `0x0` | `bootloader.bin` |
| `0x8000` | `partition-table.bin` |
| `0xd000` | `ota_data_initial.bin` |
| `0x10000` | `network_adapter.bin` |

The SHA-256 sums are in `SHA256SUMS`.

## Hardware connection

Remove the Tab5 back cover. The C6 programming header is on the PCB (the
silkscreen is inside the cover). It is the 6-pin `J1` / `C6_ISP` header:

| J1 pin | Net | Connect to |
| --- | --- | --- |
| 1 | `WLAN_3.3V` | **leave open** (Tab5 supplies C6 power) |
| 2 | `RF_C6_TXD` | adapter **RX** |
| 3 | `RF_C6_RXD` | adapter **TX** |
| 4 | `RF_C6_RST` / EN | adapter **RTS** |
| 5 | `RF_C6_IO9` / BOOT | adapter **DTR** (or manual boot strap) |
| 6 | GND | adapter **GND** |

The boot strap on the Tab5 C6 is **IO9**, not IO0. The adapter's CTS pin is not
used. Do not connect the adapter's 3V3 while the Tab5 is powered.

If the adapter has only RTS/CTS and no DTR, leave CTS open, use RTS for the
reset line (or reset manually), and pull **IO9** to GND while resetting to enter
download mode. There is no P4 UART path to the C6 on the Tab5; the `C6_ISP`
header is the only programming interface.

## Flashing

The root README contains the [complete C6 flashing procedure](../../README.md#flash-the-esp32-c6-wi-fible-firmware),
including tool setup, checksum verification, wiring, and a full manual-reset
command for adapters without DTR/RTS. The short procedure below assumes the
ESP-IDF environment is active and the adapter supports automatic reset.

1. Power the Tab5 over USB-C.
2. Stop the P4 application from driving the C6 reset line. Put the P4 into
   download mode by holding the Tab5 power button until the green LED flashes,
   then releasing it. Leave the P4 stopped throughout the C6 flash so its
   application cannot reset the C6 through GPIO15.
3. From the repository root, flash the C6 through the **UART adapter's port**:

   ```bash
   bash tools/c6_firmware/flash_c6.sh /dev/ttyUSB0
   ```

   The helper accepts only the port argument. For manual boot/reset, use the
   direct `python -m esptool` command in the root README instead of appending
   reset flags to this script. The C6 boot strap is `IO9`.
4. After all images are written and verified, remove any manual boot/reset
   straps, turn off the Tab5, disconnect the UART adapter, and turn the Tab5
   on normally. The P4 boot log should now show `C6 firmware 2.13.1` and no
   `Version mismatch` warning. `ble_remote` should no longer log
   `BT controller init RPC failed`.

## Rebuilding

After the host project has fetched `espressif/esp_hosted` 2.13.1
(`idf.py reconfigure` in `firmware/`), run:

```bash
source /path/to/esp-idf/export.sh
tools/c6_firmware/build_c6_firmware.sh
```

The script copies the slave project to `tools/c6_firmware/build/`, appends the
Tab5 board selection, builds for `esp32c6`, and copies the four images here.

## Rollback

M5Stack publishes the factory C6 image through M5Burner ("Tab5 ESP32-C6 Wi-Fi
Module Restore Factory Firmware"). Rolling back to 1.4.1 restores M5Stack's
demo stack but reintroduces the host/slave mismatch; this project's BLE remote
needs 2.x on both sides.
