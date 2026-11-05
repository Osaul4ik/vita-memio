/*
 * overlay_logic.h - the SELECT + arrows overlay's keys and menu, kept free of
 * kernel calls so the host tests can drive it (tests/test_overlay.c).
 * Included by overlay.c only.
 *
 * What the game had held as the overlay closed stays hidden from it until
 * released (`hide`), so the Cross of Close or the combination that closed it
 * does not act in the game. Looks from several ports are put together
 * (ov_merge): an idle port's empty buffer is no release of the buttons.
 *
 * Keys:
 *   SELECT + Right   closed -> overlay -> HUD (the overlay closes, the HUD shows)
 *   SELECT + Left    HUD -> overlay -> closed
 *   SELECT + Up/Down everything closed
 * With nothing shown, the buttons are looked at every OV_SAMPLE_US only
 * (overlay.c), so a game's own polling costs next to nothing. SELECT + Right
 * opens it at once, once per press; everything else (the overlay open, or the
 * HUD shown) acts at once on a newly pressed arrow with SELECT held.
 * While the overlay is open, the game gets no buttons, and without SELECT:
 *   Up / Down        the cursor (over the lines shown: see `mask`)
 *   Left / Right     the value under it, back / on (Cross: on)
 *   Cross            on Save: do it; on Close: closed
 * A change is not made here: it is left in `op` for the worker, which owns
 * the settings and the files.
 */
#ifndef VITAMEMIO_OVERLAY_LOGIC_H
#define VITAMEMIO_OVERLAY_LOGIC_H

#include <stdint.h>

#define OV_CLOSED  0
#define OV_MENU    1
#define OV_HUD     2

#define OV_SAMPLE_US  (20u * 1000u)    /* closed: look at the buttons this often */
#define OV_HUD_SAMPLE_US (20u * 1000u) /* the HUD shown: this often (a press lasts longer) */
#define OV_PORTS      5                /* controller ports: 0, and 1..4 on a PS TV */

/* SceCtrl button bits */
#define OV_BTN_SELECT 0x00000001u
#define OV_BTN_UP     0x00000010u
#define OV_BTN_RIGHT  0x00000020u
#define OV_BTN_DOWN   0x00000040u
#define OV_BTN_LEFT   0x00000080u
#define OV_BTN_CROSS  0x00004000u

/* The menu's items, top to bottom: the app in front's own settings, then
 * then the actions. Which are shown is the worker's (a mask of 1 << item): the
 * thresholds only with read ahead On, nothing but Close with no app in front. */
enum {
	OV_ITEM_TEST = 0,    /* the read ahead test: start / stop                 */
	OV_ITEM_RA,          /* read ahead: Global / Off / On                     */
	OV_ITEM_C,           /* ... On: reads in a row before it starts, cached   */
	OV_ITEM_NC,          /* ... non-cacheable                                 */
	OV_ITEM_OFFLOAD,     /* copy on core 3                                    */
	OV_ITEM_ADV,         /* the two below shown or not                        */
	OV_ITEM_PIPE,        /* overlapped reads                                  */
	OV_ITEM_GPUSAFE,     /* GPU-safe reads                                    */
	OV_ITEM_SAVE,        /* the app's settings as they are now, into its profile */
	OV_ITEM_CLOSE,       /* the overlay closed                                */
	OV_ITEMS
};
#define OV_MASK_ALL  ((1u << OV_ITEMS) - 1)

/* Items done with Cross only (an action, not a value). */
static inline int ov_item_action(int i)
{
	return i == OV_ITEM_TEST || i == OV_ITEM_ADV || i == OV_ITEM_SAVE || i == OV_ITEM_CLOSE;
}

/* The next item shown from `i`, `dir` -1 / +1 (wrapping); `i` itself if
 * nothing else is shown. */
static inline int ov_next(int i, int dir, uint32_t mask)
{
	for (int k = 1; k <= OV_ITEMS; k++) {
		const int j = ((i + dir * k) % OV_ITEMS + OV_ITEMS) % OV_ITEMS;

		if (mask & (1u << j))
			return j;
	}
	return i;
}

/* An op for the worker: the item + 1 in bits 8.., the step (-1, +1) below. */
#define OV_OP(item, dir)   ((uint32_t)(((item) + 1) << 8) | ((uint32_t)(dir) & 0xFFu))
#define OV_OP_ITEM(op)     ((int)((op) >> 8) - 1)
#define OV_OP_DIR(op)      ((int)(int8_t)((op) & 0xFFu))

typedef struct OvLogic {
	uint32_t prev;       /* the buttons last seen                              */
	uint32_t hold;       /* closed: the combination being held (0: none) ...   */
	int      held;       /* ... and it has acted (once per press)              */
	int      state;      /* OV_*                                               */
	int      cursor;     /* OV_ITEM_*                                          */
	uint32_t op;         /* 0, or an OV_OP not yet taken by the worker         */
	uint32_t hide;       /* closed with these held: hidden until released      */
	uint32_t ports[OV_PORTS];   /* the buttons last seen per port              */
} OvLogic;

/* The buttons of one look at `port`, with what the other ports last had. */
static inline uint32_t ov_merge(OvLogic *o, int port, uint32_t b)
{
	uint32_t all = 0;

	if (port >= 0 && port < OV_PORTS)
		o->ports[port] = b;
	for (int i = 0; i < OV_PORTS; i++)
		all |= o->ports[i];
	return all;
}

/* The state SELECT + one arrow leads to from `state` (unchanged: none). */
static inline int ov_combo(int state, uint32_t arrow)
{
	if (arrow == OV_BTN_RIGHT)
		return state == OV_CLOSED ? OV_MENU : OV_HUD;
	if (arrow == OV_BTN_LEFT)
		return state == OV_HUD ? OV_MENU : OV_CLOSED;
	if (arrow == OV_BTN_UP || arrow == OV_BTN_DOWN)
		return OV_CLOSED;
	return state;
}

/* One look at the buttons, `now` in microseconds (wrapping). Returns 1 if
 * anything visible changed (the worker should redraw). After it, o->state ==
 * OV_MENU means: hide the buttons from the game. */
static inline int ov_input(OvLogic *o, uint32_t b, uint32_t now, uint32_t mask)
{
	(void)now;
	const uint32_t down = b & ~o->prev;
	const int was_state = o->state, was_cursor = o->cursor;
	const uint32_t was_op = o->op;

	o->prev = b;
	o->hide &= b;
	if (o->state == OV_CLOSED) {
		/* nothing shown: SELECT + Right (and no other arrow) */
		const uint32_t arrows = b & (OV_BTN_UP | OV_BTN_DOWN | OV_BTN_LEFT | OV_BTN_RIGHT);
		const uint32_t combo = (b & OV_BTN_SELECT) && arrows == OV_BTN_RIGHT ? arrows : 0;

		if (combo != o->hold) {
			o->hold = combo;
			o->held = 0;
		}
		if (combo && !o->held) {
			o->held = 1;
			o->state = OV_MENU;
		}
		return o->state != was_state;
	}
	o->hold = 0;
	if (o->state == OV_HUD) {
		/* the HUD shown: at once, as in the overlay */
		if ((b & OV_BTN_SELECT) && (down & (OV_BTN_LEFT | OV_BTN_UP | OV_BTN_DOWN)))
			o->state = ov_combo(OV_HUD, down & OV_BTN_LEFT ? OV_BTN_LEFT : OV_BTN_UP);
		return o->state != was_state;
	}
	if (b & OV_BTN_SELECT) {
		if (down & OV_BTN_RIGHT)
			o->state = o->state == OV_CLOSED ? OV_MENU : OV_HUD;
		else if (down & OV_BTN_LEFT)
			o->state = o->state == OV_HUD ? OV_MENU : OV_CLOSED;
		else if (down & (OV_BTN_UP | OV_BTN_DOWN))
			o->state = OV_CLOSED;
	} else if (o->state == OV_MENU) {
		if (!(mask & OV_MASK_ALL))
			mask = 1u << OV_ITEM_CLOSE;
		if (!(mask & (1u << o->cursor))) {
			/* its line is gone (read ahead changed): a press only moves
			 * the cursor to the next one shown, so nothing acts on a line
			 * that was never under it */
			if (down & (OV_BTN_UP | OV_BTN_DOWN | OV_BTN_LEFT | OV_BTN_RIGHT | OV_BTN_CROSS)) {
				o->cursor = ov_next(o->cursor, (down & OV_BTN_UP) ? -1 : 1, mask);
				if (!(mask & (1u << o->cursor)))
					o->cursor = OV_ITEM_CLOSE;
			}
			return o->state != was_state || o->cursor != was_cursor;
		}
		if (down & OV_BTN_UP)
			o->cursor = ov_next(o->cursor, -1, mask);
		else if (down & OV_BTN_DOWN)
			o->cursor = ov_next(o->cursor, 1, mask);
		else if (o->cursor == OV_ITEM_CLOSE && (down & OV_BTN_CROSS))
			o->state = OV_CLOSED;
		else if (!o->op && !ov_item_action(o->cursor) &&
		         (down & (OV_BTN_LEFT | OV_BTN_RIGHT | OV_BTN_CROSS)))
			o->op = OV_OP(o->cursor, (down & OV_BTN_LEFT) ? -1 : 1);
		else if (!o->op && ov_item_action(o->cursor) && (down & OV_BTN_CROSS))
			o->op = OV_OP(o->cursor, 0);
	}
	if (was_state == OV_MENU && o->state != OV_MENU)
		o->hide = b;
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
