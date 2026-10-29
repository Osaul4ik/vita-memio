/*
 * bench.c - the benchmark engine. See bench.h. No Vita calls: everything goes
 * through BenchEnv.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "pattern.h"

#define MIB 1048576u

const char *const bench_metric_name[BM_COUNT] = {
	"seq write 1M", "seq read 1M", "seq read 128K",
	"rnd read 4K", "rnd read 32K", "rnd read 128K", "rnd read 1M",
	"rnd write 4K", "rnd write 32K", "rnd write 128K", "rnd write 1M",
};

typedef enum { K_SEQ_W, K_SEQ_R, K_RR, K_RW } Kind;

static const struct { Kind kind; uint32_t io; } k_def[BM_COUNT] = {
	[BM_SEQ_W_1M]  = { K_SEQ_W, MIB },
	[BM_SEQ_R_1M]  = { K_SEQ_R, MIB },
	[BM_SEQ_R_128K] = { K_SEQ_R, 128 * 1024 },
	[BM_RR_4K]     = { K_RR, 4 * 1024 },
	[BM_RR_32K]    = { K_RR, 32 * 1024 },
	[BM_RR_128K]   = { K_RR, 128 * 1024 },
	[BM_RR_1M]     = { K_RR, MIB },
	[BM_RW_4K]     = { K_RW, 4 * 1024 },
	[BM_RW_32K]    = { K_RW, 32 * 1024 },
	[BM_RW_128K]   = { K_RW, 128 * 1024 },
	[BM_RW_1M]     = { K_RW, MIB },
};

const BenchProfile bench_profile_quick = {
	"quick", 16, 2, { 100, 80, 40, 10 }, { 20, 20, 10, 6 },
};

const BenchProfile bench_profile_standard = {
	"standard", 64, 3, { 400, 300, 150, 24 }, { 40, 40, 20, 10 },
};

int bench_metric_is_iops(int metric)
{
	return k_def[metric].kind == K_RR || k_def[metric].kind == K_RW;
}

int bench_device_key(int dev)
{
	return dev == TS_DEV_INTERNAL ? TS_KEY_INTERNAL :
	       dev == TS_DEV_MEMCARD  ? TS_KEY_MEMCARD : TS_KEY_SD2VITA;
}

const char *bench_device_name(int dev)
{
	return dev == TS_DEV_INTERNAL ? "Internal" :
	       dev == TS_DEV_MEMCARD  ? "Memory card" : "SD2Vita";
}

/* ---- engine state ------------------------------------------------------------ */

enum { BR_OK = 0, BR_CANCEL = 1, BR_FAIL = -1 };

typedef struct Ctx {
	const BenchEnv    *env;
	const BenchConfig *cfg;
	BenchResult       *res;
	uint8_t           *buf;       /* 1 MiB I/O buffer                          */
	uint32_t          *lat;       /* per-call times of the current metric, us  */
	uint32_t           lat_cap;
	char               file[BENCH_PATH_MAX + 16];
	int                run_total;
} Ctx;

static void fail(Ctx *c, const char *why)
{
	if (!c->res->failed) {
		c->res->failed = 1;
		snprintf(c->res->fail_reason, sizeof(c->res->fail_reason), "%s", why);
	}
}

static int is_cancelled(Ctx *c)
{
	return c->env->cancelled && c->env->cancelled(c->env->ctx);
}

static void progress(Ctx *c, int phase, const BenchRun *r, int metric, uint32_t permille)
{
	BenchProgress p;

	if (!c->env->progress)
		return;
	p.phase = phase;
	p.run_index = c->res->nruns > 0 ? c->res->nruns - 1 : 0;
	p.run_total = c->run_total;
	p.round = r ? r->round : 0;
	p.mode_on = r ? r->mode_on : 0;
	p.metric = metric;
	p.permille = permille > 1000 ? 1000 : permille;
	p.run = r;
	c->env->progress(c->env->ctx, &p);
}

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

/* Fill value/median/p99 of metric m from the n call times in c->lat. */
static void finish_metric(Ctx *c, BenchRun *r, int m, uint32_t n, uint64_t total_us, uint64_t bytes)
{
	double secs;

	if (!n)
		return;
	if (!total_us)
		total_us = 1;
	secs = (double)total_us / 1e6;
	r->ops[m] = n;
	r->value[m] = bench_metric_is_iops(m) ? (double)n / secs
	                                      : ((double)bytes / (double)MIB) / secs;
	qsort(c->lat, n, sizeof(c->lat[0]), cmp_u32);
	r->med_us[m] = c->lat[n / 2];
	r->p99_us[m] = c->lat[(uint64_t)(n - 1) * 99 / 100];
}

/* ---- the tests --------------------------------------------------------------- */

static int run_seq(Ctx *c, BenchRun *r, int m, int *h)
{
	const BenchEnv *e = c->env;
	const int write = k_def[m].kind == K_SEQ_W;
	const uint32_t io = k_def[m].io, S = c->cfg->profile->file_mib * MIB, n = S / io;
	uint64_t total = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint64_t off = (uint64_t)i * io, t0, t1;
		int got;

		if (is_cancelled(c))
			return BR_CANCEL;
		if (write)
			tp_fill(c->buf, off, io);
		t0 = e->now_us(e->ctx);
		got = write ? e->pwrite(e->ctx, *h, c->buf, io, off)
		            : e->pread(e->ctx, *h, c->buf, io, off);
		t1 = e->now_us(e->ctx);
		if (got != (int)io) {
			r->io_errors++;
			fail(c, write ? "write error (card full?)" : "read error");
			return BR_FAIL;
		}
		c->lat[i] = (uint32_t)(t1 - t0);
		total += t1 - t0;
		if (!write)
			r->verify_bad += tp_verify(c->buf, off, io, NULL);
		progress(c, BP_METRIC, r, m, (uint32_t)((uint64_t)(i + 1) * 1000 / n));
	}
	if (write) {
		/* closing is part of a write: the data must be out of the file system. */
		uint64_t t0 = e->now_us(e->ctx), t1;

		e->close(e->ctx, *h);
		*h = -1;
		t1 = e->now_us(e->ctx);
		total += t1 - t0;
	}
	finish_metric(c, r, m, n, total, (uint64_t)S);
	return BR_OK;
}

static int run_rand(Ctx *c, BenchRun *r, int m, int h)
{
	const BenchEnv *e = c->env;
	const int write = k_def[m].kind == K_RW;
	const int sz = write ? m - BM_RW_4K : m - BM_RR_4K;
	const uint32_t io = k_def[m].io, S = c->cfg->profile->file_mib * MIB;
	const uint32_t n = write ? c->cfg->profile->rw_ops[sz] : c->cfg->profile->rr_ops[sz];
	const uint32_t slots = S / io;
	uint32_t rng = 0x1234567u + (uint32_t)m * 7919u;     /* same offsets in both modes */
	uint64_t total = 0;

	if (!n || !slots)
		return BR_OK;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t off, t0, t1;
		int got;

		if (is_cancelled(c))
			return BR_CANCEL;
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		off = (uint64_t)(rng % slots) * io;
		if (write)
			tp_fill(c->buf, off, io);          /* the pattern, so the file stays valid */
		t0 = e->now_us(e->ctx);
		got = write ? e->pwrite(e->ctx, h, c->buf, io, off)
		            : e->pread(e->ctx, h, c->buf, io, off);
		t1 = e->now_us(e->ctx);
		if (got != (int)io) {
			r->io_errors++;
			fail(c, write ? "random write error" : "random read error");
			return BR_FAIL;
		}
		c->lat[i] = (uint32_t)(t1 - t0);
		total += t1 - t0;
		if (!write)
			r->verify_bad += tp_verify(c->buf, off, io, NULL);
		progress(c, BP_METRIC, r, m, (uint32_t)((uint64_t)(i + 1) * 1000 / n));
	}
	finish_metric(c, r, m, n, total, 0);
	return BR_OK;
}

/* Read the whole file in the current mode and count words that differ. */
static int read_all(Ctx *c, BenchRun *r_for_progress, int phase, uint32_t *bad)
{
	const BenchEnv *e = c->env;
	const uint32_t S = c->cfg->profile->file_mib * MIB, n = S / MIB;
	int h = e->open(e->ctx, c->file, 0);

	if (h < 0) {
		fail(c, "cannot reopen the test file");
		return BR_FAIL;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint64_t off = (uint64_t)i * MIB;
		int got;

		if (is_cancelled(c)) {
			e->close(e->ctx, h);
			return BR_CANCEL;
		}
		got = e->pread(e->ctx, h, c->buf, MIB, off);
		if (got != (int)MIB) {
			e->close(e->ctx, h);
			fail(c, "read error while checking the previous file");
			return BR_FAIL;
		}
		*bad += tp_verify(c->buf, off, MIB, NULL);
		progress(c, phase, r_for_progress, -1, (uint32_t)((uint64_t)(i + 1) * 1000 / n));
	}
	e->close(e->ctx, h);
	return BR_OK;
}

/* ---- modes ------------------------------------------------------------------- */

/* Switch the device under test and check that it took. */
static int set_mode(Ctx *c, int on, char *note, size_t note_cap)
{
	BenchPluginState st;
	static const char *const names[] = { "off", "ACTIVE", "FALLBACK", "no buffer", "unavailable" };

	if (c->env->plugin_set(c->env->ctx, on) != 0) {
		fail(c, "the plugin refused the setting");
		return BR_FAIL;
	}
	if (c->env->plugin_get(c->env->ctx, &st) != 0 || !st.available) {
		fail(c, "the plugin stopped answering");
		return BR_FAIL;
	}
	if (on && st.status != TS_ST_ACTIVE) {
		snprintf(note, note_cap, "device status is '%s', not ACTIVE",
		         st.status >= 0 && st.status <= TS_ST_UNAVAIL ? names[st.status] : "?");
		fail(c, "the plugin did not become active for this device");
		return BR_FAIL;
	}
	if (!on && st.status != TS_ST_OFF) {
		snprintf(note, note_cap, "device status is '%s', not off",
		         st.status >= 0 && st.status <= TS_ST_UNAVAIL ? names[st.status] : "?");
		fail(c, "the plugin did not switch off for this device");
		return BR_FAIL;
	}
	return BR_OK;
}

static void check_active(BenchRun *r)
{
	const BenchPluginState *b = &r->before, *a = &r->after;
	int grew = a->reads > b->reads && a->writes > b->writes;
	int clean = a->errors == b->errors && a->mismatches == b->mismatches;

	r->active_ok = 1;
	if (r->mode_on) {
		if (a->status != TS_ST_ACTIVE) {
			r->active_ok = 0;
			snprintf(r->note, sizeof(r->note), "plugin left ACTIVE state during the run");
		} else if (!grew) {
			r->active_ok = 0;
			snprintf(r->note, sizeof(r->note),
			         "no transfer took the faster path (wrong device or folder?)");
		} else if (!clean) {
			r->active_ok = 0;
			snprintf(r->note, sizeof(r->note), "the plugin counted errors or mismatches");
		}
	} else if (a->reads != b->reads || a->writes != b->writes) {
		r->active_ok = 0;
		snprintf(r->note, sizeof(r->note), "faster path was used during an OFF run");
	}
}

static int do_run(Ctx *c, int round, int on)
{
	BenchResult *res = c->res;
	BenchRun *r = &res->runs[res->nruns];
	const BenchEnv *e = c->env;
	int h, rc, m;

	memset(r, 0, sizeof(*r));
	r->mode_on = on;
	r->round = round + 1;
	res->nruns++;
	progress(c, BP_SETUP, r, -1, 0);

	rc = set_mode(c, on, r->note, sizeof(r->note));
	if (rc)
		return rc;
	e->plugin_get(e->ctx, &r->before);

	if (res->nruns > 1) {                       /* the file the previous run wrote */
		r->cross_done = 1;
		r->cross_from_on = res->runs[res->nruns - 2].mode_on;
		rc = read_all(c, r, BP_CROSS, &r->cross_bad);
		if (rc)
			return rc;
	}

	h = e->open(e->ctx, c->file, 1);
	if (h < 0) {
		fail(c, "cannot create the test file (folder missing or read-only?)");
		return BR_FAIL;
	}
	rc = run_seq(c, r, BM_SEQ_W_1M, &h);          /* closes h */
	if (rc) {
		if (h >= 0)
			e->close(e->ctx, h);
		return rc;
	}
	h = e->open(e->ctx, c->file, 0);              /* reopened: read it back from the card */
	if (h < 0) {
		fail(c, "cannot reopen the test file");
		return BR_FAIL;
	}
	for (m = BM_SEQ_R_1M; m < BM_COUNT && !rc; m++) {
		switch (k_def[m].kind) {
		case K_SEQ_R: rc = run_seq(c, r, m, &h); break;
		case K_RR:
		case K_RW:    rc = run_rand(c, r, m, h); break;
		default:      break;
		}
	}
	e->close(e->ctx, h);
	if (rc)
		return rc;

	e->plugin_get(e->ctx, &r->after);
	check_active(r);
	r->complete = 1;
	return BR_OK;
}

/* ---- entry point -------------------------------------------------------------- */

static int profile_ok(const BenchProfile *p)
{
	return p && p->file_mib >= 1 && p->file_mib <= 1024 && p->rounds >= 1 && p->rounds <= BENCH_MAX_RUNS / 2;
}

int bench_run(const BenchEnv *env, const BenchConfig *cfg, BenchResult *res)
{
	Ctx c;
	int rc = BR_OK, began = 0, round, k;
	uint32_t maxops = 0;

	memset(res, 0, sizeof(*res));
	memset(&c, 0, sizeof(c));
	c.env = env;
	c.cfg = cfg;
	c.res = res;
	if (!profile_ok(cfg->profile)) {
		fail(&c, "bad test profile");
		return BR_FAIL;
	}
	res->profile = *cfg->profile;
	res->device = cfg->device;
	snprintf(res->dir, sizeof(res->dir), "%s", cfg->dir);
	snprintf(c.file, sizeof(c.file), "%sbench.bin", cfg->dir);
	c.run_total = (int)cfg->profile->rounds * 2;

	if (env->plugin_get(env->ctx, &res->plugin0) != 0 || !res->plugin0.available) {
		fail(&c, "the Turbo Storage kernel module does not answer");
		return BR_FAIL;
	}
	if (res->plugin0.disabled != TS_DIS_NONE) {
		fail(&c, "the plugin is inactive (safe mode, unknown firmware or no hooks)");
		return BR_FAIL;
	}
	if (res->plugin0.status == TS_ST_NO_BUFFER) {
		fail(&c, "the plugin has no staging buffer for this device");
		return BR_FAIL;
	}

	for (k = 0; k < 4; k++) {
		if (cfg->profile->rr_ops[k] > maxops) maxops = cfg->profile->rr_ops[k];
		if (cfg->profile->rw_ops[k] > maxops) maxops = cfg->profile->rw_ops[k];
	}
	c.lat_cap = cfg->profile->file_mib * 8;          /* the 128K sequential read    */
	if (maxops > c.lat_cap)
		c.lat_cap = maxops;
	c.buf = malloc(MIB);
	c.lat = malloc((size_t)c.lat_cap * sizeof(c.lat[0]));
	if (!c.buf || !c.lat) {
		fail(&c, "out of memory");
		rc = BR_FAIL;
		goto out;
	}
	if (env->mkdirs(env->ctx, cfg->dir) != 0) {
		fail(&c, "cannot create the test folder");
		rc = BR_FAIL;
		goto out;
	}
	if (env->plugin_begin(env->ctx) != 0) {
		fail(&c, "cannot read the current plugin settings");
		rc = BR_FAIL;
		goto out;
	}
	began = 1;

	/* OFF, ON | ON, OFF | OFF, ON ...: each round flips the order, so neither mode
	 * always gets the card in the same state. */
	for (round = 0; round < (int)cfg->profile->rounds && rc == BR_OK; round++) {
		int first_on = round & 1;

		rc = do_run(&c, round, first_on);
		if (rc == BR_OK)
			rc = do_run(&c, round, !first_on);
	}

	if (rc == BR_OK && res->nruns > 0) {
		/* The last file, read in the opposite mode. */
		int last_on = res->runs[res->nruns - 1].mode_on;
		char note[BENCH_REASON_MAX] = "";

		progress(&c, BP_FINAL, &res->runs[res->nruns - 1], -1, 0);
		rc = set_mode(&c, !last_on, note, sizeof(note));
		if (rc == BR_OK) {
			res->final_cross_done = 1;
			res->final_cross_from_on = last_on;
			rc = read_all(&c, &res->runs[res->nruns - 1], BP_FINAL, &res->final_cross_bad);
		}
	}

out:
	progress(&c, BP_CLEANUP, res->nruns ? &res->runs[res->nruns - 1] : NULL, -1, 0);
	if (began) {
		env->remove(env->ctx, c.file);
		env->plugin_end(env->ctx);
	}
	free(c.buf);
	free(c.lat);
	if (rc == BR_CANCEL)
		res->cancelled = 1;
	return rc;
}

/* ---- results ------------------------------------------------------------------ */

void bench_aggregate(const BenchResult *res, BenchAgg *out)
{
	memset(out, 0, sizeof(*out));
	for (int i = 0; i < res->nruns; i++) {
		const BenchRun *r = &res->runs[i];
		double *v = r->mode_on ? out->on : out->off;
		double *med = r->mode_on ? out->on_med_ms : out->off_med_ms;
		double *p99 = r->mode_on ? out->on_p99_ms : out->off_p99_ms;

		if (!r->complete)
			continue;
		if (r->mode_on)
			out->n_on++;
		else
			out->n_off++;
		for (int m = 0; m < BM_COUNT; m++) {
			v[m] += r->value[m];
			med[m] += r->med_us[m] / 1000.0;
			p99[m] += r->p99_us[m] / 1000.0;
		}
	}
	for (int m = 0; m < BM_COUNT; m++) {
		if (out->n_off) {
			out->off[m] /= out->n_off;
			out->off_med_ms[m] /= out->n_off;
			out->off_p99_ms[m] /= out->n_off;
		}
		if (out->n_on) {
			out->on[m] /= out->n_on;
			out->on_med_ms[m] /= out->n_on;
			out->on_p99_ms[m] /= out->n_on;
		}
	}
}

int bench_integrity_ok(const BenchResult *res)
{
	if (res->final_cross_bad)
		return 0;
	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].verify_bad || res->runs[i].cross_bad || res->runs[i].io_errors)
			return 0;
	return 1;
}

int bench_active_ok(const BenchResult *res)
{
	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].complete && !res->runs[i].active_ok)
			return 0;
	return 1;
}

int bench_valid(const BenchResult *res)
{
	return !res->failed && !res->cancelled && res->nruns > 0;
}
