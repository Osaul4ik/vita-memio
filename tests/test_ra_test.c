/*
 * test_ra_test.c - read ahead Adaptive's model (kernel/ra_test.h): its sums,
 * its picks, on or off from what read ahead did, the game's pauses, tiny reads.
 */
#include <string.h>
#include <stdlib.h>

#include "../kernel/ra_test.h"
#include "vm_test.h"

static RaTest T;

#define SMALL_FIXED 4                    /* On's 32 KiB reads' threshold: 16 reads, in fours */

/* `count` runs of `len` reads of n sectors, counted as stage.c counts them
 * (large reads: 128 sectors and more; 32 KiB reads: 64). */
static void runs(int count, int len, uint32_t n, int nc)
{
	const uint32_t have = RT_PIECES * RT_PIECE_SECTORS;

	for (int r = 0; r < count; r++) {
		for (int i = 1; i <= len; i++) {
			T.total_sectors += n;
			if (n >= 128) {
				T.pos[nc][i < RT_POS ? i : RT_POS - 1] += n < have ? n : have;
			} else {
				const int b = (i - 1) / 4;

				T.spos[nc][b < RT_POS ? b : RT_POS - 1] += n;
			}
		}
		if (n >= 128) {
			T.run[nc][len < RT_RUN ? len : RT_RUN - 1]++;
		} else {
			const int b = len / 4;

			T.srun[nc][b < RT_RUN ? b : RT_RUN - 1]++;
		}
		T.ended[nc]++;
	}
}

static void go(void)
{
	memset(&T, 0, sizeof(T));
}

int vm_failures;

/* Tiny reads' model (rt_tiny_*): `count` runs of `reads` reads of `sec`
 * sectors, then Adaptive's pick from (8, 64). */
static uint32_t tc[RT_TB], tr[RT_TB], ts[RT_TB];

static void tiny_runs(uint32_t count, uint32_t reads, uint32_t sec)
{
	const int b = rt_tiny_bucket(reads);

	tc[b] += count * 16;
	tr[b] += count * reads * 16;
	ts[b] += count * reads * sec * 16;
}

static int tiny_pick(int *t, int *blk)
{
	*t = 8;
	*blk = 64;
	return rt_tiny_pick(tc, tr, ts, t, blk, 0, &rt_tiny_fixed);
}

static void test_tiny_model(void)
{
	int t, b;

	CHECK(rt_tiny_bucket(2) == 1 && rt_tiny_bucket(16) == 15 && rt_tiny_bucket(17) == 16 &&
	      rt_tiny_bucket(200) == 21 && rt_tiny_rep[rt_tiny_bucket(40)] == 40);
	/* long runs of one-sector reads: from the 2nd, the biggest block */
	memset(tc, 0, sizeof(tc)); memset(tr, 0, sizeof(tr)); memset(ts, 0, sizeof(ts));
	tiny_runs(32, 40, 1);
	CHECK(tiny_pick(&t, &b) && t == 2 && b == 64);
	/* runs of four: from the 2nd, a small block (a big one is read mostly
	 * for nothing) */
	memset(tc, 0, sizeof(tc)); memset(tr, 0, sizeof(tr)); memset(ts, 0, sizeof(ts));
	tiny_runs(32, 4, 1);
	CHECK(tiny_pick(&t, &b) && t == 2 && b == 16);
	CHECK(rt_tiny_score(tc, tr, ts, 2, 16, &rt_tiny_fixed) ==
	      (int64_t)32 * 16 * (2 * (395 - 46) - (int64_t)25 * 15));
	/* runs of two: a block brings nothing - left as it is */
	memset(tc, 0, sizeof(tc)); memset(tr, 0, sizeof(tr)); memset(ts, 0, sizeof(ts));
	tiny_runs(32, 2, 1);
	CHECK(!tiny_pick(&t, &b) && t == 8 && b == 64);
	/* reads of 20 KiB: only the 32 KiB block holds one, one a block - it
	 * only costs: a block too small for them, so none is read */
	memset(tc, 0, sizeof(tc)); memset(tr, 0, sizeof(tr)); memset(ts, 0, sizeof(ts));
	tiny_runs(32, 30, 40);
	CHECK(tiny_pick(&t, &b) && b < 40);
	/* reads of 4 KiB in long runs: more of them a block, the biggest */
	memset(tc, 0, sizeof(tc)); memset(tr, 0, sizeof(tr)); memset(ts, 0, sizeof(ts));
	tiny_runs(32, 100, 8);
	CHECK(tiny_pick(&t, &b) && t == 2 && b == 64);
	/* a margin: a move only if worth more than it */
	CHECK(!rt_tiny_pick(tc, tr, ts, &(int){ 8 }, &(int){ 64 }, INT64_MAX / 2, &rt_tiny_fixed));
}

/* The one-pass sums (rt_scores) against the sums written out as they read:
 * the sectors after the m-th read, less min(l - m + 1, 3) pieces a run of l
 * (two pieces ahead: min(l - m + 1, 2)). */
static void test_scores_one_pass(void)
{
	srand(7);
	for (int n = 0; n < 500; n++) {
		uint32_t pos[RT_POS], run[RT_RUN];
		const int32_t g = 1 + rand() % 400, w = rand() % 5000;
		RtScores o;

		for (int i = 0; i < RT_POS; i++)
			pos[i] = (uint32_t)(rand() % 100000);
		for (int i = 0; i < RT_RUN; i++)
			run[i] = (uint32_t)(rand() % 3000);
		for (int two = 0; two < 2; two++) {
			const int ahead = two ? 2 : 3;

			rt_scores(pos, run, g, w, two, &o);
			for (int m = 0; m < RT_RUN; m++) {
				uint64_t sect = 0;
				int64_t waste = 0;

				for (int i = m + 1; i < RT_POS; i++)
					sect += pos[i];
				for (int l = m; l < RT_RUN; l++)
					waste += (int64_t)run[l] * (l - m + 1 < ahead ? l - m + 1 : ahead);
				CHECK(o.sect[m] == sect && o.sc[m] == (int64_t)sect * g / 16 - waste * w);
			}
		}
	}
}

/* Adaptive's 32 KiB reads' threshold, on its own: long runs - from 4 reads;
 * runs of 9 - from 12 (sooner only reads for nothing), and the large reads'
 * threshold is left as it is; within the margin nothing moves. */
static void test_small_pick(void)
{
	int thr = 5, sm = SMALL_FIXED;

	go();
	runs(32, 40, 64, 0);
	CHECK(ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && sm == 1 && thr == 5);
	go();
	runs(32, 9, 64, 0);
	CHECK(ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && sm == 3 && thr == 5);
	sm = SMALL_FIXED;                     /* from 16: nothing either way */
	CHECK(!ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && sm == SMALL_FIXED);
	go();
	runs(32, 40, 64, 0);
	CHECK(!ra_adapt_pick(&T, 0, &thr, &sm, INT64_MAX / 2, &rt_cost_fixed) && sm == SMALL_FIXED);
	/* the large reads' on their own: long runs of them move only theirs */
	go();
	runs(32, 20, 256, 0);
	CHECK(ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && thr == RT_THR_MIN &&
	      sm == SMALL_FIXED);
}

/* On or off, from what read ahead did: off once served reads, against the
 * standard ones of the same stretch, less what pieces for nothing held up,
 * cost more than the margin; on again when the model (with the device's
 * costs) says it brings more than it: on at once, and the next off asks for
 * more (strikes). */
static void test_ev(void)
{
	RtEv e;
	RtCost c;

	/* the first off asks for 1 MiB of the standard way and 2 MiB served, a
	 * loss in two rounds in a row */
	CHECK(RT_EV_MIN_STD == 2048u && RT_EV_MIN_SRV == 4096u && RT_EV_LOSSES == 2u);
	/* served 1 MiB in 10 ms, the standard way 50 ms a MiB: on, it brings */
	memset(&e, 0, sizeof(e));
	for (int i = 0; i < 20; i++)
		CHECK(!rt_ev_round(&e, 0, 50000, 2048, 10000, 2048, 0, 0, 0, NULL, NULL) && !e.off);
	CHECK(e.std_rate > 49900 && e.std_rate < 50100 && e.srv_rate > 9900 && e.srv_rate < 10100 &&
	      rt_ev_net(&e, 0) > 0);
	/* ... the pieces for nothing hold up more than it brings: off */
	memset(&e, 0, sizeof(e));
	{
		int moved = 0, r = 0;

		for (; r < 20 && !moved; r++)
			moved = rt_ev_round(&e, 0, 50000, 2048, 10000, 2048, 50000, 10, 0, NULL, NULL);
		CHECK(moved && e.off && e.strikes == 0 && e.held_each > 4800 && e.held_each <= 5000);
	}
	/* off: the model's say alone, and it is enough at once - no round to wait out */
	CHECK(!rt_ev_round(&e, 0, 50000, 2048, 0, 0, 0, 0, 10, NULL, NULL) && e.off);   /* below the margin */
	CHECK(rt_ev_round(&e, 0, 50000, 2048, 0, 0, 0, 0, 500, NULL, NULL) && !e.off);  /* now on */
	/* ... what it served and held up before is cleared: a loss is shown afresh */
	CHECK(e.strikes == 1 && !e.srv_q && !e.srv_us && !e.held_us && !e.waste);
	CHECK(rt_ev_net(&e, 0) == 0);
	/* a return makes the next off harder: twice the margin, twice the reads.
	 * 3 % lost over 1 MiB served and 1 MiB standard: off at strike 0 ... */
	memset(&e, 0, sizeof(e));
	{
		int moved = 0;

		for (int i = 0; i < 20 && !moved; i++)
			moved = rt_ev_round(&e, 0, 50000, 2048, 53100, 2048, 0, 0, 0, NULL, NULL);
		CHECK(moved && e.off);
	}
	/* ... not after one return (the margin is 4 %), however long it goes on */
	memset(&e, 0, sizeof(e));
	e.strikes = 1;
	for (int i = 0; i < 40; i++)
		CHECK(!rt_ev_round(&e, 0, 50000, 2048, 53100, 2048, 0, 0, 0, NULL, NULL) && !e.off);
	/* a handful of served reads (a round of 600 sectors), pieces for nothing:
	 * too little to turn it off; so is 1 MiB served */
	memset(&e, 0, sizeof(e));
	CHECK(!rt_ev_round(&e, 0, 55000, 2048, 2000, 600, 24000, 4, 0, NULL, NULL) && !e.off);
	CHECK(rt_ev_net(&e, 0) == 0);
	memset(&e, 0, sizeof(e));
	CHECK(!rt_ev_round(&e, 0, 55000, 2048, 56000, 2048, 5000, 4, 0, NULL, NULL) && !e.off);
	CHECK(rt_ev_net(&e, 0) == 0);
	/* the same loss over 2 MiB served: a loss shown, off at the second round in a row */
	memset(&e, 0, sizeof(e));
	CHECK(!rt_ev_round(&e, 0, 55000, 2048, 112000, 4096, 10000, 8, 0, NULL, NULL) && !e.off &&
	      e.losses == 1);
	CHECK(rt_ev_round(&e, 0, 55000, 2048, 112000, 4096, 10000, 8, 0, NULL, NULL) && e.off &&
	      e.losses == 0);
	/* ... a round without a loss between: the count starts again */
	memset(&e, 0, sizeof(e));
	CHECK(!rt_ev_round(&e, 0, 55000, 2048, 112000, 4096, 10000, 8, 0, NULL, NULL) && e.losses == 1);
	CHECK(!rt_ev_round(&e, 0, 55000, 2048, 20000, 4096, 0, 0, 0, NULL, NULL) && !e.off &&
	      e.losses == 0);
	CHECK(!rt_ev_round(&e, 0, 55000, 2048, 112000, 4096, 10000, 8, 0, NULL, NULL) && !e.off);
	/* off, on, off, on ...: each return makes the loss to show larger (here
	 * 5 %: off at strikes 0 and 1, not at 2) - it settles on */
	{
		int offs = 0;

		memset(&e, 0, sizeof(e));
		for (int cyc = 0; cyc < 12; cyc++) {
			int moved = 0;

			for (int i = 0; i < 400 && !moved; i++)
				moved = rt_ev_round(&e, 0, 55000, 2048, 56000, 2048, 5000, 4, 0, NULL, NULL);
			if (!moved)
				break;
			offs++;
			CHECK(rt_ev_round(&e, 0, 55000, 2048, 0, 0, 0, 0, 500, NULL, NULL) && !e.off);
		}
		CHECK(offs == 2 && e.strikes == 2 && !e.off);
	}
	/* too little read either way: never off from it */
	memset(&e, 0, sizeof(e));
	for (int i = 0; i < 40; i++)
		CHECK(!rt_ev_round(&e, 0, 50000, 100, 90000, 100, 9000, 1, 0, NULL, NULL) && !e.off);
	/* tiny: reads, not sectors - 600 us the standard way, 14 from the block,
	 * blocks of 1100 us: many hits pay, few lose */
	memset(&e, 0, sizeof(e));
	for (int i = 0; i < 20; i++)
		CHECK(!rt_ev_round(&e, 1, 600 * 100, 100, 14 * 90 + 1100 * 10, 100, 0, 0, 0, NULL, NULL) && !e.off);
	memset(&e, 0, sizeof(e));
	{
		int moved = 0;

		for (int i = 0; i < 20 && !moved; i++)
			moved = rt_ev_round(&e, 1, 600 * 100, 100, 14 * 5 + 1100 * 45, 50, 0, 0, 0, NULL, NULL);
		CHECK(moved && e.off);
	}
	/* very long rounds: the sums never wrap, the rates stay */
	memset(&e, 0, sizeof(e));
	for (int i = 0; i < 50; i++)
		CHECK(!rt_ev_round(&e, 0, 0xF0000000u, 0x00F00000u, 0x30000000u, 0x00F00000u, 0, 0, 0,
		                   NULL, NULL) && !e.off);
	{
		const uint32_t want = (uint32_t)((uint64_t)0xF0000000u * 2048u / 0x00F00000u);

		CHECK(e.std_us <= 0x7FFFFFFFu && e.std_rate > want - want / 100 &&
		      e.std_rate < want + want / 100 && e.srv_rate < e.std_rate);
	}
	/* the model's cost, from this game's: 40 ms a MiB saved is 312/16 us a
	 * sector; a piece for nothing what it held up here */
	memset(&e, 0, sizeof(e));
	e.std_rate = 50000;
	e.srv_rate = 10000;
	e.held_each = 2500;
	rt_ev_cost(&e, 1, &rt_cost_fixed, &c);
	CHECK(c.gain16[1] == 312 && c.gain16[0] == rt_cost_fixed.gain16[0] && c.waste_us == 2500);
	/* off: nothing of the game's read ahead is measured now - the device's costs alone */
	e.off = 1;
	rt_ev_cost(&e, 1, &rt_cost_fixed, &c);
	CHECK(c.gain16[1] == rt_cost_fixed.gain16[1] && c.waste_us == rt_cost_fixed.waste_us);
	e.off = 0;
	e.srv_rate = 0;
	e.held_each = 0;
	rt_ev_cost(&e, 1, &rt_cost_fixed, &c);
	CHECK(c.gain16[1] == rt_cost_fixed.gain16[1] && c.waste_us == rt_cost_fixed.waste_us);
}

/* How many pieces ahead, from how many were read for nothing: one less over
 * a quarter of what was read ahead, all again under a sixteenth; not on too
 * little served, not while off. */
static void test_pieces(void)
{
	RtEv e;

	memset(&e, 0, sizeof(e));
	e.srv_q = RT_EV_MIN_SRV - 1;
	e.waste = 100;                                 /* far over, but too little served */
	CHECK(!rt_ev_pieces(&e) && !e.fewer);
	e.srv_q = 30 * RT_PIECE_SECTORS;               /* 10 of 40 for nothing: a quarter, not over */
	e.waste = 10;
	CHECK(!rt_ev_pieces(&e) && !e.fewer);
	e.waste = 11;                                  /* over: one less */
	CHECK(rt_ev_pieces(&e) && e.fewer);
	e.waste = 6;                                   /* 6 of 36: a sixth - kept at one less */
	CHECK(!rt_ev_pieces(&e) && e.fewer);
	e.waste = 2;                                   /* 2 of 32: a sixteenth, not under - kept */
	CHECK(!rt_ev_pieces(&e) && e.fewer);
	e.waste = 1;                                   /* 1 of 31: under a sixteenth - all again */
	CHECK(rt_ev_pieces(&e) && !e.fewer);
	e.off = 1;
	e.waste = 100;
	CHECK(!rt_ev_pieces(&e) && !e.fewer);          /* off: nothing served to judge */
	/* the model with two ahead: fewer pieces for nothing at a run's end */
	{
		uint32_t pos[RT_POS] = { 0 }, run[RT_RUN] = { 0 };
		RtScores a, b;

		run[6] = 10;
		pos[6] = 2560;
		rt_scores(pos, run, 176, 3000, 0, &a);
		rt_scores(pos, run, 176, 3000, 1, &b);
		CHECK(a.sect[4] == b.sect[4] && b.sc[4] - a.sc[4] == 10 * 3000);   /* m 4: 3 left, 2 read */
		CHECK(a.sc[5] == b.sc[5]);                               /* m 5: 2 left either way */
	}
}

/* The model over the reads' stretch: each round's own part of the gain (the
 * counts' gain less half the last: they were halved), kept 7/8 a round as
 * the reads are. A loading screen, then quiet play: the counts' figure falls
 * to nothing within a few rounds, the stretch's falls as the reads' does. */
static void test_model_stretch(void)
{
	RtModelStretch m;
	int64_t counts = 0, time = 0, pm = 0, pm_counts = 0;

	memset(&m, 0, sizeof(m));
	/* steady: 1000 of gain a round over 4000 of time - the stretch says 200 permille */
	for (int r = 0; r < 60; r++) {
		counts = counts / 2 + 1000;       /* the counts, halved each round */
		time = time / 2 + 4000;
		pm = rt_model_stretch(&m, counts, 4000, 0);
	}
	CHECK(pm >= 195 && pm <= 205 && m.gain > 7900 && m.gain <= 8000);
	/* quiet: no new gain, the reads' time goes on */
	for (int r = 0; r < 6; r++) {
		counts /= 2;
		time = time / 2 + 4000;
		pm = rt_model_stretch(&m, counts, 4000, 0);
		pm_counts = rt_pm_div(counts * 1000, time + counts);
	}
	CHECK(pm_counts < 20);                /* the counts' figure: next to nothing */
	CHECK(pm > 80);                       /* the stretch's: still much of it (as the reads) */
	/* off: no gain added on top of the time */
	memset(&m, 0, sizeof(m));
	CHECK(rt_model_stretch(&m, 1000, 1000, 1) == 1000 && rt_model_stretch(&m, 500, 0, 0) < 1000);
}

/* The game's pauses: what of a piece's read time they cover. */
static void test_hide(void)
{
	uint32_t sec[RT_GAP_B];
	RtCost c = rt_cost_fixed;

	/* the steps */
	CHECK(rt_gap_bucket(0) == 0 && rt_gap_bucket(249) == 0 && rt_gap_bucket(250) == 1 &&
	      rt_gap_bucket(499) == 1 && rt_gap_bucket(500) == 2 && rt_gap_bucket(1999) == 3 &&
	      rt_gap_bucket(2000) == 4 && rt_gap_bucket(15999) == 6 && rt_gap_bucket(16000) == 7 &&
	      rt_gap_bucket(0xFFFFFFFFu) == 7);
	/* too little seen: nothing taken off */
	memset(sec, 0, sizeof(sec));
	sec[0] = RT_GAP_MIN_SEC - 1;
	CHECK(rt_hide_pm(sec, RT_PIECE_US) == 1000 && !rt_hide_known(sec));
	sec[0] = RT_GAP_MIN_SEC;
	CHECK(rt_hide_known(sec));
	sec[0] = 0;
	/* reads back to back (pauses under 250 us): next to nothing to overlap */
	sec[0] = 20000;
	CHECK(rt_hide_pm(sec, RT_PIECE_US) < 30);
	/* pauses of 16 ms and more: the whole piece is covered */
	memset(sec, 0, sizeof(sec));
	sec[7] = 20000;
	CHECK(rt_hide_pm(sec, RT_PIECE_US) == 1000);
	/* half the reads after a long pause, half back to back: about half */
	memset(sec, 0, sizeof(sec));
	sec[0] = 10000;
	sec[7] = 10000;
	{
		const uint32_t pm = rt_hide_pm(sec, RT_PIECE_US);

		CHECK(pm > 480 && pm < 530);
	}
	/* a pause as long as the piece covers it: a slower card needs a longer one */
	memset(sec, 0, sizeof(sec));
	sec[5] = 20000;                                  /* 6 ms */
	CHECK(rt_hide_pm(sec, 6000) == 1000 && rt_hide_pm(sec, 12000) == 500);
	CHECK(rt_hide_pm(sec, 0) == 1000);
	/* the saving a sector is scaled, never raised */
	rt_hide_apply(&c, 0, 500);
	CHECK(c.gain16[0] == rt_cost_fixed.gain16[0] / 2 && c.gain16[1] == rt_cost_fixed.gain16[1]);
	rt_hide_apply(&c, 1, 5000);
	CHECK(c.gain16[1] == rt_cost_fixed.gain16[1]);
	rt_hide_apply(&c, 1, 0);
	CHECK(c.gain16[1] == 0);
}

int main(void)
{
	test_ev();
	test_pieces();
	test_model_stretch();
	test_hide();
	test_scores_one_pass();
	test_small_pick();
	test_tiny_model();

	printf(vm_failures ? "test_ra_test: %d FAILED\n" : "test_ra_test: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}