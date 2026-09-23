#!/usr/bin/env python3
"""Bake the Weather module's fonts into a C header.

The TRACE module has no font engine and no files, so every face it uses is
rendered here once, as 4-bit coverage maps, at 1x and 2x: the module renders at
640x480 or 1280x960 depending on how big the picture is on screen, and picks
the matching set so text is always drawn at its real pixel size (TERMinator
scales with nearest-neighbour, which would smear a scaled-up glyph).

ASCII, with the degree sign as character 127 in every face; HUGE (the big
temperature) has only digits, minus and the degree sign.

Four themes, each a full set of the six faces, chosen in the module (T):
  SMOOTH  Ubuntu (Ubuntu Font Licence 1.0).
  RETRO   the IBM VGA 8x16 text-mode font, scaled so its capitals are as tall
          as Ubuntu's at each size.
  CYBER   Orbitron for headings, Rajdhani for text (SIL OFL 1.1).
  HACKER  VT323 for headings, Share Tech Mono for text (SIL OFL 1.1).
Every theme keeps Ubuntu's ascent and line height, so the layout doesn't move.

Usage: make_fonts.py [out.h]
"""
import gzip
import os
import sys

from PIL import Image, ImageDraw, ImageFont

TTF = "/usr/share/fonts/truetype/ubuntu/Ubuntu[wdth,wght].ttf"
FONTS = os.path.expanduser("~/tools/fonts")        # Orbitron, Rajdhani, VT323, Share Tech Mono (Google Fonts)
VGA = "/usr/share/consolefonts/FullGreek-VGA16.psf.gz"   # the One Liners door's source
VGA_FALLBACK = "/usr/share/consolefonts/Lat15-VGA16.psf.gz"  # the Latin-1 letters FullGreek lacks

ASCII = "".join(chr(c) for c in range(32, 127)) + "°"
DIGITS = " -0123456789°"

# name, weight, logical pixel size, characters
FACES = [
    ("SMALL", b"Medium",  10, ASCII),     # small caps labels
    ("BODY",  b"Regular", 12, ASCII),
    ("BOLD",  b"Bold",    12, ASCII),
    ("HEAD",  b"Medium",  16, ASCII),
    ("TITLE", b"Medium",  21, ASCII),     # the condition in the hero
    ("HUGE",  b"Light",   78, DIGITS),    # the temperature
]
SCALES = (1, 2)

# The other themes' face for each slot: (file, variation or None).
CYBER = {
    "SMALL": ("Rajdhani-SemiBold.ttf", None),
    "BODY":  ("Rajdhani-Medium.ttf", None),
    "BOLD":  ("Rajdhani-Bold.ttf", None),
    "HEAD":  ("Orbitron.ttf", b"Medium"),
    "TITLE": ("Orbitron.ttf", b"Medium"),
    "HUGE":  ("Orbitron.ttf", b"Regular"),
}
HACKER = {
    "SMALL": ("ShareTechMono-Regular.ttf", None),
    "BODY":  ("ShareTechMono-Regular.ttf", None),
    "BOLD":  ("ShareTechMono-Regular.ttf", None),
    "HEAD":  ("VT323-Regular.ttf", None),
    "TITLE": ("VT323-Regular.ttf", None),
    "HUGE":  ("VT323-Regular.ttf", None),
}
TTF_THEMES = {"CYBER": CYBER, "HACKER": HACKER}
THEMES = ("SMOOTH", "RETRO", "CYBER", "HACKER")


def code_of(ch):
    return 127 if ch == "°" else ord(ch)


def load(path, weight, px):
    font = ImageFont.truetype(path, px)
    if weight:
        font.set_variation_by_name(weight)
    return font


def cap_height(font):
    x0, y0, x1, y1 = font.getbbox("0", anchor="ls")         # a digit: HUGE has nothing else
    return y1 - y0


def bake(font, chars, ascent=None):
    """Glyphs of a TrueType face. ascent, if given, moves the baseline there."""
    own, descent = font.getmetrics()
    shift = 0 if ascent is None else ascent - own
    line = own + descent
    pad = int(font.size)
    glyphs = {}
    for ch in chars:
        adv = int(round(font.getlength(ch)))
        img = Image.new("L", (adv + pad * 2, line + pad * 2), 0)
        ImageDraw.Draw(img).text((pad, pad + own), ch, font=font, fill=255, anchor="ls")
        box = img.getbbox()
        if box is None:
            glyphs[code_of(ch)] = (0, 0, 0, 0, adv, b"")
            continue
        x0, y0, x1, y1 = box
        glyphs[code_of(ch)] = (x0 - pad, y0 - pad + shift, x1 - x0, y1 - y0, adv, img.crop(box).tobytes())
    return own, line, glyphs


def sized_like(path, weight, cap):
    """The face at the pixel size whose capitals are closest to cap pixels tall."""
    best = None
    for px in range(4, 400):
        f = load(path, weight, px)
        d = abs(cap_height(f) - cap)
        if best is None or d < best[0]:
            best = (d, f)
        if cap_height(f) > cap + 2:
            break
    return best[1]


def psf(path):
    d = gzip.open(path).read()
    assert d[0:2] == b"\x36\x04", "expects a PSF1 font"
    mode, h = d[2], d[3]
    count = 512 if mode & 1 else 256
    rows = [d[4 + i * h:4 + (i + 1) * h] for i in range(count)]
    uni, p = {}, 4 + count * h
    for i in range(count):
        while p + 1 < len(d):
            c = d[p] | d[p + 1] << 8
            p += 2
            if c == 0xFFFF:
                break
            if c != 0xFFFE:
                uni.setdefault(chr(c), rows[i])
    return uni


_vga = None


def vga_rows(ch):
    global _vga
    if _vga is None:
        _vga = psf(VGA_FALLBACK)
        _vga.update(psf(VGA))
    return _vga.get(ch) or _vga["?"]


VGA_CAP = 10        # rows 2..11 of the 16: a capital's height
VGA_BASE = 12       # the baseline sits under row 11


def bake_vga(bold, ascent, cap, chars):
    """The VGA face, scaled by area coverage so its capitals are cap pixels tall.
    bold ORs each row with itself shifted a dot right (heavy when scaled: unused)."""
    k = cap / VGA_CAP
    adv = int(round(8 * k))
    top = ascent - VGA_BASE * k            # where row 0 lands, relative to the line top
    oy = int(top // 1)                     # whole-pixel part, into the glyph's y
    fy = top - oy                          # and the fraction, into the sampling
    W, H = int(-(-8 * k // 1)) + 1, int(-(-(16 * k + fy) // 1)) + 1
    glyphs = {}
    for ch in chars:
        rows = vga_rows(ch)
        if bold:
            rows = bytes(r | r >> 1 for r in rows)
        img = Image.new("L", (W, H), 0)
        px = img.load()
        for Y in range(H):
            sy0, sy1 = (Y - fy) / k, (Y + 1 - fy) / k
            for X in range(W):
                sx0, sx1 = X / k, (X + 1) / k
                cov = 0.0
                for r in range(max(0, int(sy0)), min(16, int(-(-sy1 // 1)))):
                    hy = min(sy1, r + 1) - max(sy0, r)
                    if hy <= 0 or not rows[r]:
                        continue
                    for c in range(max(0, int(sx0)), min(8, int(-(-sx1 // 1)))):
                        if rows[r] >> (7 - c) & 1:
                            hx = min(sx1, c + 1) - max(sx0, c)
                            if hx > 0:
                                cov += hx * hy
                v = cov * k * k                # 0..1 of the output pixel
                # A little contrast, so the scaled dots stay crisp.
                v = min(1.0, max(0.0, (v - 0.5) * 1.5 + 0.5)) if v < 1 else 1.0
                px[X, Y] = int(round(v * 255))
        box = img.getbbox()
        if box is None:
            glyphs[code_of(ch)] = (0, 0, 0, 0, adv, b"")
            continue
        x0, y0, x1, y1 = box
        glyphs[code_of(ch)] = (x0, y0 + oy, x1 - x0, y1 - y0, adv, img.crop(box).tobytes())
    return glyphs


def pack4(data):
    """8-bit coverage to 4 bits, two pixels a byte (high nibble first)."""
    q = [(v * 15 + 127) // 255 for v in data]
    if len(q) & 1:
        q.append(0)
    return bytes(q[i] << 4 | q[i + 1] for i in range(0, len(q), 2))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "fonts.h"

    lines = [
        "// Generated by tools/make_fonts.py -- do not edit.",
        "// Four themes of six faces, as 4-bit coverage at 1x and 2x, two pixels a byte:",
        "// SMOOTH is Ubuntu (Ubuntu Font Licence 1.0); RETRO the IBM VGA 8x16 text-mode face;",
        "// CYBER Orbitron and Rajdhani; HACKER VT323 and Share Tech Mono (all SIL Open Font Licence 1.1).",
        "// Character 127 is the degree sign.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "typedef struct { int16_t x, y; uint8_t w, h, adv, pad; uint32_t off; } FontGlyph;",
        "typedef struct { uint8_t ascent, line, first, count; const FontGlyph *g; const uint8_t *alpha; } FontFace;",
        "",
    ]
    faces = []
    total = 0
    for theme in THEMES:
        for name, weight, size, chars in FACES:
            for s in SCALES:
                ubuntu = load(TTF, weight, size * s)
                ascent, line = ubuntu.getmetrics()
                line += ascent
                cap = cap_height(ubuntu)
                if theme == "SMOOTH":
                    _, _, glyphs = bake(ubuntu, chars)
                elif theme == "RETRO":
                    glyphs = bake_vga(False, ascent, cap, chars)     # text mode had no bold, only brighter colours
                else:
                    path, var = TTF_THEMES[theme][name]
                    _, _, glyphs = bake(sized_like(os.path.join(FONTS, path), var, cap), chars, ascent)
                first, last = min(glyphs), max(glyphs)
                tag = f"{theme.lower()}_{name.lower()}{s}"
                blob = bytearray()
                table = []
                for code in range(first, last + 1):
                    if code in glyphs:
                        x, y, w, h, adv, data = glyphs[code]
                        table.append((x, y, w, h, adv, len(blob)))
                        blob += pack4(data)
                    else:
                        table.append((0, 0, 0, 0, 0, 0))
                total += len(blob)
                lines.append(f"static const uint8_t g_{tag}_alpha[{max(len(blob), 1)}] = {{")
                for i in range(0, len(blob), 32):
                    lines.append("    " + ",".join(str(b) for b in blob[i:i + 32]) + ",")
                if not blob:
                    lines.append("    0,")
                lines.append("};")
                lines.append(f"static const FontGlyph g_{tag}_glyphs[{len(table)}] = {{")
                for t in table:
                    lines.append("    {%d,%d,%d,%d,%d,0,%d}," % t)
                lines.append("};")
                faces.append((theme, name, s, ascent, line, first, len(table), tag))

    lines.append("")
    lines.append("enum { " + ", ".join(f"F_{n}" for n, *_ in FACES) + ", F_COUNT };")
    lines.append("enum { " + ", ".join(f"THEME_{t}" for t in THEMES) + ", THEME_COUNT };")
    lines.append(f"static const FontFace g_fonts[THEME_COUNT][{len(SCALES)}][F_COUNT] = {{")
    for theme in THEMES:
        lines.append(f"  {{  // {theme}")
        for s in SCALES:
            lines.append("    {")
            for th, name, fs, ascent, line, first, count, tag in faces:
                if th == theme and fs == s:
                    lines.append(f"        {{{ascent},{line},{first},{count},g_{tag}_glyphs,g_{tag}_alpha}},  // {name}")
            lines.append("    },")
        lines.append("  },")
    lines.append("};")
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"{out}: {total} bytes of glyph coverage")


if __name__ == "__main__":
    main()
