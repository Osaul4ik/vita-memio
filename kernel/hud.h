/*
 * hud.h - storage overlay, kernel side.
 *
 * Drawn by hooking SceDisplay's set-framebuffer path, so it appears over
 * whatever is on screen in any app, always in the top right corner. It draws
 * only text the worker hands it (vm_hud_set_text); the frame hook itself
 * formats nothing.
 *
 * A black panel with rounded corners and coloured values in the Terminus font,
 * sized by the framebuffer like PSVshell's overlay (8x14 up to 640 px wide, 10x18
 * up to 720, 12x24 above).
 */
#ifndef VITAMEMIO_HUD_H
#define VITAMEMIO_HUD_H

#define VM_HUD_LINES     16   /* the HUD uses up to 9, the overlay up to 16 */
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
/* (head, index) pairs the frame hook has seen: bit head*2+index, bit 4 others */
uint32_t vm_hud_planes(void);

/* Replace the text (n lines, n = 0 hides the HUD) and the look
 * Worker thread only. */
void vm_hud_set_text(const char lines[][VM_HUD_LINE_MAX], int n);

/* The overlay's text (n = 0 hides it): a PSVshell-style panel in the top
 * left corner. Worker thread only. */
void vm_hud_set_overlay(const char lines[][VM_HUD_LINE_MAX], int n);

#endif /* VITAMEMIO_HUD_H */
