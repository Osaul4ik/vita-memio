/*
 * overlay_logic.h - the SELECT + arrows overlay's keys and menu, kept free of
 * kernel calls so the host tests can drive it (tests/test_overlay.c).
 * Included by overlay.c only.
 *
 * Keys (SELECT held, the arrow newly pressed):
 *   SELECT + Right   closed -> overlay -> HUD (the overlay closes, the HUD shows)
 *   SELECT + Left    HUD -> overlay -> closed
 *   SELECT + Up/Down everything closed
 * While the overlay is open, the game gets no buttons, and without SELECT:
 *   Up / Down        the cursor
 *   Left / Right     the setting under it, back / on (Cross: on)
 *   Cross            on "Reset Auto profile": do it
 * A change is not made here: it is left in `op` for the worker, which owns
 * the settings and the files.
 */
#ifndef VITAMEMIO_OVERLAY_LOGIC_H
#define VITAMEMIO_OVERLAY_LOGIC_H

#include <stdint.h>

#define OV_CLOSED  0
#define OV_MENU    1
#define OV_HUD     2

/* SceCtrl button bits */
#define OV_BTN_SELECT 0x00000001u
#define OV_BTN_UP     0x00000010u
#define OV_BTN_RIGHT  0x00000020u
#define OV_BTN_DOWN   0x00000040u
#define OV_BTN_LEFT   0x00000080u
#define OV_BTN_CROSS  0x00004000u

/* The menu's items, top to bottom. */
enum {
	OV_ITEM_RA = 0,      /* read ahead: Off / On / Auto                       */
	OV_ITEM_OFFLOAD,     /* copy on the system core (core 3)                  */
	OV_ITEM_PIPE,        /* overlapped reads                                  */
	OV_ITEM_GPUSAFE,     /* GPU-safe reads                                    */
	OV_ITEM_RESET,       /* delete the game's Auto profile: learnt again      */
	OV_ITEMS
};

/* An op for the worker: the item + 1 in bits 8.., the step (-1, +1) below. */
#define OV_OP(item, dir)   ((uint32_t)(((item) + 1) << 8) | ((uint32_t)(dir) & 0xFFu))
#define OV_OP_ITEM(op)     ((int)((op) >> 8) - 1)
#define OV_OP_DIR(op)      ((int)(int8_t)((op) & 0xFFu))

typedef struct OvLogic {
	uint32_t prev;       /* the buttons last seen                              */
	int      state;      /* OV_*                                               */
	int      cursor;     /* OV_ITEM_*                                          */
	uint32_t op;         /* 0, or an OV_OP not yet taken by the worker         */
} OvLogic;

/* One look at the buttons. Returns 1 if anything visible changed (the worker
 * should redraw). After it, o->state == OV_MENU means: hide the buttons from
 * the game. */
static inline int ov_input(OvLogic *o, uint32_t b)
{
	const uint32_t down = b & ~o->prev;
	const int was_state = o->state, was_cursor = o->cursor;
	const uint32_t was_op = o->op;

	o->prev = b;
	if (b & OV_BTN_SELECT) {
		if (down & OV_BTN_RIGHT)
			o->state = o->state == OV_CLOSED ? OV_MENU : OV_HUD;
		else if (down & OV_BTN_LEFT)
			o->state = o->state == OV_HUD ? OV_MENU : OV_CLOSED;
		else if (down & (OV_BTN_UP | OV_BTN_DOWN))
			o->state = OV_CLOSED;
	} else if (o->state == OV_MENU) {
		if (down & OV_BTN_UP)
			o->cursor = o->cursor > 0 ? o->cursor - 1 : OV_ITEMS - 1;
		else if (down & OV_BTN_DOWN)
			o->cursor = o->cursor + 1 < OV_ITEMS ? o->cursor + 1 : 0;
		else if (!o->op && o->cursor != OV_ITEM_RESET && (down & (OV_BTN_LEFT | OV_BTN_RIGHT |
		                                                          OV_BTN_CROSS)))
			o->op = OV_OP(o->cursor, (down & OV_BTN_LEFT) ? -1 : 1);
		else if (!o->op && o->cursor == OV_ITEM_RESET && (down & OV_BTN_CROSS))
			o->op = OV_OP(OV_ITEM_RESET, 0);
	}
	return o->state != was_state || o->cursor != was_cursor || o->op != was_op;
}

/* The next value of a setting with these choices, in this order, one step
 * `dir` from `cur` (wrapping); `cur` not among them: the first. */
static inline int ov_step(const int *vals, int n, int cur, int dir)
{
	int i = 0;

	while (i < n && vals[i] != cur)
		i++;
	if (i == n)
		return vals[0];
	i = (i + (dir < 0 ? n - 1 : 1)) % n;
	return vals[i];
}

/* Percent a / (a + b), or -1 without data. */
static inline int ov_pct(uint32_t a, uint32_t b)
{
	const uint64_t t = (uint64_t)a + b;

	return t ? (int)(((uint64_t)a * 100u + t / 2) / t) : -1;
}

#endif /* VITAMEMIO_OVERLAY_LOGIC_H */
