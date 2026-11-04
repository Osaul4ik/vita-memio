/*
 * test_ra_test.c - the read ahead test's sums (kernel/ra_test.h): what each kind
 * of game should get, above all that Off comes only when nothing would be gained.
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

int main(void)
{
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
	CHECK(P.ra && P.thr[1] >= RT_THR_MIN && P.thr[1] <= RT_THR_MAX && P.copy_nc);

	/* nothing but scattered 128 KiB reads: Off is right, nothing to gain */
	go();
	for (int i = 0; i < 400; i++)
		rd(1000000u + (uint32_t)i * 7777u, 256, 0);
	ra_test_pick(&T, &P);
	CHECK(!P.little && !P.ra && P.pct == 0);

	/* short streams only, two reads each, and the same scattered: never read
	 * ahead for them, but not On for nothing either */
	go();
	runs(200, 2, 256, 1, &at);
	ra_test_pick(&T, &P);
	CHECK(!P.ra);

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
	CHECK(P.ra && P.thr[0] != RT_NEVER && P.thr[0] != 0 && P.thr[1] == RT_NEVER && !P.copy_nc);

	/* a game reading a sector at a time (GTA): on for its tiny reads alone */
	go();
	for (uint32_t i = 0; i < 40000; i++, at++)
		rd(at, 1, 0);
	runs(1, 1, 64, 0, &at);
	for (int i = 0; i < 300; i++)
		rd(7000000u + (uint32_t)i * 7777u, 128, 0);
	ra_test_pick(&T, &P);
	CHECK(P.ra);

	/* 32 KiB reads in long runs */
	go();
	runs(5, 300, 64, 0, &at);
	ra_test_pick(&T, &P);
	CHECK(P.ra && P.thr[0] != RT_NEVER);
	/* ... in runs too short to start: nothing */
	go();
	runs(100, 10, 64, 0, &at);
	ra_test_pick(&T, &P);
	CHECK(!P.ra);

	/* a pause too long, as a video: every read on its own, nothing to gain */
	go();
	for (int i = 0; i < 400; i++, at += 256) {
		now += RT_MAX_AGE_US + 1;
		ra_test_note(&T, &K, at, 256, 0, 0, now);
	}
	ra_test_pick(&T, &P);
	CHECK(!P.little && !P.ra);

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
