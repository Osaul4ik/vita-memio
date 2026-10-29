/*
 * bench.c - the benchmark engine. See bench.h. No Vita calls: everything goes
 * through BenchEnv. It only reads the plugin's state, it never changes it.
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
	"files create", "files read", "files delete",
	"NC read 1M", "NC read 128K",
};

typedef enum { K_SEQ_W, K_SEQ_R, K_RR, K_RW, K_SF, K_NC_R } Kind;

#define SF_BYTES 4096u          /* one small file */

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
	[BM_SF_CREATE] = { K_SF, SF_BYTES },
	[BM_SF_READ]   = { K_SF, SF_BYTES },
	[BM_SF_DELETE] = { K_SF, 0 },
	[BM_NC_R_1M]   = { K_NC_R, MIB },
	[BM_NC_R_128K] = { K_NC_R, 128 * 1024 },
};

const BenchProfile bench_profile_quick = {
	"quick", 16, 3, { 100, 80, 40, 10 }, { 20, 20, 10, 6 }, 64,
};

const BenchProfile bench_profile_standard = {
	"standard", 64, 5, { 400, 300, 150, 24 }, { 40, 40, 20, 10 }, 200,
};

int bench_metric_is_iops(int metric)
{
	return k_def[metric].kind == K_RR || k_def[metric].kind == K_RW || k_def[metric].kind == K_SF;
}

const char *bench_metric_unit(int metric)
{
	return k_def[metric].kind == K_SF ? "files/s" : bench_metric_is_iops(metric) ? "IOPS" : "MiB/s";
}

const char *bench_device_name(int dev)
{
	return dev == VM_DEV_INTERNAL ? "Internal" :
	       dev == VM_DEV_MEMCARD  ? "Memory card" : "SD2Vita";
}

int bench_plugin_mode(const BenchPluginState *st)
{
	if (!st->available)
		return BPM_UNKNOWN;
	if (st->disabled != VM_DIS_NONE)
		return BPM_INACTIVE;
	switch (st->status) {
	case VM_ST_OFF:    return BPM_OFF;
	case VM_ST_ACTIVE: return BPM_ON;
	default:           return BPM_TROUBLE;
	}
}

const char *bench_plugin_mode_name(int mode)
{
	switch (mode) {
	case BPM_OFF:      return "OFF";
	case BPM_ON:       return "ON";
	case BPM_INACTIVE: return "INACTIVE";
	case BPM_TROUBLE:  return "TROUBLE";
	default:           return "UNKNOWN";
	}
}

/* ---- engine state ------------------------------------------------------------ */

enum { BR_OK = 0, BR_CANCEL = 1, BR_FAIL = -1 };

typedef struct Ctx {
	const BenchEnv    *env;
	const BenchConfig *cfg;
	BenchResult       *res;
	uint8_t           *buf;       /* 1 MiB I/O buffer                          */
	uint8_t           *nc;        /* 1 MiB non-cacheable buffer, or NULL       */
	uint32_t          *lat;       /* per-call times of the current metric, us  */
	uint32_t           lat_cap;
	char               file[BENCH_PATH_MAX + 16];
	int                run_total;
	uint32_t           sf_made;   /* small files that may exist (to clean up)  */
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
	p.plugin_mode = c->res->plugin_mode;
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
	uint8_t *buf = k_def[m].kind == K_NC_R ? c->nc : c->buf;
	uint64_t total = 0;

	for (uint32_t i = 0; i < n; i++) {
		uint64_t off = (uint64_t)i * io, t0, t1;
		int got;

		if (is_cancelled(c))
			return BR_CANCEL;
		if (write)
			tp_fill(buf, off, io);
		t0 = e->now_us(e->ctx);
		got = write ? e->pwrite(e->ctx, *h, buf, io, off)
		            : e->pread(e->ctx, *h, buf, io, off);
		t1 = e->now_us(e->ctx);
		if (got != (int)io) {
			r->io_errors++;
			fail(c, write ? "write error (card full?)" : "read error");
			return BR_FAIL;
		}
		c->lat[i] = (uint32_t)(t1 - t0);
		total += t1 - t0;
		if (!write)
			r->verify_bad += tp_verify(buf, off, io, NULL);
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

static void sf_path(const Ctx *c, uint32_t i, char *out, size_t cap)
{
	snprintf(out, cap, "%svmsf%03u.bin", c->cfg->dir, (unsigned)i);
}

/* Small files: create n, read them back (each opened anew), delete them. Each
 * call is open + one 4 KiB transfer + close, so it is mostly the file
 * system's own bookkeeping: directory entries, allocation, the file's data. */
static int run_small_files(Ctx *c, BenchRun *r)
{
	const BenchEnv *e = c->env;
	const uint32_t n = c->cfg->profile->sf_files;
	char path[BENCH_PATH_MAX + 32];
	uint64_t total;

	if (!n)
		return BR_OK;
	for (int m = BM_SF_CREATE; m <= BM_SF_DELETE; m++) {
		total = 0;
		for (uint32_t i = 0; i < n; i++) {
			uint64_t off = 0x40000000ull + (uint64_t)i * SF_BYTES, t0, t1;
			int h = 0, got = 0, ok;

			if (is_cancelled(c))
				return BR_CANCEL;
			sf_path(c, i, path, sizeof(path));
			if (m == BM_SF_CREATE)
				tp_fill(c->buf, off, SF_BYTES);
			t0 = e->now_us(e->ctx);
			if (m == BM_SF_DELETE) {
				ok = e->remove(e->ctx, path) == 0;
			} else {
				h = e->open(e->ctx, path, m == BM_SF_CREATE);
				if (h >= 0) {
					got = m == BM_SF_CREATE ? e->pwrite(e->ctx, h, c->buf, SF_BYTES, 0)
					                        : e->pread(e->ctx, h, c->buf, SF_BYTES, 0);
					e->close(e->ctx, h);
				}
				ok = h >= 0 && got == (int)SF_BYTES;
			}
			t1 = e->now_us(e->ctx);
			if (m == BM_SF_CREATE)
				c->sf_made = i + 1;
			if (!ok) {
				r->io_errors++;
				fail(c, m == BM_SF_CREATE ? "cannot create a small file (card full?)" :
				        m == BM_SF_READ ? "cannot read a small file" : "cannot delete a small file");
				return BR_FAIL;
			}
			if (m == BM_SF_READ)
				r->verify_bad += tp_verify(c->buf, off, SF_BYTES, NULL);
			c->lat[i] = (uint32_t)(t1 - t0);
			total += t1 - t0;
			progress(c, BP_METRIC, r, m, (uint32_t)((uint64_t)(i + 1) * 1000 / n));
		}
		finish_metric(c, r, m, n, total, 0);
	}
	c->sf_made = 0;
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
			fail(c, "read error while checking the file");
			return BR_FAIL;
		}
		*bad += tp_verify(c->buf, off, MIB, NULL);
		progress(c, phase, r_for_progress, -1, (uint32_t)((uint64_t)(i + 1) * 1000 / n));
	}
	e->close(e->ctx, h);
	return BR_OK;
}

/* ---- the plugin (read only) --------------------------------------------------- */

static void get_state(Ctx *c, BenchPluginState *st)
{
	memset(st, 0, sizeof(*st));
	if (!c->env->plugin_get || c->env->plugin_get(c->env->ctx, st) != 0)
		memset(st, 0, sizeof(*st));        /* not available: state unknown */
}

/* Did the plugin behave as its state at the start of the test says? Only OFF and
 * ON can be checked; for the other states there is nothing to compare. */
static void check_plugin(const BenchResult *res, BenchRun *r)
{
	const BenchPluginState *b = &r->before, *a = &r->after;
	const int mode = res->plugin_mode;
	int grew = a->reads > b->reads && a->writes > b->writes;
	int moved = a->reads != b->reads || a->writes != b->writes;

	r->plugin_ok = 1;
	if (mode != BPM_OFF && mode != BPM_ON)
		return;
	/* Checked first: a mismatch or an error also switches the device to stock,
	 * which would otherwise only show as "the state changed". */
	if (mode == BPM_ON && a->mismatches != b->mismatches) {
		r->plugin_ok = 0;
		snprintf(r->note, sizeof(r->note),
		         "plugin verify: %u mismatch(es), the device went back to stock",
		         (unsigned)(a->mismatches - b->mismatches));
	} else if (mode == BPM_ON && a->errors != b->errors) {
		r->plugin_ok = 0;
		snprintf(r->note, sizeof(r->note),
		         "the faster path had %u error(s), the device went back to stock",
		         (unsigned)(a->errors - b->errors));
	} else if (bench_plugin_mode(b) != mode || bench_plugin_mode(a) != mode) {
		r->plugin_ok = 0;
		snprintf(r->note, sizeof(r->note), "the plugin state changed during the test");
	} else if (mode == BPM_ON && !grew) {
		r->plugin_ok = 0;
		snprintf(r->note, sizeof(r->note),
		         "no transfer took the faster path (wrong device or folder?)");
	} else if (mode == BPM_ON && b->verify && b->verify_ready && a->verified == b->verified) {
		r->plugin_ok = 0;
		snprintf(r->note, sizeof(r->note), "plugin verify is on but checked no transfer");
	} else if (mode == BPM_OFF && moved) {
		r->plugin_ok = 0;
		snprintf(r->note, sizeof(r->note), "faster path used although the plugin is OFF");
	}
}

static int do_run(Ctx *c, int round)
{
	BenchResult *res = c->res;
	BenchRun *r = &res->runs[res->nruns];
	const BenchEnv *e = c->env;
	int h, rc, m;

	memset(r, 0, sizeof(*r));
	r->round = round + 1;
	res->nruns++;
	progress(c, BP_SETUP, r, -1, 0);

	get_state(c, &r->before);

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
		case K_NC_R:  rc = c->nc ? run_seq(c, r, m, &h) : BR_OK; break;
		case K_RR:
		case K_RW:    rc = run_rand(c, r, m, h); break;
		default:      break;
		}
	}
	e->close(e->ctx, h);
	if (rc)
		return rc;

	/* The random writes changed the file: read the whole of it once more. */
	rc = read_all(c, r, BP_FINAL, &r->final_bad);
	if (rc)
		return rc;
	r->final_done = 1;

	rc = run_small_files(c, r);
	if (rc)
		return rc;

	get_state(c, &r->after);
	check_plugin(res, r);
	r->complete = 1;
	return BR_OK;
}

/* ---- entry point -------------------------------------------------------------- */

static int profile_ok(const BenchProfile *p)
{
	return p && p->file_mib >= 1 && p->file_mib <= 1024 && p->rounds >= 1 &&
	       p->rounds <= BENCH_MAX_RUNS && p->sf_files <= 999;   /* vmsf000..999 */
}

int bench_run(const BenchEnv *env, const BenchConfig *cfg, BenchResult *res)
{
	Ctx c;
	int rc = BR_OK, round, k, dir_ready = 0;
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
	c.run_total = (int)cfg->profile->rounds;

	/* Read only. A plugin that does not answer does not stop the test. */
	get_state(&c, &res->plugin0);
	res->plugin_mode = bench_plugin_mode(&res->plugin0);

	for (k = 0; k < 4; k++) {
		if (cfg->profile->rr_ops[k] > maxops) maxops = cfg->profile->rr_ops[k];
		if (cfg->profile->rw_ops[k] > maxops) maxops = cfg->profile->rw_ops[k];
	}
	c.lat_cap = cfg->profile->file_mib * 8;          /* the 128K sequential read    */
	if (cfg->profile->sf_files > maxops)
		maxops = cfg->profile->sf_files;
	if (maxops > c.lat_cap)
		c.lat_cap = maxops;
	c.buf = malloc(MIB);
	c.lat = malloc((size_t)c.lat_cap * sizeof(c.lat[0]));
	if (!c.buf || !c.lat) {
		fail(&c, "out of memory");
		rc = BR_FAIL;
		goto out;
	}
	if (env->nc_alloc && env->nc_free)
		c.nc = env->nc_alloc(env->ctx, MIB);   /* none: the NC reads are skipped */
	if (env->mkdirs(env->ctx, cfg->dir) != 0) {
		fail(&c, "cannot create the test folder");
		rc = BR_FAIL;
		goto out;
	}
	dir_ready = 1;

	for (round = 0; round < (int)cfg->profile->rounds && rc == BR_OK; round++)
		rc = do_run(&c, round);

out:
	progress(&c, BP_CLEANUP, res->nruns ? &res->runs[res->nruns - 1] : NULL, -1, 0);
	if (dir_ready) {
		char path[BENCH_PATH_MAX + 32];

		env->remove(env->ctx, c.file);
		for (uint32_t i = 0; i < c.sf_made; i++) {   /* stopped half way */
			sf_path(&c, i, path, sizeof(path));
			env->remove(env->ctx, path);
		}
	}
	free(c.buf);
	free(c.lat);
	if (c.nc)
		env->nc_free(env->ctx, c.nc);
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

		if (!r->complete)
			continue;
		out->n++;
		for (int m = 0; m < BM_COUNT; m++) {
			out->v[m] += r->value[m];
			out->med_ms[m] += r->med_us[m] / 1000.0;
			out->p99_ms[m] += r->p99_us[m] / 1000.0;
		}
	}
	if (!out->n)
		return;
	for (int m = 0; m < BM_COUNT; m++) {
		out->v[m] /= out->n;
		out->med_ms[m] /= out->n;
		out->p99_ms[m] /= out->n;
	}
}

int bench_metric_measured(const BenchResult *res, int m)
{
	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].complete && res->runs[i].ops[m])
			return 1;
	return 0;
}

/* Sums of the counter differences, then per block. */
typedef struct { uint64_t rs, ri, rc, ws, wi, wc; } TimingSum;

static void timing_add(TimingSum *t, const BenchRun *r)
{
	const BenchPluginState *b = &r->before, *a = &r->after;

	if (!r->complete || !b->timing_ok || !a->timing_ok)
		return;
	t->rs += (uint32_t)(a->rd_sectors - b->rd_sectors);
	t->ri += (uint32_t)(a->rd_io_us - b->rd_io_us);
	t->rc += (uint32_t)(a->rd_copy_us - b->rd_copy_us);
	t->ws += (uint32_t)(a->wr_sectors - b->wr_sectors);
	t->wi += (uint32_t)(a->wr_io_us - b->wr_io_us);
	t->wc += (uint32_t)(a->wr_copy_us - b->wr_copy_us);
}

static void timing_finish(const TimingSum *t, BenchTiming *out)
{
	memset(out, 0, sizeof(*out));
	out->rd_sectors = t->rs;
	out->wr_sectors = t->ws;
	if (t->rs) {
		out->rd_ok = 1;
		out->rd_io_us = (double)t->ri / (double)t->rs;
		out->rd_copy_us = (double)t->rc / (double)t->rs;
	}
	if (t->ws) {
		out->wr_ok = 1;
		out->wr_io_us = (double)t->wi / (double)t->ws;
		out->wr_copy_us = (double)t->wc / (double)t->ws;
	}
}

void bench_timing_run(const BenchRun *r, BenchTiming *out)
{
	TimingSum t = { 0 };

	timing_add(&t, r);
	timing_finish(&t, out);
}

void bench_timing(const BenchResult *res, BenchTiming *out)
{
	TimingSum t = { 0 };

	for (int i = 0; i < res->nruns; i++)
		timing_add(&t, &res->runs[i]);
	timing_finish(&t, out);
}

int bench_integrity_ok(const BenchResult *res)
{
	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].verify_bad || res->runs[i].final_bad || res->runs[i].io_errors)
			return 0;
	return 1;
}

int bench_plugin_ok(const BenchResult *res)
{
	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].complete && !res->runs[i].plugin_ok)
			return 0;
	return 1;
}

void bench_verify_run(const BenchRun *r, BenchVerify *out)
{
	memset(out, 0, sizeof(*out));
	out->on = r->before.verify;
	out->ready = r->before.verify_ready;
	if (!out->on || !r->complete)
		return;
	out->checked = r->after.verified - r->before.verified;
	out->mismatches = r->after.mismatches - r->before.mismatches;
}

void bench_verify(const BenchResult *res, BenchVerify *out)
{
	memset(out, 0, sizeof(*out));
	out->on = res->plugin0.verify;
	out->ready = res->plugin0.verify_ready;
	for (int i = 0; i < res->nruns; i++) {
		BenchVerify v;

		bench_verify_run(&res->runs[i], &v);
		out->checked += v.checked;
		out->mismatches += v.mismatches;
	}
}

uint32_t bench_passthru(const BenchResult *res)
{
	uint32_t n = 0;

	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].complete)
			n += res->runs[i].after.passthru - res->runs[i].before.passthru;
	return n;
}

int bench_valid(const BenchResult *res)
{
	return !res->failed && !res->cancelled && res->nruns > 0;
}
