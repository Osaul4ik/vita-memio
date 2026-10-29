/*
 * hud.h - storage overlay, kernel side.
 *
 * Drawn by hooking SceDisplay's set-framebuffer path, so it appears over
 * whatever is on screen in any app, always in the top right corner. It draws
 * only text the 1 Hz worker hands it (vm_hud_set_text); the frame hook itself
 * formats nothing.
 *
 * Two looks (VM_HUD_STYLE_*):
 *   classic   white text with a drop shadow, the 8x8 font at 2x;
 *   PSVshell  a black panel with rounded corners and coloured values in the
 *             Terminus font, sized by the framebuffer like PSVshell's overlay
 *             (8x14 up to 640 px wide, 10x18 up to 720, 12x24 above).
 */
#ifndef VITAMEMIO_HUD_H
#define VITAMEMIO_HUD_H

#define VM_HUD_LINES     6
#define VM_HUD_LINE_MAX  64   /* bytes incl. colour codes and NUL; ASCII    */

/* Colour codes inside a line: they switch the colour of the text after them
 * (PSVshell style) and take no room. The classic style skips them. */
#define HUD_C_TEXT    "\x01"   /* white: names                              */
#define HUD_C_DIM     "\x02"   /* grey: labels, units                       */
#define HUD_C_GOOD    "\x03"   /* green: the faster path is in use          */
#define HUD_C_BAD     "\x04"   /* red: fell back after an error             */
#define HUD_C_WARN    "\x05"   /* yellow: no buffer, slow mode, verify      */
#define HUD_C_ACCENT  "\x06"   /* blue                                      */
#define HUD_C_LAST    6

int  vm_hud_init(void);   /* install the set-framebuffer hook              */
void vm_hud_fini(void);
int  vm_hud_installed(void);

/* Replace the text (n lines, n = 0 hides the HUD) and the look
 * (VM_HUD_STYLE_*). Worker thread only. */
void vm_hud_set_text(const char lines[][VM_HUD_LINE_MAX], int n, int style);

#endif /* VITAMEMIO_HUD_H */
