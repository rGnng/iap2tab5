#!/usr/bin/env python3
"""Add the Simple Line Icons disc SVG to a build copy of Bootstrap Icons."""
from pathlib import Path
import xml.etree.ElementTree as ET

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.svgLib.path import parse_path
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]


def main():
    # Preserve the vendored glyph boxes: recomputing them changes how the
    # converter rasterizes existing icons, even though their paths are unchanged.
    font = TTFont(ROOT / 'assets/fonts/bootstrap-icons.ttf',
                  recalcBBoxes=False, recalcTimestamp=False)
    cmap = font.getBestCmap()
    codepoint, name = 0xE000, 'tab5-albums'
    if codepoint in cmap:
        raise ValueError('Custom Albums codepoint is already occupied')

    # Match the original Disc's size, advance and baseline in the menubar.
    reference = cmap[0xF2FF]
    glyphs = font.getGlyphSet()
    bounds = BoundsPen(glyphs)
    glyphs[reference].draw(bounds)
    left, bottom, right, top = bounds.bounds
    svg = ET.parse(ROOT / 'assets/icons/albums.svg').getroot()
    x, y, width, height = map(float, svg.attrib['viewBox'].split())
    sx, sy = (right - left) / width, (top - bottom) / height
    pen = TTGlyphPen(None)
    curves = Cu2QuPen(pen, max_err=font['head'].unitsPerEm / 4096)
    transformed = TransformPen(curves, (sx, 0, 0, -sy, left - x * sx, top + y * sy))
    for path in svg.findall('{http://www.w3.org/2000/svg}path'):
        if path.get('fill') != 'none':
            parse_path(path.attrib['d'], transformed)

    font.setGlyphOrder(font.getGlyphOrder() + [name])
    glyph = pen.glyph()
    glyph.recalcBounds(font['glyf'])
    font['glyf'][name] = glyph
    font['hmtx'][name] = font['hmtx'][reference]
    for table in font['cmap'].tables:
        if table.isUnicode():
            table.cmap[codepoint] = name
    output = ROOT / '.deps/tab5-icons.ttf'
    output.parent.mkdir(exist_ok=True)
    font.save(output)
    print(f'wrote {output.relative_to(ROOT)}')


if __name__ == '__main__':
    main()
