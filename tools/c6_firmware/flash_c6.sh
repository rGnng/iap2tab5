#!/usr/bin/env bash
# Flash the M5Stack Tab5 on-board ESP32-C6 (ESP-Hosted slave) over the
# internal USB-TTL programming header.
#
# Usage: tools/c6_firmware/flash_c6.sh /dev/ttyUSB0
#
# The Tab5's P4 drives the C6 EN pin through GPIO15.  Put the P4 into
# download mode first so it does not reset the C6 mid-flash: with USB-C
# connected, hold the Tab5 power button until the green LED flashes, then
# release it. Leave the P4 stopped while this script programs the C6.
# For adapters without DTR/RTS, use the manual procedure in the root README.
#
# C6_ISP J1: 1=3V3 (leave open) 2=TXD 3=RXD 4=RST/EN 5=IO9/BOOT 6=GND
# DTR goes to IO9, RTS goes to EN.  CTS is unused.
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

echo "Done. Power-cycle the Tab5 (or reset the P4) to boot the new C6 firmware."
