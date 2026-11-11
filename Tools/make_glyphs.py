#!/usr/bin/env python3
"""Build the "Walker Mono" map label font used by the WALKER styles.

    pip install fonttools freetype-py numpy scipy
    python3 Tools/make_glyphs.py App/Resources/Fonts/VT323-Regular.ttf

MapLibre draws labels from signed-distance-field (SDF) glyph files, one
protobuf per 256 code points. This script renders every character the pixel
font (VT323, SIL OFL) has as an SDF and merges it over the bundled
"Noto Sans Regular" ranges, writing App/Resources/Glyphs/Walker Mono/.
Characters VT323 does not have (Cyrillic, Arabic, Thai, ...) therefore still
appear, in Noto Sans, instead of disappearing.

The glyph format follows the Noto files exactly (checked against them):
24 px em, 3 px buffer, distance radius 8 px, edge value 191, top measured
from 26 px above the baseline, advance rounded down. VT323 is drawn 1.3x
larger than that em so its x-height matches Noto Sans and the label sizes
in the style work for both fonts.

The output is committed; run this only when changing the font.
"""
import math
import os
import sys

import numpy as np
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.freetypePen import FreeTypePen
from fontTools.ttLib import TTFont
from scipy.ndimage import distance_transform_edt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLYPHS = os.path.join(ROOT, "App", "Resources", "Glyphs")
BASE = "Noto Sans Regular"
NAME = "Walker Mono"

SIZE, BUFFER, RADIUS, CUTOFF, TOP_OFFSET = 24, 3, 8.0, 0.25, 26
OVERSAMPLE = 8
SCALE = 1.3


# --- glyphs.proto ------------------------------------------------------------

def _varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def _fields(b):
    i = 0
    while i < len(b):
        k, i = _varint(b, i)
        f, t = k >> 3, k & 7
        if t == 0:
            v, i = _varint(b, i)
        elif t == 2:
            n, i = _varint(b, i)
            v = b[i:i + n]
            i += n
        else:
            raise ValueError(f"unexpected wire type {t}")
        yield f, v


def _unzigzag(v):
    return (v >> 1) ^ -(v & 1)


KEYS = {1: "id", 2: "bitmap", 3: "width", 4: "height", 5: "left", 6: "top", 7: "advance"}


def read_range(data):
    glyphs = []
    for f, v in _fields(data):
        if f != 1:
            continue
        for g, w in _fields(v):
            if g != 3:
                continue
            gl = {"bitmap": b""}
            for h, x in _fields(w):
                gl[KEYS[h]] = _unzigzag(x) if h in (5, 6) else x
            glyphs.append(gl)
    return glyphs


def _enc(n):
    out = bytearray()
    while True:
        c = n & 0x7F
        n >>= 7
        if n:
            out.append(c | 0x80)
        else:
            out.append(c)
            return bytes(out)


def _ld(f, b):
    return _enc((f << 3) | 2) + _enc(len(b)) + b


def _vi(f, n):
    return _enc(f << 3) + _enc(n)


def _zigzag(n):
    return ((n << 1) ^ (n >> 63)) & 0xFFFFFFFFFFFFFFFF


def write_range(name, rng, glyphs):
    body = _ld(1, name.encode()) + _ld(2, rng.encode())
    for g in sorted(glyphs, key=lambda g: g["id"]):
        gb = _vi(1, g["id"])
        if g["bitmap"]:
            gb += _ld(2, g["bitmap"])
        gb += (_vi(3, g["width"]) + _vi(4, g["height"]) + _vi(5, _zigzag(g["left"]))
               + _vi(6, _zigzag(g["top"])) + _vi(7, g["advance"]))
        body += _ld(3, gb)
    return _ld(1, body)


# --- SDF rendering -----------------------------------------------------------

def render(glyphset, name, upm, codepoint):
    sc = SIZE / upm * SCALE
    g = glyphset[name]
    advance = math.floor(g.width * sc)
    bp = BoundsPen(glyphset)
    g.draw(bp)
    if bp.bounds is None:  # space and other empty glyphs
        return {"id": codepoint, "bitmap": b"", "width": 0, "height": 0,
                "left": 0, "top": -TOP_OFFSET, "advance": advance}
    x0, y0, x1, y1 = [v * sc for v in bp.bounds]
    left, bottom, top = math.floor(x0), math.floor(y0), math.ceil(y1)
    w, h = math.ceil(x1) - left, top - bottom
    W, H, S = w + 2 * BUFFER, h + 2 * BUFFER, OVERSAMPLE
    pen = FreeTypePen(glyphset)
    g.draw(pen)
    k = sc * S
    arr = pen.array(width=W * S, height=H * S, contain=False,
                    transform=(k, 0, 0, k, -(left - BUFFER) * S, -(bottom - BUFFER) * S))
    inside = arr >= 0.5
    signed = (distance_transform_edt(~inside) - distance_transform_edt(inside)) / S
    c = S // 2
    sd = signed[c::S, c::S][:H, :W]
    bmp = np.clip(np.round(255 - 255 * (sd / RADIUS + CUTOFF)), 0, 255).astype(np.uint8)
    return {"id": codepoint, "bitmap": bmp.tobytes(), "width": w, "height": h,
            "left": left, "top": top - TOP_OFFSET, "advance": advance}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    font = TTFont(sys.argv[1])
    upm = font["head"].unitsPerEm
    cmap = font.getBestCmap()
    glyphset = font.getGlyphSet()

    by_range = {}
    for cp, name in cmap.items():
        if cp > 0xFFFF:
            continue
        by_range.setdefault(cp // 256, {})[cp] = name

    base_dir = os.path.join(GLYPHS, BASE)
    out_dir = os.path.join(GLYPHS, NAME)
    os.makedirs(out_dir, exist_ok=True)
    for f in os.listdir(out_dir):
        if f.endswith(".pbf"):
            os.remove(os.path.join(out_dir, f))

    ranges = {int(f.split("-")[0]) // 256 for f in os.listdir(base_dir) if f.endswith(".pbf")}
    ranges |= set(by_range)
    replaced = 0
    for r in sorted(ranges):
        rng = f"{r * 256}-{r * 256 + 255}"
        glyphs = {}
        base_path = os.path.join(base_dir, rng + ".pbf")
        if os.path.exists(base_path):
            with open(base_path, "rb") as fh:
                glyphs = {g["id"]: g for g in read_range(fh.read())}
        for cp, name in by_range.get(r, {}).items():
            glyphs[cp] = render(glyphset, name, upm, cp)
            replaced += 1
        with open(os.path.join(out_dir, rng + ".pbf"), "wb") as fh:
            fh.write(write_range(NAME, rng, list(glyphs.values())))
    print(f"{out_dir}: {len(ranges)} ranges, {replaced} glyphs from {os.path.basename(sys.argv[1])}")


if __name__ == "__main__":
    main()
