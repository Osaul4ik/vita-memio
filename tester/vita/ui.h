/*
 * ui.h - a tiny text UI on the Vita framebuffer (960x544), using the plugin's own
 * 8x8 font scaled to 12x12 glyphs on a 12x14 grid: 80 columns x 38 rows.
 */
#ifndef STORAGETURBO_TESTER_UI_H
#define STORAGETURBO_TESTER_UI_H

#include <stdint.h>

#define UI_COLS 80
#define UI_ROWS 38

#define UI_RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define UI_BG      UI_RGB(16, 18, 24)
#define UI_FG      UI_RGB(220, 224, 232)
#define UI_DIM     UI_RGB(130, 136, 150)
#define UI_ACCENT  UI_RGB(90, 170, 255)
#define UI_GOOD    UI_RGB(90, 220, 120)
#define UI_BAD     UI_RGB(255, 100, 90)
#define UI_WARN    UI_RGB(255, 200, 80)
#define UI_SELBG   UI_RGB(40, 52, 80)

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

void ui_clear(void);
void ui_fill(int x, int y, int w, int h, uint32_t color);
void ui_text(int col, int row, uint32_t color, const char *s);
void ui_textf(int col, int row, uint32_t color, const char *fmt, ...);
void ui_row_bg(int row, uint32_t color);
void ui_bar(int col, int row, int cols, uint32_t permille, uint32_t color);

/* Show what was drawn and wait for the next frame. */
void ui_present(void);

/* New button presses since the last call. */
unsigned ui_poll(void);
/* 1 if the back button is down right now (used to stop a running test). */
int ui_back_down(void);

#endif
