#!/usr/bin/env python3
"""Rasterise Material Symbols into the monochrome sprite table the UI draws its icons from.

This is not part of the build. Run it by hand when include/mesh/ui/icons.def changes and commit
the generated file, so the build stays dependency-free and CI never reaches the network.

    python3 -m venv .venv && .venv/bin/pip install fonttools pillow
    curl -sSLo MaterialSymbolsRounded.ttf 'https://raw.githubusercontent.com/google/\
material-design-icons/master/variablefont/MaterialSymbolsRounded%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf'
    .venv/bin/python scripts/gen-icons.py MaterialSymbolsRounded.ttf src/ui/icon_glyphs.c

The icons and their glyph names come from include/mesh/ui/icons.def, which is also what builds
`enum mesh_ui_icon` - so the enum and the sprites are generated from one list and cannot drift.

Material Symbols is under the Apache License 2.0; licenses/Apache-2.0-MaterialSymbols.txt
travels with the generated data.

Why the axes and the crop are what they are:

  FILL=1     A filled symbol survives being drawn at a couple of dozen pixels; an outlined one
             is a 1 px stroke by then, and thins to nothing wherever it curves. Compare `star`.
  wght=500   Half a step heavier than the default, which is what keeps the strokes that stay
             strokes at any fill - the chevron, the check, the bluetooth rune - above one pixel.
  opsz=20    The optical size the font itself is drawn for at this scale.
  crop       Material draws on a 24 grid, and a symbol is *usually* inside the central 20 - but
             only usually: at FILL 1 and wght 500 the full-bleed ones (`hub`, `settings_input_
             antenna`, `warning`) reach the grid's edge and a hair past it. So the window is the
             whole grid plus a little air rather than the 20 body, because a window that clips
             is a bug in every sprite it touches and a window a few per cent wide costs a
             fraction of a sprite pixel.
  anchor     The window is placed off the *baseline*, not off the drawing origin. Material sits
             its grid on the baseline, one em tall, while PIL's default anchor puts the origin
             on the ascender - and this face's ascender is a tenth of an em above the em box.
             Cropping as if the two were the same put the window a descender too high and shaved
             the bottom off every symbol in the set.
  SIZE=32    Bigger than the cell an icon usually lands in (28 px at the body scale), because
             the empty screens draw one at several times that and a sprite scaled up 5x reads
             as a smudge. The whole set is still under 10 KB.
"""

import re
import sys

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont

SIZE = 32  # sprite edge, in pixels
LEVELS = 16  # coverage values per pixel: 4 bits, 0 to 15
EM = 240  # the em the glyph is rasterised at, which is Material's 24 grid
WINDOW = 256  # the part of it a sprite keeps: the grid, plus air for the symbols that spill
AXES = {"FILL": 1.0, "GRAD": 0.0, "opsz": 20.0, "wght": 500.0}

ORIGIN = (EM, EM)  # where the pen is put on the canvas, clear of every edge at any anchor

ENTRY = re.compile(r'^MESH_ICON_ENTRY\(\s*(\w+)\s*,\s*"([^"]+)"\s*\)')


def catalog(path):
    """(id, glyph name) for every icon in icons.def, in enum order."""
    out = []
    for line in open(path):
        match = ENTRY.match(line.strip())
        if match is not None:
            out.append((match.group(1), match.group(2)))
    return out


def window(font):
    """The crop box, in canvas pixels, for a glyph drawn at ORIGIN.

    Material's grid is one em square sitting *on the baseline*: its left edge is the pen, its
    bottom is the baseline and its top is an em above that. PIL's default anchor puts the drawing
    origin on the ascender instead, which on this face is a tenth of an em higher - so the box is
    measured from the baseline the font reports rather than from the origin we drew at.
    """
    ascent, _ = font.getmetrics()
    centre_x = ORIGIN[0] + EM / 2.0
    centre_y = (ORIGIN[1] + ascent) - EM / 2.0
    half = WINDOW / 2.0
    return (round(centre_x - half), round(centre_y - half),
            round(centre_x + half), round(centre_y + half))


def render(font, codepoint, box):
    """One glyph as SIZE x SIZE coverage values (0 to LEVELS - 1), and its ink box on the canvas.

    The ink box comes back with it because whether the window cut the symbol is a question about
    the canvas, not about the sprite: at 32 px a symbol that reaches the window's edge and one
    the window cut a slice off both put coverage in the outermost row.
    """
    canvas = Image.new("L", (EM * 3, EM * 3), 0)
    ImageDraw.Draw(canvas).text(ORIGIN, chr(codepoint), fill=255, font=font)
    small = canvas.crop(box).resize((SIZE, SIZE), Image.LANCZOS)
    step = 255 // (LEVELS - 1)
    pixels = [min(LEVELS - 1, (value + step // 2) // step) for value in small.getdata()]
    return pixels, canvas.getbbox()


def spill(ink, box):
    """How far a glyph's ink reaches outside the window, in grid units. 0 when it fits."""
    if ink is None:
        return 0
    return max(box[0] - ink[0], box[1] - ink[1], ink[2] - box[2], ink[3] - box[3], 0)


def rle(values):
    """(count, value) pairs, counts capped at 255."""
    out = []
    for value in values:
        if out and out[-1][1] == value and out[-1][0] < 255:
            out[-1][0] += 1
        else:
            out.append([1, value])
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    font_path, out_path = sys.argv[1], sys.argv[2]

    root = __file__.rsplit("/", 2)[0]
    icons = catalog(root + "/include/mesh/ui/icons.def")

    cmap = {name: cp for cp, name in TTFont(font_path, lazy=True).getBestCmap().items()}
    missing = [glyph for _, glyph in icons if glyph not in cmap]
    if missing:
        print("not in the font: " + ", ".join(missing), file=sys.stderr)
        return 1

    face = ImageFont.truetype(font_path, EM)
    face.set_variation_by_axes([AXES[axis.axisTag] for axis in TTFont(font_path, lazy=True)["fvar"].axes])

    # MESH_UI_ICON_NONE is enum id 0 and draws nothing, so it gets a blank sprite rather than a
    # special case in the decoder.
    box = window(face)
    sprites = [(("NONE", "-"), [0] * (SIZE * SIZE))]
    for name, glyph in icons:
        pixels, ink = render(face, cmap[glyph], box)
        sprites.append(((name, glyph), pixels))
        cut = spill(ink, box)
        if cut > 0:
            # A symbol the window cut is cut on every screen it is drawn on, and the miss is a
            # couple of pixels at the size these are drawn - easy to read as "that is just what
            # the icon looks like". So it fails the run rather than being committed.
            print("%s (%s) spills %d units past the %d-unit window" % (name, glyph, cut, WINDOW),
                  file=sys.stderr)
            return 1

    runs = []
    offsets = []
    for _, pixels in sprites:
        offsets.append(len(runs) // 2)
        for count, value in rle(pixels):
            runs.append(count)
            runs.append(value)
    offsets.append(len(runs) // 2)

    with open(out_path, "w") as f:
        w = f.write
        w("/*\n")
        w(" * Generated by scripts/gen-icons.py from Material Symbols Rounded - do not edit by hand.\n")
        w(" *\n")
        w(" * Material Symbols is licensed under the Apache License 2.0; the licence text is in\n")
        w(" * licenses/Apache-2.0-MaterialSymbols.txt and covers this derived data too.\n")
        w(" *\n")
        w(" * %d sprites of %dx%d at %d coverage levels, %d runs. The icons and their glyph names\n"
          % (len(sprites), SIZE, SIZE, LEVELS, len(runs) // 2))
        w(" * are include/mesh/ui/icons.def; the axes are FILL %g, GRAD %g, opsz %g, wght %g, and\n"
          % (AXES["FILL"], AXES["GRAD"], AXES["opsz"], AXES["wght"]))
        w(" * each sprite is a %d-unit window on the %d-unit design grid, centred on it.\n"
          % (WINDOW, EM))
        w(" */\n\n")
        w('#include "mesh/ui/icon.h"\n\n')

        w("/* (count, coverage) pairs, one run after another, icon by icon. */\n")
        w("static const uint8_t k_runs[] = {\n")
        for i in range(0, len(runs), 12):
            w("    " + " ".join("0x%02X," % value for value in runs[i:i + 12]) + "\n")
        w("};\n\n")

        w("/* Where each icon's runs start, plus a final entry so the last icon has an end.\n")
        w("   One line per icon, named, because this is the table a bad merge shows up in. */\n")
        w("static const uint32_t k_run_offsets[MESH_UI_ICON_COUNT + 1] = {\n")
        for index, ((name, glyph), _) in enumerate(sprites):
            w("    %-6s /* MESH_UI_ICON_%s (%s) */\n" % (str(offsets[index]) + ",", name, glyph))
        w("    %-6s /* end */\n" % (str(offsets[-1]) + ","))
        w("};\n\n")

        w("const struct mesh_ui_icon_table mesh_ui_icon_table = {\n")
        w("    .runs = k_runs,\n")
        w("    .run_offsets = k_run_offsets,\n")
        w("};\n")

    print("%s: %d sprites, %d runs, %d bytes of run data"
          % (out_path, len(sprites), len(runs) // 2, len(runs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
