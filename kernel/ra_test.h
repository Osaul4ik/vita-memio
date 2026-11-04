/*
 * ra_test.h - the overlay's read ahead test, kept free of kernel calls so the
 * host tests can drive it (tests/test_ra_test.c). Included by stage.c only.
 *
 * While it runs, every read of the app is followed as read ahead follows it:
 * one stream of large reads, one of 32 KiB reads, one of tiny reads, broken by
 * a read elsewhere, a large write or a pause too long for what was read early
 * to be used. It works out what read ahead would have given whatever read ahead
 * is set to, so Off is only ever the answer when it would not have helped.
 *
 * For each threshold it adds up time saved and time lost. Times are from the
 * console's measurements: a 128 KiB read in a stream 7.5 -> 4.6 ms, into
 * non-cacheable memory 6.5 -> 2.7 ms; a one-sector read 395 -> 46 us. A piece
 * read for nothing takes about 7.5 ms of the card on the system core, but only
 * holds up a read that comes meanwhile, so it counts for less.
 */
#ifndef VITAMEMIO_RA_TEST_H
#define VITAMEMIO_RA_TEST_H

#include <stdint.h>

#define RT_MIN_SECTORS     128u          /* large reads: as RA_MIN_SECTORS      */
#define RT_SMALL_SECTORS   64u           /* 32 KiB reads: as RA_SMALL_SECTORS   */
#define RT_PIECE_SECTORS   256u
#define RT_PIECES          3u
#define RT_TINY_STREAK     8u
#define RT_MAX_AGE_US      500000u
#define RT_POS             10            /* positions 0..9, the last: 9 and on  */
#define RT_RUN             11            /* run lengths 0..10, the last: 10 and on */
#define RT_THR_MIN         2
#define RT_THR_MAX         8
#define RT_THR_FIXED       5             /* as RA_STREAK                        */
#define RT_SMALL_MIN_BUCKET 4            /* 16 reads, in fours                  */
#define RT_NEVER           0xFFFFu       /* as VM_RA_NEVER                      */

#define RT_GAIN_C_US       11            /* per sector from the pieces, cached  */
#define RT_GAIN_NC_US      15            /* ... non-cacheable                   */
#define RT_WASTE_US        3000          /* a piece read for nothing            */
#define RT_TINY_GAIN_US    350           /* a tiny read from its block          */
#define RT_TINY_WASTE_US   1400          /* half a block left when a run ends   */
#define RT_MIN_TOTAL_SECTORS (16u * 2048u)   /* below 16 MiB: too little to tell */
#define RT_MIN_KIND_SECTORS  2048u       /* below 1 MiB of a kind: the fixed one */

typedef struct {
	const void *key;                     /* the storage                         */
	uint32_t    end, len, us;
	int         nc;
} RtStream;

typedef struct {
	uint32_t pos[2][RT_POS];             /* sectors of large reads, by place in the run */
	uint32_t run[2][RT_RUN];             /* runs of large reads, by length      */
	uint32_t spos[2][RT_POS];            /* 32 KiB reads, in fours              */
	uint32_t srun[2][RT_RUN];
	uint32_t kind_sectors[2];
	uint32_t total_sectors;              /* every read                          */
	uint32_t tiny_hits, tiny_runs;
	RtStream large, small;
	const void *tiny_key;
	uint32_t tiny_end, tiny_len;
} RaTest;

typedef struct {
	int      little;                     /* too little was read to tell         */
	int      ra;                         /* 1 on                                */
	uint32_t thr[2];                     /* C, NC: 2..8, RT_NEVER, 0 the fixed  */
	int      copy_nc;                    /* what read ahead holds for NC is worth copying */
	uint32_t pct;                        /* of what was read, from read ahead   */
} RaPick;

static inline void rt_end_large(RaTest *t)
{
	RtStream *s = &t->large;

	if (s->len)
		t->run[s->nc][s->len < RT_RUN ? s->len : RT_RUN - 1]++;
	s->len = 0;
}

static inline void rt_end_small(RaTest *t)
{
	RtStream *s = &t->small;

	if (s->len) {
		const uint32_t b = s->len / 4;

		t->srun[s->nc][b < RT_RUN ? b : RT_RUN - 1]++;
	}
	s->len = 0;
}

/* One transfer: `key` tells the storage apart, `nc` its buffer is non-cacheable. */
static inline void ra_test_note(RaTest *t, const void *key, uint32_t sector, uint32_t n, int nc,
                                int write, uint32_t now)
{
	if (write) {
		if (n >= RT_MIN_SECTORS) {       /* as read ahead: a large write ends the streams */
			rt_end_large(t);
			rt_end_small(t);
		}
		return;
	}
	t->total_sectors += n;
	if (n < RT_SMALL_SECTORS) {
		t->tiny_len = key == t->tiny_key && sector == t->tiny_end && t->tiny_len ? t->tiny_len + 1 : 1;
		t->tiny_key = key;
		t->tiny_end = sector + n;
		if (t->tiny_len == RT_TINY_STREAK)
			t->tiny_runs++;
		else if (t->tiny_len > RT_TINY_STREAK)
			t->tiny_hits++;
		return;
	}
	nc = !!nc;
	t->kind_sectors[nc] += n;
	{
		const int large = n >= RT_MIN_SECTORS;
		RtStream *s = large ? &t->large : &t->small;

		if (!s->len || key != s->key || sector != s->end || nc != s->nc ||
		    now - s->us >= RT_MAX_AGE_US) {
			if (large)
				rt_end_large(t);
			else
				rt_end_small(t);
		}
		s->len++;
		s->key = key;
		s->end = sector + n;
		s->nc = nc;
		s->us = now;
		if (large) {
			const uint32_t have = RT_PIECES * RT_PIECE_SECTORS;

			t->pos[nc][s->len < RT_POS ? s->len : RT_POS - 1] += n < have ? n : have;
		} else {
			const uint32_t b = (s->len - 1) / 4;

			t->spos[nc][b < RT_POS ? b : RT_POS - 1] += n;
		}
	}
}

/* Pieces left unread by the runs that reached `m`, and sectors after it. */
static inline int64_t rt_score(const uint32_t *pos, const uint32_t *run, int m, int gain,
                               uint64_t *sectors)
{
	uint64_t sect = 0;
	int64_t waste = 0;

	for (int i = m + 1; i < RT_POS; i++)
		sect += pos[i];
	for (int l = m; l < RT_RUN; l++) {
		const int ahead = l - m + 1;

		waste += (int64_t)run[l] * (ahead < (int)RT_PIECES ? ahead : (int)RT_PIECES);
	}
	*sectors += sect;
	return (int64_t)sect * gain - waste * RT_WASTE_US;
}

/* What threshold t would bring for kind k, both sizes. */
static inline int64_t rt_kind_score(const RaTest *t, int k, int thr, uint64_t *sectors)
{
	const int gain = k ? RT_GAIN_NC_US : RT_GAIN_C_US;
	/* 32 KiB reads: from 16 in a row; a threshold of its own, four times it */
	const int sm = thr == RT_THR_FIXED || thr < RT_SMALL_MIN_BUCKET ? RT_SMALL_MIN_BUCKET : thr;
	uint64_t ss = 0;
	int64_t sc = rt_score(t->pos[k], t->run[k], thr, gain, sectors);

	/* a 32 KiB read at bucket b is from the pieces once b >= sm */
	{
		uint64_t from = 0;
		int64_t waste = 0;

		for (int i = sm; i < RT_POS; i++)
			from += t->spos[k][i];
		for (int l = sm; l < RT_RUN; l++) {
			const int ahead = l - sm + 1;

			waste += (int64_t)t->srun[k][l] * (ahead < (int)RT_PIECES ? ahead : (int)RT_PIECES);
		}
		ss = from;
		sc += (int64_t)from * gain - waste * RT_WASTE_US;
	}
	*sectors += ss;
	return sc;
}

/* The answer; the runs still open count as ended. */
static inline void ra_test_pick(RaTest *t, RaPick *p)
{
	int64_t total = 0, nc_best = 0;
	uint64_t from = 0;

	rt_end_large(t);
	rt_end_small(t);
	p->little = t->total_sectors < RT_MIN_TOTAL_SECTORS;
	p->ra = 0;
	p->thr[0] = p->thr[1] = 0;
	p->copy_nc = 0;
	p->pct = 0;
	if (p->little)
		return;
	for (int k = 0; k < 2; k++) {
		int best_t = RT_THR_FIXED;
		uint64_t best_s = 0;
		int64_t best = rt_kind_score(t, k, RT_THR_FIXED, &best_s);

		if (t->kind_sectors[k] < RT_MIN_KIND_SECTORS)
			continue;                    /* too little of it: the fixed one */
		for (int i = RT_THR_MIN; i <= RT_THR_MAX; i++) {
			uint64_t s = 0;
			const int64_t sc = rt_kind_score(t, k, i, &s);

			if (sc > best) {
				best = sc;
				best_t = i;
				best_s = s;
			}
		}
		if (best > 0) {
			p->thr[k] = (uint32_t)best_t;
			total += best;
			from += best_s;
			if (k)
				nc_best = best;
		} else {
			p->thr[k] = RT_NEVER;
		}
	}
	if (t->tiny_hits) {
		const int64_t tiny = (int64_t)t->tiny_hits * RT_TINY_GAIN_US -
		                     (int64_t)t->tiny_runs * RT_TINY_WASTE_US;

		if (tiny > 0) {
			total += tiny;
			from += t->tiny_hits;
		}
	}
	p->ra = total > 0;
	p->copy_nc = p->ra && nc_best > 0;
	p->pct = t->total_sectors ? (uint32_t)(from * 100u / t->total_sectors) : 0;
	if (!p->ra)
		p->thr[0] = p->thr[1] = 0;
}

#endif /* VITAMEMIO_RA_TEST_H */
