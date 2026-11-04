/*
 * test_overlay.c - the SELECT + arrows overlay's keys and menu
 * (kernel/overlay_logic.h): the states as the README describes them, the
 * cursor, the changes asked of the worker, and the helpers.
 */
#include <string.h>

#include "../kernel/overlay_logic.h"
#include "vm_test.h"

static uint32_t T = 0xFFFF0000u;            /* the clock, us (it wraps soon) */
static uint32_t g_mask = OV_MASK_ALL;        /* the items shown */

/* one look at the buttons, a frame later */
static int in(OvLogic *o, uint32_t b)
{
	T += 16667;
	return ov_input(o, b, T, g_mask);
}

/* press `b` (with what is held), then let go of everything */
static int tap(OvLogic *o, uint32_t held, uint32_t b)
{
	int c = in(o, held);

	c |= in(o, held | b);
	in(o, 0);
	return c;
}

/* closed: hold SELECT + `b` for `ms`, looked at every OV_SAMPLE_US (as the
 * hook does), then let go */
static int hold(OvLogic *o, uint32_t b, uint32_t ms)
{
	int c = 0;

	for (uint32_t t = 0; t <= ms * 1000u; t += OV_SAMPLE_US) {
		T += t ? OV_SAMPLE_US : 0;
		c |= ov_input(o, OV_BTN_SELECT | b, T, g_mask);
	}
	T += OV_SAMPLE_US;
	ov_input(o, 0, T, g_mask);
	return c;
}

static void test_states(void)
{
	OvLogic o;

	memset(&o, 0, sizeof(o));
	CHECK(o.state == OV_CLOSED);
	/* nothing shown: only SELECT + Right held half a second opens it */
	CHECK(!hold(&o, OV_BTN_RIGHT, 400) && o.state == OV_CLOSED);
	CHECK(!tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT) && o.state == OV_CLOSED);   /* a tap: no */
	hold(&o, OV_BTN_LEFT, 600);
	hold(&o, OV_BTN_UP, 600);
	hold(&o, OV_BTN_DOWN, 600);
	CHECK(o.state == OV_CLOSED);                                          /* nothing else */
	CHECK(hold(&o, OV_BTN_RIGHT, 500) && o.state == OV_MENU);             /* opens */
	/* open: a press acts at once */
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT) && o.state == OV_HUD);      /* HUD */
	/* the HUD shown: at once too */
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	CHECK(o.state == OV_HUD);                                              /* stays */
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_LEFT) && o.state == OV_MENU);      /* back */
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_LEFT) && o.state == OV_CLOSED);    /* closed */
	/* SELECT + Up or Down: everything closed, from either */
	hold(&o, OV_BTN_RIGHT, 500);
	tap(&o, OV_BTN_SELECT, OV_BTN_UP);
	CHECK(o.state == OV_CLOSED);
	hold(&o, OV_BTN_RIGHT, 500);
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	CHECK(o.state == OV_HUD);
	CHECK(tap(&o, OV_BTN_SELECT, OV_BTN_DOWN) && o.state == OV_CLOSED);
	/* held on after it opened: once per hold - not then the HUD */
	for (int i = 0; i < 20; i++) {
		T += OV_SAMPLE_US;
		ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT, T, g_mask);
	}
	CHECK(o.state == OV_MENU);
	in(&o, OV_BTN_SELECT | OV_BTN_RIGHT);             /* open, still held: not new */
	CHECK(o.state == OV_MENU);
	in(&o, 0);
	tap(&o, OV_BTN_SELECT, OV_BTN_UP);
	/* nothing shown: the arrow alone, two arrows, SELECT alone: nothing */
	hold(&o, 0, 600);
	CHECK(o.state == OV_CLOSED);
	hold(&o, OV_BTN_RIGHT | OV_BTN_UP, 600);
	CHECK(o.state == OV_CLOSED);
	for (int i = 0; i < 10; i++) {
		T += OV_SAMPLE_US;
		ov_input(&o, OV_BTN_RIGHT, T, g_mask);
	}
	CHECK(o.state == OV_CLOSED && !o.op);
	/* the hold broken (another arrow) starts over */
	T += OV_SAMPLE_US;
	ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT, T, g_mask);
	T += 300000;
	ov_input(&o, OV_BTN_SELECT | OV_BTN_UP, T, g_mask);
	T += 300000;
	ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT, T, g_mask);
	CHECK(o.state == OV_CLOSED);
	T += 300000;
	ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT, T, g_mask);
	CHECK(o.state == OV_CLOSED);
	T += 300000;
	ov_input(&o, OV_BTN_SELECT | OV_BTN_RIGHT, T, g_mask);
	CHECK(o.state == OV_MENU);
	ov_input(&o, 0, T, g_mask);
}

static void test_menu(void)
{
	OvLogic o;

	memset(&o, 0, sizeof(o));
	g_mask = OV_MASK_ALL;
	/* closed: the arrows and Cross belong to the game */
	tap(&o, 0, OV_BTN_DOWN);
	tap(&o, 0, OV_BTN_CROSS);
	CHECK(o.cursor == 0 && !o.op);
	hold(&o, OV_BTN_RIGHT, 500);
	CHECK(o.state == OV_MENU && o.cursor == OV_ITEM_RA);
	CHECK(tap(&o, 0, OV_BTN_DOWN) && o.cursor == OV_ITEM_C);
	tap(&o, 0, OV_BTN_UP);
	tap(&o, 0, OV_BTN_UP);
	CHECK(o.cursor == OV_ITEM_CLOSE);                 /* wraps */
	tap(&o, 0, OV_BTN_DOWN);
	CHECK(o.cursor == OV_ITEM_RA);
	/* a value: left for the worker, one at a time */
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
	/* actions: only Cross */
	CHECK(ov_item_action(OV_ITEM_SAVE) && ov_item_action(OV_ITEM_RELEARN) &&
	      ov_item_action(OV_ITEM_CLOSE) && !ov_item_action(OV_ITEM_RA) &&
	      !ov_item_action(OV_ITEM_C) && !ov_item_action(OV_ITEM_GPUSAFE));
	for (int it = OV_ITEM_RELEARN; it <= OV_ITEM_SAVE; it++) {
		o.cursor = it;
		tap(&o, 0, OV_BTN_RIGHT);
		tap(&o, 0, OV_BTN_LEFT);
		CHECK(!o.op);
		tap(&o, 0, OV_BTN_CROSS);
		CHECK(OV_OP_ITEM(o.op) == it && OV_OP_DIR(o.op) == 0);
		o.op = 0;
	}
	/* the lines not shown are skipped: no thresholds (not On), no Learn
	 * it again (not Auto) */
	g_mask = OV_MASK_ALL & ~((1u << OV_ITEM_C) | (1u << OV_ITEM_NC) | (1u << OV_ITEM_RELEARN));
	o.cursor = OV_ITEM_RA;
	tap(&o, 0, OV_BTN_DOWN);
	CHECK(o.cursor == OV_ITEM_OFFLOAD);
	tap(&o, 0, OV_BTN_UP);
	CHECK(o.cursor == OV_ITEM_RA);
	o.cursor = OV_ITEM_GPUSAFE;
	tap(&o, 0, OV_BTN_DOWN);
	CHECK(o.cursor == OV_ITEM_SAVE);
	/* the cursor on a line that went away (read ahead changed): the next
	 * one shown, before anything acts */
	o.cursor = OV_ITEM_C;
	tap(&o, 0, OV_BTN_CROSS);
	CHECK(o.cursor == OV_ITEM_OFFLOAD && !o.op);     /* moved only */
	o.cursor = OV_ITEM_NC;
	tap(&o, 0, OV_BTN_UP);
	CHECK(o.cursor == OV_ITEM_RA && !o.op);
	o.op = 0;
	/* no app in front: only Close; Cross on it closes */
	g_mask = 1u << OV_ITEM_CLOSE;
	tap(&o, 0, OV_BTN_UP);
	CHECK(o.cursor == OV_ITEM_CLOSE);
	tap(&o, 0, OV_BTN_DOWN);
	CHECK(o.cursor == OV_ITEM_CLOSE);
	CHECK(tap(&o, 0, OV_BTN_CROSS) && o.state == OV_CLOSED && !o.op);
	/* a mask of nothing: as only Close */
	g_mask = OV_MASK_ALL;
	hold(&o, OV_BTN_RIGHT, 500);
	g_mask = 0;
	tap(&o, 0, OV_BTN_DOWN);
	CHECK(o.cursor == OV_ITEM_CLOSE);
	g_mask = OV_MASK_ALL;
	/* the HUD state: the game has its buttons, nothing changes */
	o.cursor = OV_ITEM_SAVE;
	tap(&o, OV_BTN_SELECT, OV_BTN_RIGHT);
	CHECK(o.state == OV_HUD);
	tap(&o, 0, OV_BTN_DOWN);
	tap(&o, 0, OV_BTN_CROSS);
	CHECK(o.cursor == OV_ITEM_SAVE && !o.op);
}

static void test_helpers(void)
{
	static const int off[] = { 0, 1, 2, 4, 3 };   /* the Settings page's order */
	static const int ra[] = { -1, 0, 1, 2 };      /* Global first */
	static const int thr[] = { 2, 3, 4, 5, 6, 7, 8, 0xFFFF };

	CHECK(ov_step(off, 5, 2, 1) == 4 && ov_step(off, 5, 4, 1) == 3 && ov_step(off, 5, 3, 1) == 0);
	CHECK(ov_step(off, 5, 0, -1) == 3 && ov_step(off, 5, 4, -1) == 2);
	CHECK(ov_step(off, 5, 9, 1) == 0);
	CHECK(ov_step(ra, 4, -1, 1) == 0 && ov_step(ra, 4, -1, -1) == 2 && ov_step(ra, 4, 2, 1) == -1);
	CHECK(ov_next(0, -1, OV_MASK_ALL) == OV_ITEMS - 1 && ov_next(0, 1, 0) == 0);
	CHECK(ov_step(thr, 8, 8, 1) == 0xFFFF && ov_step(thr, 8, 0xFFFF, 1) == 2 &&
	      ov_step(thr, 8, 2, -1) == 0xFFFF && ov_step(thr, 8, -5, 1) == 2);   /* never negative */
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
