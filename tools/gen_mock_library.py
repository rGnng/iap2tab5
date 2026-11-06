#!/usr/bin/env python3
"""Generate the Tab5 mock media library and abstract album covers.

The C firmware only reads the resulting JSON and PNG files; all mock data
lives here so it is easy to change without recompiling C tables.

Usage:
    python3 tools/gen_mock_library.py
    python3 tools/gen_mock_library.py --albums 100 --seed 20261104
"""

from __future__ import annotations

import argparse
import json
import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ARTISTS = [
    "Clara Voss",
    "Harbour Lights",
    "The Paper Kite Ensemble",
    "Ivor Mallory",
    "Redwood Line",
    "Anna Reinholt",
    "The Halberd Quartet",
    "Sable Avenue",
    "Juniper North",
    "Mira Calder",
    "The Quiet Index",
    "Owen Mercer",
    "Fallow Field",
    "Tess Arden",
    "Low Season",
    "The Glass Office",
    "Nadia Voss",
    "Pale Harbour",
    "Emil Rook",
    "The Long Room",
    "Iris Bell",
    "Vesper Lane",
    "Northbound",
    "Cedar & Stone",
]

ADJECTIVES = [
    "Northern", "Slow", "Glass", "Second", "Silent", "Hidden",
    "Distant", "Golden", "Faded", "Electric", "Midnight", "Coastal",
]

NOUNS = [
    "Lines", "Meridian", "Harbour", "Tide", "Records",
    "Hall", "Drift", "Correspondence", "Sessions",
]

TRACK_TITLES = [
    "Prelude", "Northern Lines", "Interlude I", "Meridian",
    "Nocturne", "Second Movement", "Interlude II", "Coda",
    "Reprise", "Epilogue", "Variation", "Finale",
    "Harbour Lights", "Slow Current", "Night Passage", "Standing Orders",
    "Morning Relief", "Crosscurrent", "Field Notes", "Quiet Signal",
    "Afterimage", "Low Tide", "Paper Weather", "Last Light",
]

# Golden Sessions is the long-album test case for the right-hand scrolling
# pane. Keep it at 15 unique tracks so the 9-row viewport has to scroll.
GOLDEN_EXTRA_TRACKS = [
    ("Harbour Lights", 199),
    ("Slow Current", 252),
    ("Night Passage", 225),
    ("Morning Relief", 178),
    ("Crosscurrent", 273),
    ("Field Notes", 187),
    ("Quiet Signal", 301),
]


def build_albums(rng: random.Random, count: int):
    artists = [{"id": 10 + i, "name": name} for i, name in enumerate(ARTISTS)]

    combos = [(a, n) for a in ADJECTIVES for n in NOUNS]
    rng.shuffle(combos)
    if count > len(combos):
        raise SystemExit(f"only {len(combos)} unique title combinations available")

    # Give every artist at least one album, then distribute the rest randomly.
    assignments = [i % len(artists) for i in range(count)]
    rng.shuffle(assignments)

    albums = []
    tracks = []
    track_id = 1000

    for i in range(count):
        title = " ".join(combos[i])
        artist_index = assignments[i]
        artist = artists[artist_index]
        year = rng.randint(1990, 2024)
        track_count = rng.randint(3, 8)
        base = 38 + (i * 17) % 92
        color = (base << 16) | (base << 8) | base

        album_id = 100 + i
        album = {
            "id": album_id,
            "title": title,
            "artist_id": artist["id"],
            "artist": artist["name"],
            "year": year,
            "track_count": track_count,
            "color": color,
            "cover": f"covers/{i:03d}.png",
        }
        albums.append(album)

        titles = rng.sample(TRACK_TITLES, track_count)
        for t in range(track_count):
            tracks.append({
                "id": track_id,
                "album_id": album_id,
                "artist_id": artist["id"],
                "title": titles[t],
                "artist": artist["name"],
                "album": title,
                "duration_sec": rng.randint(150, 420),
                "track_no": t + 1,
                "year": year,
            })
            track_id += 1

    return artists, albums, tracks


PLAYLIST_NAMES = [
    "Late Night",
    "Focus",
    "Weekend Drive",
    "Slow Sunday",
    "Discoveries",
    "All-Time Favorites",
]


def build_playlists(tracks):
    """Deterministic device playlists for the mock library."""
    playlists = []
    n = len(tracks)
    if n == 0:
        return playlists
    for i, name in enumerate(PLAYLIST_NAMES):
        count = 8 + (i % 5)
        start = (i * 53 + 7) % n
        step = 37
        ids = []
        titles = set()
        for j in range(n):
            track = tracks[(start + j * step) % n]
            if track["title"] in titles:
                continue
            titles.add(track["title"])
            ids.append(track["id"])
            if len(ids) >= count:
                break
        playlists.append({
            "id": 2000 + i,
            "name": name,
            "track_ids": ids,
        })
    return playlists


def ensure_golden_sessions_tracks(albums, tracks):
    """Give Golden Sessions 15 unique tracks without disturbing the RNG."""
    album = next((a for a in albums if a["title"] == "Golden Sessions"), None)
    if album is None:
        return

    existing = [t for t in tracks if t["album_id"] == album["id"]]
    used = {t["title"] for t in existing}
    next_no = max((t["track_no"] for t in existing), default=0) + 1
    next_id = max((t["id"] for t in tracks), default=999) + 1

    candidates = list(GOLDEN_EXTRA_TRACKS)
    for i, title in enumerate(TRACK_TITLES):
        candidates.append((title, 180 + (i * 17) % 240))

    for title, duration in candidates:
        if len(existing) >= 15:
            break
        if title in used:
            continue
        track = {
            "id": next_id,
            "album_id": album["id"],
            "artist_id": album["artist_id"],
            "title": title,
            "artist": album["artist"],
            "album": album["title"],
            "duration_sec": duration,
            "track_no": next_no,
            "year": album["year"],
        }
        tracks.append(track)
        existing.append(track)
        used.add(title)
        next_id += 1
        next_no += 1

    album["track_count"] = len(existing)


PALETTES = [
    # (background top, background bottom, accent, secondary accent)
    ((4, 7, 18), (10, 20, 50), (0, 229, 255), (255, 45, 170)),
    ((10, 4, 20), (40, 8, 60), (168, 85, 247), (0, 212, 255)),
    ((5, 10, 20), (8, 40, 45), (0, 255, 200), (80, 140, 255)),
    ((15, 7, 5), (55, 15, 10), (255, 107, 53), (255, 46, 147)),
    ((5, 10, 5), (10, 40, 20), (57, 255, 136), (0, 200, 255)),
    ((12, 5, 5), (55, 10, 20), (255, 59, 92), (255, 176, 0)),
    ((5, 8, 25), (15, 20, 65), (45, 125, 255), (168, 85, 247)),
    ((10, 10, 5), (45, 40, 10), (255, 224, 102), (0, 229, 255)),
    ((5, 5, 15), (30, 10, 45), (255, 0, 128), (0, 255, 255)),
    ((3, 10, 15), (5, 30, 45), (100, 200, 255), (255, 120, 50)),
]


def _mix(a, b, t: float):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def _gradient(size: int, top, bottom) -> Image.Image:
    image = Image.new("RGB", (size, size))
    draw = ImageDraw.Draw(image)
    for y in range(size):
        t = y / (size - 1)
        draw.line([(0, y), (size, y)], fill=_mix(top, bottom, t))
    return image


def _glow(base: Image.Image, layer: Image.Image, radius: int) -> Image.Image:
    glow = layer.filter(ImageFilter.GaussianBlur(radius))
    base.alpha_composite(glow)
    base.alpha_composite(layer)
    return base


def _stars(base: Image.Image, rng: random.Random, size: int) -> None:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for _ in range(rng.randint(45, 120)):
        x = rng.randrange(size)
        y = rng.randrange(size)
        r = 1 if rng.random() < 0.82 else rng.choice((2, 2, 3))
        brightness = rng.randint(150, 255)
        tint = rng.choice(((brightness, brightness, brightness),
                           (180, 230, 255), (255, 210, 240)))
        draw.ellipse([x, y, x + r, y + r], fill=(*tint, rng.randint(90, 220)))
    base.alpha_composite(layer)


def _scanlines(base: Image.Image, size: int) -> None:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for y in range(0, size, 4):
        draw.line([(0, y), (size, y)], fill=(0, 0, 0, 16))
    base.alpha_composite(layer)


def _planet(rng: random.Random, base: Image.Image, accent, accent2, size: int) -> None:
    cx = rng.randint(int(size * 0.30), int(size * 0.70))
    cy = rng.randint(int(size * 0.30), int(size * 0.62))
    radius = rng.randint(int(size * 0.20), int(size * 0.34))

    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([cx - radius * 1.35, cy - radius * 1.35,
                cx + radius * 1.35, cy + radius * 1.35],
               fill=(*accent, 110))
    base = _glow(base, glow, int(size * 0.08))

    planet = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pd = ImageDraw.Draw(planet)
    pd.ellipse([cx - radius, cy - radius, cx + radius, cy + radius],
               fill=(*accent, 235))
    # Offset dark disc makes a crescent-lit planet.
    pd.ellipse([cx - radius + radius * 0.30, cy - radius - radius * 0.18,
                cx + radius + radius * 0.30, cy + radius - radius * 0.18],
               fill=(3, 5, 14, 235))
    base.alpha_composite(planet)

    ring = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    rd = ImageDraw.Draw(ring)
    rd.ellipse([cx - radius * 1.7, cy - radius * 0.38,
                cx + radius * 1.7, cy + radius * 0.38],
               outline=(*accent2, 210), width=max(2, size // 90))
    rd.ellipse([cx - radius * 1.35, cy - radius * 0.22,
                cx + radius * 1.35, cy + radius * 0.22],
               outline=(*accent2, 100), width=1)
    ring = ring.rotate(rng.uniform(-22, 22), resample=Image.Resampling.BICUBIC,
                       center=(cx, cy))
    base.alpha_composite(ring)


def _horizon(rng: random.Random, base: Image.Image, accent, accent2, size: int) -> None:
    horizon = int(size * rng.uniform(0.52, 0.62))
    cx = size // 2
    sun_r = rng.randint(int(size * 0.12), int(size * 0.20))

    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([cx - sun_r * 1.5, horizon - sun_r * 1.5,
                cx + sun_r * 1.5, horizon + sun_r * 1.5],
               fill=(*accent, 120))
    base = _glow(base, glow, int(size * 0.07))

    sun = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sun)
    sd.ellipse([cx - sun_r, horizon - sun_r, cx + sun_r, horizon + sun_r],
               fill=(*accent, 235))
    base.alpha_composite(sun)

    grid = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(grid)
    gd.line([(0, horizon), (size, horizon)], fill=(*accent2, 220), width=2)
    vp_x = rng.randint(int(size * 0.35), int(size * 0.65))
    for i in range(1, 9):
        x = int(size * i / 8)
        gd.line([(vp_x, horizon), (x, size)], fill=(*accent2, 90), width=1)
    for i in range(1, 8):
        y = horizon + int((size - horizon) * (i / 7) ** 1.7)
        gd.line([(0, y), (size, y)], fill=(*accent2, 80), width=1)
    base.alpha_composite(grid)

    # Darken below the horizon so the grid reads as a surface.
    shade = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    sd = ImageDraw.Draw(shade)
    sd.rectangle([0, horizon + 1, size, size], fill=(2, 4, 10, 80))
    base.alpha_composite(shade)


def _shards(rng: random.Random, base: Image.Image, accent, accent2, size: int) -> None:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for _ in range(rng.randint(4, 8)):
        cx = rng.randint(0, size)
        cy = rng.randint(0, size)
        radius = rng.randint(int(size * 0.12), int(size * 0.45))
        points = []
        for n in range(3):
            angle = rng.uniform(0, math.tau) + n * math.tau / 3
            points.append((cx + math.cos(angle) * radius,
                           cy + math.sin(angle) * radius))
        color = accent if rng.random() < 0.55 else accent2
        draw.polygon(points, fill=(*color, rng.randint(45, 130)))
        draw.line(points + [points[0]], fill=(*color, 220),
                  width=rng.choice((1, 2, 3)))
    base = _glow(base, layer, int(size * 0.035))


def _orbits(rng: random.Random, base: Image.Image, accent, accent2, size: int) -> None:
    cx = rng.randint(int(size * 0.30), int(size * 0.70))
    cy = rng.randint(int(size * 0.32), int(size * 0.68))
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for i in range(rng.randint(3, 5)):
        rx = rng.randint(int(size * 0.20), int(size * 0.46))
        ry = rng.randint(int(size * 0.08), int(size * 0.24))
        color = accent if i % 2 == 0 else accent2
        draw.ellipse([cx - rx, cy - ry, cx + rx, cy + ry],
                     outline=(*color, rng.randint(100, 220)),
                     width=rng.choice((1, 2, 2, 3)))
    layer = layer.rotate(rng.uniform(-28, 28), resample=Image.Resampling.BICUBIC,
                         center=(cx, cy))
    core = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cd = ImageDraw.Draw(core)
    r = rng.randint(int(size * 0.025), int(size * 0.06))
    cd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(*accent, 255))
    base = _glow(base, layer, int(size * 0.045))
    base.alpha_composite(core)


def _beams(rng: random.Random, base: Image.Image, accent, accent2, size: int) -> None:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    for _ in range(rng.randint(3, 5)):
        x = rng.randint(-size // 4, size)
        width = rng.randint(int(size * 0.04), int(size * 0.16))
        color = accent if rng.random() < 0.55 else accent2
        draw.rectangle([x, 0, x + width, size], fill=(*color, rng.randint(35, 95)))
        draw.line([(x, 0), (x + width, size)], fill=(*color, 210), width=2)
    for _ in range(rng.randint(1, 3)):
        y = rng.randint(0, size)
        draw.line([(0, y), (size, y - rng.randint(0, size // 3))],
                  fill=(*accent2, 160), width=rng.choice((1, 2, 4)))
    base = _glow(base, layer, int(size * 0.03))


def make_cover(rng: random.Random, path: Path, size: int = 256) -> int:
    """Create a text-free sci-fi cover and return its dominant accent color."""
    top, bottom, accent, accent2 = rng.choice(PALETTES)
    base = _gradient(size, top, bottom).convert("RGBA")
    _stars(base, rng, size)

    style = rng.choice((_planet, _horizon, _shards, _orbits, _beams))
    style(rng, base, accent, accent2, size)

    # A soft tinted vignette keeps the centre bright and the edges dark.
    vignette = Image.new("L", (size, size), 0)
    vd = ImageDraw.Draw(vignette)
    vd.ellipse([-size * 0.15, -size * 0.15, size * 1.15, size * 1.15], fill=255)
    vignette = vignette.filter(ImageFilter.GaussianBlur(int(size * 0.12)))
    dark = Image.new("RGBA", (size, size), (0, 0, 0, 150))
    base = Image.composite(base, Image.alpha_composite(base, dark), vignette)

    _scanlines(base, size)
    base.convert("RGB").save(path, "PNG", optimize=True)
    return (accent[0] << 16) | (accent[1] << 8) | accent[2]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--albums", type=int, default=100)
    parser.add_argument("--seed", type=int, default=20261104)
    parser.add_argument("--out", type=Path,
                        default=Path("assets/mock"))
    args = parser.parse_args()

    rng = random.Random(args.seed)
    out_dir: Path = args.out
    covers_dir = out_dir / "covers"
    covers_dir.mkdir(parents=True, exist_ok=True)

    artists, albums, tracks = build_albums(rng, args.albums)
    ensure_golden_sessions_tracks(albums, tracks)

    for i, album in enumerate(albums):
        album["color"] = make_cover(rng, covers_dir / f"{i:03d}.png")

    library = {
        "generated_by": "tools/gen_mock_library.py",
        "seed": args.seed,
        "artists": artists,
        "albums": albums,
        "tracks": tracks,
        "playlists": build_playlists(tracks),
    }
    library_path = out_dir / "library.json"
    library_path.write_text(json.dumps(library, indent=2) + "\n")

    print(f"wrote {library_path}")
    print(f"  {len(albums)} albums, {len(tracks)} tracks, {len(artists)} artists")
    print(f"  {len(list(covers_dir.glob('*.png')))} covers in {covers_dir}")


if __name__ == "__main__":
    main()
