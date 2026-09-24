#!/usr/bin/env python3
"""Draw the desktop app icons from the one square source image.

This is not part of the build. Run it by hand when the artwork changes and commit what it
writes, so neither package script needs an image library (the Windows one runs where there is
no Python at all):

    pip install pillow
    python3 scripts/gen-icons.py

Reads packaging/icon/meshclient.png - square, the artwork on a flat background colour - and
writes:

    packaging/macos/MeshClient.icon/Assets/lizard.png
        the artwork with its background taken out, as the one layer of an Icon Composer icon.
        packaging/macos/MeshClient.icon/icon.json puts it on a fill of that same colour, and
        scripts/package-macos.sh compiles the whole .icon with actool.
    packaging/windows/meshclient.ico
        the executable's icon resource and the installer's, 16 px to 256 px.

Why the Mac gets a layer and not a picture: macOS 26 draws every app icon in its own squircle,
and one that arrives as a finished picture - an .icns of a rounded tile - is shrunk and set inside
a grey one. actool turns a .icon into an Assets.car, which macOS 26 masks itself, and an .icns it
draws for macOS 11 to 15, which have no such mask. A Windows icon has neither, so it is the
finished tile, drawn nearly to the edge.
"""

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "packaging" / "icon" / "meshclient.png"
LAYER = ROOT / "packaging" / "macos" / "MeshClient.icon" / "Assets" / "lizard.png"
ICO = ROOT / "packaging" / "windows" / "meshclient.ico"

MASTER = 1024
# Windows: a thin margin and a rounded corner, at 1024.
WIN_TILE = 984
WIN_RADIUS = 220
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]


def without_background(art: Image.Image) -> Image.Image:
    """The artwork with the corner pixel's colour made transparent.

    Every pixel is read as the artwork blended over that background, and the least opacity that
    explains it is taken - so an anti-aliased edge keeps its partial coverage, and puts no rim of
    the old background around a fill of another colour.
    """
    bg = art.getpixel((0, 0))[:3]
    out = Image.new("RGBA", art.size)
    src, dst = art.load(), out.load()
    for y in range(art.height):
        for x in range(art.width):
            c = src[x, y][:3]
            alpha = 0.0
            for ci, bi in zip(c, bg):
                if ci > bi:
                    alpha = max(alpha, (ci - bi) / (255 - bi))
                elif ci < bi:
                    alpha = max(alpha, (bi - ci) / bi)
            if alpha <= 0.0:
                dst[x, y] = (0, 0, 0, 0)
                continue
            alpha = min(1.0, alpha)
            fg = tuple(
                max(0, min(255, round((ci - (1.0 - alpha) * bi) / alpha))) for ci, bi in zip(c, bg)
            )
            dst[x, y] = fg + (round(alpha * 255),)
    return out


def rounded_tile(art: Image.Image, size: int, radius: int) -> Image.Image:
    """The artwork scaled to `size`, clipped to a rounded square, centred on a 1024 canvas."""
    tile = art.resize((size, size), Image.LANCZOS)
    # Drawn at 4x and brought down, which is the anti-aliasing the corner needs.
    big = Image.new("L", (size * 4, size * 4), 0)
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, size * 4 - 1, size * 4 - 1), radius=radius * 4, fill=255
    )
    tile.putalpha(big.resize((size, size), Image.LANCZOS))
    canvas = Image.new("RGBA", (MASTER, MASTER), (0, 0, 0, 0))
    offset = (MASTER - size) // 2
    canvas.alpha_composite(tile, (offset, offset))
    return canvas


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    if source.width != source.height:
        raise SystemExit(f"{SOURCE} is {source.width}x{source.height}; it has to be square.")
    master = source.resize((MASTER, MASTER), Image.LANCZOS)

    LAYER.parent.mkdir(parents=True, exist_ok=True)
    without_background(master).save(LAYER, optimize=True)

    ICO.parent.mkdir(parents=True, exist_ok=True)
    rounded_tile(master, WIN_TILE, WIN_RADIUS).save(
        ICO, format="ICO", sizes=[(s, s) for s in ICO_SIZES]
    )

    for path in (LAYER, ICO):
        print(f"wrote {path.relative_to(ROOT)} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
