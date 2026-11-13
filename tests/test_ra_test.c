/*
 * test_ra_test.c - the read ahead test's sums (kernel/ra_test.h): what each kind
 * of game should get, above all that Off comes only when it would cost and bring nothing.
 */
#include <string.h>
#include <stdlib.h>

#include "../kernel/ra_test.h"
#include "vm_test.h"

static RaTest T;
static RaPick P;
static uint32_t now;
static const int K;

static void rd(uint32_t at, uint32_t n, int nc)
{
	now += 5000;
	ra_test_note(&T, &K, at, n, nc, 0, now);
}

/* `runs` runs of `len` reads of n sectors, each run far from the last */
static void runs(int count, int len, uint32_t n, int nc, uint32_t *at)
{
	for (int r = 0; r < count; r++) {
		for (int i = 0; i < len; i++, *at += n)
			rd(*at, n, nc);
		*at += 100000;
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
 * the sectors after the m-th read, less min(l - m + 1, 3) pieces a run of l. */
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
		rt_scores(pos, run, g, w, &o);
		for (int m = 0; m < RT_RUN; m++) {
			uint64_t sect = 0;
			int64_t waste = 0;

			for (int i = m + 1; i < RT_POS; i++)
				sect += pos[i];
			for (int l = m; l < RT_RUN; l++)
				waste += (int64_t)run[l] * (l - m + 1 < 3 ? l - m + 1 : 3);
			CHECK(o.sect[m] == sect && o.sc[m] == (int64_t)sect * g / 16 - waste * w);
		}
	}
}

/* Adaptive's 32 KiB reads' threshold, on its own: long runs - from 4 reads;
 * runs of 9 - from 12 (sooner only reads for nothing), and the large reads'
 * threshold is left as it is; within the margin nothing moves. */
static void test_small_pick(void)
{
	uint32_t at = 0;
	int thr = 5, sm = RT_SMALL_MIN_BUCKET;

	go();
	runs(32, 40, 64, 0, &at);
	CHECK(ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && sm == 1 && thr == 5);
	go();
	runs(32, 9, 64, 0, &at);
	CHECK(ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && sm == 3 && thr == 5);
	sm = RT_SMALL_MIN_BUCKET;                     /* from 16: nothing either way */
	CHECK(!ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && sm == RT_SMALL_MIN_BUCKET);
	go();
	runs(32, 40, 64, 0, &at);
	CHECK(!ra_adapt_pick(&T, 0, &thr, &sm, INT64_MAX / 2, &rt_cost_fixed) && sm == RT_SMALL_MIN_BUCKET);
	/* the large reads' on their own: long runs of them move only theirs */
	go();
	runs(32, 20, 256, 0, &at);
	CHECK(ra_adapt_pick(&T, 0, &thr, &sm, 0, &rt_cost_fixed) && thr == RT_THR_MIN &&
	      sm == RT_SMALL_MIN_BUCKET);
}

int main(void)
{
	test_scores_one_pass();
	test_small_pick();
	test_tiny_model();
	uint32_t at = 0;

	/* too little read: no answer, not an Off */
	go();
	runs(2, 20, 256, 0, &at);
	ra_test_pick(&T, &P);
	CHECK(P.little && !P.ra);

	/* one long stream into cached memory */
	go();
	runs(1, 400, 256, 0, &at);
	ra_test_pick(&T, &P);
	CHECK(!P.little && P.ra && P.thr[0] == RT_THR_MIN && P.thr[1] == 0 && !P.copy_nc && P.pct > 90);

	/* long streams into non-cacheable memory: and their copies are worth moving */
	go();
	runs(30, 40, 256, 1, &at);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[1] >= RT_THR_MIN && P.thr[1] <= RT_THR_MAX && P.thr[0] == 0 && P.copy_nc);

	/* nothing but scattered 128 KiB reads: no gain, no loss, left as it is */
	go();
	for (int i = 0; i < 400; i++)
		rd(1000000u + (uint32_t)i * 7777u, 256, 0);
	ra_test_pick(&T, &P);
	CHECK(!P.little && P.ra && P.thr[0] == 0 && P.thr[1] == 0 && !P.copy_nc && P.pct == 0);

	/* short streams, two reads each: the fixed threshold never starts: left */
	go();
	runs(200, 2, 256, 1, &at);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[1] == 0);

	/* streams just long enough to start it and no more: it would only cost: off */
	go();
	runs(100, 6, 128, 1, &at);
	ra_test_pick(&T, &P);
	CHECK(!P.little && !P.ra && P.thr[1] == 0 && !P.copy_nc);

	/* mostly scattered with some long streams: on, it is worth it, not Off */
	go();
	for (int i = 0; i < 500; i++)
		rd(5000000u + (uint32_t)i * 7777u, 256, 1);
	runs(4, 60, 256, 1, &at);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[1] != RT_NEVER && P.copy_nc);

	/* streams of one kind, scattered reads of the other: only that kind on */
	go();
	runs(10, 60, 256, 0, &at);
	for (int i = 0; i < 300; i++)
		rd(6000000u + (uint32_t)i * 7777u, 256, 1);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[0] != RT_NEVER && P.thr[0] != 0 && P.thr[1] == 0 && !P.copy_nc);

	/* a game reading a sector at a time (GTA): on for its tiny reads alone */
	go();
	for (uint32_t i = 0; i < 40000; i++, at++)
		rd(at, 1, 0);
	runs(1, 1, 64, 0, &at);
	for (int i = 0; i < 300; i++)
		rd(7000000u + (uint32_t)i * 7777u, 128, 0);
	ra_test_pick(&T, &P);
	CHECK(P.ra);
	/* ... and next to nothing into non-cacheable memory: left, nothing to copy */
	CHECK(P.thr[1] == 0 && !P.copy_nc);

	/* one kind gains, the other would only cost: that one never */
	go();
	runs(10, 60, 256, 0, &at);
	runs(100, 6, 128, 1, &at);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[0] >= RT_THR_MIN && P.thr[1] == RT_NEVER && !P.copy_nc);

	/* 32 KiB reads in long runs */
	go();
	runs(5, 300, 64, 0, &at);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[0] != RT_NEVER);
	/* ... in runs too short to start: left */
	go();
	runs(100, 10, 64, 0, &at);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[0] == 0);

	/* a pause too long, as a video: every read on its own: no gain, no loss */
	go();
	for (int i = 0; i < 400; i++, at += 256) {
		now += RT_MAX_AGE_US + 1;
		ra_test_note(&T, &K, at, 256, 0, 0, now);
	}
	ra_test_pick(&T, &P);
	CHECK(!P.little && P.ra && P.thr[0] == 0);

	/* a large write ends a stream, a small one does not */
	go();
	rd(0, 256, 0);
	rd(256, 256, 0);
	ra_test_note(&T, &K, 9000, 8, 0, 1, now);
	CHECK(T.large.len == 2);
	ra_test_note(&T, &K, 9000, 256, 0, 1, now);
	CHECK(T.large.len == 0 && T.run[0][2] == 1);

	/* two storages are not one stream */
	go();
	now += 1;
	ra_test_note(&T, &K, 0, 256, 0, 0, now);
	ra_test_note(&T, &T, 256, 256, 0, 0, now);
	CHECK(T.large.len == 1);

	printf(vm_failures ? "test_ra_test: %d FAILED\n" : "test_ra_test: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
