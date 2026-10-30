#!/usr/bin/env python3
"""gen_art.py - draws vita-memio's artwork (all original):

  assets/logo.png                              the logo for the README
  assets/logo-mark.png                         the mark alone, 512x512
  tester/sce_sys/icon0.png                     128x128  the tester's bubble
  tester/sce_sys/livearea/contents/bg.png      840x500  its LiveArea background
  tester/sce_sys/livearea/contents/startup.png 280x158  its start button

The mark: a memory card in a cyan-to-violet gradient with a lightning bolt,
on a deep indigo tile. Everything is drawn at 4x and scaled down, so edges are
smooth. The Vita wants 8-bit palette PNGs for the bubble and the LiveArea, so
those are quantised (with dithering, to keep the gradients smooth).

Text uses the Inter typeface (SIL Open Font License), which must be installed
(Debian/Ubuntu: fonts-inter). Run from the repo root:
    python3 tools/gen_art.py        (needs Pillow)
"""
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

SS = 4                                   # supersampling

INK = (7, 10, 24)                        # the darkest background
TILE_A = (40, 30, 92)                    # tile gradient, top left ...
TILE_B = (9, 13, 33)                     # ... to bottom right
CYAN = (34, 211, 238)
VIOLET = (139, 92, 246)
PINK = (236, 72, 153)
WHITE = (248, 250, 252)
SOFT = (165, 180, 252)                   # indigo-200
DIM = (148, 163, 184)                    # slate-400

FONT_DIRS = ["/usr/share/fonts/opentype/inter", "/usr/share/fonts/truetype/inter",
             os.path.expanduser("~/.fonts"), "/Library/Fonts", "C:/Windows/Fonts"]


def font(name, px):
    for d in FONT_DIRS:
        for ext in (".otf", ".ttf"):
            p = os.path.join(d, name + ext)
            if os.path.exists(p):
                return ImageFont.truetype(p, px)
    raise SystemExit("Inter not found (%s): install fonts-inter" % name)


# ---- small helpers ------------------------------------------------------------

def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def linear(size, c0, c1, angle_deg=45.0, stops=None):
    """A linear gradient image (RGB) across `size` at `angle_deg`."""
    w, h = size
    small = (max(2, w // 8), max(2, h // 8))
    img = Image.new("RGB", small)
    px = img.load()
    a = math.radians(angle_deg)
    dx, dy = math.cos(a), math.sin(a)
    corners = [x * dx + y * dy for x in (0, small[0]) for y in (0, small[1])]
    lo, hi = min(corners), max(corners)
    for y in range(small[1]):
        for x in range(small[0]):
            t = ((x * dx + y * dy) - lo) / (hi - lo)
            if stops:
                for i in range(len(stops) - 1):
                    t0, ca = stops[i]
                    t1, cb = stops[i + 1]
                    if t <= t1 or i == len(stops) - 2:
                        u = 0 if t1 == t0 else min(1, max(0, (t - t0) / (t1 - t0)))
                        px[x, y] = lerp(ca, cb, u)
                        break
            else:
                px[x, y] = lerp(c0, c1, t)
    return img.resize(size, Image.BICUBIC)


def radial(size, center, radius, color, strength=1.0):
    """An RGBA glow: `color`, fading out from `center` over `radius`."""
    w, h = size
    small = (max(2, w // 8), max(2, h // 8))
    m = Image.new("L", small)
    px = m.load()
    cx, cy, r = center[0] / 8, center[1] / 8, radius / 8
    for y in range(small[1]):
        for x in range(small[0]):
            d = math.hypot(x - cx, y - cy) / r
            v = max(0.0, 1.0 - d)
            px[x, y] = int(255 * strength * v * v)
    m = m.resize(size, Image.BICUBIC)
    out = Image.new("RGBA", size, color + (0,))
    out.putalpha(m)
    return out


def card_polygon(x, y, w, h, notch):
    """A memory card: a rectangle with its top right corner cut."""
    return [(x, y), (x + w - notch, y), (x + w, y + notch), (x + w, y + h), (x, y + h)]


def rounded_poly_mask(size, pts, radius):
    """A mask of a polygon with rounded corners (drawn, then smoothed)."""
    m = Image.new("L", size, 0)
    d = ImageDraw.Draw(m)
    d.polygon(pts, fill=255)
    if radius > 0:
        # blur and cut at the middle: convex corners come out round
        m = m.filter(ImageFilter.GaussianBlur(radius)).point(lambda v: 255 if v >= 128 else 0)
    return m


def bolt_points(cx, cy, s):
    """A lightning bolt, about s wide and 1.55*s tall, centred at (cx, cy)."""
    p = [(0.18, -0.78), (-0.42, 0.08), (-0.02, 0.08), (-0.20, 0.78),
         (0.44, -0.12), (0.04, -0.12), (0.26, -0.78)]
    return [(cx + x * s, cy + y * s) for x, y in p]


def paste_fill(dst, fill_img, mask):
    dst.paste(fill_img, (0, 0), mask)


def text_gradient(dst, xy, txt, fnt, c0, c1):
    """Text filled with a left-to-right gradient."""
    m = Image.new("L", dst.size, 0)
    ImageDraw.Draw(m).text(xy, txt, font=fnt, fill=255)
    bbox = m.getbbox()
    if not bbox:
        return
    g = Image.new("RGB", dst.size, c0)
    gw = bbox[2] - bbox[0]
    g.paste(linear((gw, bbox[3] - bbox[1]), c0, c1, 0), (bbox[0], bbox[1]))
    dst.paste(g, (0, 0), m)


# ---- the mark -------------------------------------------------------------------

def draw_card(img, cx, cy, h, glow=True):
    """The card (gradient, contacts, bolt) centred at (cx, cy), h tall."""
    w = h * 0.78
    x, y = cx - w / 2, cy - h / 2
    notch = w * 0.30
    pts = card_polygon(x, y, w, h, notch)
    r = max(1, int(h * 0.055))
    mask = rounded_poly_mask(img.size, pts, r)
    if glow:
        g = Image.new("RGBA", img.size, CYAN + (0,))
        g.putalpha(mask.filter(ImageFilter.GaussianBlur(h * 0.16)).point(lambda v: v * 0.55))
        img.alpha_composite(g)
        g2 = Image.new("RGBA", img.size, VIOLET + (0,))
        g2.putalpha(mask.filter(ImageFilter.GaussianBlur(h * 0.30)).point(lambda v: v * 0.35))
        img.alpha_composite(g2)
    # the gradient spans the card itself, wherever it is
    fill = Image.new("RGBA", img.size, VIOLET + (255,))
    fill.paste(linear((int(w) + 1, int(h) + 1), CYAN, VIOLET, 60,
                      stops=[(0.0, CYAN), (0.55, VIOLET), (1.0, PINK)]).convert("RGBA"),
               (int(x), int(y)))
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    layer.paste(fill, (0, 0), mask)
    # a soft highlight along the top
    hl = Image.new("L", img.size, 0)
    ImageDraw.Draw(hl).rectangle([x, y, x + w, y + h * 0.42], fill=60)
    hl = ImageChops.multiply(hl, mask).filter(ImageFilter.GaussianBlur(h * 0.05))
    white = Image.new("RGBA", img.size, WHITE + (0,))
    white.putalpha(hl)
    layer.alpha_composite(white)
    img.alpha_composite(layer)
    d = ImageDraw.Draw(img)
    # contacts: four dark pads near the top
    cw, ch = w * 0.085, h * 0.17
    gap = (w - notch * 0.55 - 4 * cw) / 5
    for i in range(4):
        px = x + gap * (i + 1) + cw * i
        d.rounded_rectangle([px, y + h * 0.08, px + cw, y + h * 0.08 + ch],
                            radius=cw * 0.45, fill=TILE_B + (170,))
    # the bolt, white, with a little shadow
    b = bolt_points(cx + w * 0.02, cy + h * 0.16, h * 0.33)
    sh = Image.new("L", img.size, 0)
    ImageDraw.Draw(sh).polygon([(px_ + h * 0.012, py + h * 0.02) for px_, py in b], fill=110)
    sh = sh.filter(ImageFilter.GaussianBlur(h * 0.02))
    dark = Image.new("RGBA", img.size, TILE_B + (0,))
    dark.putalpha(sh)
    img.alpha_composite(dark)
    d.polygon(b, fill=WHITE + (255,))


def mark(size):
    """The mark on its rounded tile, RGBA, size x size."""
    S = size * SS
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    tile = Image.new("L", (S, S), 0)
    ImageDraw.Draw(tile).rounded_rectangle([0, 0, S - 1, S - 1], radius=S * 0.23, fill=255)
    bg = linear((S, S), TILE_A, TILE_B, 50).convert("RGBA")
    bg.alpha_composite(radial((S, S), (S * 0.25, S * 0.18), S * 0.75, VIOLET, 0.45))
    bg.alpha_composite(radial((S, S), (S * 0.85, S * 0.95), S * 0.6, CYAN, 0.18))
    img.paste(bg, (0, 0), tile)
    # a thin light rim
    rim = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(rim).rounded_rectangle([S * 0.006, S * 0.006, S * 0.994, S * 0.994],
                                          radius=S * 0.225, outline=(255, 255, 255, 34),
                                          width=max(1, int(S * 0.008)))
    img.alpha_composite(rim)
    draw_card(img, S * 0.5, S * 0.5, S * 0.60)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), tile))
    return img.resize((size, size), Image.LANCZOS)


# ---- outputs -------------------------------------------------------------------

def save_rgba(img, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, optimize=True)


def save_p(img, path):
    """8-bit palette PNG, as the Vita wants for the bubble and the LiveArea."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                                dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)


def space_bg(w, h, glows):
    """A dark background with soft colour glows and a faint dot grid."""
    img = linear((w, h), (14, 17, 42), INK, 70).convert("RGBA")
    for (gx, gy, gr, col, st) in glows:
        img.alpha_composite(radial((w, h), (gx, gy), gr, col, st))
    dots = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(dots)
    step = max(8, w // 42)
    for y in range(step // 2, h, step):
        for x in range(step // 2, w, step):
            r = max(1, w // 900)
            d.ellipse([x - r, y - r, x + r, y + r], fill=(255, 255, 255, 14))
    img.alpha_composite(dots)
    return img


def icon0():
    S = 128 * SS
    img = space_bg(S, S, [(S * 0.3, S * 0.2, S * 0.9, VIOLET, 0.55),
                          (S * 0.8, S * 0.9, S * 0.7, CYAN, 0.30)])
    # the bubble is shown as a circle: keep the card well inside it
    draw_card(img, S * 0.5, S * 0.5, S * 0.58)
    save_p(img.resize((128, 128), Image.LANCZOS), "tester/sce_sys/icon0.png")


def background():
    W, H = 840 * SS, 500 * SS
    img = space_bg(W, H, [(W * 0.78, H * 0.45, W * 0.45, VIOLET, 0.55),
                          (W * 0.95, H * 0.95, W * 0.35, CYAN, 0.35),
                          (W * 0.05, H * 0.0, W * 0.4, (59, 130, 246), 0.25)])
    draw_card(img, W * 0.80, H * 0.46, H * 0.62)
    d = ImageDraw.Draw(img)
    # The start button sits in the bottom middle: the text stays above it.
    x0, y0 = 62 * SS, 70 * SS
    f_title = font("Inter-ExtraBold", 74 * SS)
    d.text((x0, y0), "vita-", font=f_title, fill=WHITE)
    tw = d.textlength("vita-", font=f_title)
    text_gradient(img, (x0 + tw, y0), "memio", f_title, CYAN, VIOLET)
    d.text((x0 + 3 * SS, y0 + 98 * SS), "Storage Tester", font=font("Inter-SemiBold", 32 * SS),
           fill=SOFT)
    d.rounded_rectangle([x0 + 3 * SS, y0 + 152 * SS, x0 + 63 * SS, y0 + 156 * SS],
                        radius=2 * SS, fill=CYAN)
    f_small = font("Inter-Medium", 19 * SS)
    d.text((x0 + 3 * SS, y0 + 172 * SS), "Read and write speed, every byte checked",
           font=f_small, fill=DIM)
    d.text((x0 + 3 * SS, y0 + 200 * SS), "SD2Vita  \u00b7  Memory card  \u00b7  Internal storage",
           font=f_small, fill=DIM)
    save_p(img.resize((840, 500), Image.LANCZOS), "tester/sce_sys/livearea/contents/bg.png")


def startup():
    W, H = 280 * SS, 158 * SS
    img = space_bg(W, H, [(W * 0.15, H * 0.2, W * 0.7, VIOLET, 0.5),
                          (W * 0.9, H * 1.0, W * 0.6, CYAN, 0.3)])
    d = ImageDraw.Draw(img)
    # a gradient frame
    frame = Image.new("L", (W, H), 0)
    fd = ImageDraw.Draw(frame)
    fd.rounded_rectangle([3 * SS, 3 * SS, W - 4 * SS, H - 4 * SS], radius=16 * SS, fill=255)
    fd.rounded_rectangle([6 * SS, 6 * SS, W - 7 * SS, H - 7 * SS], radius=13 * SS, fill=0)
    img.paste(linear((W, H), CYAN, VIOLET, 20).convert("RGBA"), (0, 0), frame)
    draw_card(img, 58 * SS, H / 2, 84 * SS)
    f1 = font("Inter-ExtraBold", 27 * SS)
    tx = 104 * SS
    d.text((tx, 46 * SS), "vita-", font=f1, fill=WHITE)
    tw = d.textlength("vita-", font=f1)
    text_gradient(img, (tx + tw, 46 * SS), "memio", f1, CYAN, VIOLET)
    d.text((tx + 2 * SS, 86 * SS), "TESTER", font=font("Inter-Bold", 17 * SS), fill=SOFT)
    save_p(img.resize((280, 158), Image.LANCZOS), "tester/sce_sys/livearea/contents/startup.png")


def logo():
    """The README logo: the mark and the name on a dark rounded banner."""
    W, H = 1280 * SS, 400 * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    banner = Image.new("L", (W, H), 0)
    ImageDraw.Draw(banner).rounded_rectangle([0, 0, W - 1, H - 1], radius=40 * SS, fill=255)
    bg = space_bg(W, H, [(W * 0.12, H * 0.3, W * 0.35, VIOLET, 0.55),
                         (W * 0.95, H * 1.05, W * 0.45, CYAN, 0.28),
                         (W * 0.6, H * -0.2, W * 0.4, (59, 130, 246), 0.18)])
    img.paste(bg, (0, 0), banner)
    m = mark(250).resize((250 * SS, 250 * SS), Image.LANCZOS)
    img.alpha_composite(m, (75 * SS, 75 * SS))
    d = ImageDraw.Draw(img)
    x0 = 380 * SS
    f_title = font("Inter-ExtraBold", 120 * SS)
    d.text((x0, 88 * SS), "vita-", font=f_title, fill=WHITE)
    tw = d.textlength("vita-", font=f_title)
    text_gradient(img, (x0 + tw, 88 * SS), "memio", f_title, CYAN, VIOLET)
    d.text((x0 + 6 * SS, 250 * SS), "Faster storage for PS Vita", font=font("Inter-SemiBold", 44 * SS),
           fill=SOFT)
    img.putalpha(ImageChops.multiply(img.getchannel("A"), banner))
    save_rgba(img.resize((1280, 400), Image.LANCZOS), "assets/logo.png")
    save_rgba(mark(512), "assets/logo-mark.png")


if __name__ == "__main__":
    logo()
    icon0()
    background()
    startup()
    print("wrote assets/logo.png, assets/logo-mark.png, icon0.png, bg.png, startup.png")
