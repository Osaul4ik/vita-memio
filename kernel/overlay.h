/*
 * overlay.h - the SELECT + arrows overlay: keys (SceCtrl hooks) and state.
 * The worker (main.c) draws it through the HUD and makes its changes.
 */
#ifndef VITAMEMIO_OVERLAY_H
#define VITAMEMIO_OVERLAY_H

#include <stdint.h>

/* Install / release the button hooks (worker thread). `on`: the setting. */
void vm_overlay_enable(int on, void (*wake)(void));
int  vm_overlay_installed(void);

/* OV_CLOSED, OV_MENU or OV_HUD (overlay_logic.h); OV_CLOSED when off. */
int  vm_overlay_state(void);
int  vm_overlay_cursor(void);
/* The items shown (1 << OV_ITEM_*), set by the worker as it draws them. */
void vm_overlay_set_mask(uint32_t mask);
/* The change asked for (an OV_OP), taken: 0 if none. */
uint32_t vm_overlay_take_op(void);

#endif /* VITAMEMIO_OVERLAY_H */
