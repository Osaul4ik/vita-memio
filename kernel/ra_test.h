/*
 * ra_test.h - the overlay's read ahead test, kept free of kernel calls so the
 * host tests can drive it (tests/test_ra_test.c). Included by stage.c only.
 *
 * While it runs, every read of the app is followed as read ahead follows it:
 * one stream of large reads, one of 32 KiB reads, one of tiny reads, broken by
 * a read elsewhere, a large write or a pause too long for what was read early
 * to be used. It works out what read ahead would have given whatever read ahead
 * is set to. What brings nothing and costs nothing is left as it is; what would
 * only cost is switched off, for that kind of read or altogether.
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
#define RT_MAX_AGE_US      5000000u      /* as RA_MAX_AGE_US                    */
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

/* What a sector from the pieces saves (sixteenths of a us: C, NC) and a
 * piece read for nothing costs (us): the figures above, or this console's
 * own measured ones (stage.c: calibration). */
typedef struct {
	int32_t gain16[2];
	int32_t waste_us;
	int32_t pieces;                      /* read ahead's depth: 3, or 4 (deeper) */
} RtCost;

static const RtCost rt_cost_fixed = { { RT_GAIN_C_US * 16, RT_GAIN_NC_US * 16 }, RT_WASTE_US,
                                      RT_PIECES };

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
	uint32_t total_sectors;              /* every read                          */
	uint32_t tiny_hits, tiny_runs;
	uint32_t ended[2];                   /* runs ended, both sizes (Adaptive's rounds) */
	RtStream large, small;
	const void *tiny_key;
	uint32_t tiny_end, tiny_len;
} RaTest;

typedef struct {
	int      little;                     /* too little was read to tell         */
	int      ra;                         /* 1 on                                */
	uint32_t thr[2];                     /* C, NC: 2..8; 0 the fixed: no gain, no loss;
	                                      * RT_NEVER: the fixed one would lose  */
	int      copy_nc;                    /* what read ahead holds for NC is worth copying */
	uint32_t pct;                        /* of what was read, from read ahead   */
} RaPick;

static inline void rt_end_large(RaTest *t)
{
	RtStream *s = &t->large;

	if (s->len) {
		t->run[s->nc][s->len < RT_RUN ? s->len : RT_RUN - 1]++;
		t->ended[s->nc]++;
	}
	s->len = 0;
}

static inline void rt_end_small(RaTest *t)
{
	RtStream *s = &t->small;

	if (s->len) {
		const uint32_t b = s->len / 4;

		t->srun[s->nc][b < RT_RUN ? b : RT_RUN - 1]++;
		t->ended[s->nc]++;
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
static inline int64_t rt_score(const uint32_t *pos, const uint32_t *run, int m, int32_t gain16,
                               int32_t waste_us, int pieces, uint64_t *sectors)
{
	uint64_t sect = 0;
	int64_t waste = 0;

	for (int i = m + 1; i < RT_POS; i++)
		sect += pos[i];
	for (int l = m; l < RT_RUN; l++) {
		const int ahead = l - m + 1;

		waste += (int64_t)run[l] * (ahead < pieces ? ahead : pieces);
	}
	*sectors += sect;
	return (int64_t)sect * gain16 / 16 - waste * waste_us;
}

/* What threshold t would bring for kind k, both sizes. */
static inline int64_t rt_kind_score(const RaTest *t, int k, int thr, const RtCost *cost,
                                    uint64_t *sectors)
{
	const int32_t gain16 = cost->gain16[k], waste_us = cost->waste_us;
	/* 32 KiB reads: from 16 in a row; a threshold of its own, four times it */
	const int sm = thr == RT_THR_FIXED || thr < RT_SMALL_MIN_BUCKET ? RT_SMALL_MIN_BUCKET : thr;
	uint64_t ss = 0;
	int64_t sc = rt_score(t->pos[k], t->run[k], thr, gain16, waste_us, cost->pieces, sectors);

	/* a 32 KiB read at bucket b is from the pieces once b >= sm */
	{
		uint64_t from = 0;
		int64_t waste = 0;

		for (int i = sm; i < RT_POS; i++)
			from += t->spos[k][i];
		for (int l = sm; l < RT_RUN; l++) {
			const int ahead = l - sm + 1;

			waste += (int64_t)t->srun[k][l] * (ahead < cost->pieces ? ahead : cost->pieces);
		}
		ss = from;
		sc += (int64_t)from * gain16 / 16 - waste * waste_us;
	}
	*sectors += ss;
	return sc;
}

/* The answer; the runs still open count as ended. */
static inline void ra_test_pick_cost(RaTest *t, RaPick *p, const RtCost *cost)
{
	int64_t total = 0, nc_best = 0;
	int loss = 0;
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
		const int64_t fixed = rt_kind_score(t, k, RT_THR_FIXED, cost, &best_s);
		int64_t best = fixed;

		for (int i = RT_THR_MIN; i <= RT_THR_MAX; i++) {
			uint64_t s = 0;
			const int64_t sc = rt_kind_score(t, k, i, cost, &s);

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
		} else if (fixed < 0) {
			p->thr[k] = RT_NEVER;        /* it would only cost */
			loss = 1;
		}                                /* else nothing either way: left as it is */
	}
	if (t->tiny_hits) {
		const int64_t tiny = (int64_t)t->tiny_hits * RT_TINY_GAIN_US -
		                     (int64_t)t->tiny_runs * RT_TINY_WASTE_US;

		if (tiny > 0) {
			total += tiny;
			from += t->tiny_hits;
		} else if (tiny < 0) {
			loss = 1;
		}
	}
	p->ra = total > 0 || !loss;          /* off only if it would cost and bring nothing */
	p->copy_nc = p->ra && nc_best > 0;
	p->pct = t->total_sectors ? (uint32_t)(from * 100u / t->total_sectors) : 0;
	if (!p->ra)
		p->thr[0] = p->thr[1] = 0;
}

static inline void ra_test_pick(RaTest *t, RaPick *p)
{
	ra_test_pick_cost(t, p, &rt_cost_fixed);
}

/* Read ahead Adaptive's pick for kind k: the threshold 2..8 that would have
 * brought the most over the runs so far, `cur` unless another is better by
 * more than `margin` (us). */
static inline int ra_adapt_pick(const RaTest *t, int k, int cur, int64_t margin, const RtCost *cost)
{
	int64_t sc[RT_THR_MAX + 1];
	int best = cur;

	for (int i = RT_THR_MIN; i <= RT_THR_MAX; i++) {
		uint64_t s = 0;

		sc[i] = rt_kind_score(t, k, i, cost, &s);
	}
	for (int i = RT_THR_MIN; i <= RT_THR_MAX; i++)
		if (sc[i] > sc[best])
			best = i;
	return sc[best] - sc[cur] > margin ? best : cur;
}

/* Kind k's counts halved: what was read long ago weighs less (a game's
 * loading and its play differ). */
static inline void ra_adapt_age(RaTest *t, int k)
{
	for (int i = 0; i < RT_POS; i++) {
		t->pos[k][i] >>= 1;
		t->spos[k][i] >>= 1;
	}
	for (int i = 0; i < RT_RUN; i++) {
		t->run[k][i] >>= 1;
		t->srun[k][i] >>= 1;
	}
	t->ended[k] = 0;
}

/* ---- tiny reads (under 32 KiB): Adaptive's model -----------------------
 * A run of tiny reads in a row: from the T-th on, a read that finds nothing
 * reads a block of B sectors at once (on its own thread), and the next ones
 * that lie in it are copied from it. A run is kept by how many reads it had
 * (RT_TB buckets) with their sectors, so the read size is known; runs of one
 * read are not kept (no T starts on them). What (T, B) brings a run of R
 * reads of s sectors, with M = R - T + 1 reads from the T-th on, k = B / s of
 * them a block, F = ceil(M / k) blocks:
 *   (M - F) reads copied from a block save   card(s) - hit
 *   F blocks read in place of F reads cost   card(B) - card(s)
 * with a card read of n sectors taking a + b n. */
#define RT_TB            22
#define RT_TINY_B_N      3
static const uint16_t rt_tiny_rep[RT_TB] = {   /* a bucket's reads, as counted below */
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 28, 40, 56, 96, 192 };
static const uint8_t rt_tiny_thr[] = { 2, 3, 4, 6, 8, 12, 16 };
#define RT_TINY_T_N      ((int)sizeof(rt_tiny_thr))
static const uint8_t rt_tiny_blk[RT_TINY_B_N] = { 16, 32, 64 };   /* sectors */

typedef struct {
	int32_t a_us, b16_us;                /* a card read: a + b/16 a sector     */
	int32_t hit_us;                      /* a read copied from the block       */
} RtTinyCost;

/* The figures measured on the console earlier: one sector 395 us, a block of
 * 64 about 2 ms, from the block 46 us. */
static const RtTinyCost rt_tiny_fixed = { 370, 25 * 16, 46 };

static inline int rt_tiny_bucket(uint32_t reads)
{
	if (reads <= 16)
		return (int)reads - 1;
	return reads <= 24 ? 16 : reads <= 32 ? 17 : reads <= 48 ? 18 : reads <= 64 ? 19
	     : reads <= 128 ? 20 : 21;
}

static inline int64_t rt_tiny_card(const RtTinyCost *c, uint32_t sectors)
{
	return (int64_t)c->a_us + (int64_t)c->b16_us * sectors / 16;
}

/* The runs, ready to be weighed: each kept bucket's runs, a run's reads and
 * a read's sectors and card time - worked out once for every (T, B). */
typedef struct {
	int      n;
	uint32_t cnt[RT_TB], r[RT_TB], s[RT_TB];
	int64_t  card_s[RT_TB];
} RtTinyRuns;

static inline void rt_tiny_prep(const uint32_t *cnt, const uint32_t *reads, const uint32_t *sec,
                                const RtTinyCost *c, RtTinyRuns *o)
{
	o->n = 0;
	for (int i = 1; i < RT_TB; i++) {    /* bucket 0 (one read) is never kept */
		uint32_t s;

		if (!cnt[i] || !reads[i])
			continue;
		s = (sec[i] + reads[i] / 2) / reads[i];
		o->cnt[o->n] = cnt[i];
		o->r[o->n] = (reads[i] + cnt[i] / 2) / cnt[i];
		o->s[o->n] = s ? s : 1;
		o->card_s[o->n] = rt_tiny_card(c, o->s[o->n]);
		o->n++;
	}
}

static inline int64_t rt_tiny_score_prep(const RtTinyRuns *p, int t, int blk, const RtTinyCost *c)
{
	const int64_t card_b = rt_tiny_card(c, (uint32_t)blk);
	int64_t sc = 0;

	for (int i = 0; i < p->n; i++) {
		const uint32_t r = p->r[i], s = p->s[i];
		uint32_t m, k, f;

		if (r < (uint32_t)t || s > (uint32_t)blk)
			continue;
		m = r - (uint32_t)t + 1;
		k = (uint32_t)blk / s;
		f = (m + k - 1) / k;
		sc += (int64_t)p->cnt[i] * ((int64_t)(m - f) * (p->card_s[i] - c->hit_us) -
		                            (int64_t)f * (card_b - p->card_s[i]));
	}
	return sc;
}

/* What (T, B) brings over the runs: cnt[i] runs in bucket i (any scale),
 * reads[i] and sec[i] their reads and sectors (the same scale). */
static inline int64_t rt_tiny_score(const uint32_t *cnt, const uint32_t *reads, const uint32_t *sec,
                                    int t, int blk, const RtTinyCost *c)
{
	RtTinyRuns p;

	rt_tiny_prep(cnt, reads, sec, c, &p);
	return rt_tiny_score_prep(&p, t, blk, c);
}

/* Adaptive's pick: the (T, B) that would have brought the most, (*t, *blk)
 * unless another is better by more than `margin`. 1 if it changed. */
static inline int rt_tiny_pick(const uint32_t *cnt, const uint32_t *reads, const uint32_t *sec,
                               int *t, int *blk, int64_t margin, const RtTinyCost *c)
{
	RtTinyRuns p;
	int64_t cur, best;
	int bt = *t, bb = *blk;

	rt_tiny_prep(cnt, reads, sec, c, &p);
	cur = best = rt_tiny_score_prep(&p, *t, *blk, c);
	for (int i = 0; i < RT_TINY_T_N; i++)
		for (int j = 0; j < RT_TINY_B_N; j++) {
			const int64_t v = rt_tiny_score_prep(&p, rt_tiny_thr[i], rt_tiny_blk[j], c);

			if (v > best) {
				best = v;
				bt = rt_tiny_thr[i];
				bb = rt_tiny_blk[j];
			}
		}
	if (best - cur <= margin || (bt == *t && bb == *blk))
		return 0;
	*t = bt;
	*blk = bb;
	return 1;
}

#endif /* VITAMEMIO_RA_TEST_H */
