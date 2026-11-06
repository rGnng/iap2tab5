#!/usr/bin/env bash
# Regenerate the optional VT323 pixel font for the boot sequence.
# Set BOOT_USE_PIXEL_FONT to 1 in app/ui/boot.c to use it.
#
# VT323 is licensed under the SIL Open Font License 1.1. The TTF and license
# are vendored under assets/fonts/. Only printable ASCII is included; the
# boot screen does not render any other glyphs.
#
# Requirements: node/npm (npx lv_font_conv).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FONT="$ROOT/assets/fonts/VT323-Regular.ttf"
OUT="$ROOT/app/ui/fonts/boot_pixel_32.c"

if [[ ! -f "$FONT" ]]; then
    echo "VT323 font not found: $FONT" >&2
    exit 1
fi

npx --yes lv_font_conv \
    --font "$FONT" \
    --size 32 \
    --bpp 1 \
    --format lvgl \
    --lv-font-name "boot_pixel_32" \
    --no-compress \
    -r 0x20-0x7F \
    -o "$OUT"

echo "wrote $OUT"
