#!/usr/bin/env bash
# Regenerate the Inter LVGL font subsets used by the UI.
#
# Inter is an open-source (SIL OFL 1.1) sans-serif that is visually close to
# SF Pro. Regular text uses the default proportional figures; the two clock /
# timer subsets enable Inter's `tnum` feature so digits stay equal-width.
#
# Requirements: python3-fonttools, node/npx (lv_font_conv).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/assets/fonts/Inter-Variable.ttf"
OUT_DIR="$ROOT/app/ui/fonts"
TMP_DIR="${TMPDIR:-/tmp}/iap2tab5-inter-fonts"

if [[ ! -f "$SRC" ]]; then
    echo "Inter variable font not found: $SRC" >&2
    exit 1
fi

mkdir -p "$TMP_DIR"

python3 - "$SRC" "$TMP_DIR" <<'PY'
import os
import sys

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

src, out_dir = sys.argv[1], sys.argv[2]
sizes = [14, 16, 18, 20, 22, 24, 28, 32, 36, 40]


def tabular_map(font):
    gsub = font["GSUB"].table
    mapping = {}
    for record in gsub.FeatureList.FeatureRecord:
        if record.FeatureTag != "tnum":
            continue
        for lookup_index in record.Feature.LookupListIndex:
            lookup = gsub.LookupList.Lookup[lookup_index]
            for subtable in lookup.SubTable:
                mapping.update(getattr(subtable, "mapping", {}))
    return mapping


def instance(size, tabular):
    font = TTFont(src)
    opsz = min(max(size, 14), 32)
    instantiateVariableFont(font, {"wght": 400, "opsz": opsz}, inplace=True)
    if tabular:
        mapping = tabular_map(font)
        for table in font["cmap"].tables:
            for code, name in list(table.cmap.items()):
                if name in mapping:
                    table.cmap[code] = mapping[name]
    path = os.path.join(out_dir, f"Inter-{'Tabular' if tabular else 'Regular'}-{size}.ttf")
    font.save(path)
    return path


for size in sizes:
    instance(size, tabular=False)
for size in (18, 20, 24, 28, 36):
    instance(size, tabular=True)
PY

for SIZE in 14 16 18 20 22 24 28 32 36 40; do
    npx --yes lv_font_conv \
        --font "$TMP_DIR/Inter-Regular-${SIZE}.ttf" \
        --size "$SIZE" \
        --bpp 4 \
        --format lvgl \
        --lv-font-name "inter_${SIZE}" \
        --no-compress \
        -r 0x20-0x7F -r 0x2014 \
        -o "$OUT_DIR/inter_${SIZE}.c"
done

for SIZE in 18 20 24 28 36; do
    npx --yes lv_font_conv \
        --font "$TMP_DIR/Inter-Tabular-${SIZE}.ttf" \
        --size "$SIZE" \
        --bpp 4 \
        --format lvgl \
        --lv-font-name "inter_tabular_${SIZE}" \
        --no-compress \
        -r 0x20-0x7F -r 0x2014 \
        -o "$OUT_DIR/inter_tabular_${SIZE}.c"
done
