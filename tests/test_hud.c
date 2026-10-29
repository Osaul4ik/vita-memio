/*
 * test_hud.c - the HUD's drawing (kernel/hud_draw.h) into an ordinary buffer:
 * both styles at the three framebuffer sizes games use, nothing written outside
 * the HUD's own box, colour codes take no room. With HUD_PPM=1 it also writes
 * hud_<style>_<w>.ppm to look at.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/hud_draw.h"
#include "vm_test.h"

#define BG 0xFF336699u              /* "the game's picture"                    */

static void set_text(HudText *t, int style, const char *const *lines, int n)
{
	memset(t, 0, sizeof(*t));
	for (int i = 0; i < n; i++) {
		strncpy(t->lines[i], lines[i], VM_HUD_LINE_MAX - 1);
		if (hud_visible_len(t->lines[i]) > t->maxlen)
			t->maxlen = hud_visible_len(t->lines[i]);
	}
	t->n = n;
	t->style = style;
}

static void dump(const uint32_t *fb, int pitch, int w, int h, const char *name)
{
	FILE *f;

	if (!getenv("HUD_PPM") || !(f = fopen(name, "wb")))
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t p = fb[y * pitch + x];
			unsigned char c[3] = { p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF };

			fwrite(c, 1, 3, f);
		}
	fclose(f);
}

/* The bounding box of everything that is not the background. */
static void changed_box(const uint32_t *fb, int pitch, int w, int h,
                        int *x0, int *y0, int *x1, int *y1)
{
	*x0 = w; *y0 = h; *x1 = -1; *y1 = -1;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			if (fb[y * pitch + x] != BG) {
				if (x < *x0) *x0 = x;
				if (y < *y0) *y0 = y;
				if (x > *x1) *x1 = x;
				if (y > *y1) *y1 = y;
			}
}

static const char *const k_lines[] = {
	HUD_C_TEXT "SD2VITA  " HUD_C_DIM "R " HUD_C_GOOD " 17.3 " HUD_C_DIM "W " HUD_C_GOOD "  5.2 " HUD_C_DIM "MB/s",
	HUD_C_DIM "  812/96 MiB " HUD_C_GOOD "ON " HUD_C_DIM "ERR " HUD_C_DIM "0 " HUD_C_WARN "VERIFY",
	HUD_C_TEXT "MEMCARD  " HUD_C_DIM "R " HUD_C_BAD "  0.0 " HUD_C_DIM "W " HUD_C_BAD "  0.0 " HUD_C_DIM "MB/s " HUD_C_BAD "ERROR",
	HUD_C_DIM "  3/0 MiB " HUD_C_BAD "STANDARD " HUD_C_DIM "ERR " HUD_C_BAD "1",
};

static void test_panel_sizes(void)
{
	static const int sizes[][2] = { { 960, 544 }, { 720, 408 }, { 640, 368 } };
	static uint32_t row[HUD_ROW_MAX];
	HudText t;

	set_text(&t, VM_HUD_STYLE_PSVSHELL, k_lines, 4);
	CHECK(t.maxlen == (int)strlen("MEMCARD  R   0.0 W   0.0 MB/s ERROR"));
	for (int k = 0; k < 3; k++) {
		int w = sizes[k][0], h = sizes[k][1], pitch = w == 960 ? 960 : 1024;
		uint32_t *fb = malloc((size_t)pitch * h * 4);   /* exactly the buffer: ASan */
		HudFont f = hud_font_for(w);
		int x0, y0, x1, y1;
		char name[64];

		for (int i = 0; i < pitch * h; i++)
			fb[i] = BG;
		hud_draw_panel(fb, pitch, w, h, &t, row);
		changed_box(fb, pitch, w, h, &x0, &y0, &x1, &y1);
		/* top right, clear of the status bar, the size the text needs */
		CHECK(x1 == w - 1 - (w / 96 > 4 ? w / 96 : 4));
		CHECK(y0 == h * 36 / 544);
		CHECK(x1 - x0 + 1 == t.maxlen * f.w + 2 * f.w);
		CHECK(y1 - y0 + 1 == t.n * f.h + 2 * (f.h / 3));
		/* nothing beyond the pitch's visible part either */
		for (int y = 0; y < h; y++)
			for (int x = w; x < pitch; x++)
				CHECK(fb[y * pitch + x] == BG);
		/* rounded: the corner pixel is left alone, the middle of the edge is not */
		CHECK(fb[y0 * pitch + x0] == BG && fb[y0 * pitch + x1] == BG);
		CHECK(fb[y0 * pitch + (x0 + x1) / 2] == HUD_PANEL_BG);
		/* coloured text is there: the green of "17.3" */
		{
			int green = 0;

			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++)
					green |= fb[y * pitch + x] == hud_color(3);
			CHECK(green);
		}
		snprintf(name, sizeof(name), "hud_psvshell_%d.ppm", w);
		dump(fb, pitch, w, h, name);
		free(fb);
	}
}

static void test_panel_limits(void)
{
	static uint32_t row[HUD_ROW_MAX];
	static const char *const one[] = { "x" };
	char wide[VM_HUD_LINE_MAX];
	const char *const wl[] = { wide };
	uint32_t *fb = malloc(960 * 544 * 4);
	HudText t;
	int x0, y0, x1, y1;

	/* the widest line that fits the row buffer still draws, in bounds */
	memset(wide, 'W', sizeof(wide) - 1);
	wide[sizeof(wide) - 1] = 0;
	set_text(&t, VM_HUD_STYLE_PSVSHELL, wl, 1);
	for (int i = 0; i < 960 * 544; i++) fb[i] = BG;
	hud_draw_panel(fb, 960, 960, 544, &t, row);
	changed_box(fb, 960, 960, 544, &x0, &y0, &x1, &y1);
	CHECK(x1 > 0 && x1 < 960);

	/* a framebuffer too small for the box: nothing drawn at all */
	set_text(&t, VM_HUD_STYLE_PSVSHELL, k_lines, 4);
	for (int i = 0; i < 960 * 544; i++) fb[i] = BG;
	hud_draw_panel(fb, 200, 200, 60, &t, row);
	changed_box(fb, 960, 960, 544, &x0, &y0, &x1, &y1);
	CHECK(x1 < 0);

	/* no lines: nothing drawn */
	set_text(&t, VM_HUD_STYLE_PSVSHELL, one, 0);
	hud_draw_panel(fb, 960, 960, 544, &t, row);
	changed_box(fb, 960, 960, 544, &x0, &y0, &x1, &y1);
	CHECK(x1 < 0);
	free(fb);
}

static void test_classic(void)
{
	uint32_t *fb = malloc(960 * 544 * 4);
	HudText t;
	int x0, y0, x1, y1;

	set_text(&t, VM_HUD_STYLE_CLASSIC, k_lines, 4);
	for (int i = 0; i < 960 * 544; i++) fb[i] = BG;
	hud_draw_classic(fb, 960, 960, 544, &t);
	changed_box(fb, 960, 960, 544, &x0, &y0, &x1, &y1);
	CHECK(x1 <= 960 - 24 && x1 > 960 - 24 - 16 * 2);   /* the top right corner */
	CHECK(y0 >= 48 && y1 < 48 + 4 * 20 + 2);
	CHECK(x0 >= 960 - 24 - t.maxlen * 16);
	dump(fb, 960, 960, 544, "hud_classic_960.ppm");
	free(fb);
}

static void test_corner(void)
{
	CHECK(hud_corner_inset(0, 8) > 0);
	CHECK(hud_corner_inset(8, 8) == 0 && hud_corner_inset(20, 8) == 0);
	for (int r = 1; r < 8; r++)
		CHECK(hud_corner_inset(r, 8) <= hud_corner_inset(r - 1, 8));
	CHECK(hud_visible_len(HUD_C_TEXT "ab" HUD_C_BAD "c") == 3);
}

int main(void)
{
	RUN(test_panel_sizes);
	RUN(test_panel_limits);
	RUN(test_classic);
	RUN(test_corner);
	printf(vm_failures ? "test_hud: %d FAILED\n" : "test_hud: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
