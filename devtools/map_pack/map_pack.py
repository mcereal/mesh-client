#!/usr/bin/env python3
"""Build, inspect and verify the client's raster tile packs (``*.mctp``).

Stdlib only, like scripts/frames.py and devtools/tile_bench/gen_tiles.py, so it runs on a host
with nothing installed. It never ships in the pak: a pack is made here and sideloaded, which is
the delivery path docs/maps-roadmap.md keeps for step 4.

    map_pack.py build --mbtiles region.mbtiles -o region.mctp \\
        --name "Vancouver" --attribution "(c) OpenStreetMap contributors"
    map_pack.py build --xyz tiles/ -o region.mctp --attribution "..." --max-zoom 16
    map_pack.py info region.mctp
    map_pack.py verify region.mctp

Why a format of its own rather than shipping MBTiles or PMTiles: the Brick measurement in
docs/maps-roadmap.md. On a FAT32 card with 32 KiB clusters mounted ``sync``, a single file with
a sorted index reached a cold tile in 0.80 ms where MBTiles took 4.6 ms and a z/x/y tree took
4.6 ms with a 40 ms tail - and SQLite cost 718 KB of binary on top. So the conversion happens
once, here, on a machine where none of that matters.

The on-disk layout is documented in src/map/source_pack.c, which is the reader this writes for,
and reproduced in docs/maps-roadmap.md. Little-endian throughout:

    0    8   "MCTPACK2"
    8    2   tile size in pixels
    10   1   tile format (1 = PNG)
    11   1   reserved, zero
    12   4   index offset from the start of the file
    16   4   tile count
    20   4   reserved, zero
    24   8   generated: seconds since the epoch, 0 when the builder did not say
    32   64  attribution, UTF-8, NUL padded
    96   32  name, UTF-8, NUL padded
    128      index: count 24-byte entries sorted by (z, x, y), then the tile bytes

An entry is (u8 zoom, 3 pad, u32 x, u32 y, u32 length, u64 offset). Twenty-four bytes because
that is what the device benchmark measured, so the numbers it produced are the numbers the
client gets.
"""

import argparse
import math
import os
import sqlite3
import struct
import sys
import time

MAGIC = b"MCTPACK2"
HEADER = struct.Struct("<8sHBBIII")  # magic, tile size, format, reserved, index offset, count, reserved
HEADER_LEN = 128
ENTRY = struct.Struct("<BxxxIIIQ")
ENTRY_LEN = 24
ATTRIBUTION_MAX = 64
NAME_MAX = 32
TILE_SIZE = 256
FORMAT_PNG = 1
TILE_BYTES_MAX = 1024 * 1024
TILES_MAX = 1000000
ZOOM_MAX = 18
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# PNG colour types, for the palette advice `verify` gives. The device measured a palette tile at
# 1.24 ms to decode against 2.09 for the same drawing in 24-bit, and 2.7x the bytes.
COLOUR_TYPES = {0: "grey", 2: "rgb", 3: "palette", 4: "grey+alpha", 6: "rgba"}


def die(message):
    print(f"map_pack: {message}", file=sys.stderr)
    sys.exit(1)


def fixed(degrees):
    """Degrees to the 1e-7 fixed point every coordinate in the client is carried in."""
    return int(round(degrees * 1e7))


def tile_bounds(z, x, y):
    """The (north, west, south, east) corners of one XYZ tile, in degrees."""
    span = 1.0 / (1 << z)
    west = (x * span) * 360.0 - 180.0
    east = ((x + 1) * span) * 360.0 - 180.0
    north = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * (y * span)))))
    south = math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * ((y + 1) * span)))))
    return north, west, south, east


def png_size(data):
    """(width, height, colour type) from a PNG's IHDR, or None when it is not a PNG."""
    if len(data) < 33 or not data.startswith(PNG_SIGNATURE) or data[12:16] != b"IHDR":
        return None
    width, height = struct.unpack(">II", data[16:24])
    return width, height, data[25]


def text_field(value, cap, label):
    encoded = value.encode("utf-8")
    if len(encoded) > cap - 1:
        die(f"{label} is {len(encoded)} bytes; the pack holds {cap - 1}")
    return encoded + b"\0" * (cap - len(encoded))


def read_xyz(root):
    """Every z/x/y.png under a directory, as {(z, x, y): bytes}."""
    tiles = {}
    for z_name in os.listdir(root):
        z_dir = os.path.join(root, z_name)
        if not z_name.isdigit() or not os.path.isdir(z_dir):
            continue
        for x_name in os.listdir(z_dir):
            x_dir = os.path.join(z_dir, x_name)
            if not x_name.isdigit() or not os.path.isdir(x_dir):
                continue
            for y_name in os.listdir(x_dir):
                stem, extension = os.path.splitext(y_name)
                if extension.lower() != ".png" or not stem.isdigit():
                    continue
                with open(os.path.join(x_dir, y_name), "rb") as handle:
                    tiles[(int(z_name), int(x_name), int(stem))] = handle.read()
    return tiles


def read_mbtiles(path):
    """Every tile in an MBTiles file, with its TMS row turned the right way up.

    The one conversion in this tool, and it is here rather than in the client on purpose: the
    row convention is a property of the storage, so it is normalised by whatever reads that
    storage and nothing above ever sees two conventions. An MBTiles row is counted from the
    *bottom* of the world, and XYZ counts from the top.
    """
    tiles = {}
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        fmt = dict(db.execute("SELECT name, value FROM metadata")).get("format", "png")
        if fmt.lower() not in ("png",):
            die(f"{path} holds {fmt} tiles; this client draws PNG")
        for z, x, row, blob in db.execute(
            "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles"
        ):
            tiles[(z, x, (1 << z) - 1 - row)] = bytes(blob)
    finally:
        db.close()
    return tiles


def filter_tiles(tiles, min_zoom, max_zoom, bbox):
    kept = {}
    for (z, x, y), data in tiles.items():
        if min_zoom is not None and z < min_zoom:
            continue
        if max_zoom is not None and z > max_zoom:
            continue
        if bbox is not None:
            south, west, north, east = bbox
            t_north, t_west, t_south, t_east = tile_bounds(z, x, y)
            if t_east <= west or t_west >= east or t_south >= north or t_north <= south:
                continue
        kept[(z, x, y)] = data
    return kept


def build(args):
    if (args.mbtiles is None) == (args.xyz is None):
        die("give exactly one of --mbtiles and --xyz")
    tiles = read_mbtiles(args.mbtiles) if args.mbtiles else read_xyz(args.xyz)
    if args.bbox is not None:
        parts = [float(p) for p in args.bbox.split(",")]
        if len(parts) != 4:
            die("--bbox wants south,west,north,east")
        args.bbox = tuple(parts)
    tiles = filter_tiles(tiles, args.min_zoom, args.max_zoom, args.bbox)
    if not tiles:
        die("nothing to pack")
    if len(tiles) > TILES_MAX:
        die(f"{len(tiles)} tiles; the reader holds {TILES_MAX}")

    # Refused rather than packed, because every one of these is a tile the client would open the
    # pack for and then be unable to draw - and a pack that fails at the blit fails on the
    # device, where nobody is watching a terminal.
    for key, data in sorted(tiles.items()):
        shape = png_size(data)
        if shape is None:
            die(f"tile {key} is not a PNG")
        if shape[0] != TILE_SIZE or shape[1] != TILE_SIZE:
            die(f"tile {key} is {shape[0]}x{shape[1]}; the client draws {TILE_SIZE}")
        if len(data) > TILE_BYTES_MAX:
            die(f"tile {key} is {len(data)} bytes; the reader holds {TILE_BYTES_MAX}")
        if key[0] > ZOOM_MAX:
            die(f"tile {key} is above zoom {ZOOM_MAX}, which the client cannot address")

    order = sorted(tiles)
    index_at = HEADER_LEN
    offset = index_at + ENTRY_LEN * len(order)
    generated = 0 if args.no_date else int(time.time())

    with open(args.output, "wb") as out:
        header = HEADER.pack(MAGIC, TILE_SIZE, FORMAT_PNG, 0, index_at, len(order), 0)
        out.write(header)
        out.write(struct.pack("<q", generated))
        out.write(text_field(args.attribution, ATTRIBUTION_MAX, "--attribution"))
        out.write(text_field(args.name, NAME_MAX, "--name"))
        assert out.tell() == HEADER_LEN, out.tell()
        for key in order:
            out.write(ENTRY.pack(key[0], key[1], key[2], len(tiles[key]), offset))
            offset += len(tiles[key])
        for key in order:
            out.write(tiles[key])

    print(f"{args.output}: {len(order)} tiles, {os.path.getsize(args.output) / 1e6:.1f} MB")
    describe(args.output)


def load(path):
    """A pack's header and index, checked the way the client checks them."""
    with open(path, "rb") as handle:
        raw = handle.read(HEADER_LEN)
        if len(raw) < HEADER_LEN:
            die(f"{path} is too short to be a pack")
        magic, tile_size, fmt, _, index_at, count, _ = HEADER.unpack(raw[: HEADER.size])
        if magic != MAGIC:
            die(f"{path} is not a tile pack")
        generated = struct.unpack("<q", raw[24:32])[0]
        attribution = raw[32:96].split(b"\0", 1)[0].decode("utf-8", "replace")
        name = raw[96:128].split(b"\0", 1)[0].decode("utf-8", "replace")
        handle.seek(index_at)
        index_raw = handle.read(ENTRY_LEN * count)
    entries = [ENTRY.unpack_from(index_raw, i * ENTRY_LEN) for i in range(count)]
    return {
        "path": path,
        "tile_size": tile_size,
        "format": fmt,
        "index_at": index_at,
        "count": count,
        "generated": generated,
        "attribution": attribution,
        "name": name,
        "entries": entries,
        "size": os.path.getsize(path),
    }


def coverage(entries):
    """The bounding box of the tiles, the way the reader derives it."""
    north = -90.0
    south = 90.0
    east = -180.0
    west = 180.0
    for z, x, y, _, _ in entries:
        t_north, t_west, t_south, t_east = tile_bounds(z, x, y)
        north = max(north, t_north)
        south = min(south, t_south)
        east = max(east, t_east)
        west = min(west, t_west)
    return south, west, north, east


def describe(path):
    pack = load(path)
    zooms = sorted({entry[0] for entry in pack["entries"]})
    south, west, north, east = coverage(pack["entries"])
    when = (
        time.strftime("%Y-%m-%d", time.gmtime(pack["generated"])) if pack["generated"] else "unset"
    )
    print(f"name         {pack['name'] or '(unset)'}")
    print(f"attribution  {pack['attribution'] or '(unset)'}")
    print(f"generated    {when}")
    print(f"tiles        {pack['count']} in {pack['size'] / 1e6:.1f} MB")
    print(f"zooms        {zooms[0]}-{zooms[-1]}")
    for z in zooms:
        at = sum(1 for entry in pack["entries"] if entry[0] == z)
        print(f"  z{z:<2}        {at} tiles")
    print(f"coverage     {south:.5f},{west:.5f} to {north:.5f},{east:.5f}")
    print(f"             fixed 1e-7: south={fixed(south)} west={fixed(west)} "
          f"north={fixed(north)} east={fixed(east)}")
    print(f"index        {pack['count'] * ENTRY_LEN / 1e6:.2f} MB held in RAM while open")


def verify(args):
    pack = load(args.pack)
    problems = []
    if pack["tile_size"] != TILE_SIZE:
        problems.append(f"tile size {pack['tile_size']}, not {TILE_SIZE}")
    if pack["format"] != FORMAT_PNG:
        problems.append(f"tile format {pack['format']}, not PNG")
    if pack["count"] == 0:
        problems.append("no tiles")

    tiles_at = pack["index_at"] + pack["count"] * ENTRY_LEN
    previous = None
    colours = {}
    with open(args.pack, "rb") as handle:
        for z, x, y, length, offset in pack["entries"]:
            key = (z, x, y)
            if z > ZOOM_MAX or x >= (1 << z) or y >= (1 << z):
                problems.append(f"tile {key} is not a place in the pyramid")
            if previous is not None and key <= previous:
                problems.append(f"tile {key} is out of order after {previous}")
            previous = key
            if length == 0 or length > TILE_BYTES_MAX:
                problems.append(f"tile {key} is {length} bytes")
                continue
            if offset < tiles_at or offset + length > pack["size"]:
                problems.append(f"tile {key} lies outside the file")
                continue
            handle.seek(offset)
            data = handle.read(length)
            shape = png_size(data)
            if shape is None:
                problems.append(f"tile {key} is not a PNG")
                continue
            if shape[0] != TILE_SIZE or shape[1] != TILE_SIZE:
                problems.append(f"tile {key} is {shape[0]}x{shape[1]}")
            colours[shape[2]] = colours.get(shape[2], 0) + 1

    for problem in problems:
        print(f"  {problem}")
    kinds = ", ".join(
        f"{count} {COLOUR_TYPES.get(kind, kind)}" for kind, count in sorted(colours.items())
    )
    print(f"{args.pack}: {pack['count']} tiles checked ({kinds})")
    if colours.get(3, 0) < pack["count"]:
        # Advice, not a failure: a 24-bit pack is a legal pack that costs 1.7x the decode and
        # 2.7x the bytes, which the Brick measured. Quantising is a renderer's job, not this
        # tool's - it has no image library and is not going to grow one.
        print("  note: not every tile is a palette PNG; quantise the style for a smaller,")
        print("        faster pack (the device measured 1.24 ms against 2.09 ms per tile)")
    if problems:
        print(f"  {len(problems)} problem(s)")
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    made = commands.add_parser("build", help="write a pack from an MBTiles file or a z/x/y tree")
    made.add_argument("--mbtiles", help="source MBTiles file")
    made.add_argument("--xyz", help="source directory of z/x/y.png")
    made.add_argument("-o", "--output", required=True)
    made.add_argument("--name", default="", help=f"what the pack calls itself ({NAME_MAX - 1} bytes)")
    made.add_argument(
        "--attribution",
        default="",
        help=f"the credit line the map draws ({ATTRIBUTION_MAX - 1} bytes)",
    )
    made.add_argument("--min-zoom", type=int)
    made.add_argument("--max-zoom", type=int)
    made.add_argument("--bbox", help="clip to south,west,north,east in degrees")
    made.add_argument(
        "--no-date", action="store_true", help="leave the generation date unset (reproducible)"
    )
    made.set_defaults(run=build)

    shown = commands.add_parser("info", help="print a pack's metadata and derived coverage")
    shown.add_argument("pack")
    shown.set_defaults(run=lambda args: describe(args.pack))

    checked = commands.add_parser("verify", help="check every tile the way the reader would")
    checked.add_argument("pack")
    checked.set_defaults(run=verify)

    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
