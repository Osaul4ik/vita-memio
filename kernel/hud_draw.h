/*
 * hud_draw.h - the HUD's drawing, kept free of kernel calls so the host tests
 * can draw into an ordinary buffer (tests/test_hud.c). Included by hud.c only;
 * memcpy must already be declared (sysclib in the kernel, string.h on a PC).
 *
 * The panel sits in the top right corner, below the system status bar's
 * height so they never cover the clock and battery on the home screen.
 */
#ifndef VITAMEMIO_HUD_DRAW_H
#define VITAMEMIO_HUD_DRAW_H

#include <stdint.h>

#include "hud.h"
#include "../include/vitamemio.h"
#include "../include/vm_font_terminus.h"

typedef struct {
	char     lines[VM_HUD_LINES][VM_HUD_LINE_MAX];
	int      n;
	int      maxlen;   /* longest line in characters, codes not counted     */
	int      left;     /* 1: the top left corner (the overlay)              */
	uint32_t gen;      /* changes whenever the text does (for the cache)    */
} HudText;

/* Widest panel the row buffer is for: VM_HUD_LINE_MAX characters of 12 px
 * plus the padding. */
#define HUD_ROW_MAX  ((VM_HUD_LINE_MAX + 2) * 12)

#define HUD_ABGR(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static inline int hud_is_code(char c)
{
	return (unsigned char)c >= 1 && (unsigned char)c <= HUD_C_LAST;
}

/* Characters that take room (colour codes do not). */
static inline int hud_visible_len(const char *s)
{
	int n = 0;

	for (; *s; s++)
		if (!hud_is_code(*s))
			n++;
	return n;
}

/* ---- PSVshell style: a black panel, Terminus font, colours ------------------ */

static inline uint32_t hud_color(int code)
{
	switch (code) {
	case 2:  return HUD_ABGR(150, 150, 160);   /* dim    */
	case 3:  return HUD_ABGR(80, 235, 110);    /* good   */
	case 4:  return HUD_ABGR(255, 80, 70);     /* bad    */
	case 5:  return HUD_ABGR(255, 210, 60);    /* warn   */
	case 6:  return HUD_ABGR(0, 200, 255);     /* accent */
	default: return HUD_ABGR(255, 255, 255);   /* text   */
	}
}

#define HUD_PANEL_BG  HUD_ABGR(0, 0, 0)

/* glyphs: 16 bits a row; glyphs8 instead for a font 8 px wide (8x14). */
typedef struct { const uint16_t *glyphs; const uint8_t *glyphs8; int w, h; } HudFont;

/* Row `gy` of character `c`'s glyph, the leftmost pixel in bit 15. */
static inline uint16_t hud_glyph_row(const HudFont *f, unsigned char c, int gy)
{
	const int i = (c - TER_FIRST) * f->h + gy;

	return f->glyphs8 ? (uint16_t)(f->glyphs8[i] << 8) : f->glyphs[i];
}

/* The font for a framebuffer `fb_w` wide: the one nearest the size the 24 px
 * font has on a 960 one, as the display scales the frame to the screen - so
 * the panel looks the same size in every game (an 800 wide frame, shown
 * 1.2 times larger, takes the 18 px font, not the 24). A tie takes the
 * smaller. */
static inline HudFont hud_font_for(int fb_w)
{
	HudFont f;
	const int want = TER24_H * fb_w;              /* x 960: the size wanted */
	const int d14 = TER14_H * 960 - want, d18 = TER18_H * 960 - want, d24 = TER24_H * 960 - want;
	const int a14 = d14 < 0 ? -d14 : d14, a18 = d18 < 0 ? -d18 : d18, a24 = d24 < 0 ? -d24 : d24;

	f.glyphs8 = 0;
	if (a14 <= a18 && a14 <= a24) {
		f.glyphs = 0; f.glyphs8 = &ter14[0][0]; f.w = TER14_W; f.h = TER14_H;
	} else if (a18 <= a24) {
		f.glyphs = &ter18[0][0]; f.w = TER18_W; f.h = TER18_H;
	} else {
		f.glyphs = &ter24[0][0]; f.w = TER24_W; f.h = TER24_H;
	}
	return f;
}

/* How far row `r` of a `radius` rounded corner is cut in (r counted from the
 * box edge). */
static inline int hud_corner_inset(int r, int radius)
{
	int dy = radius - r, i = 0;

	if (r >= radius)
		return 0;
	while (i < radius) {
		int dx = radius - i;

		if (dx * dx + dy * dy <= radius * radius)
			break;
		i++;
	}
	return i;
}

/* Where the panel goes on a w x h framebuffer, and its size. */
typedef struct {
	HudFont f;
	int pad_x, pad_y, radius, box_w, box_h, x0, y0;
} HudGeom;

/* 0 if the panel cannot be drawn on this framebuffer (or there is no text). */
static inline int hud_panel_geom(int w, int h, const HudText *t, HudGeom *g)
{
	const int margin = w / 96 > 4 ? w / 96 : 4;

	g->f = hud_font_for(w);
	g->pad_x = g->f.w;
	g->pad_y = g->f.h / 3;
	g->radius = g->f.h / 3;
	g->box_w = t->maxlen * g->f.w + 2 * g->pad_x;
	g->box_h = t->n * g->f.h + 2 * g->pad_y;
	g->x0 = t->left ? margin : w - margin - g->box_w;
	g->y0 = h * 36 / 544;
	return t->n > 0 && g->box_w <= HUD_ROW_MAX && g->x0 >= 0 && g->y0 + g->box_h <= h;
}

/* Bytes a cached panel needs on a framebuffer `fb_w` wide (its font): four
 * bits a pixel, the panel has seven colours. */
static inline int hud_cache_bytes(const HudText *t, int fb_w)
{
	const HudFont f = hud_font_for(fb_w);

	return ((t->maxlen * f.w + 2 * f.w + 1) / 2) * (t->n * f.h + 2 * (f.h / 3));
}

/* How far row `r` of the box is cut in by the rounded corners. */
static inline int hud_row_inset(const HudGeom *g, int r)
{
	int top = r < g->radius ? r : g->box_h - 1 - r < g->radius ? g->box_h - 1 - r : g->radius;

	return hud_corner_inset(top, g->radius);
}

/* Row `r` of the panel into row[in .. box_w - in). */
static inline void hud_compose_row(const HudText *t, const HudGeom *g, int r, uint32_t *row)
{
	const int in = hud_row_inset(g, r);
	const int li = (r - g->pad_y) / g->f.h, gy = (r - g->pad_y) % g->f.h;

	for (int i = in; i < g->box_w - in; i++)
		row[i] = HUD_PANEL_BG;
	if (r < g->pad_y || li >= t->n)
		return;
	uint32_t color = hud_color(1);
	int cx = g->pad_x;

	for (const char *s = t->lines[li]; *s; s++) {
		unsigned char c = (unsigned char)*s;
		uint16_t bits;

		if (hud_is_code((char)c)) {
			color = hud_color(c);
			continue;
		}
		if (c >= TER_FIRST && c <= TER_LAST &&
		    (bits = hud_glyph_row(&g->f, c, gy)) != 0)
			for (int dx = 0; dx < g->f.w; dx++)
				if (bits & (0x8000u >> dx))
					row[cx + dx] = color;
		cx += g->f.w;
	}
}

/* Draws the panel row by row: each row is composed in `row` (at least
 * HUD_ROW_MAX words of ordinary memory) and copied into the framebuffer in one
 * go, so the framebuffer is only ever written in straight runs. */
static inline void hud_draw_panel(uint32_t *fb, int pitch, int w, int h, const HudText *t,
                                  uint32_t *row)
{
	HudGeom g;

	if (!hud_panel_geom(w, h, t, &g))
		return;
	for (int r = 0; r < g.box_h; r++) {
		int in = hud_row_inset(&g, r);

		hud_compose_row(t, &g, r, row);
		memcpy(fb + (g.y0 + r) * pitch + g.x0 + in, row + in, (size_t)(g.box_w - 2 * in) * 4);
	}
}

/* The panel kept composed between frames, at four bits a pixel (0: the panel's
 * background, else a colour code): built again only when the text or the
 * framebuffer size changes, otherwise each frame only expands it, row by row. */
typedef struct {
	uint8_t  *px;
	int       bytes;          /* size of px                                 */
	uint32_t  gen;            /* what is in it: text generation ...         */
	int       w, h;           /* ... for this framebuffer size (w = 0: none) */
} HudCache;

/* Row `r` of the panel in `stride` bytes, four bits a pixel, high nibble first. */
static inline void hud_pack_row(const HudText *t, const HudGeom *g, int r, uint8_t *out, int stride)
{
	const int li = (r - g->pad_y) / g->f.h, gy = (r - g->pad_y) % g->f.h;

	for (int i = 0; i < stride; i++)
		out[i] = 0;
	if (r < g->pad_y || li >= t->n)
		return;
	{
		unsigned color = 1;
		int cx = g->pad_x;

		for (const char *s = t->lines[li]; *s; s++) {
			unsigned char c = (unsigned char)*s;
			uint16_t bits;

			if (hud_is_code((char)c)) {
				color = c;
				continue;
			}
			if (c >= TER_FIRST && c <= TER_LAST &&
			    (bits = hud_glyph_row(&g->f, c, gy)) != 0)
				for (int dx = 0; dx < g->f.w; dx++)
					if (bits & (0x8000u >> dx)) {
						const int x = cx + dx;

						out[x >> 1] |= (uint8_t)((x & 1) ? color : color << 4);
					}
			cx += g->f.w;
		}
	}
}

/* 0 if the cache is too small for this panel (then draw it uncached). `row`: at
 * least HUD_ROW_MAX words of ordinary memory, for the copy in one straight run. */
static inline int hud_draw_panel_cached(uint32_t *fb, int pitch, int w, int h, const HudText *t,
                                        HudCache *c, uint32_t *row)
{
	uint32_t pal[16];
	HudGeom g;
	int stride;

	if (!hud_panel_geom(w, h, t, &g))
		return 1;                       /* nothing to draw is not a failure */
	stride = (g.box_w + 1) / 2;
	if (!c->px || stride * g.box_h > c->bytes)
		return 0;
	if (c->gen != t->gen || c->w != w || c->h != h) {
		for (int r = 0; r < g.box_h; r++)
			hud_pack_row(t, &g, r, c->px + r * stride, stride);
		c->gen = t->gen;
		c->w = w;
		c->h = h;
	}
	pal[0] = HUD_PANEL_BG;
	for (int i = 1; i < 16; i++)
		pal[i] = hud_color(i);
	for (int r = 0; r < g.box_h; r++) {
		const int in = hud_row_inset(&g, r), end = g.box_w - in;
		const uint8_t *p = c->px + r * stride;
		int x = in;

		if (x & 1) {
			row[x] = pal[p[x >> 1] & 15];
			x++;
		}
		for (; x + 1 < end; x += 2) {
			const uint8_t b = p[x >> 1];

			row[x] = pal[b >> 4];
			row[x + 1] = pal[b & 15];
		}
		if (x < end)
			row[x] = pal[p[x >> 1] >> 4];
		memcpy(fb + (g.y0 + r) * pitch + g.x0 + in, row + in, (size_t)(end - in) * 4);
	}
	return 1;
}

#endif /* VITAMEMIO_HUD_DRAW_H */
