#!/usr/bin/env python3
"""Turn captured frames into a PNG or an animated GIF.

Two producers feed this and they agree on the pixel format, so one encoder serves both:

  * `deploy-device.sh shot|clip` dd's pages straight off a Brick's /dev/fb0 - raw XRGB8888,
    B,G,R,X per pixel, `--raw WxH`.
  * `meshclient_uicap` renders the same UI off-screen on the build host and writes binary PPM,
    which carries its own geometry.

The Python standard library and nothing else. A developer with a Brick is on macOS as often as
not, and asking them to install Pillow or ffmpeg to look at a screenshot is how a convenience
stops being one. That is also why the GIF encoder below is written out longhand.

  frames.py png  --out shot.png frame.ppm
  frames.py png  --out shot.png --raw 1024x768 page.bin
  frames.py gif  --out clip.gif --manifest capture/frames.txt
  frames.py gif  --out clip.gif --raw 1024x768 --delay 200 pages.bin
  frames.py gif  --out clip.gif --delay 140 --downscale 2 frame-*.ppm
  frames.py selftest
"""

import argparse
import os
import struct
import sys
import zlib

# ---- reading frames -------------------------------------------------------------------------


class Frame:
    """One image as packed RGB triples, row-major, no padding."""

    def __init__(self, width, height, rgb):
        self.width = width
        self.height = height
        self.rgb = rgb


def read_ppm(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if not data.startswith(b"P6"):
        raise ValueError("%s is not a binary PPM" % path)

    # Three whitespace-separated numbers after the magic, '#' comments allowed between them.
    fields = []
    offset = 2
    while len(fields) < 3:
        while offset < len(data) and data[offset : offset + 1].isspace():
            offset += 1
        if data[offset : offset + 1] == b"#":
            while offset < len(data) and data[offset : offset + 1] != b"\n":
                offset += 1
            continue
        start = offset
        while offset < len(data) and not data[offset : offset + 1].isspace():
            offset += 1
        fields.append(int(data[start:offset]))
    offset += 1  # the single whitespace byte before the pixels

    width, height, maxval = fields
    if maxval != 255:
        raise ValueError("%s: only 8-bit PPMs are supported" % path)
    expected = width * height * 3
    pixels = data[offset : offset + expected]
    if len(pixels) != expected:
        raise ValueError("%s: %d bytes of pixels, expected %d" % (path, len(pixels), expected))
    return Frame(width, height, bytearray(pixels))


def read_raw(path, width, height):
    """Pages off /dev/fb0: little-endian XRGB8888, so B,G,R,X in memory.

    A `clip` is one long dd through one SSH connection rather than one connection per frame, so
    the file that arrives holds every page end to end. Any whole number of them is a sequence."""
    with open(path, "rb") as handle:
        data = handle.read()
    page = width * height * 4
    if len(data) < page:
        raise ValueError("%s: %d bytes, expected at least %d for %dx%d"
                         % (path, len(data), page, width, height))

    out = []
    for start in range(0, len(data) - page + 1, page):
        chunk = data[start : start + page]
        rgb = bytearray(width * height * 3)
        rgb[0::3] = chunk[2::4]
        rgb[1::3] = chunk[1::4]
        rgb[2::3] = chunk[0::4]
        out.append(Frame(width, height, rgb))
    return out


def read_frames(path, raw_size):
    if raw_size is not None:
        return read_raw(path, raw_size[0], raw_size[1])
    return [read_ppm(path)]


def downscale(frame, factor):
    """Integer box average. A 1024x768 clip is four times the GIF a 512x384 one is, and the
    Brick's panel is small enough that half size still shows everything the change did."""
    if factor <= 1:
        return frame
    out_w = frame.width // factor
    out_h = frame.height // factor
    if out_w == 0 or out_h == 0:
        raise ValueError("--downscale %d leaves nothing of a %dx%d frame"
                         % (factor, frame.width, frame.height))

    src = frame.rgb
    out = bytearray(out_w * out_h * 3)
    area = factor * factor
    for y in range(out_h):
        row_base = y * factor
        out_row = y * out_w * 3
        for x in range(out_w):
            col_base = x * factor
            r = g = b = 0
            for dy in range(factor):
                base = ((row_base + dy) * frame.width + col_base) * 3
                for dx in range(factor):
                    r += src[base]
                    g += src[base + 1]
                    b += src[base + 2]
                    base += 3
            index = out_row + x * 3
            out[index] = r // area
            out[index + 1] = g // area
            out[index + 2] = b // area
    return Frame(out_w, out_h, out)


# ---- PNG ------------------------------------------------------------------------------------


def write_png(frame, path):
    stride = frame.width * 3
    rows = bytearray()
    for y in range(frame.height):
        rows.append(0)  # filter type 0 (none), one byte per scanline
        rows += frame.rgb[y * stride : (y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", frame.width, frame.height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as handle:
        handle.write(png)


# ---- palette --------------------------------------------------------------------------------

# Above this many distinct colours a clip is photographic rather than the HUD, and the exact
# histogram stops being worth its cost: a 512x384 frame of gradients holds six figures of them,
# and both median cut and the nearest-colour search scale with that count. The HUD's own frames
# sit in the dozens, so the exact path - which is faster *and* lossless - is what they take.
EXACT_COLOUR_LIMIT = 4096

# Colours are folded to 5 bits a channel once that limit is passed, capping the histogram at
# 32768 buckets however many colours the screen actually held. The output palette is 256 entries
# either way, so the three low bits were never going to survive.
COLOUR_BUCKET_SHIFT = 3


def _histogram(frames, shift, limit=None):
    """Colour -> pixel count, or None if it passed `limit`. Keys are shifted when `shift` is."""
    counts = {}
    for frame in frames:
        rgb = frame.rgb
        if shift:
            for index in range(0, len(rgb), 3):
                key = (rgb[index] >> shift, rgb[index + 1] >> shift, rgb[index + 2] >> shift)
                counts[key] = counts.get(key, 0) + 1
        else:
            for index in range(0, len(rgb), 3):
                key = (rgb[index], rgb[index + 1], rgb[index + 2])
                counts[key] = counts.get(key, 0) + 1
        # Checked per frame rather than per pixel: one frame cannot hold more distinct colours
        # than it has pixels, so the dict stays bounded either way, and the inner loop stays free
        # of the test.
        if limit is not None and len(counts) > limit:
            return None
    return counts


class _Box:
    """A median-cut box, carrying its own bounds.

    The bounds are the point. Recomputing min/max over every colour in every box on each of the
    255 splits is 255 full scans of the histogram, which is what made a photographic frame take
    half a minute; a split touches two boxes, so their bounds are the only ones that change."""

    __slots__ = ("colours", "weight", "span", "channel")

    def __init__(self, colours):
        self.colours = colours
        self.weight = sum(weight for _, weight in colours)
        self.span = 0
        self.channel = 0
        if len(colours) < 2:
            return
        for channel in range(3):
            low = high = colours[0][0][channel]
            for colour, _ in colours:
                value = colour[channel]
                if value < low:
                    low = value
                elif value > high:
                    high = value
            if high - low > self.span:
                self.span = high - low
                self.channel = channel

    def split(self):
        """Halves the box at the weighted median of its widest channel."""
        ordered = sorted(self.colours, key=lambda item: item[0][self.channel])
        half = self.weight // 2
        running = 0
        cut = 1
        for position, (_, weight) in enumerate(ordered):
            running += weight
            if running >= half:
                cut = max(1, min(position + 1, len(ordered) - 1))
                break
        return _Box(ordered[:cut]), _Box(ordered[cut:])

    def average(self):
        total = self.weight or 1
        return tuple(
            sum(colour[channel] * weight for colour, weight in self.colours) // total
            for channel in range(3)
        )


def median_cut(counts, wanted):
    """Classic median cut over the colours present, weighted by how often they occur."""
    boxes = [_Box(list(counts.items()))]
    while len(boxes) < wanted:
        target = -1
        target_span = 0
        for index, box in enumerate(boxes):
            if box.span > target_span:
                target, target_span = index, box.span
        if target < 0:
            break  # every box holds one colour; there is nothing left to split
        boxes[target : target + 1] = list(boxes[target].split())
    return [box.average() for box in boxes]


class Palette:
    """The clip's colour table, and the map from a pixel to an index in it.

    Two modes, because the two things this encodes could not be less alike. The HUD is a flat
    palette and a 1-bit font — a whole clip of it holds a few dozen colours, so they are used
    exactly and looked up by dict. A screen filmed off the device can be a photo, and there the
    colours are folded into buckets first so that neither median cut nor the nearest-colour
    search is unbounded."""

    def __init__(self, frames):
        counts = _histogram(frames, 0, EXACT_COLOUR_LIMIT)
        if counts is not None and len(counts) <= 256:
            self.shift = 0
            entries = sorted(counts)
        else:
            self.shift = COLOUR_BUCKET_SHIFT
            if counts is None:
                counts = _histogram(frames, self.shift)
            else:
                # Between 256 and EXACT_COLOUR_LIMIT colours: fold what we already counted
                # rather than walking every pixel a second time.
                folded = {}
                for colour, weight in counts.items():
                    key = tuple(value >> self.shift for value in colour)
                    folded[key] = folded.get(key, 0) + weight
                counts = folded
            # Median cut wants real colours, so each bucket stands in as its own centre.
            half = 1 << (self.shift - 1)
            counts = {
                tuple((value << self.shift) | half for value in key): weight
                for key, weight in counts.items()
            }
            entries = median_cut(counts, 256)

        # A GIF colour table is a power of two, and at least four entries: the minimum LZW code
        # size is 2, which is a four-entry table.
        size = 4
        while size < len(entries):
            size *= 2
        self.entries = list(entries) + [(0, 0, 0)] * (size - len(entries))

        if self.shift == 0:
            self.cache = {colour: index for index, colour in enumerate(entries)}
        else:
            self.cache = {}

    def index_of(self, colour):
        key = colour if self.shift == 0 else tuple(value >> self.shift for value in colour)
        found = self.cache.get(key)
        if found is None:
            best = 0
            best_distance = None
            for index, entry in enumerate(self.entries):
                dr = colour[0] - entry[0]
                dg = colour[1] - entry[1]
                db = colour[2] - entry[2]
                distance = dr * dr + dg * dg + db * db
                if best_distance is None or distance < best_distance:
                    best, best_distance = index, distance
                    if distance == 0:
                        break
            found = best
            self.cache[key] = found
        return found

    def map_frame(self, frame):
        rgb = frame.rgb
        out = bytearray(frame.width * frame.height)
        cache = self.cache
        shift = self.shift
        for position in range(len(out)):
            index = position * 3
            colour = (rgb[index], rgb[index + 1], rgb[index + 2])
            key = colour if shift == 0 else (colour[0] >> shift, colour[1] >> shift,
                                             colour[2] >> shift)
            found = cache.get(key)
            out[position] = found if found is not None else self.index_of(colour)
        return out



# ---- GIF ------------------------------------------------------------------------------------


class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.accumulator = 0
        self.bits = 0

    def write(self, value, width):
        self.accumulator |= value << self.bits
        self.bits += width
        while self.bits >= 8:
            self.out.append(self.accumulator & 0xFF)
            self.accumulator >>= 8
            self.bits -= 8

    def finish(self):
        if self.bits > 0:
            self.out.append(self.accumulator & 0xFF)
            self.accumulator = 0
            self.bits = 0
        return bytes(self.out)


def lzw_encode(indices, min_code_size):
    """GIF's variable-width LZW.

    The width bump is the part everyone gets wrong. A decoder cannot build the entry for a code
    until it has read that code, so its table runs exactly one entry behind the encoder's. The
    encoder therefore has to widen using the count it held *before* adding the current entry -
    which is the count the decoder will have when it reads the next code. Hence the check sitting
    after the write, on the not-yet-incremented next_code."""
    clear_code = 1 << min_code_size
    end_code = clear_code + 1

    writer = BitWriter()
    state = {"size": min_code_size + 1, "next": clear_code + 2}

    def emit(code):
        writer.write(code, state["size"])
        if state["next"] > (1 << state["size"]) - 1 and state["size"] < 12:
            state["size"] += 1

    def fresh_table():
        return {bytes([value]): value for value in range(clear_code)}

    table = fresh_table()
    emit(clear_code)

    if not indices:
        emit(end_code)
        return writer.finish()

    buffer = bytes(indices[0:1])
    for position in range(1, len(indices)):
        symbol = bytes(indices[position : position + 1])
        candidate = buffer + symbol
        if candidate in table:
            buffer = candidate
            continue
        emit(table[buffer])
        if state["next"] < 4096:
            table[candidate] = state["next"]
            state["next"] += 1
        else:
            emit(clear_code)
            table = fresh_table()
            state["size"] = min_code_size + 1
            state["next"] = clear_code + 2
        buffer = symbol

    emit(table[buffer])
    emit(end_code)
    return writer.finish()


def sub_blocks(payload):
    out = bytearray()
    for start in range(0, len(payload), 255):
        piece = payload[start : start + 255]
        out.append(len(piece))
        out += piece
    out.append(0)
    return bytes(out)


def changed_box(previous, current, width, height):
    """The smallest rectangle covering every pixel that moved, or None if none did."""
    if previous is None:
        return (0, 0, width, height)
    top = None
    bottom = 0
    for y in range(height):
        row = y * width
        if previous[row : row + width] != current[row : row + width]:
            if top is None:
                top = y
            bottom = y
    if top is None:
        return None

    left = width
    right = 0
    for y in range(top, bottom + 1):
        row = y * width
        for x in range(left):
            if previous[row + x] != current[row + x]:
                left = x
                break
        for x in range(width - 1, right - 1, -1):
            if previous[row + x] != current[row + x]:
                right = x
                break
    return (left, top, right - left + 1, bottom - top + 1)


def write_gif(frames, delays, path, loop=0):
    width, height = frames[0].width, frames[0].height
    for frame in frames:
        if frame.width != width or frame.height != height:
            raise ValueError("every frame in a clip has to be the same size")

    palette = Palette(frames)
    bits = max(2, (len(palette.entries) - 1).bit_length())
    out = bytearray(b"GIF89a")
    out += struct.pack("<HHBBB", width, height, 0xF0 | (bits - 1), 0, 0)
    for entry in palette.entries:
        out += bytes(entry)
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\x00"

    # Quantise first and fold each run of identical frames into one. A scene that presses a
    # button the screen ignores, or a device filmed while nothing happened, produces frames that
    # are pixel-identical; those are time on screen, not frames, and giving that time to the
    # frame already showing is both smaller and more honest than repeating it.
    mapped = []
    for frame, delay_ms in zip(frames, delays):
        current = palette.map_frame(frame)
        if mapped and mapped[-1][0] == current:
            mapped[-1][1] += delay_ms
            continue
        mapped.append([current, delay_ms])

    previous = None
    for current, delay_ms in mapped:
        box = changed_box(previous, current, width, height)
        if box is None:
            continue

        left, top, box_w, box_h = box
        centiseconds = max(1, min(65535, (delay_ms + 5) // 10))
        # Disposal 1 (leave it in place) is what makes a sub-rectangle legal: each frame paints
        # over the last rather than replacing it.
        out += b"\x21\xF9\x04" + bytes([1 << 2]) + struct.pack("<H", centiseconds) + b"\x00\x00"
        out += b"\x2C" + struct.pack("<HHHH", left, top, box_w, box_h) + b"\x00"

        window = bytearray(box_w * box_h)
        for y in range(box_h):
            source = (top + y) * width + left
            window[y * box_w : (y + 1) * box_w] = current[source : source + box_w]
        out.append(bits)
        out += sub_blocks(lzw_encode(window, bits))
        previous = current

    out += b"\x3B"
    with open(path, "wb") as handle:
        handle.write(out)
    return len(mapped)


# ---- self test ------------------------------------------------------------------------------


def lzw_decode(payload, min_code_size):
    """An independent decoder, written from the decoder's side of the contract, so `selftest`
    is a real check on lzw_encode rather than the same mistake made twice."""
    clear_code = 1 << min_code_size
    end_code = clear_code + 1

    bit = 0
    def read(width):
        nonlocal bit
        value = 0
        for step in range(width):
            byte = payload[(bit + step) // 8]
            value |= ((byte >> ((bit + step) % 8)) & 1) << step
        bit += width
        return value

    table = []
    size = min_code_size + 1
    out = bytearray()
    previous = None

    while True:
        code = read(size)
        if code == end_code:
            return bytes(out)
        if code == clear_code:
            table = [bytes([value]) for value in range(clear_code)] + [b"", b""]
            size = min_code_size + 1
            previous = None
            continue
        if previous is None:
            entry = table[code]
        elif code < len(table):
            entry = table[code]
            table.append(previous + entry[:1])
        else:
            entry = previous + previous[:1]
            table.append(entry)
        out += entry
        previous = entry
        if len(table) > (1 << size) - 1 and size < 12:
            size += 1


def selftest():
    import random

    random.seed(20260906)
    failures = 0
    cases = [
        bytearray(),
        bytearray([0]),
        bytearray([7] * 5000),
        bytearray(range(256)) * 4,
        bytearray(random.randrange(256) for _ in range(70000)),
        bytearray((position * position) % 251 for position in range(40000)),
    ]
    for index, case in enumerate(cases):
        encoded = lzw_encode(case, 8)
        decoded = lzw_decode(encoded, 8)
        if decoded != bytes(case):
            failures += 1
            print("lzw case %d: %d bytes in, %d out" % (index, len(case), len(decoded)))
    for min_size in (2, 3, 4):
        limit = 1 << min_size
        case = bytearray(random.randrange(limit) for _ in range(9000))
        if lzw_decode(lzw_encode(case, min_size), min_size) != bytes(case):
            failures += 1
            print("lzw min code size %d round trip failed" % min_size)

    # The palette, and specifically the bound on how much work a photographic frame can cause.
    # Median cut and the nearest-colour search both scale with the number of distinct colours,
    # so an unbounded histogram is not a slow path, it is a hang: 255 splits over six figures of
    # colours took half a minute for one 512x384 frame before the fold existed.
    flat = Frame(64, 64, bytearray())
    for position in range(64 * 64):
        colour = ((position % 5) * 40, (position % 3) * 60, (position % 7) * 30)
        flat.rgb += bytes(colour)
    flat_palette = Palette([flat])
    if flat_palette.shift != 0:
        failures += 1
        print("a flat frame should use exact colours, not buckets")
    for position in range(0, len(flat.rgb), 3):
        colour = (flat.rgb[position], flat.rgb[position + 1], flat.rgb[position + 2])
        if flat_palette.entries[flat_palette.index_of(colour)] != colour:
            failures += 1
            print("exact palette did not round trip %s" % (colour,))
            break

    noise = Frame(128, 128, bytearray(random.randrange(256) for _ in range(128 * 128 * 3)))
    noisy_palette = Palette([noise])
    bucket_limit = 1 << (3 * (8 - COLOUR_BUCKET_SHIFT))
    if noisy_palette.shift != COLOUR_BUCKET_SHIFT:
        failures += 1
        print("a high-colour frame should fold into buckets")
    if len(noisy_palette.entries) > 256:
        failures += 1
        print("palette has %d entries; a GIF takes 256" % len(noisy_palette.entries))
    folded = _histogram([noise], COLOUR_BUCKET_SHIFT)
    if len(folded) > bucket_limit:
        failures += 1
        print("folded histogram holds %d colours; the bound is %d" % (len(folded), bucket_limit))

    if failures:
        print("frames.py: %d failure(s)" % failures)
        return 1
    print("frames.py: LZW round trip and palette bounds ok")
    return 0


# ---- command line ---------------------------------------------------------------------------


def parse_size(text):
    parts = text.lower().split("x")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("expected WxH, got '%s'" % text)
    return (int(parts[0]), int(parts[1]))


def read_manifest(path):
    """`name<TAB>delay_ms` per line, as meshclient_uicap writes it."""
    entries = []
    directory = os.path.dirname(path)
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, delay = line.partition("\t")
            entries.append((os.path.join(directory, name), int(delay) if delay else 0))
    return entries


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    png = sub.add_parser("png", help="one frame to a PNG")
    png.add_argument("input")
    png.add_argument("--out", required=True)
    png.add_argument("--raw", type=parse_size, metavar="WxH",
                     help="the input is a raw XRGB8888 page rather than a PPM")
    png.add_argument("--downscale", type=int, default=1)

    gif = sub.add_parser("gif", help="a strip of frames to an animated GIF")
    gif.add_argument("inputs", nargs="*")
    gif.add_argument("--out", required=True)
    gif.add_argument("--manifest", help="frames.txt naming each frame and its delay in ms")
    gif.add_argument("--raw", type=parse_size, metavar="WxH")
    gif.add_argument("--delay", type=int, default=140, help="ms per frame without a manifest")
    gif.add_argument("--downscale", type=int, default=1)
    gif.add_argument("--loop", type=int, default=0, help="0 loops forever")

    sub.add_parser("selftest", help="round-trip the GIF compressor")

    args = parser.parse_args(argv)

    if args.command == "selftest":
        return selftest()

    if args.command == "png":
        frame = downscale(read_frames(args.input, args.raw)[0], args.downscale)
        write_png(frame, args.out)
        print("%s  %dx%d" % (args.out, frame.width, frame.height))
        return 0

    if args.manifest:
        entries = read_manifest(args.manifest)
    else:
        if not args.inputs:
            parser.error("gif needs frames, or a --manifest naming them")
        entries = [(path, args.delay) for path in args.inputs]
    if not entries:
        parser.error("no frames to encode")

    frames = []
    delays = []
    for path, delay in entries:
        for frame in read_frames(path, args.raw):
            frames.append(downscale(frame, args.downscale))
            delays.append(delay or args.delay)
    written = write_gif(frames, delays, args.out, loop=args.loop)
    size = os.path.getsize(args.out)
    folded = "" if written == len(frames) else " (%d captured)" % len(frames)
    print("%s  %d frames%s  %dx%d  %.1f KB"
          % (args.out, written, folded, frames[0].width, frames[0].height, size / 1024.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
