/*
 * hud.h - storage overlay, kernel side.
 *
 * Drawn by hooking SceDisplay's set-framebuffer path, so it appears over
 * whatever is on screen in any app. It draws only text the 1 Hz worker hands
 * it (vm_hud_set_text); the frame hook itself formats nothing.
 */
#ifndef VITAMEMIO_HUD_H
#define VITAMEMIO_HUD_H

#define VM_HUD_LINES     6
#define VM_HUD_LINE_MAX  48   /* chars incl. NUL; ASCII only               */

int  vm_hud_init(void);   /* install the set-framebuffer hook              */
void vm_hud_fini(void);
int  vm_hud_installed(void);

/* Replace the text (n lines, n = 0 hides the HUD) and the screen corner
 * (VM_POS_*). Worker thread only. */
void vm_hud_set_text(const char lines[][VM_HUD_LINE_MAX], int n, int pos);

#endif /* VITAMEMIO_HUD_H */
