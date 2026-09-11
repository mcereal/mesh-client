#!/usr/bin/env python3
"""Synthetic raster map tiles for devtools/tile_bench, in the three layouts it compares.

Stdlib only, like scripts/frames.py, so it runs on the host with nothing installed.

The tiles are not a map of anywhere. They are drawn to cost what a real raster tile costs to
read and decode: an 8-bit palette PNG of land-use polygons, buildings, cased roads with
anti-aliased edges and label-shaped glyph noise, which is the shape of a Carto-style OSM tile and
lands in the same size range. The RGB set shades the same drawing with a smooth relief field and
stores it as 24-bit PNG, standing in for a hillshaded style - the expensive end.

    gen_tiles.py -o build/tile_bench/tiles          # palette set, all three layouts
    gen_tiles.py -o build/tile_bench/tiles --rgb    # plus the RGB set, pack layout only

Layout of a set directory, which is the contract tile_bench reads:

    manifest.txt    zoom=16 x0=.. y0=.. width=.. height=.. minzoom=..
    xyz/z/x/y.png   one file per tile
    tiles.mbtiles   MBTiles 1.3: SQLite, TMS row order
    pack.mctp       "MCTPACK1", u32 count, u32 0, then count sorted 24-byte entries
                    (u8 z, 3 pad, u32 x, u32 y, u32 length, u64 offset), then the tile bytes
"""

import argparse
import math
import multiprocessing
import os
import random
import sqlite3
import struct
import sys
import zlib

TILE = 256
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PACK_HEADER = struct.Struct("<8sII")
PACK_ENTRY = struct.Struct("<BxxxIIIQ")
PACK_MAGIC = b"MCTPACK1"
FAT_CLUSTER = 32768  # measured on the Brick's card: 64 sectors of 512 bytes

# Where the pyramid sits. Aligned so every zoom down to MIN_ZOOM covers whole tiles; nothing
# about the benchmark depends on it being anywhere in particular.
MAX_ZOOM = 16
MIN_ZOOM = 12
X0, Y0 = 34000, 22000

LAND = (242, 239, 233)
AREAS = {
    "residential": (224, 223, 223),
    "park": (200, 250, 204),
    "forest": (173, 209, 158),
    "water": (170, 211, 223),
}
BUILDING = (217, 208, 201)
BUILDING_EDGE = (196, 182, 171)
LABEL = (51, 51, 51)
HALO = (254, 254, 254)
LABEL_AA_SHADES = 6
# name, fill, casing, width at MAX_ZOOM
ROADS = [
    ("motorway", (232, 146, 162), (220, 42, 103), 12),
    ("primary", (252, 214, 164), (161, 120, 41), 9),
    ("secondary", (247, 250, 191), (112, 122, 10), 7),
    ("minor", (255, 255, 255), (190, 188, 182), 5),
]


def _blend(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def _build_palette():
    colours = [LAND]
    index = {}

    def add(name, rgb):
        index[name] = len(colours)
        colours.append(rgb)

    for name, rgb in AREAS.items():
        add(name, rgb)
        add(name + "_edge", _blend(rgb, LAND, 0.5))
    add("building", BUILDING)
    add("building_edge", BUILDING_EDGE)
    add("label", LABEL)
    add("halo", HALO)
    for i in range(LABEL_AA_SHADES):
        add(f"label_aa{i}", _blend(LABEL, HALO, (i + 1) / (LABEL_AA_SHADES + 1)))
    for name, fill, casing, _ in ROADS:
        add(name, fill)
        add(name + "_casing", casing)
        add(name + "_aa", _blend(casing, LAND, 0.55))
    return colours, index


PALETTE, IDX = _build_palette()


def fill_poly(img, pts, c):
    """Even-odd scanline fill, one slice assignment per span - fast enough in pure Python."""
    ys = [p[1] for p in pts]
    y0 = max(0, int(min(ys)))
    y1 = min(TILE - 1, int(max(ys)))
    n = len(pts)
    edges = [(pts[i], pts[(i + 1) % n]) for i in range(n)]
    cb = bytes([c])
    for y in range(y0, y1 + 1):
        yc = y + 0.5
        xs = []
        for (ax, ay), (bx, by) in edges:
            if (ay <= yc) != (by <= yc):
                xs.append(ax + (yc - ay) * (bx - ax) / (by - ay))
        xs.sort()
        row = y * TILE
        for i in range(0, len(xs) - 1, 2):
            xa = max(0, int(xs[i] + 0.5))
            xb = min(TILE, int(xs[i + 1] + 0.5))
            if xb > xa:
                img[row + xa : row + xb] = cb * (xb - xa)


def stroke(img, x0, y0, x1, y1, w, c):
    dx, dy = x1 - x0, y1 - y0
    length = math.hypot(dx, dy) or 1.0
    nx, ny = -dy / length * w / 2, dx / length * w / 2
    fill_poly(img, [(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], c)


def outline(img, pts, w, c):
    for i in range(len(pts)):
        (ax, ay), (bx, by) = pts[i], pts[(i + 1) % len(pts)]
        stroke(img, ax, ay, bx, by, w, c)


def plot(img, px, py, c):
    if 0 <= px < TILE and 0 <= py < TILE:
        img[py * TILE + px] = c


def draw_tile(z, x, y):
    rng = random.Random((z << 40) ^ (x << 20) ^ y)
    img = bytearray(TILE * TILE)  # index 0 is land
    shrink = MAX_ZOOM - z

    for _ in range(rng.randint(2, 6)):
        name = rng.choice(list(AREAS))
        cx, cy = rng.uniform(-64, 320), rng.uniform(-64, 320)
        r = rng.uniform(40, 160)
        k = rng.randint(5, 9)
        angles = sorted(rng.uniform(0, 2 * math.pi) for _ in range(k))
        pts = [(cx + math.cos(a) * r * rng.uniform(0.6, 1.0), cy + math.sin(a) * r * rng.uniform(0.6, 1.0)) for a in angles]
        fill_poly(img, pts, IDX[name])
        outline(img, pts, 1.2, IDX[name + "_edge"])

    if shrink <= 1:
        for _ in range(rng.randint(40, 120) >> shrink):
            cx, cy = rng.uniform(0, TILE), rng.uniform(0, TILE)
            hw, hh = rng.uniform(3, 10), rng.uniform(3, 10)
            t = rng.uniform(0, math.pi)
            ct, st = math.cos(t), math.sin(t)
            pts = [(cx + ct * px - st * py, cy + st * px + ct * py) for px, py in ((-hw, -hh), (hw, -hh), (hw, hh), (-hw, hh))]
            fill_poly(img, pts, IDX["building"])
            outline(img, pts, 1.0, IDX["building_edge"])

    roads = []
    for _ in range(int(rng.randint(4, 10) * (1 + shrink * 0.5))):
        name, _, _, width = rng.choice(ROADS)
        w = max(2.0, width * (0.7**shrink))
        pts = [(rng.uniform(-20, 276), rng.choice((-10.0, 266.0)))]
        for _ in range(rng.randint(2, 4)):
            pts.append((rng.uniform(-20, 276), rng.uniform(-20, 276)))
        roads.append((name, w, pts))
    for name, w, pts in roads:  # every casing first, so fills join over them at crossings
        for a, b in zip(pts, pts[1:]):
            stroke(img, a[0], a[1], b[0], b[1], w + 1.5, IDX[name + "_aa"])
            stroke(img, a[0], a[1], b[0], b[1], w, IDX[name + "_casing"])
    for name, w, pts in roads:
        for a, b in zip(pts, pts[1:]):
            stroke(img, a[0], a[1], b[0], b[1], max(1.0, w - 2.5), IDX[name])

    # A rendered tile's labels and point symbols are anti-aliased, so they come out of palette
    # quantisation as a scatter of in-between shades - which is most of what a real tile's bytes
    # are spent on, and what a flat synthetic drawing lacks.
    shades = [IDX["label"]] + [IDX[f"label_aa{i}"] for i in range(LABEL_AA_SHADES)]
    for _ in range(rng.randint(8, 18) >> max(0, shrink - 2)):
        lx, ly = rng.randint(-20, 236), rng.randint(0, 248)
        glyphs = [[(gx, gy) for gy in range(7) for gx in range(5) if rng.random() < 0.45] for _ in range(rng.randint(4, 12))]
        for pass_ in ("halo", "label"):
            for gi, pixels in enumerate(glyphs):
                ox = lx + gi * 6
                for gx, gy in pixels:
                    if pass_ == "halo":
                        for ddx in (-1, 0, 1):
                            for ddy in (-1, 0, 1):
                                plot(img, ox + gx + ddx, ly + gy + ddy, IDX["halo"])
                    else:
                        plot(img, ox + gx, ly + gy, rng.choice(shades))
    for _ in range(rng.randint(20, 60)):
        sx, sy = rng.randint(0, TILE - 4), rng.randint(0, TILE - 4)
        for _ in range(rng.randint(4, 10)):
            plot(img, sx + rng.randint(0, 3), sy + rng.randint(0, 3), rng.choice(shades))
    return img


def _chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png_palette(img):
    plte = b"".join(bytes(c) for c in PALETTE)
    raw = b"".join(b"\x00" + bytes(img[y * TILE : (y + 1) * TILE]) for y in range(TILE))
    return (
        PNG_SIGNATURE
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", TILE, TILE, 8, 3, 0, 0, 0))
        + _chunk(b"PLTE", plte)
        + _chunk(b"IDAT", zlib.compress(raw, 9))
        + _chunk(b"IEND", b"")
    )


SHADES = 16
ROW_BYTES = TILE * 3
_HIGH = int.from_bytes(b"\x80" * ROW_BYTES, "big")
_LOW = int.from_bytes(b"\x7f" * ROW_BYTES, "big")


def _shade_tables():
    padded = PALETTE + [(0, 0, 0)] * (256 - len(PALETTE))
    tables = []
    for s in range(SHADES):
        f = 0.78 + 0.03 * s
        tables.append(tuple(bytes(min(255, round(c[ch] * f)) for c in padded) for ch in range(3)))
    return tables


SHADE_TABLES = _shade_tables()


def up_filter(cur, prev):
    """PNG filter 2 over a whole row at once: bytewise (cur - prev) mod 256 as one SWAR subtract."""
    return ((cur | _HIGH) - (prev & _LOW)) ^ ((cur ^ ~prev) & _HIGH)


def png_rgb(img, z, x, y):
    rng = random.Random((z << 41) ^ (x << 21) ^ y ^ 0x5EED)
    a, b, c = rng.uniform(0.02, 0.06), rng.uniform(0.02, 0.06), rng.uniform(0.01, 0.04)
    p1, p2, p3 = (rng.uniform(0, 6.3) for _ in range(3))
    shade = [
        [
            max(0, min(SHADES - 1, int((math.sin(a * (bx * 16 + 8) + p1) + math.sin(b * (by * 16 + 8) + p2) + math.sin(c * (bx + by) * 16 + p3) + 3) / 6 * SHADES)))
            for bx in range(16)
        ]
        for by in range(16)
    ]
    out = bytearray()
    prev = 0
    for yy in range(TILE):
        row = bytearray(ROW_BYTES)
        base = yy * TILE
        levels = shade[yy >> 4]
        for bx in range(16):
            tr, tg, tb = SHADE_TABLES[levels[bx]]
            seg = bytes(img[base + bx * 16 : base + bx * 16 + 16])
            o = bx * 48
            row[o : o + 48 : 3] = seg.translate(tr)
            row[o + 1 : o + 48 : 3] = seg.translate(tg)
            row[o + 2 : o + 48 : 3] = seg.translate(tb)
        cur = int.from_bytes(row, "big")
        out += b"\x02" + up_filter(cur, prev).to_bytes(ROW_BYTES, "big")
        prev = cur
    return (
        PNG_SIGNATURE
        + _chunk(b"IHDR", struct.pack(">IIBBBBB", TILE, TILE, 8, 2, 0, 0, 0))
        + _chunk(b"IDAT", zlib.compress(bytes(out), 9))
        + _chunk(b"IEND", b"")
    )


def _render(job):
    z, x, y, rgb = job
    img = draw_tile(z, x, y)
    return (z, x, y), (png_rgb(img, z, x, y) if rgb else png_palette(img))


def keys_for(span, minzoom):
    keys = []
    for z in range(minzoom, MAX_ZOOM + 1):
        s = MAX_ZOOM - z
        for x in range(X0 >> s, (X0 + span) >> s):
            for y in range(Y0 >> s, (Y0 + span) >> s):
                keys.append((z, x, y))
    return keys


def write_manifest(root, span, minzoom):
    with open(os.path.join(root, "manifest.txt"), "w") as f:
        f.write(f"zoom={MAX_ZOOM} x0={X0} y0={Y0} width={span} height={span} minzoom={minzoom}\n")


def write_xyz(root, tiles):
    for (z, x, y), data in tiles.items():
        d = os.path.join(root, "xyz", str(z), str(x))
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, f"{y}.png"), "wb") as f:
            f.write(data)


def write_pack(path, tiles):
    order = sorted(tiles)
    offset = PACK_HEADER.size + PACK_ENTRY.size * len(order)
    with open(path, "wb") as f:
        f.write(PACK_HEADER.pack(PACK_MAGIC, len(order), 0))
        for key in order:
            f.write(PACK_ENTRY.pack(key[0], key[1], key[2], len(tiles[key]), offset))
            offset += len(tiles[key])
        for key in order:
            f.write(tiles[key])


def write_mbtiles(path, tiles, span, minzoom):
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.executescript(
        "CREATE TABLE metadata (name text, value text);"
        "CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob);"
        "CREATE UNIQUE INDEX tile_index on tiles (zoom_level, tile_column, tile_row);"
    )
    db.executemany(
        "INSERT INTO metadata VALUES (?, ?)",
        [("name", "tile_bench synthetic"), ("format", "png"), ("type", "baselayer"), ("version", "1"), ("minzoom", str(minzoom)), ("maxzoom", str(MAX_ZOOM))],
    )
    db.executemany(
        "INSERT INTO tiles VALUES (?, ?, ?, ?)",
        [(z, x, (1 << z) - 1 - y, sqlite3.Binary(tiles[(z, x, y)])) for z, x, y in sorted(tiles)],
    )
    db.commit()
    db.execute("VACUUM")
    db.close()


def describe(name, tiles):
    sizes = sorted(len(v) for v in tiles.values())
    n = len(sizes)
    total = sum(sizes)
    on_fat = sum(-(-s // FAT_CLUSTER) * FAT_CLUSTER for s in sizes)
    print(
        f"{name}: {n} tiles, {total / 2**20:.1f} MiB; per tile p50 {sizes[n // 2] / 1024:.1f} KiB, "
        f"p90 {sizes[n * 9 // 10] / 1024:.1f} KiB, max {sizes[-1] / 1024:.1f} KiB; "
        f"as files on 32 KiB FAT clusters {on_fat / 2**20:.1f} MiB"
    )


def build_set(root, span, minzoom, rgb, layouts, pool):
    os.makedirs(root, exist_ok=True)
    jobs = [(z, x, y, rgb) for z, x, y in keys_for(span, minzoom)]
    tiles = dict(pool.imap_unordered(_render, jobs, chunksize=8))
    describe(os.path.basename(root), tiles)
    write_manifest(root, span, minzoom)
    if "xyz" in layouts:
        write_xyz(root, tiles)
    if "pack" in layouts:
        write_pack(os.path.join(root, "pack.mctp"), tiles)
    if "mbtiles" in layouts:
        write_mbtiles(os.path.join(root, "tiles.mbtiles"), tiles, span, minzoom)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", required=True, help="output directory; sets are written beneath it")
    ap.add_argument("--span", type=int, default=48, help=f"tiles across at zoom {MAX_ZOOM} (multiple of 16)")
    ap.add_argument("--rgb", action="store_true", help="also write the 24-bit set (zoom 16, pack only)")
    args = ap.parse_args(argv)
    if args.span % (1 << (MAX_ZOOM - MIN_ZOOM)):
        ap.error(f"--span must be a multiple of {1 << (MAX_ZOOM - MIN_ZOOM)}")
    with multiprocessing.Pool() as pool:
        build_set(os.path.join(args.out, "palette"), args.span, MIN_ZOOM, False, ("xyz", "pack", "mbtiles"), pool)
        if args.rgb:
            build_set(os.path.join(args.out, "rgb"), args.span, MAX_ZOOM, True, ("pack",), pool)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
