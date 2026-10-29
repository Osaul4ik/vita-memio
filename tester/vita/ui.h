/*
 * ui.h - a small text UI on the Vita framebuffer (960x544).
 *
 * Text sits on a grid of 10x18 cells (the 10x18 bitmap font): 96 columns x 30
 * rows. Titles use the 12x24 font at pixel positions. On top of plain text
 * there are panels (a box with a title in its top edge), bars, a header and a
 * footer, in the style of an overlay menu such as PSVshell's.
 */
#ifndef VITAMEMIO_TESTER_UI_H
#define VITAMEMIO_TESTER_UI_H

#include <stdint.h>

#define UI_W     960
#define UI_H     544
#define UI_CW    10                 /* cell width, px                            */
#define UI_CH    18                 /* cell height (row pitch), px               */
#define UI_COLS  96
#define UI_ROWS  30
#define UI_TOP   2                  /* the grid starts this many px down         */

#define UI_RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define UI_BG      UI_RGB(10, 11, 15)      /* screen                             */
#define UI_PANEL   UI_RGB(20, 22, 30)      /* inside a panel                     */
#define UI_EDGE    UI_RGB(52, 60, 84)      /* panel border, separators           */
#define UI_BAR     UI_RGB(26, 30, 42)      /* header and footer                  */
#define UI_FG      UI_RGB(232, 234, 240)
#define UI_DIM     UI_RGB(128, 134, 150)
#define UI_ACCENT  UI_RGB(70, 170, 255)
#define UI_GOOD    UI_RGB(90, 220, 120)
#define UI_BAD     UI_RGB(255, 96, 86)
#define UI_WARN    UI_RGB(255, 196, 70)
#define UI_SELBG   UI_RGB(34, 52, 86)

/* Buttons (edge-detected presses; the d-pad repeats while held). */
#define UI_UP      0x01u
#define UI_DOWN    0x02u
#define UI_LEFT    0x04u
#define UI_RIGHT   0x08u
#define UI_OK      0x10u    /* cross  */
#define UI_BACK    0x20u    /* circle */
#define UI_PGUP    0x40u    /* L      */
#define UI_PGDN    0x80u    /* R      */

int  ui_init(void);
void ui_shutdown(void);

/* Pixels. */
void ui_clear(void);
void ui_fill(int x, int y, int w, int h, uint32_t color);
void ui_frame(int x, int y, int w, int h, uint32_t color);      /* 1-px outline */
void ui_big(int x, int y, uint32_t color, const char *s);        /* 12x24 font  */
int  ui_big_width(const char *s);

/* Grid cells. */
void ui_text(int col, int row, uint32_t color, const char *s);
void ui_textf(int col, int row, uint32_t color, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
void ui_row_bg(int row, uint32_t color);
void ui_bar(int col, int row, int cols, uint32_t permille, uint32_t color);

/* A panel over the cells [col, col+cols) x [row, row+rows), with `title` (may
 * be NULL) in its top edge. Text goes inside, from row+1 and col+2. */
void ui_panel(int col, int row, int cols, int rows, const char *title);
/* A selected line inside a panel: a highlight bar with markers at both ends. */
void ui_select(int col, int row, int cols);

/* The title bar (rows 0-1) and the button hints (the last row). */
void ui_header(const char *title, const char *right);
void ui_footer(const char *hints);

/* Show what was drawn and wait for the next frame. */
void ui_present(void);

/* New button presses since the last call. */
unsigned ui_poll(void);
/* 1 if the back button is down right now (used to stop a running test). */
int ui_back_down(void);

#endif
