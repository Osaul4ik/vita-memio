#!/usr/bin/env python3
"""gen_livearea.py - draws the tester's LiveArea pictures (original artwork):

  tester/sce_sys/icon0.png                    128x128  the bubble
  tester/sce_sys/livearea/contents/bg.png     840x500  the LiveArea background
  tester/sce_sys/livearea/contents/startup.png 280x158 the start button

Same palette and font (Terminus, include/vm_font_terminus.h) as the tester's
screens. The Vita wants 8-bit palette PNGs, so each picture is quantised.
Run from the repo root:  python3 tools/gen_livearea.py   (needs Pillow)
"""
import math
import os
import re

from PIL import Image, ImageDraw

BG0 = (10, 11, 15)
BG1 = (22, 30, 52)
PANEL = (20, 22, 30)
EDGE = (52, 60, 84)
FG = (232, 234, 240)
DIM = (128, 134, 150)
ACCENT = (70, 170, 255)
GOOD = (90, 220, 120)


def load_font(name):
    src = open("include/vm_font_terminus.h").read()
    w = int(re.search(r"#define %s_W (\d+)" % name.upper(), src).group(1))
    h = int(re.search(r"#define %s_H (\d+)" % name.upper(), src).group(1))
    body = src.split("static const uint16_t %s[" % name)[1].split("};")[0]
    rows = [int(x, 16) for x in re.findall(r"0x([0-9a-f]{4})", body)]
    glyphs = [rows[i * h:(i + 1) * h] for i in range(len(rows) // h)]
    return w, h, glyphs


FONTS = {n: load_font(n) for n in ("ter14", "ter18", "ter24")}


def text(img, x, y, s, color, font="ter24", scale=1):
    w, h, glyphs = FONTS[font]
    px = img.load()
    for ch in s:
        g = glyphs[ord(ch) - 32]
        for gy in range(h):
            bits = g[gy]
            for gx in range(w):
                if bits & (0x8000 >> gx):
                    for sy in range(scale):
                        for sx in range(scale):
                            X, Y = x + gx * scale + sx, y + gy * scale + sy
                            if 0 <= X < img.width and 0 <= Y < img.height:
                                px[X, Y] = color
        x += w * scale


def text_width(s, font="ter24", scale=1):
    return len(s) * FONTS[font][0] * scale


def gradient(w, h, a, b, radial=False):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            if radial:
                t = min(1.0, math.hypot(x - w / 2, y - h * 0.4) / (0.75 * max(w, h)))
                t = 1.0 - t
            else:
                t = (x / w) * 0.35 + (y / h) * 0.65
                t = 1.0 - t
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    return img


def card(d, x, y, w, h, outline, fill, width):
    """An SD-card outline: a rectangle with the top right corner cut."""
    c = w // 4
    pts = [(x, y), (x + w - c, y), (x + w, y + c), (x + w, y + h), (x, y + h)]
    d.polygon(pts, fill=fill)
    d.line(pts + [pts[0]], fill=outline, width=width, joint="curve")
    return c


def chevrons(d, x, y, size, n, color, width):
    for i in range(n):
        cx = x + i * size * 0.7
        d.line([(cx, y - size / 2), (cx + size / 2, y), (cx, y + size / 2)],
               fill=color, width=width, joint="curve")


def save_p(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=255, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)


def icon():
    s = 128
    img = gradient(s, s, BG0, BG1, radial=True)
    d = ImageDraw.Draw(img)
    # the bubble is shown as a circle: keep everything inside r ~ 56
    card(d, 38, 24, 52, 66, ACCENT, PANEL, 4)
    for i in range(4):                               # the card's contacts
        d.rectangle([46 + i * 9, 30, 50 + i * 9, 40], fill=EDGE)
    chevrons(d, 48, 66, 18, 3, GOOD, 5)
    text(img, 64 - text_width("memio", "ter18") // 2, 94, "memio", FG, "ter18")
    save_p(img, "tester/sce_sys/icon0.png")


def background():
    w, h = 840, 500
    img = gradient(w, h, BG0, BG1)
    d = ImageDraw.Draw(img)
    for x in range(0, w, 40):                        # a faint grid
        d.line([(x, 0), (x, h)], fill=(18, 22, 34))
    for y in range(0, h, 40):
        d.line([(0, y), (w, y)], fill=(18, 22, 34))
    card(d, 590, 150, 190, 250, EDGE, (16, 20, 32), 6)
    for i in range(4):
        d.rectangle([616 + i * 34, 170, 632 + i * 34, 210], fill=(30, 38, 58))
    chevrons(d, 625, 320, 56, 3, (40, 110, 70), 12)
    # the start button sits in the bottom middle (about x 280..560, y 290..):
    # the text stays above it
    text(img, 60, 40, "vita-memio", ACCENT, "ter24", 3)
    text(img, 64, 124, "storage tester", FG, "ter24", 2)
    d.line([(64, 186), (500, 186)], fill=EDGE, width=2)
    text(img, 64, 202, "SD2Vita  memory card  internal storage", DIM, "ter18")
    text(img, 64, 228, "read and write speed, every byte checked", DIM, "ter18")
    save_p(img, "tester/sce_sys/livearea/contents/bg.png")


def startup():
    w, h = 280, 158
    img = gradient(w, h, BG0, BG1)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([4, 4, w - 5, h - 5], radius=14, fill=PANEL, outline=ACCENT, width=3)
    text(img, w // 2 - text_width("vita-memio", "ter24") // 2, 34, "vita-memio", ACCENT, "ter24")
    text(img, w // 2 - text_width("TESTER", "ter18") // 2, 66, "TESTER", FG, "ter18")
    chevrons(d, w // 2 - 22, 116, 24, 3, GOOD, 5)
    save_p(img, "tester/sce_sys/livearea/contents/startup.png")


if __name__ == "__main__":
    icon()
    background()
    startup()
    print("wrote icon0.png, bg.png, startup.png")
