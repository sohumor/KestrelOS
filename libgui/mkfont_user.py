#!/usr/bin/env python3
"""Generate libgui/font_data.c -- the userspace font tables.

Four faces are produced from ONE piece of hand-drawn art:

    6x12   small   (0.75x)   antialiased
    8x16   body    (1.00x)   the native bitmap, bit-for-bit
    12x24  large   (1.50x)   antialiased
    16x32  huge    (2.00x)   antialiased

The art is not duplicated here.  This script imports tools/mkfont.py --
the one place the typeface is designed -- calls its build(), re-emits the
8x16 bitmaps under userspace symbol names, and derives the other three
faces from them.  Change a glyph in tools/mkfont.py and the kernel
console, the GUI and every derived size pick it up on the next run.

How the derived faces are built
-------------------------------
Nearest-neighbour scaling of a 1-bit face is unusable: at 1.5x it makes
stems alternately one and two pixels wide, and every diagonal turns into
a staircase.  Instead each glyph is treated as a *shape* and re-sampled:

 1. Reconstruction.  The 8x16 bitmap is read as point samples on a grid
    (pixel (i,j) is a sample at (i+0.5, j+0.5)) and bilinearly
    interpolated into a continuous field f(u,v) over the cell, zero
    outside it.
 2. Contour.  The glyph outline is the iso-line f = THRESH.  For a lone
    one-pixel stem the field is 1 at the stem centre and falls linearly
    to 0 at the neighbouring centres, so the f >= 0.5 band is exactly one
    source pixel wide: at 2x that reproduces a crisp two-pixel stem, at
    1.5x a one-and-a-half pixel stem, and diagonals become straight
    ramps rather than staircases.
 3. Area sampling.  Each target pixel takes SUB x SUB sub-samples of its
    own square and its coverage is the fraction of them inside the
    contour.  That is analytic-ish antialiasing: interiors stay solid,
    the background stays clean and only the edge pixels get grey.
 4. Stem darkening.  Downscaling puts a 1px stem in 0.75 of a pixel,
    which reads as washed-out grey.  The small face therefore drops the
    threshold slightly (dilating the shape) and multiplies coverage by a
    gain, the same trick a hinting engine uses to keep small text dark.
 5. Hand correction.  A handful of glyphs come out badly at 6x12 -- the
    ones whose art has one-pixel gaps that 0.75x cannot resolve.  Those
    are overridden below with hand-drawn coverage art.

Coverage is quantised to 4 bits (16 levels, plenty for text) and packed
two pixels per byte, low nibble first, which keeps the whole table at
about 41 KiB.

Bold and italic are NOT stored: font.c synthesises them while blitting
(a one-column coverage smear and a 1:4 shear), so the tables stay small
and every size gets all four combinations.

Usage
-----
    python3 libgui/mkfont_user.py                 # write font_data.c
    python3 libgui/mkfont_user.py --check         # verify it is current
    python3 libgui/mkfont_user.py --dump 6x12 Ag  # coverage art, for
                                                  # hand-correcting
"""

import importlib.util
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESIGN = os.path.join(ROOT, "tools", "mkfont.py")
OUT = os.path.join(ROOT, "libgui", "font_data.c")

SRC_W = 8           # source cell, must match tools/mkfont.py
SRC_H = 16
SUB = 6             # sub-samples per axis per target pixel
LEVELS = 15         # 4-bit coverage: 0..15

HEX = "0123456789abcdef"

# name, width, height, contour threshold, coverage gain
FACES = [
    ("6x12",  6, 12, 0.44, 1.30),
    ("12x24", 12, 24, 0.50, 1.00),
    ("16x32", 16, 32, 0.50, 1.00),
]

# --------------------------------------------------------------------------
# Hand corrections.
#
# Keys are (face, character); values are coverage art, one line per row,
# one character per pixel: '.' = paper, '#' = full ink, or a hex digit
# 0..f for an intermediate level.  Rows may be omitted from the bottom.
# Run --dump to print the generated art and edit from there.
# --------------------------------------------------------------------------

OVERRIDES = {}

# The 6x12 face is the only one the re-sampler cannot do on its own.  At
# 0.75x a one-pixel gap is 0.75 of a pixel, so every glyph whose art
# depends on a one-pixel counter -- '#', '%', 'm', 'w', 'W', 'M', '@',
# '&' -- fills in and reads as a lozenge.  Those eight are redrawn here
# on the 6x12 grid (cap line row 1, x line row 3, baseline row 9), with
# stroke levels near 'd' so they carry the same colour as their
# re-sampled neighbours.  Everything else scales cleanly and is left
# alone; run --dump 6x12 to compare a redraw against the machine's.

OVERRIDES[("6x12", "#")] = """
......
......
......
.d.d..
adada.
.d.d..
adada.
.d.d..
.d.d..
"""

OVERRIDES[("6x12", "%")] = """
......
......
......
cc...a
cc..a.
...a..
..a...
.a..cc
a...cc
"""

OVERRIDES[("6x12", "&")] = """
......
.dd...
d..d..
d..d..
.dd...
dd.d..
d..d.d
d...dd
.ddd.d
"""

OVERRIDES[("6x12", "@")] = """
......
.dddd.
d....d
d.dd.d
d.d.dd
d.d.dd
d.ddd.
d.....
.dddd.
"""

OVERRIDES[("6x12", "M")] = """
......
d...d.
dd.dd.
d.d.d.
d...d.
d...d.
d...d.
d...d.
d...d.
"""

OVERRIDES[("6x12", "W")] = """
......
c...c.
c...c.
c...c.
c...c.
c...c.
c.a.c.
c.e.c.
.ded..
"""

OVERRIDES[("6x12", "m")] = """
......
......
......
......
ddddd.
d.d.d.
d.d.d.
d.d.d.
d.d.d.
"""

OVERRIDES[("6x12", "w")] = """
......
......
......
......
c...c.
c...c.
c.a.c.
c.e.c.
.ded..
"""


def load_design():
    """Import tools/mkfont.py as a module without running its main()."""
    spec = importlib.util.spec_from_file_location("kestrel_mkfont", DESIGN)
    if spec is None or spec.loader is None:
        raise SystemExit(f"mkfont_user: cannot load {DESIGN}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# --------------------------------------------------------------------------
# Bitmap -> continuous field -> coverage
# --------------------------------------------------------------------------

def bit(bm, x, y):
    if x < 0 or y < 0 or x >= SRC_W or y >= SRC_H:
        return 0.0
    return 1.0 if bm[y] & (0x80 >> x) else 0.0


def field(bm, u, v):
    """Bilinear reconstruction of the bitmap at source coordinate (u, v)."""
    fx = u - 0.5
    fy = v - 0.5
    i = math.floor(fx)
    j = math.floor(fy)
    tx = fx - i
    ty = fy - j
    return (bit(bm, i, j) * (1.0 - tx) * (1.0 - ty)
            + bit(bm, i + 1, j) * tx * (1.0 - ty)
            + bit(bm, i, j + 1) * (1.0 - tx) * ty
            + bit(bm, i + 1, j + 1) * tx * ty)


def scale_glyph(bm, tw, th, thresh, gain):
    """Return tw*th coverage levels (0..LEVELS), row-major."""
    sx = SRC_W / float(tw)
    sy = SRC_H / float(th)
    out = []
    for ty_ in range(th):
        vs = [(ty_ + (b + 0.5) / SUB) * sy for b in range(SUB)]
        for tx_ in range(tw):
            us = [(tx_ + (a + 0.5) / SUB) * sx for a in range(SUB)]
            hit = 0
            for v in vs:
                for u in us:
                    if field(bm, u, v) >= thresh:
                        hit += 1
            cov = hit / float(SUB * SUB) * gain
            if cov > 1.0:
                cov = 1.0
            out.append(int(round(cov * LEVELS)))
    return out


def parse_art(art, tw, th, what):
    """Hand-drawn coverage art -> tw*th levels."""
    rows = [r for r in art.split("\n") if r != ""]
    if len(rows) > th:
        raise ValueError(f"{what}: {len(rows)} rows, at most {th}")
    rows += ["." * tw] * (th - len(rows))
    out = []
    for y, row in enumerate(rows):
        if len(row) != tw:
            raise ValueError(f"{what} row {y}: width {len(row)}, want {tw}")
        for ch in row:
            if ch == ".":
                out.append(0)
            elif ch == "#":
                out.append(LEVELS)
            elif ch in HEX:
                out.append(HEX.index(ch))
            else:
                raise ValueError(f"{what} row {y}: bad character {ch!r}")
    return out


def build_face(glyphs, fallback, face):
    name, tw, th, thresh, gain = face
    cells = []
    for i, bm in enumerate(list(glyphs) + [fallback]):
        ch = chr(32 + i) if i < len(glyphs) else None
        key = (name, ch)
        if ch is not None and key in OVERRIDES:
            cells.append(parse_art(OVERRIDES[key], tw, th, f"{name} {ch!r}"))
        else:
            cells.append(scale_glyph(bm, tw, th, thresh, gain))
    return cells


def pack(cells, tw, th):
    """Two 4-bit pixels per byte, low nibble = even x."""
    data = []
    for cell in cells:
        for y in range(th):
            for x in range(0, tw, 2):
                lo = cell[y * tw + x]
                hi = cell[y * tw + x + 1] if x + 1 < tw else 0
                data.append(lo | (hi << 4))
    return data


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------

BANNER = """\
/* Generated by libgui/mkfont_user.py -- do not edit by hand.
 *
 * The userspace font tables.  gui_font8x16 is the native face designed in
 * tools/mkfont.py (there is exactly one place where the glyph art lives);
 * the gui_font_cov* tables are that same art re-sampled to 6x12, 12x24 and
 * 16x32 as 4-bit coverage, two pixels per byte, low nibble first.  Bold and
 * italic are synthesised at blit time by font.c and are not stored.
 *
 * Regenerate after changing the typeface:
 *
 *     python3 libgui/mkfont_user.py
 */\
"""


def emit(mod):
    glyphs, fallback = mod.build()
    first = mod.FIRST
    lines = [BANNER, "", '#include "font.h"', ""]

    lines.append("const uint8_t gui_font8x16[95][16] = {")
    for i, g in enumerate(glyphs):
        ch = chr(first + i)
        shown = "\\\\" if ch == "\\" else ch
        lines.append(f"    /* 0x{first + i:02X}  '{shown}' */")
        lines.append("    { " + ", ".join(f"0x{b:02X}" for b in g) + " },")
    lines.append("};")
    lines.append("")
    lines.append("/* Drawn in place of any code point outside 0x20..0x7E. */")
    lines.append("const uint8_t gui_font8x16_fallback[16] = {")
    lines.append("    " + ", ".join(f"0x{b:02X}" for b in fallback))
    lines.append("};")

    total = 0
    for face in FACES:
        name, tw, th, _, _ = face
        cells = build_face(glyphs, fallback, face)
        data = pack(cells, tw, th)
        per = tw * th // 2
        total += len(data)
        lines.append("")
        lines.append(f"/* {name}: {len(cells)} cells of {per} bytes"
                     f" ({tw}x{th}, 4-bit coverage). */")
        lines.append(f"const uint8_t gui_font_cov{name}[{len(data)}] = {{")
        for i in range(len(cells)):
            ch = chr(first + i) if i < len(glyphs) else None
            if ch is None:
                lines.append("    /* fallback */")
            else:
                shown = "\\\\" if ch == "\\" else ch
                lines.append(f"    /* 0x{first + i:02X}  '{shown}' */")
            cell = data[i * per:(i + 1) * per]
            step = tw // 2
            for r in range(0, len(cell), step):
                row = cell[r:r + step]
                lines.append("    " + " ".join(f"0x{b:02X}," for b in row))
        lines.append("};")

    lines.append("")
    return "\n".join(lines), len(glyphs), total


def dump(mod, which, chars):
    glyphs, fallback = mod.build()
    face = None
    for f in FACES:
        if f[0] == which:
            face = f
    if face is None:
        raise SystemExit(f"mkfont_user: no face {which!r}")
    name, tw, th, thresh, gain = face
    for ch in chars:
        code = ord(ch)
        if not mod.FIRST <= code <= mod.LAST:
            continue
        bm = glyphs[code - mod.FIRST]
        key = (name, ch)
        if key in OVERRIDES:
            cell = parse_art(OVERRIDES[key], tw, th, "override")
            tag = " (hand-corrected)"
        else:
            cell = scale_glyph(bm, tw, th, thresh, gain)
            tag = ""
        print(f'OVERRIDES[("{name}", "{ch}")] = """{tag}')
        for y in range(th):
            print("".join(HEX[cell[y * tw + x]].replace("0", ".")
                          for x in range(tw)))
        print('"""')
        print()


def main():
    args = sys.argv[1:]
    mod = load_design()

    if args and args[0] == "--dump":
        if len(args) < 3:
            print("usage: mkfont_user.py --dump FACE CHARS")
            return 1
        dump(mod, args[1], args[2])
        return 0

    text, count, cov = emit(mod)

    if args and args[0] == "--check":
        try:
            with open(OUT, "r", newline="\n") as f:
                current = f.read()
        except OSError:
            print("mkfont_user: font_data.c is missing")
            return 1
        if current != text:
            print("mkfont_user: font_data.c is stale, rerun the generator")
            return 1
        print("mkfont_user: font_data.c is up to date")
        return 0
    if args:
        print(__doc__)
        return 1

    with open(OUT, "w", newline="\n") as f:
        f.write(text)
    print(f"mkfont_user: wrote {OUT} ({count} glyphs, "
          f"{cov} bytes of coverage)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
