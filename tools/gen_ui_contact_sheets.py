#!/usr/bin/env python3
"""Render the README's Gray and Aqua UI contact sheets from mock data.

Build tab5_sim first and install the optional renderer with `pip install cairosvg`.
Run from any directory: python3 tools/gen_ui_contact_sheets.py --sim build/tab5_sim
Screenshots remain at their native 1280 x 720 size in a labelled SVG layout,
which CairoSVG renders to a standalone PNG for GitHub and downloads.
"""

from __future__ import annotations

import argparse
import base64
from concurrent.futures import ThreadPoolExecutor, as_completed
from html import escape
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCREENS = (
    ("Now Playing", ("--screen", "1")),
    ("Up Next", ("--screen", "1", "--keys", "pageup")),
    ("Albums", ("--screen", "2")),
    ("Artists", ("--screen", "3")),
    ("Songs", ("--screen", "4")),
    ("Playlists", ("--screen", "5")),
    # Navigate from Playlists: internal screen 6 is the Bluetooth subpage.
    ("Settings", ("--screen", "5", "--keys", "next,down,down,down")),
    ("Library Sync", ("--afc", "ready")),
)
THEMES = (
    (0, "gray", "GRAY / DEFAULT", "#f1f1ef", "#191919", "#626260", "#303030"),
    (1, "aqua", "AQUA", "#f1f6fb", "#102f43", "#49677d", "#1269b4"),
)
SCREEN_W, SCREEN_H = 1280, 720
MARGIN, GUTTER, HEADER, CAPTION, ROW_GAP = 48, 48, 208, 48, 48
SHEET_W = 2 * MARGIN + 2 * SCREEN_W + GUTTER
SHEET_H = HEADER + 4 * (CAPTION + SCREEN_H) + 3 * ROW_GAP + 96


def capture(sim: Path, theme: int, args: tuple[str, ...], output: Path) -> None:
    command = [str(sim), "--headless", "--menubar", "left", "--theme", str(theme),
               *args, "--screenshot", str(output), "--wait", "1000"]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                            timeout=30)
    if result.returncode:
        raise RuntimeError(f"Simulator failed for {output.name}:\n"
                           f"{result.stdout}\n{result.stderr}")
    png = output.read_bytes()
    if png[:8] != b"\x89PNG\r\n\x1a\n" or struct.unpack(">II", png[16:24]) != (SCREEN_W, SCREEN_H):
        raise RuntimeError(f"Expected a {SCREEN_W} x {SCREEN_H} PNG: {output}")


def sheet_svg(theme: tuple, captures: Path) -> str:
    _, slug, label, background, ink, muted, accent = theme
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{SHEET_W}" height="{SHEET_H}" viewBox="0 0 {SHEET_W} {SHEET_H}">',
        f'<title>iAP2 Tab5 — {escape(label)} interface contact sheet</title>',
        '<desc>Eight desktop simulator screens using fictional media and mock device data.</desc>',
        f'<rect width="100%" height="100%" fill="{background}"/>',
        f'<g font-family="DejaVu Sans, sans-serif" fill="{ink}">',
        f'<text x="{MARGIN}" y="98" font-size="60" font-weight="bold">iAP2 Tab5</text>',
        f'<text x="{MARGIN}" y="150" font-size="28" fill="{muted}">Interface contact sheet</text>',
        f'<rect x="{SHEET_W - MARGIN - 304}" y="50" width="304" height="52" fill="{accent}"/>',
        f'<text x="{SHEET_W - MARGIN - 280}" y="85" font-size="24" font-weight="bold" '
        f'fill="#ffffff">{escape(label)}</text>',
        f'<text x="{SHEET_W - MARGIN}" y="150" text-anchor="end" font-size="24" '
        f'fill="{muted}">1280 × 720 · Mock data · 8 views</text>',
        f'<path d="M {MARGIN} 178 H {SHEET_W - MARGIN}" stroke="{muted}" stroke-opacity="0.25"/>',
    ]
    for index, (title, _) in enumerate(SCREENS):
        x = MARGIN + (index % 2) * (SCREEN_W + GUTTER)
        y = HEADER + (index // 2) * (CAPTION + SCREEN_H + ROW_GAP)
        png = (captures / f"{slug}-{index}.png").read_bytes()
        encoded = base64.b64encode(png).decode("ascii")
        parts.extend((
            f'<text x="{x}" y="{y + 30}" font-size="22" fill="{muted}">{index + 1:02}</text>',
            f'<text x="{x + 56}" y="{y + 30}" font-size="30" font-weight="bold">{escape(title)}</text>',
            f'<rect x="{x - 1}" y="{y + CAPTION - 1}" width="{SCREEN_W + 2}" '
            f'height="{SCREEN_H + 2}" fill="none" stroke="{muted}" stroke-opacity="0.3"/>',
            f'<image x="{x}" y="{y + CAPTION}" width="{SCREEN_W}" height="{SCREEN_H}" '
            f'xlink:href="data:image/png;base64,{encoded}"/>',
        ))
    parts.extend((
        f'<text x="{MARGIN}" y="{SHEET_H - 38}" font-size="22" fill="{muted}">'
        'Rendered from the desktop simulator · Fictional artists, albums and artwork</text>',
        '</g></svg>',
    ))
    return "\n".join(parts)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, default=ROOT / "build/tab5_sim",
                        help="Built desktop simulator (default: build/tab5_sim)")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "docs/previews",
                        help="Contact sheet destination (default: docs/previews)")
    args = parser.parse_args()
    sim = args.sim.resolve()
    if not sim.is_file():
        parser.error(f"Build tab5_sim first; executable not found: {sim}")
    try:
        import cairosvg
    except ImportError:
        parser.error("Install the optional renderer: python3 -m pip install cairosvg")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="iap2tab5-contact-") as temporary:
        captures = Path(temporary)
        with ThreadPoolExecutor(max_workers=4) as pool:
            jobs = {
                pool.submit(capture, sim, theme_id, options,
                            captures / f"{slug}-{index}.png"): f"{slug}: {title}"
                for theme_id, slug, *_ in THEMES
                for index, (title, options) in enumerate(SCREENS)
            }
            for job in as_completed(jobs):
                job.result()
                print(f"Captured {jobs[job]}", flush=True)
        for theme in THEMES:
            output = args.output_dir / f"ui-{theme[1]}.png"
            cairosvg.svg2png(bytestring=sheet_svg(theme, captures).encode("utf-8"),
                            write_to=str(output))
            print(f"Wrote {output} ({SHEET_W} × {SHEET_H})", flush=True)


if __name__ == "__main__":
    main()
