/*
 * hud_draw.h - the HUD's drawing, kept free of kernel calls so the host tests
 * can draw into an ordinary buffer (tests/test_hud.c). Included by hud.c only;
 * memcpy must already be declared (sysclib in the kernel, string.h on a PC).
 *
 * Both looks sit in the top right corner, below the system status bar's
 * height so they never cover the clock and battery on the home screen.
 */
#ifndef VITAMEMIO_HUD_DRAW_H
#define VITAMEMIO_HUD_DRAW_H

#include <stdint.h>

#include "hud.h"
#include "font8x8.h"
#include "../include/vitamemio.h"
#include "../include/vm_font_terminus.h"

typedef struct {
	char lines[VM_HUD_LINES][VM_HUD_LINE_MAX];
	int  n;
	int  style;    /* VM_HUD_STYLE_*                                        */
	int  maxlen;   /* longest line in characters, colour codes not counted  */
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

/* ---- classic: white text with a shadow, 8x8 font at 2x -------------------- */

#define HUD_CLASSIC_FG      0xFFFFFFFFu
#define HUD_CLASSIC_SHADOW  0xFF000000u

static inline void hud_classic_char(uint32_t *fb, int pitch, int w, int h, int ox, int oy,
                                    char ch, uint32_t color)
{
	const unsigned char *g;

	if ((unsigned char)ch < VM_FONT_FIRST || (unsigned char)ch > VM_FONT_LAST)
		return;
	g = vm_font8x8[(unsigned char)ch - VM_FONT_FIRST];
	for (int row = 0; row < 8; row++) {
		unsigned char bits = g[row];

		for (int col = 0; bits && col < 8; col++, bits >>= 1) {
			if (!(bits & 1))
				continue;
			for (int sy = 0; sy < 2; sy++) {
				int y = oy + row * 2 + sy, x = ox + col * 2;

				if (y < 0 || y >= h || x < 0 || x + 1 >= w)
					continue;
				fb[y * pitch + x] = color;
				fb[y * pitch + x + 1] = color;
			}
		}
	}
}

static inline void hud_draw_classic(uint32_t *fb, int pitch, int w, int h, const HudText *t)
{
	const int line_h = 20, margin = 24;
	int x = w - margin - t->maxlen * 16, y = margin + 24;

	if (x < 0)
		x = 0;
	for (int i = 0; i < t->n; i++, y += line_h) {
		int cx = x;

		for (const char *s = t->lines[i]; *s; s++) {
			if (hud_is_code(*s))
				continue;
			hud_classic_char(fb, pitch, w, h, cx + 1, y + 1, *s, HUD_CLASSIC_SHADOW);
			hud_classic_char(fb, pitch, w, h, cx, y, *s, HUD_CLASSIC_FG);
			cx += 16;
		}
	}
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

typedef struct { const uint16_t *glyphs; int w, h; } HudFont;

static inline HudFont hud_font_for(int fb_w)
{
	HudFont f;

	if (fb_w <= 640) {
		f.glyphs = &ter14[0][0]; f.w = TER14_W; f.h = TER14_H;
	} else if (fb_w <= 720) {
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

/* Draws the panel row by row: each row is composed in `row` (at least
 * HUD_ROW_MAX words of ordinary memory) and copied into the framebuffer in one
 * go, so the framebuffer is only ever written in straight runs. */
static inline void hud_draw_panel(uint32_t *fb, int pitch, int w, int h, const HudText *t,
                                  uint32_t *row)
{
	const HudFont f = hud_font_for(w);
	const int pad_x = f.w, pad_y = f.h / 3, radius = f.h / 3;
	const int box_w = t->maxlen * f.w + 2 * pad_x, box_h = t->n * f.h + 2 * pad_y;
	const int margin = w / 96 > 4 ? w / 96 : 4;
	const int x0 = w - margin - box_w, y0 = h * 36 / 544;

	if (t->n <= 0 || box_w > HUD_ROW_MAX || x0 < 0 || y0 + box_h > h)
		return;
	for (int r = 0; r < box_h; r++) {
		int top = r < radius ? r : box_h - 1 - r < radius ? box_h - 1 - r : radius;
		int in = hud_corner_inset(top, radius);
		int li = (r - pad_y) / f.h, gy = (r - pad_y) % f.h;

		for (int i = in; i < box_w - in; i++)
			row[i] = HUD_PANEL_BG;
		if (r >= pad_y && li < t->n) {
			uint32_t color = hud_color(1);
			int cx = pad_x;

			for (const char *s = t->lines[li]; *s; s++) {
				unsigned char c = (unsigned char)*s;
				uint16_t bits;

				if (hud_is_code((char)c)) {
					color = hud_color(c);
					continue;
				}
				if (c >= TER_FIRST && c <= TER_LAST &&
				    (bits = f.glyphs[(c - TER_FIRST) * f.h + gy]) != 0)
					for (int dx = 0; dx < f.w; dx++)
						if (bits & (0x8000u >> dx))
							row[cx + dx] = color;
				cx += f.w;
			}
		}
		memcpy(fb + (y0 + r) * pitch + x0 + in, row + in, (size_t)(box_w - 2 * in) * 4);
	}
}

#endif /* VITAMEMIO_HUD_DRAW_H */
