#!/usr/bin/env bash
# Regenerate the LVGL bootstrap-icons font subset.
#
# Prerequisites: node/npm (npx). Bootstrap Icons is MIT licensed; the TTF and
# license are vendored under assets/fonts/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FONT="$ROOT/assets/fonts/bootstrap-icons.ttf"
OUT_DIR="$ROOT/app/ui/fonts"

# Keep this list in sync with assets/fonts/bootstrap-icons-codepoints.txt.
# Add new icons here when the UI needs them; keeping the subset small matters
# on the Tab5's 16 MB flash.
RANGES=(
  0xF4E7 # phone
  0xF4F7 # plug
  0xF655 # sd-card
  0xF64E # question-lg
  0xF4FF # power
  0xF186 # battery-full
  0xF187 # battery-half
  0xF185 # battery-charging
  0xF188 # battery
  0xF911 # battery-low
  0xF4F4 # play-fill
  0xF4F5 # play
  0xF4C3 # pause-fill
  0xF563 # skip-start-fill
  0xF557 # skip-end-fill
  0xF592 # stop-fill
  0xF610 # volume-up-fill
  0xF60A # volume-down-fill
  0xF60C # volume-mute-fill
  0xF544 # shuffle
  0xF813 # repeat
  0xF812 # repeat-1
  0xF49F # music-note-list
  0xF2FF # disc
  0xF4E1 # person
  0xF3E5 # gear
  0xF479 # list
  0xF425 # house
  0xF12F # arrow-left
  0xF286 # chevron-up
  0xF282 # chevron-down
  0xF633 # check-lg
  0xF659 # x-lg
  0xF61C # wifi
  0xF682 # bluetooth
  0xF61B # wifi-off
  0xF586 # star-fill
  0xF588 # star
  0xF341 # eye
  0xF302 # display
  0xF2CA # collection-play-fill
  0xF1D2 # brightness-high
  0xF293 # clock
  0xF49E # music-note-beamed
  0xF287 # circle-fill
)

ARGS=()
for r in "${RANGES[@]}"; do
  ARGS+=(-r "$r")
done

for SIZE in 24 32 40 48; do
  OUT="$OUT_DIR/bootstrap_icons_${SIZE}.c"
  npx --yes lv_font_conv \
    --font "$FONT" \
    --size "$SIZE" --bpp 4 --format lvgl \
    --lv-font-name "bootstrap_icons_${SIZE}" \
    --no-compress \
    "${ARGS[@]}" \
    -o "$OUT"
  echo "wrote $OUT"
done
