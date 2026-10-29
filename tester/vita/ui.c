/*
 * ui.c - see ui.h. Draws into a normal-memory back buffer and copies it to the
 * display memory once per frame.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>

#include "../../include/vm_font_terminus.h"
#include "ui.h"

#define FB_BYTES 0x200000          /* 960*544*4 = 2088960, rounded up to 2 MiB  */

static uint32_t *g_back;
static uint32_t *g_front;
static SceUID    g_block = -1;

int ui_init(void)
{
	void *base = NULL;

	g_back = malloc((size_t)UI_W * UI_H * 4);
	if (!g_back)
		return -1;
	g_block = sceKernelAllocMemBlock("vm_tester_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
	                                 FB_BYTES, NULL);
	if (g_block < 0 || sceKernelGetMemBlockBase(g_block, &base) < 0 || !base) {
		free(g_back);
		g_back = NULL;
		return -1;
	}
	g_front = base;
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);
	ui_clear();
	ui_present();
	return 0;
}

void ui_shutdown(void)
{
	if (g_block >= 0)
		sceKernelFreeMemBlock(g_block);
	g_block = -1;
	free(g_back);
	g_back = NULL;
}

/* ---- pixels ------------------------------------------------------------------ */

void ui_fill(int x, int y, int w, int h, uint32_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > UI_W) w = UI_W - x;
	if (y + h > UI_H) h = UI_H - y;
	for (int j = 0; j < h; j++) {
		uint32_t *p = g_back + (size_t)(y + j) * UI_W + x;

		for (int i = 0; i < w; i++)
			p[i] = color;
	}
}

void ui_frame(int x, int y, int w, int h, uint32_t color)
{
	ui_fill(x, y, w, 1, color);
	ui_fill(x, y + h - 1, w, 1, color);
	ui_fill(x, y, 1, h, color);
	ui_fill(x + w - 1, y, 1, h, color);
}

void ui_clear(void)
{
	ui_fill(0, 0, UI_W, UI_H, UI_BG);
}

static void plot(int x, int y, uint32_t color)
{
	if (x >= 0 && y >= 0 && x < UI_W && y < UI_H)
		g_back[(size_t)y * UI_W + x] = color;
}

static void glyph(int x, int y, const uint16_t *rows, int w, int h, uint32_t color)
{
	for (int dy = 0; dy < h; dy++) {
		uint16_t r = rows[dy];

		for (int dx = 0; dx < w && r; dx++)
			if (r & (0x8000u >> dx))
				plot(x + dx, y + dy, color);
	}
}

/* ---- button icons (in text: \x01 cross, \x02 circle, \x03 up/down,
 * \x04 left/right, \x05 L, \x06 R); each takes two cells. -------------------- */

#define ICON_CROSS  UI_RGB(120, 176, 236)
#define ICON_CIRCLE UI_RGB(255, 104, 104)

static void icon(int x, int y, int code)
{
	const int cx = x + 9, cy = y + 9;

	switch (code) {
	case 1:                                       /* cross */
		for (int i = -5; i <= 5; i++)
			for (int t = 0; t < 2; t++) {
				plot(cx + i + t, cy + i, ICON_CROSS);
				plot(cx + i + t, cy - i, ICON_CROSS);
			}
		break;
	case 2:                                       /* circle: a ring of radius 6 */
		for (int j = -7; j <= 7; j++)
			for (int i = -7; i <= 7; i++) {
				int d = i * i + j * j;

				if (d >= 22 && d <= 42)
					plot(cx + i, cy + j, ICON_CIRCLE);
			}
		break;
	case 3:                                       /* up/down arrows */
	case 4:                                       /* left/right arrows */
		for (int k = 0; k < 5; k++)
			for (int i = -k; i <= k; i++) {
				if (code == 3) {
					plot(cx + i, cy - 7 + k, UI_FG);
					plot(cx + i, cy + 7 - k, UI_FG);
				} else {
					plot(cx - 7 + k, cy + i, UI_FG);
					plot(cx + 7 - k, cy + i, UI_FG);
				}
			}
		break;
	case 5:                                       /* shoulder buttons */
	case 6:
		ui_frame(x + 1, y + 3, 18, 13, UI_DIM);
		glyph(x + 5, y, ter18[(code == 5 ? 'L' : 'R') - TER_FIRST], TER18_W, TER18_H, UI_FG);
		break;
	default:
		break;
	}
}

/* ---- text ---------------------------------------------------------------------- */

void ui_big(int x, int y, uint32_t color, const char *s)
{
	for (; *s; s++, x += TER24_W)
		if ((unsigned char)*s >= TER_FIRST && (unsigned char)*s <= TER_LAST)
			glyph(x, y, ter24[(unsigned char)*s - TER_FIRST], TER24_W, TER24_H, color);
}

int ui_big_width(const char *s)
{
	return (int)strlen(s) * TER24_W;
}

/* Text in the 10x18 font from pixel (x, y), on the cell grid's columns. */
static void text_at(int col, int y, uint32_t color, const char *s)
{
	for (; *s && col < UI_COLS; s++) {
		unsigned char c = (unsigned char)*s;

		if (c >= 1 && c <= 6) {               /* an icon: two cells */
			if (col >= 0 && col + 1 < UI_COLS)
				icon(col * UI_CW, y, c);
			col += 2;
			continue;
		}
		if (col >= 0 && c >= TER_FIRST && c <= TER_LAST)
			glyph(col * UI_CW, y, ter18[c - TER_FIRST], TER18_W, TER18_H, color);
		col++;
	}
}

void ui_text(int col, int row, uint32_t color, const char *s)
{
	if (row >= 0 && row < UI_ROWS)
		text_at(col, UI_TOP + row * UI_CH, color, s);
}

void ui_textf(int col, int row, uint32_t color, const char *fmt, ...)
{
	char buf[UI_COLS + 1];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	ui_text(col, row, color, buf);
}

void ui_row_bg(int row, uint32_t color)
{
	ui_fill(0, UI_TOP + row * UI_CH, UI_W, UI_CH, color);
}

void ui_bar(int col, int row, int cols, uint32_t permille, uint32_t color)
{
	int x = col * UI_CW, y = UI_TOP + row * UI_CH + 4, w = cols * UI_CW, h = UI_CH - 8;
	uint32_t fill = permille > 1000 ? 1000 : permille;

	ui_fill(x, y, w, h, UI_SELBG);
	ui_fill(x, y, (int)((uint64_t)w * fill / 1000), h, color);
	ui_frame(x, y, w, h, UI_EDGE);
}

/* ---- panels, header, footer ------------------------------------------------------- */

void ui_panel(int col, int row, int cols, int rows, const char *title)
{
	int x = col * UI_CW, y = UI_TOP + row * UI_CH + UI_CH / 2;
	int w = cols * UI_CW, h = rows * UI_CH;

	ui_fill(x, y, w, h, UI_PANEL);
	ui_frame(x, y, w, h, UI_EDGE);
	if (title && *title) {
		int tw = (int)strlen(title) + 2;

		ui_fill(x + 2 * UI_CW, y - 1, tw * UI_CW, 2, UI_PANEL);
		ui_fill(x + 2 * UI_CW, UI_TOP + row * UI_CH, tw * UI_CW, UI_CH / 2, UI_BG);
		ui_text(col + 3, row, UI_ACCENT, title);
	}
}

void ui_select(int col, int row, int cols)
{
	int y = UI_TOP + row * UI_CH;

	ui_fill(col * UI_CW, y, cols * UI_CW, UI_CH, UI_SELBG);
	ui_fill(col * UI_CW, y, 3, UI_CH, UI_ACCENT);
	ui_text(col + 1, row, UI_ACCENT, ">");
	ui_text(col + cols - 2, row, UI_ACCENT, "<");
}

void ui_header(const char *title, const char *right)
{
	ui_clear();
	ui_fill(0, 0, UI_W, 2 * UI_CH + UI_TOP, UI_BAR);
	ui_fill(0, 2 * UI_CH + UI_TOP, UI_W, 2, UI_ACCENT);
	ui_big(UI_CW * 2, 7, UI_ACCENT, "vita-memio");
	ui_big(UI_CW * 2 + ui_big_width("vita-memio "), 7, UI_FG, title);
	if (right && *right)
		text_at(UI_COLS - 2 - (int)strlen(right), 11, UI_DIM, right);
}

void ui_footer(const char *hints)
{
	int y = UI_TOP + (UI_ROWS - 1) * UI_CH;

	ui_fill(0, y - 3, UI_W, UI_H - y + 3, UI_BAR);
	ui_fill(0, y - 3, UI_W, 1, UI_EDGE);
	ui_text(2, UI_ROWS - 1, UI_DIM, hints);
}

void ui_present(void)
{
	SceDisplayFrameBuf fb;

	if (!g_front)
		return;
	memcpy(g_front, g_back, (size_t)UI_W * UI_H * 4);
	memset(&fb, 0, sizeof(fb));
	fb.size = sizeof(fb);
	fb.base = g_front;
	fb.pitch = UI_W;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width = UI_W;
	fb.height = UI_H;
	sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
	sceDisplayWaitVblankStart();
}

/* ---- buttons ---------------------------------------------------------------------- */

static unsigned map_buttons(unsigned b)
{
	unsigned r = 0;

	if (b & SCE_CTRL_UP)       r |= UI_UP;
	if (b & SCE_CTRL_DOWN)     r |= UI_DOWN;
	if (b & SCE_CTRL_LEFT)     r |= UI_LEFT;
	if (b & SCE_CTRL_RIGHT)    r |= UI_RIGHT;
	if (b & SCE_CTRL_CROSS)    r |= UI_OK;
	if (b & SCE_CTRL_CIRCLE)   r |= UI_BACK;
	if (b & SCE_CTRL_LTRIGGER) r |= UI_PGUP;
	if (b & SCE_CTRL_RTRIGGER) r |= UI_PGDN;
	return r;
}

static unsigned read_buttons(void)
{
	SceCtrlData d;

	memset(&d, 0, sizeof(d));
	if (sceCtrlPeekBufferPositive(0, &d, 1) < 0)
		return 0;
	return map_buttons(d.buttons);
}

unsigned ui_poll(void)
{
	static unsigned prev;
	static int held;
	unsigned cur = read_buttons(), pressed = cur & ~prev;

	/* Up/down/left/right repeat after about a third of a second. */
	if (cur & (UI_UP | UI_DOWN | UI_LEFT | UI_RIGHT | UI_PGUP | UI_PGDN)) {
		held++;
		if (held > 20 && (held % 4) == 0)
			pressed |= cur & (UI_UP | UI_DOWN | UI_LEFT | UI_RIGHT | UI_PGUP | UI_PGDN);
	} else {
		held = 0;
	}
	prev = cur;
	return pressed;
}

int ui_back_down(void)
{
	return (read_buttons() & UI_BACK) != 0;
}
