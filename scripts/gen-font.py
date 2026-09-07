#!/usr/bin/env python3
"""Rasterise a monospace face into the coverage table the UI draws its text from.

This is not part of the build. Run it by hand and commit the generated file, so the build stays
dependency-free and CI never reaches the network - the same arrangement scripts/gen-icons.py
has, and for the same reasons.

    python3 -m venv .venv && .venv/bin/pip install fonttools pillow
    curl -sSLo JetBrainsMono-Regular.ttf 'https://raw.githubusercontent.com/JetBrains/\
JetBrainsMono/master/fonts/ttf/JetBrainsMono-Regular.ttf'
    .venv/bin/python scripts/gen-font.py JetBrainsMono-Regular.ttf src/ui/font_ui_glyphs.c
    make format   # the table is emitted twelve values a line and clang-format repacks it


The set of characters covered is not a list in this file: it is whatever the 5x7 font can draw,
read out of src/ui/font5x7.c, because a second font that covers less is a font that turns some
node names into boxes the moment a theme selects it. tests/suites/ui_theme.c holds the two to
that parity, so a face missing something fails the build rather than the name.

Why the geometry is what it is:

  CELL 20x32   The cell the device draws at: MESH_UI_FONT_UI_W and _H are 5x8 at scale 1, and
               the Brick's body scale is 4. Rasterising at exactly that size means the master
               is drawn 1:1 on the device and resampled only when a theme asks for another
               scale - so the face is as sharp as the panel can show it.
  EM 29        The largest size at which nothing in the covered set touches an edge of the
               master - accents included, cedillas included. Found by search rather than by
               arithmetic, because the binding constraint is a different character at each end
               (an accented capital above, C-cedilla below) and neither is the one the font's
               own ascent and descent metrics describe. Bigger clips; smaller wastes the cell
               and reads smaller than the 5x7 font it replaces.
  BASELINE 29  The row in the master the pen sits on, again from that search: it is what puts
               the tallest accent and the deepest cedilla inside the master at once.
  OVERHANG 4   Rows above the cell, for the diacritics that by definition sit above the cap
               height. Four is what the line gap leaves, and what an accented capital needs.
  SUPERSAMPLE  Rendered at 4x and resampled down, rather than asked for at 20 px directly.
               A hinted 20 px rasterisation snaps stems to whole pixels, which is what makes
               small bitmap text look mechanical; downsampling an unhinted 80 px one keeps the
               face's own proportions and puts the softness in the coverage where it belongs.

JetBrains Mono is under the SIL Open Font License 1.1; licenses/OFL-1.1-JetBrainsMono.txt
travels with the generated data. Point the script at another mono face to try one - the cell
geometry is the contract, not the file.
"""

import re
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

CELL_W, CELL_H = 20, 32  # the cell, in device pixels at the body scale
OVERHANG = 4  # master rows above the cell, where the diacritics go
EM = 29  # the size the face is rasterised at
BASELINE = 29  # the baseline's row in the master, overhang included
SUPERSAMPLE = 4
LEVELS = 16  # coverage values per pixel: 4 bits, 0 to 15

MASTER_W, MASTER_H = CELL_W, CELL_H + OVERHANG

# Characters a mono face is unlikely to draw, and the one it should draw instead. These are the
# typographic variants whose plain form is what this cell would show anyway - the same
# substitution src/ui/font5x7.c makes, listed here because a missing glyph in a face has to
# resolve to something rather than failing the run.
FALLBACK = {
    0x00A0: " ", 0x2002: " ", 0x2003: " ", 0x2007: " ", 0x2008: " ", 0x2009: " ",
    0x200A: " ", 0x202F: " ", 0x205F: " ", 0x3000: " ", 0x00AD: "-", 0x2010: "-",
    0x2011: "-", 0x2015: "-", 0x2212: "-", 0x201B: "'", 0x201F: '"', 0x2032: "'",
    0x2033: '"', 0x2044: "/",
}


def coverage(path):
    """Every codepoint src/ui/font5x7.c can draw, as a sorted list.

    Parsed rather than hardcoded: the three tables there are the definition of what the UI can
    render, and a copy of them here would be a copy to keep in step.
    """
    text = open(path).read()
    found = set(range(0x20, 0x7F))  # the ASCII table, which is indexed rather than listed
    for name in ("k_literals", "k_aliases", "k_composed"):
        match = re.search(name + r"\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
        if match is None:
            raise SystemExit("no %s table in %s" % (name, path))
        for entry in re.finditer(r"\{\s*0x([0-9A-Fa-f]{4,6})\s*,", match.group(1)):
            found.add(int(entry.group(1), 16))
    return sorted(found)


def render(face, text, advance):
    """One character as MASTER_W x MASTER_H coverage values, 0 to LEVELS - 1.

    The glyph is placed by its own advance centred in the cell, so a face whose advance is
    narrower than the cell keeps its sidebearings even and its stems in the same column from one
    row of text to the next - which is the whole of what makes a monospace grid read straight.
    """
    scale = SUPERSAMPLE
    pad = EM * scale
    canvas = Image.new("L", (MASTER_W * scale + 2 * pad, MASTER_H * scale + 2 * pad), 0)
    pen_x = pad + (MASTER_W * scale - advance) / 2.0
    pen_y = pad + BASELINE * scale
    ImageDraw.Draw(canvas).text((pen_x, pen_y), text, font=face, fill=255, anchor="ls")
    box = canvas.crop((pad, pad, pad + MASTER_W * scale, pad + MASTER_H * scale))
    small = box.resize((MASTER_W, MASTER_H), Image.LANCZOS)
    step = 255 // (LEVELS - 1)
    return [min(LEVELS - 1, (value + step // 2) // step) for value in small.getdata()], canvas


def spill(canvas, pad):
    """How far ink reached outside the master, in device pixels. 0 when the glyph fits.

    Measured on the supersampled canvas and reported in the master's own units, because that is
    the scale the answer matters at: the question is not whether the rasteriser put anything
    outside the box, it is whether a *pixel* of the finished glyph is missing.
    """
    ink = canvas.getbbox()
    if ink is None:
        return 0.0
    over = max(pad - ink[0], pad - ink[1],
               ink[2] - (pad + MASTER_W * SUPERSAMPLE), ink[3] - (pad + MASTER_H * SUPERSAMPLE))
    return max(over, 0) / float(SUPERSAMPLE)


# What counts as fitting. Antialiasing spreads a stroke a fraction of a pixel past the outline
# it came from, so a glyph drawn exactly to the edge of the master bleeds a little over it and a
# zero tolerance would reject the geometry that best fills the cell. Half a pixel is the point
# at which the outermost column loses enough coverage to see; above it, the cell has cut the
# shape and the run fails rather than shipping a clipped letter.
SPILL_TOLERANCE = 0.5


def cap_rows(face, advance):
    """How many master rows a capital stands on the baseline.

    Measured off a rasterised 'H' rather than read from the face's OS/2 table, because what the
    UI matches an icon to is the ink on the panel - and after the supersample and the resample
    that is what this is.
    """
    _, canvas = render(face, "H", advance)
    ink = canvas.getbbox()
    pad = EM * SUPERSAMPLE
    top = (ink[1] - pad) / float(SUPERSAMPLE)
    return int(round(BASELINE - top))


def trim(pixels):
    """The ink box of a master, as (x, y, w, h, values inside it).

    Trimming rather than run-length encoding, which is where the icon set stores its coverage.
    The difference is what the data looks like: a filled symbol is long runs of 15 and long runs
    of 0, and a letter at this size is neither - it is a small dense patch of varying coverage
    inside a mostly empty cell. Runs over the whole master came to 95 KB; the box and four bits
    a pixel is 50, and decodes with less work.
    """
    ink = [i for i, value in enumerate(pixels) if value]
    if not ink:
        return 0, 0, 0, 0, []
    xs = [i % MASTER_W for i in ink]
    ys = [i // MASTER_W for i in ink]
    x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    box = [pixels[y * MASTER_W + x] for y in range(y0, y1) for x in range(x0, x1)]
    return x0, y0, x1 - x0, y1 - y0, box


def pack(values):
    """Coverage packed two pixels to a byte, low nibble first."""
    out = bytearray()
    for i in range(0, len(values), 2):
        high = values[i + 1] if i + 1 < len(values) else 0
        out.append(values[i] | (high << 4))
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    font_path, out_path = sys.argv[1], sys.argv[2]
    root = __file__.rsplit("/", 2)[0]

    wanted = coverage(root + "/src/ui/font5x7.c")
    cmap = TTFont(font_path, lazy=True).getBestCmap()
    face = ImageFont.truetype(font_path, EM * SUPERSAMPLE)
    advance = face.getlength("M")

    glyphs = []
    missing = []
    for codepoint in wanted:
        text = chr(codepoint)
        if codepoint not in cmap:
            text = FALLBACK.get(codepoint)
            if text is None:
                missing.append(codepoint)
                continue
        pixels, canvas = render(face, text, advance)
        over = spill(canvas, EM * SUPERSAMPLE)
        if over > SPILL_TOLERANCE:
            print("U+%04X spills %.2f px outside the %dx%d master"
                  % (codepoint, over, MASTER_W, MASTER_H), file=sys.stderr)
            return 1
        glyphs.append((codepoint, pixels))

    if missing:
        # Not a warning: a font that covers less than 5x7 turns a name the UI could draw into a
        # row of boxes, and it would do it only for the people whose names need those letters.
        print("the face has no glyph for, and no fallback for: "
              + ", ".join("U+%04X" % cp for cp in missing), file=sys.stderr)
        return 1

    blob = bytearray()
    entries = []
    for codepoint, pixels in glyphs:
        x, y, w, h, box = trim(pixels)
        entries.append((codepoint, len(blob), x, y, w, h))
        blob += pack(box)

    caps = cap_rows(face, advance)
    face_name = TTFont(font_path, lazy=True)["name"].getDebugName(4) or font_path

    with open(out_path, "w") as f:
        w = f.write
        w("/*\n")
        w(" * Generated by scripts/gen-font.py from %s - do not edit by hand.\n" % face_name)
        w(" *\n")
        w(" * JetBrains Mono is licensed under the SIL Open Font License 1.1; the licence text\n")
        w(" * is in licenses/OFL-1.1-JetBrainsMono.txt and covers this derived data too.\n")
        w(" *\n")
        w(" * %d glyphs on a %dx%d master (%d cell rows under a %d-row overhang) at %d coverage\n"
          % (len(glyphs), MASTER_W, MASTER_H, CELL_H, OVERHANG, LEVELS))
        w(" * levels, %d bytes of pixels. The coverage set is whatever src/ui/font5x7.c can\n"
          % len(blob))
        w(" * draw, so the two fonts render the same names.\n")
        w(" */\n\n")
        w('#include "mesh/ui/font_ui.h"\n\n')

        w("/* Every glyph's ink box, one after another: coverage packed two pixels to a byte,\n")
        w("   low nibble first, row by row within the box. */\n")
        w("static const uint8_t k_pixels[] = {\n")
        for i in range(0, len(blob), 12):
            w("    " + " ".join("0x%02X," % value for value in blob[i:i + 12]) + "\n")
        w("};\n\n")

        w("/* The glyphs, by ascending codepoint - which is what lets the lookup bisect. */\n")
        w("static const struct mesh_ui_font_ui_glyph k_glyphs[] = {\n")
        for codepoint, offset, x, y, gw, gh in entries:
            w("    {0x%04X, %6d, %2d, %2d, %2d, %2d},\n" % (codepoint, offset, x, y, gw, gh))
        w("};\n\n")

        w("const struct mesh_ui_font_ui_table mesh_ui_font_ui_table = {\n")
        w("    .pixels = k_pixels,\n")
        w("    .glyphs = k_glyphs,\n")
        w("    .count = %d,\n" % len(entries))
        w("    /* Measured off a rasterised 'H': what an icon beside the text is sized to. */\n")
        w("    .cap_rows = %d,\n" % caps)
        w("};\n")

    print("%s: %d glyphs, %d bytes of pixels, cap %d master rows"
          % (out_path, len(entries), len(blob), caps))
    return 0


if __name__ == "__main__":
    sys.exit(main())
