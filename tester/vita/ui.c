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

#include "../../kernel/font8x8.h"
#include "ui.h"

#define W 960
#define H 544
#define CELL 12                    /* glyph size and column pitch               */
#define ROW_H 14                   /* row pitch: a little air between lines     */
#define FB_BYTES 0x200000          /* 960*544*4 = 2088960, rounded up to 2 MiB  */

static uint32_t *g_back;
static uint32_t *g_front;
static SceUID    g_block = -1;

int ui_init(void)
{
	void *base = NULL;

	g_back = malloc((size_t)W * H * 4);
	if (!g_back)
		return -1;
	g_block = sceKernelAllocMemBlock("ts_tester_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
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

void ui_fill(int x, int y, int w, int h, uint32_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > W) w = W - x;
	if (y + h > H) h = H - y;
	for (int j = 0; j < h; j++) {
		uint32_t *p = g_back + (size_t)(y + j) * W + x;

		for (int i = 0; i < w; i++)
			p[i] = color;
	}
}

void ui_clear(void)
{
	ui_fill(0, 0, W, H, UI_BG);
}

static void put_char(int x, int y, unsigned char ch, uint32_t color)
{
	const unsigned char *g;

	if (ch < TS_FONT_FIRST || ch > TS_FONT_LAST)
		return;
	g = ts_font8x8[ch - TS_FONT_FIRST];
	for (int dy = 0; dy < CELL; dy++) {
		unsigned char row = g[dy * 8 / CELL];
		uint32_t *p = g_back + (size_t)(y + dy) * W + x;

		for (int dx = 0; dx < CELL; dx++)
			if ((row >> (dx * 8 / CELL)) & 1)       /* bit 0 is the left pixel */
				p[dx] = color;
	}
}

void ui_text(int col, int row, uint32_t color, const char *s)
{
	if (row < 0 || row >= UI_ROWS)
		return;
	for (; *s && col < UI_COLS; s++, col++)
		if (col >= 0)
			put_char(col * CELL, row * ROW_H + 1, (unsigned char)*s, color);
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
	ui_fill(0, row * ROW_H, W, ROW_H, color);
}

void ui_bar(int col, int row, int cols, uint32_t permille, uint32_t color)
{
	int x = col * CELL, y = row * ROW_H + 2, w = cols * CELL, h = ROW_H - 4;
	uint32_t fill = permille > 1000 ? 1000 : permille;

	ui_fill(x, y, w, h, UI_SELBG);
	ui_fill(x, y, (int)((uint64_t)w * fill / 1000), h, color);
}

void ui_present(void)
{
	SceDisplayFrameBuf fb;

	if (!g_front)
		return;
	memcpy(g_front, g_back, (size_t)W * H * 4);
	memset(&fb, 0, sizeof(fb));
	fb.size = sizeof(fb);
	fb.base = g_front;
	fb.pitch = W;
	fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
	fb.width = W;
	fb.height = H;
	sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
	sceDisplayWaitVblankStart();
}

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
