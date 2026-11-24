/*
 * ra_test.h - read ahead Adaptive's model, kept free of kernel calls so the
 * host tests can drive it (tests/test_ra_test.c). Included by stage.c only.
 *
 * stage.c counts the app's reads as read ahead follows them: runs of large
 * reads and of 32 KiB reads, by length and by place in the run. From those
 * counts this works out what each threshold would have given, adding up time
 * saved and time lost. Times are from the console's measurements: a 128 KiB
 * read in a stream 7.5 -> 4.6 ms, into non-cacheable memory 6.5 -> 2.7 ms; a
 * one-sector read 395 -> 46 us. A piece read for nothing takes about 7.5 ms
 * of the card on the system core, but only holds up a read that comes
 * meanwhile, so it counts for less.
 */
#ifndef VITAMEMIO_RA_TEST_H
#define VITAMEMIO_RA_TEST_H

#include <stdint.h>

#define RT_PIECE_SECTORS   256u
#define RT_PIECES          3u
#define RT_POS             10            /* positions 0..9, the last: 9 and on  */
#define RT_RUN             11            /* run lengths 0..10, the last: 10 and on */
#define RT_THR_MIN         2
#define RT_THR_MAX         8

#define RT_GAIN_C_US       11            /* per sector from the pieces, cached  */
#define RT_GAIN_NC_US      15            /* ... non-cacheable                   */
#define RT_WASTE_US        3000          /* a piece read for nothing            */

/* What a sector from the pieces saves (sixteenths of a us: C, NC) and a
 * piece read for nothing costs (us): the figures above, or this console's
 * own measured ones (stage.c: calibration). */
typedef struct {
	int32_t gain16[2];
	int32_t waste_us;
} RtCost;

static const RtCost rt_cost_fixed = { { RT_GAIN_C_US * 16, RT_GAIN_NC_US * 16 }, RT_WASTE_US };

typedef struct {
	uint32_t pos[2][RT_POS];             /* sectors of large reads, by place in the run */
	uint32_t run[2][RT_RUN];             /* runs of large reads, by length      */
	uint32_t spos[2][RT_POS];            /* 32 KiB reads, in fours              */
	uint32_t srun[2][RT_RUN];
	uint32_t total_sectors;              /* every read counted                  */
	uint32_t ended[2];                   /* runs ended, both sizes (Adaptive's rounds) */
} RaTest;

/* What each threshold m would bring a stream (pos: sectors read by place in
 * the run, run: runs by length), all in one pass from the end: sect[m] the
 * sectors after the m-th read (from the pieces), sc[m] what they save less the
 * pieces read for nothing - a run of length l >= m leaves min(l - m + 1, 3)
 * unread, which is R(m) + R(m+1) + R(m+2) with R(j) the runs of j or more. */
typedef struct {
	int64_t  sc[RT_RUN];
	uint64_t sect[RT_RUN];
} RtScores;

static inline void rt_scores(const uint32_t *pos, const uint32_t *run, int32_t gain16,
                             int32_t waste_us, RtScores *o)
{
	uint64_t sect = 0, r0 = 0, r1 = 0, r2 = 0;   /* R(m+1), R(m+2), R(m+3) */

	for (int m = RT_RUN - 1; m >= 0; m--) {
		r2 = r1;
		r1 = r0;
		r0 += run[m];
		o->sect[m] = sect;
		o->sc[m] = (int64_t)sect * gain16 / 16 - (int64_t)(r0 + r1 + r2) * waste_us;
		if (m < RT_POS)
			sect += pos[m];
	}
}

/* Kind k: large[m] for a threshold of m reads, small[b] for the 32 KiB reads'
 * of 4b (their runs are kept in fours: a piece holds four). */
static inline void rt_kind_scores(const RaTest *t, int k, const RtCost *cost, RtScores *large,
                                  RtScores *small)
{
	rt_scores(t->pos[k], t->run[k], cost->gain16[k], cost->waste_us, large);
	rt_scores(t->spos[k], t->srun[k], cost->gain16[k], cost->waste_us, small);
}

/* Adaptive's 32 KiB reads' thresholds: in fours, 4..32 reads. */
#define RT_SMALL_LO        1
#define RT_SMALL_HI        8

/* The best of sc[lo..hi]: `cur` unless another is better by more than
 * `margin` (us). */
static inline int rt_best(const int64_t *sc, int lo, int hi, int cur, int64_t margin)
{
	int best = cur;

	for (int i = lo; i <= hi; i++)
		if (sc[i] > sc[best])
			best = i;
	return sc[best] - sc[cur] > margin ? best : cur;
}

/* Read ahead Adaptive's pick for kind k, worked out in one pass for every
 * threshold: *thr (2..8) for the large reads, *sm (in fours, RT_SMALL_LO..HI)
 * for the 32 KiB reads, each on its own - kept unless another is better by
 * more than `margin`. 1 if either changed. */
static inline int ra_adapt_pick(const RaTest *t, int k, int *thr, int *sm, int64_t margin,
                                const RtCost *cost)
{
	RtScores l, s;
	int nt, ns;

	rt_kind_scores(t, k, cost, &l, &s);
	nt = rt_best(l.sc, RT_THR_MIN, RT_THR_MAX, *thr, margin);
	ns = rt_best(s.sc, RT_SMALL_LO, RT_SMALL_HI, *sm, margin);
	if (nt == *thr && ns == *sm)
		return 0;
	*thr = nt;
	*sm = ns;
	return 1;
}

/* What thresholds (thr, sm) bring kind k over the counts (both sizes):
 * time saved less the pieces read for nothing, in the counts' scale. */
static inline int64_t ra_adapt_gain(const RaTest *t, int k, int thr, int sm, const RtCost *cost)
{
	RtScores l, s;

	rt_kind_scores(t, k, cost, &l, &s);
	return l.sc[thr] + s.sc[sm];
}

/* ---- on or off, from what read ahead did ---------------------------------
 * Adaptive's thresholds come from the model; whether read ahead is on for a
 * kind at all (the large and 32 KiB reads into ordinary memory, into
 * non-cacheable memory, the tiny reads) is decided from what it did in this
 * game, read by read: each read it served (all or partly) against what the
 * reads of the kind that went the standard way took in the same stretch of
 * play, less what the pieces read for nothing held other reads up. Both
 * sides from the same moments, so what the game is doing (loading, playing)
 * weighs alike on both - comparing one stretch on with another off did not
 * work: the game's own swings (10-20 % a round) were larger than what read
 * ahead changes (2-6 %).
 *   on:  off once that net is below -RT_EV_MARGIN of the kind's time, over
 *        enough reads of both ways (RT_EV_MIN_*);
 *   off: read ahead serves nothing, so nothing of the game's is measured of
 *        it - the model alone, with the DEVICE's costs (calibration: what a
 *        sector from the pieces saves, what a piece for nothing holds up) over
 *        the streams still counted, says on brings more than RT_EV_MARGIN: on
 *        again, at once - no waiting, no clock. What the game showed while it
 *        was on (srv_rate, held_each) is of that stretch only and is not used
 *        for off.
 * Going off again after such a return needs more: each return (a "strike",
 * up to RT_EV_STRIKES_MAX) doubles the margin and the reads it takes to show a
 * loss, so a kind that is wrong both ways settles on, with a loss too small to
 * matter against a gain that could be large (a wrong off costs a multiple of
 * what a wrong on does). The sums served by read ahead are cleared on the
 * return: a loss is shown again on fresh reads.
 * The sums are kept over the last rounds (7/8 of each round left); the
 * game's costs stay with it while it runs. Put away to the home screen and
 * back, the same launch goes on with each kind on or off as it was; a new
 * launch starts with every kind on and no strikes, so an off from figures
 * gone wrong is never kept for good. */
#define RT_EV_MARGIN     20u            /* permille of the kind's time             */
#define RT_EV_MIN_STD    2048u          /* the standard way: 1 MiB (tiny: reads)  */
#define RT_EV_MIN_STD_T  64u
#define RT_EV_MIN_SRV    1024u          /* from read ahead: 512 KiB (tiny: reads) */
#define RT_EV_MIN_SRV_T  32u

#define RT_EV_STRIKES_MAX 3u            /* returns from off that make the next off harder */

typedef struct {
	uint8_t  off, strikes, pad[2];   /* strikes: returns from off, this stay in front */
	/* this stretch (kept RT_EV_KEEP a round) */
	uint32_t std_us, std_q;             /* the standard way: time, sectors (tiny: reads) */
	uint32_t srv_us, srv_q;             /* served all or partly (tiny: and the reads
	                                     * that read the block): time, sectors / reads */
	uint32_t held_us, waste;            /* pieces for nothing: what they held up, how many */
	/* this game's costs, as last measured (0: none yet) */
	uint32_t std_rate, srv_rate;        /* us a MiB (tiny: a read)                  */
	uint32_t held_each;                 /* us a piece for nothing                   */
} RtEv;

/* What read ahead brought over the stretch, us (0: too little to tell). */
static inline int64_t rt_ev_net(const RtEv *e, int tiny)
{
	/* each return from off asks for twice the reads before a loss counts */
	const uint32_t k = e->strikes < RT_EV_STRIKES_MAX ? e->strikes : RT_EV_STRIKES_MAX;

	if (e->std_q < ((tiny ? RT_EV_MIN_STD_T : RT_EV_MIN_STD) << k) ||
	    e->srv_q < ((tiny ? RT_EV_MIN_SRV_T : RT_EV_MIN_SRV) << k))
		return 0;
	return (int64_t)e->srv_q * e->std_us / e->std_q - e->srv_us - e->held_us;
}

/* A round of the kind is over: its reads added (std_*, srv_*: as above;
 * held, waste: its pieces for nothing), `pm` what the model with this
 * game's costs says read ahead brings, permille. 1 if it went on or off. */
static inline int rt_ev_round(RtEv *e, int tiny, uint32_t std_us, uint32_t std_q,
                              uint32_t srv_us, uint32_t srv_q, uint32_t held, uint32_t waste,
                              int64_t pm, int64_t *net_out, int64_t *time_out)
{
	const int was = e->off;
	const uint32_t unit = tiny ? 1u : 2048u;
	int64_t net, time;

	/* the sums kept under 2^31: the round's, then the kept ones, halved
	 * together (their ratios kept) */
	while ((std_us | srv_us | held) > 0x3FFFFFFFu) {
		std_us >>= 1;
		std_q >>= 1;
		srv_us >>= 1;
		srv_q >>= 1;
		held >>= 1;
		waste >>= 1;
	}
	while ((uint64_t)e->std_us + std_us > 0x7FFFFFFFu || (uint64_t)e->srv_us + srv_us > 0x7FFFFFFFu ||
	       (uint64_t)e->held_us + held > 0x7FFFFFFFu) {
		e->std_us >>= 1;
		e->std_q >>= 1;
		e->srv_us >>= 1;
		e->srv_q >>= 1;
		e->held_us >>= 1;
		e->waste >>= 1;
	}
	e->std_us += std_us;
	e->std_q += std_q;
	e->srv_us += srv_us;
	e->srv_q += srv_q;
	e->held_us += held;
	e->waste += waste;
	/* the game's costs, from enough of each */
	if (e->std_q >= (tiny ? RT_EV_MIN_STD_T : RT_EV_MIN_STD))
		e->std_rate = (uint32_t)((uint64_t)e->std_us * unit / e->std_q);
	if (e->srv_q >= (tiny ? RT_EV_MIN_SRV_T : RT_EV_MIN_SRV))
		e->srv_rate = (uint32_t)((uint64_t)e->srv_us * unit / e->srv_q);
	if (e->waste >= 8)
		e->held_each = e->held_us / e->waste;
	net = rt_ev_net(e, tiny);
	time = (int64_t)e->std_us + e->srv_us;
	if (net_out)
		*net_out = net;
	if (time_out)
		*time_out = time;
	if (!e->off) {
		const uint32_t k = e->strikes < RT_EV_STRIKES_MAX ? e->strikes : RT_EV_STRIKES_MAX;

		if (net && net * 1000 < -(int64_t)(RT_EV_MARGIN << k) * time)
			e->off = 1;
	} else if (pm > (int64_t)RT_EV_MARGIN) {
		e->off = 0;                      /* back on, at once: no round to wait out */
		if (e->strikes < RT_EV_STRIKES_MAX)
			e->strikes++;
		e->srv_us = e->srv_q = e->held_us = e->waste = 0;   /* a loss to be shown afresh */
	}
	/* the stretch: 7/8 of it kept a round */
	e->std_us -= e->std_us / 8;
	e->std_q -= e->std_q / 8;
	e->srv_us -= e->srv_us / 8;
	e->srv_q -= e->srv_q / 8;
	e->held_us -= e->held_us / 8;
	e->waste -= e->waste / 8;
	return e->off != was;
}

/* ---- the game's pauses: what of a piece's read time can be overlapped ------
 * Measured ONLY for a kind that is off (read ahead on for all: nothing is
 * measured, the hot path as it was); it is what tells whether to turn it on.
 * A piece read ahead saves a read only as far as the game is busy elsewhere
 * while it is read: with reads back to back (the game waits for the card) the
 * next piece is still in flight when it is asked for, and read ahead brings
 * nothing, whatever it brought on another stretch. The pause before each read
 * of a stream (its start less the end of the one before: both clocks are read
 * for the on/off measure already) is kept by size in RT_GAP_B steps, from the
 * moment the kind went off. `hide` is the part of a piece's read
 * (P us, the card's, calibrated) that those pauses cover, permille. Until
 * enough has been seen to say (RT_GAP_MIN_SEC) the kind stays off: it is not
 * turned on from figures that cannot tell. It scales what a sector from the
 * pieces is taken to save in the device's figure, which is what the model
 * uses while off. */
#define RT_GAP_B         8u
#define RT_GAP_MIN_SEC   512u           /* 256 KiB of a stream's reads, to say  */
#define RT_PIECE_US      6500u          /* a 128 KiB read on the card, unmeasured */

/* the step of a pause (us): 0-250, -500, -1 ms, -2, -4, -8, -16, longer */
static inline uint32_t rt_gap_bucket(uint32_t us)
{
	/* every edge is a power of two times 125 us: the step is the bit length of
	 * us / 125, less one (one clz, no loop: a read's hot path) */
	const uint32_t v = us / 125u;
	const uint32_t b = v < 2u ? 0u : 31u - (uint32_t)__builtin_clz(v);

	return b < RT_GAP_B - 1u ? b : RT_GAP_B - 1u;
}

static inline uint32_t rt_gap_rep(uint32_t b)       /* a step's middle (us) */
{
	static const uint32_t rep[RT_GAP_B] = { 125, 375, 750, 1500, 3000, 6000, 12000, 24000 };

	return rep[b < RT_GAP_B ? b : RT_GAP_B - 1u];
}

/* Whether the sectors seen (sec[step]) are enough to say. */
static inline int rt_hide_known(const uint32_t *sec)
{
	uint64_t all = 0;

	for (uint32_t b = 0; b < RT_GAP_B; b++)
		all += sec[b];
	return all >= RT_GAP_MIN_SEC;
}

/* n / d for the permille figures, without a 64-bit divide (a library call on the
 * Cortex-A9, ~100+ cycles): exact where both fit 32 bits, else both shifted down
 * together until n does: for a quotient up to a few thousand (permille: what is
 * compared with the margins) it is within one of the exact one; a far larger
 * one is only roughly right, and clamped to 2^31 (enormous either way, and only
 * compared with a margin of 20). d < 1 is taken as 1. */
static inline int64_t rt_pm_div(int64_t n, int64_t d)
{
	uint64_t un = n < 0 ? 0 - (uint64_t)n : (uint64_t)n;
	uint64_t ud = d < 1 ? 1u : (uint64_t)d;
	uint64_t q;

	if (un >> 32) {
		const int k = 32 - __builtin_clz((uint32_t)(un >> 32));   /* bits over 32 */

		un >>= k;
		ud >>= k;
	}
	if (ud >> 32)
		q = 0;                               /* n < d: under one */
	else
		q = (uint32_t)un / (ud ? (uint32_t)ud : 1u);
	if (q > 0x7FFFFFFFu)
		q = 0x7FFFFFFFu;
	return n < 0 ? -(int64_t)q : (int64_t)q;
}

/* permille of a piece's read time (piece_us) the pauses cover, over the
 * sectors read after each, sec[step]; 1000 where there are too few. */
static inline uint32_t rt_hide_pm(const uint32_t *sec, uint32_t piece_us)
{
	uint64_t all = 0, cov = 0;

	if (!piece_us || !rt_hide_known(sec))
		return 1000;
	for (uint32_t b = 0; b < RT_GAP_B; b++) {
		const uint32_t r = rt_gap_rep(b);

		all += sec[b];
		cov += (uint64_t)sec[b] * (r < piece_us ? r : piece_us);
	}
	return (uint32_t)rt_pm_div((int64_t)(cov * 1000u), (int64_t)(all * piece_us));
}

/* Kind k's saving a sector, scaled by the pauses. */
static inline void rt_hide_apply(RtCost *c, int k, uint32_t hide_pm)
{
	c->gain16[k] = (int32_t)((int64_t)c->gain16[k] * (hide_pm < 1000u ? hide_pm : 1000u) / 1000);
}

/* The model's cost with this game's measures where there are and read ahead
 * is on (`base`: the device's, calibrated; it alone while off): a sector from
 * the pieces saves what the standard way takes less what a served one did, a
 * piece for nothing what it held up here. */
static inline void rt_ev_cost(const RtEv *e, int k, const RtCost *base, RtCost *o)
{
	*o = *base;
	if (e->off)
		return;                          /* nothing of read ahead is measured now: the device's */
	if (e->std_rate && e->srv_rate) {
		const int64_t d16 = ((int64_t)e->std_rate - e->srv_rate) * 16 / 2048;   /* a sector */

		o->gain16[k] = d16 > 0 ? (int32_t)d16 : 0;
	}
	if (e->held_each)
		o->waste_us = (int32_t)e->held_each;
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