/*
 * test_overlay.c - the SELECT + arrows overlay's keys and menu
 * (kernel/overlay_logic.h): the states as the README describes them, the
 * cursor, the changes asked of the worker, and the helpers.
 */
#include <string.h>

#include "../kernel/overlay_logic.h"
#include "vm_test.h"

/* press `b` (with what is held), then let go of everything */
static int tap(OvLogic *o, uint32_t held, uint32_t b)
{
	int c = ov_input(o, held);

	c |= ov_input(o, held | b);
	ov_input(o, 0);
	return c;
}

static void test_states(void)
{
	OvLogic o;

	memset(&o, 0, sizeof(o));
	CHECK(o.state == OV_CLOSED);
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT) && o.state == OV_MENU);   /* opens */
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT) && o.state == OV_HUD);    /* HUD */
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	CHECK(o.state == OV_HUD);                                            /* stays */
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_LEFT) && o.state == OV_MENU);    /* back */
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_LEFT) && o.state == OV_CLOSED);  /* closed */
	tap(&o, OV_BTN_SELECT, OV_BTN_LEFT);
	CHECK(o.state == OV_CLOSED);
	/* Up or Down with SELECT: everything closed, from either */
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	tap(&o, OV_BTN_SELECT, OV_BTN_UP);
	CHECK(o.state == OV_CLOSED);
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	tap(&o, OV_BTN_SELECT, OV_BTN_DOWN);
	CHECK(o.state == OV_CLOSED);
	/* held, not newly pressed: nothing; the arrow alone (closed): nothing */
	ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT);
	CHECK(o.state == OV_MENU);
	ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT);
	ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT);
	CHECK(o.state == OV_MENU);
	ov_input(&o, 0);
	tap(&o, OV_BTN_SELECT, OV_BTN_UP);
	tap(&o, 0, OV_BTN_RIGHT);
	CHECK(o.state == OV_CLOSED && !o.op);
	/* SELECT pressed after the arrow: the arrow is not new, nothing */
	ov_input(&o, OV_BTN_RIGHT);
	ov_input(&o, OV_BTN_RIGHT | OV_BTN_SELECT);
	CHECK(o.state == OV_CLOSED);
}

static void test_menu(void)
{
	OvLogic o;

	memset(&o, 0, sizeof(o));
	/* closed: the arrows and Cross belong to the game */
	tap(&o, 0, OV_BTN_DOWN);
	tap(&o, 0, OV_BTN_CROSS);
	CHECK(o.cursor == 0 && !o.op);
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	CHECK(tap(&o, 0, OV_BTN_DOWN) && o.cursor == OV_ITEM_OFFLOAD);
	tap(&o, 0, OV_BTN_UP);
	tap(&o, 0, OV_BTN_UP);
	CHECK(o.cursor == OV_ITEMS - 1);                  /* wraps */
	tap(&o, 0, OV_BTN_DOWN);
	CHECK(o.cursor == 0);
	/* a change: left for the worker, one at a time */
	tap(&o, 0, OV_BTN_RIGHT);
	CHECK(o.op == OV_OP(OV_ITEM_RA, 1) && OV_OP_ITEM(o.op) == OV_ITEM_RA && OV_OP_DIR(o.op) == 1);
	tap(&o, 0, OV_BTN_LEFT);
	CHECK(OV_OP_DIR(o.op) == 1);                       /* not taken yet: kept */
	o.op = 0;
	tap(&o, 0, OV_BTN_LEFT);
	CHECK(OV_OP_ITEM(o.op) == OV_ITEM_RA && OV_OP_DIR(o.op) == -1);
	o.op = 0;
	tap(&o, 0, OV_BTN_CROSS);
	CHECK(OV_OP_DIR(o.op) == 1);
	o.op = 0;
	/* reset: only Cross */
	o.cursor = OV_ITEM_RESET;
	tap(&o, 0, OV_BTN_RIGHT);
	CHECK(!o.op);
	tap(&o, 0, OV_BTN_CROSS);
	CHECK(OV_OP_ITEM(o.op) == OV_ITEM_RESET && OV_OP_DIR(o.op) == 0);
	o.op = 0;
	/* the HUD state: the game has its buttons, nothing changes */
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	CHECK(o.state == OV_HUD);
	tap(&o, 0, OV_BTN_DOWN);
	tap(&o, 0, OV_BTN_CROSS);
	CHECK(o.cursor == OV_ITEM_RESET && !o.op);
}

static void test_helpers(void)
{
	static const int off[] = { 0, 1, 2, 4, 3 };   /* the Settings page's order */

	CHECK(ov_step(off, 5, 2, 1) == 4 && ov_step(off, 5, 4, 1) == 3 && ov_step(off, 5, 3, 1) == 0);
	CHECK(ov_step(off, 5, 0, -1) == 3 && ov_step(off, 5, 4, -1) == 2);
	CHECK(ov_step(off, 5, 9, 1) == 0);
	CHECK(ov_pct(0, 0) == -1 && ov_pct(1, 0) == 100 && ov_pct(0, 5) == 0 && ov_pct(1, 2) == 33 &&
	      ov_pct(2, 1) == 67);
	CHECK(ov_pct(0xFFFFFFFFu, 0xFFFFFFFFu) == 50);
}

int vm_failures;

int main(void)
{
	RUN(test_states);
	RUN(test_menu);
	RUN(test_helpers);

	printf(vm_failures ? "test_overlay: %d FAILED\n" : "test_overlay: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
