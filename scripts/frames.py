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


def median_cut(counts, wanted):
    """Classic median cut over the colours actually present, weighted by how often.

    The HUD is a flat palette and a 1-bit font, so this normally has nothing to do: only the
    colour emoji push a frame past 256 distinct colours."""
    boxes = [list(counts.items())]
    while len(boxes) < wanted:
        # Split whichever box spans the most in any one channel; stop when none can be split.
        target = -1
        target_span = 0
        target_channel = 0
        for index, box in enumerate(boxes):
            if len(box) < 2:
                continue
            for channel in range(3):
                low = min(color[channel] for color, _ in box)
                high = max(color[channel] for color, _ in box)
                if high - low > target_span:
                    target, target_span, target_channel = index, high - low, channel
        if target < 0:
            break
        box = sorted(boxes[target], key=lambda item: item[0][target_channel])
        half = sum(weight for _, weight in box) // 2
        running = 0
        split = 1
        for position, (_, weight) in enumerate(box):
            running += weight
            if running >= half:
                split = max(1, min(position + 1, len(box) - 1))
                break
        boxes[target : target + 1] = [box[:split], box[split:]]

    palette = []
    for box in boxes:
        total = sum(weight for _, weight in box) or 1
        palette.append(tuple(
            sum(color[channel] * weight for color, weight in box) // total for channel in range(3)
        ))
    return palette


def build_palette(frames):
    counts = {}
    for frame in frames:
        rgb = frame.rgb
        for index in range(0, len(rgb), 3):
            key = (rgb[index], rgb[index + 1], rgb[index + 2])
            counts[key] = counts.get(key, 0) + 1

    if len(counts) <= 256:
        palette = sorted(counts)
    else:
        palette = median_cut(counts, 256)

    # A palette has to be a power of two, and at least four entries: GIF's minimum LZW code
    # size is 2, which is a four-entry table.
    size = 4
    while size < len(palette):
        size *= 2
    palette = list(palette) + [(0, 0, 0)] * (size - len(palette))
    return palette


class Quantizer:
    def __init__(self, palette):
        self.palette = palette
        self.cache = {}

    def index_of(self, color):
        found = self.cache.get(color)
        if found is None:
            best = 0
            best_distance = None
            for index, entry in enumerate(self.palette):
                dr = color[0] - entry[0]
                dg = color[1] - entry[1]
                db = color[2] - entry[2]
                distance = dr * dr + dg * dg + db * db
                if best_distance is None or distance < best_distance:
                    best, best_distance = index, distance
                    if distance == 0:
                        break
            found = best
            self.cache[color] = found
        return found

    def map_frame(self, frame):
        rgb = frame.rgb
        out = bytearray(frame.width * frame.height)
        for position in range(len(out)):
            index = position * 3
            out[position] = self.index_of((rgb[index], rgb[index + 1], rgb[index + 2]))
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
    palette = build_palette(frames)
    quantizer = Quantizer(palette)
    width, height = frames[0].width, frames[0].height
    for frame in frames:
        if frame.width != width or frame.height != height:
            raise ValueError("every frame in a clip has to be the same size")

    bits = max(2, (len(palette) - 1).bit_length())
    out = bytearray(b"GIF89a")
    out += struct.pack("<HHBBB", width, height, 0xF0 | (bits - 1), 0, 0)
    for entry in palette:
        out += bytes(entry)
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\x00"

    # Quantise first and fold each run of identical frames into one. A scene that presses a
    # button the screen ignores, or a device filmed while nothing happened, produces frames that
    # are pixel-identical; those are time on screen, not frames, and giving that time to the
    # frame already showing is both smaller and more honest than repeating it.
    mapped = []
    for frame, delay_ms in zip(frames, delays):
        current = quantizer.map_frame(frame)
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

    if failures:
        print("frames.py: %d failure(s)" % failures)
        return 1
    print("frames.py: LZW round trip ok")
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
