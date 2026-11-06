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
silkscreen is inside the cover). Connect a **3.3 V USB-UART adapter**:

| Adapter | C6 pad |
| --- | --- |
| TX | RXD |
| RX | TXD |
| DTR | IO0 |
| RTS | EN |
| GND | GND |
| 3V3 | **leave open** (Tab5 supplies C6 power) |

Do not connect the adapter's 3V3 while the Tab5 is powered.

## Flashing

1. Power the Tab5 over USB-C.
2. Stop the P4 application from driving the C6 reset line. Put the P4 into
   bootloader mode and leave it there:

   ```bash
   # The P4 USB port is the usual /dev/ttyACM0
   esptool.py -p /dev/ttyACM0 --before default_reset --after no_reset run
   ```

   On Tab5 this is the equivalent of holding BOOT and tapping RESET. The C6
   stays powered, but GPIO15 no longer resets it mid-flash.
3. Flash the C6:

   ```bash
   tools/c6_firmware/flash_c6.sh /dev/ttyUSB0
   ```

   If the adapter does not support DTR/RTS auto-reset, hold the C6 `IO0` pad
   low, tap `EN` to GND, release `IO0`, then run the command.
4. Power-cycle the Tab5 (or reset the P4). The P4 boot log should now show
   `ESP-Hosted C6 FW: 2.13.1` (or at least a 2.13.x version) and no
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
