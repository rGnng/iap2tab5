#!/usr/bin/env bash
# Flash the M5Stack Tab5 on-board ESP32-C6 (ESP-Hosted slave) over the
# internal USB-TTL programming header.
#
# Usage: tools/c6_firmware/flash_c6.sh /dev/ttyUSB0
#
# Disconnect all Tab5 power (USB, battery, and other supplies). With the TTL
# jig unplugged, set it to 3.3 V and connect its 3.3 V output to J1 pin 1.
# The jig supplies C6 power and must also use 3.3 V UART signal levels.
# Keep Tab5 power disconnected throughout the flash; no P4 boot step is needed.
# For adapters without DTR/RTS, use the manual procedure in the root README.
#
# C6_ISP J1: 1=jig 3.3V 2=TXD 3=RXD 4=RST/EN 5=IO9/BOOT 6=GND
# Cross TX/RX. DTR goes to IO9, RTS goes to EN. Leave CTS and 5V disconnected.
#
set -euo pipefail

PORT="${1:-}"
if [[ -z "$PORT" ]]; then
    echo "usage: $0 <c6-serial-port>   (e.g. /dev/ttyUSB0)" >&2
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ESPTOOL="${ESPTOOL:-esptool.py}"
if ! command -v "$ESPTOOL" >/dev/null 2>&1; then
    ESPTOOL="python -m esptool"
fi

for f in bootloader.bin partition-table.bin ota_data_initial.bin network_adapter.bin; do
    if [[ ! -f "$HERE/$f" ]]; then
        echo "missing $HERE/$f; run tools/c6_firmware/build_c6_firmware.sh first" >&2
        exit 1
    fi
done

echo "Flashing ESP-Hosted 2.13.1 C6 firmware to $PORT ..."
$ESPTOOL --chip esp32c6 -p "$PORT" -b 460800 \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
    0x0     "$HERE/bootloader.bin" \
    0x8000  "$HERE/partition-table.bin" \
    0xd000  "$HERE/ota_data_initial.bin" \
    0x10000 "$HERE/network_adapter.bin"

echo "Done. Unplug the jig, remove all jig wiring/boot straps, then restore Tab5 power."
