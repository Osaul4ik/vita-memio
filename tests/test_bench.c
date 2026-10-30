/*
 * test_bench.c - host tests for the tester's engine (tester/bench.c), report
 * (report.c), results file (results.c) and test pattern (pattern.h).
 *
 * The "console" is a fake: one file in memory, a clock that advances by a simple
 * speed model (reads and writes are faster with the plugin ON), a fake plugin
 * with the same states and counters as the real one, and switches that inject
 * trouble: corrupted data, a plugin that is not really used, a setting changed
 * mid-test, failing writes, a cancel part-way. The engine can only read the
 * plugin; every test also checks the setting is left exactly as it was.
 * Built with AddressSanitizer + UBSan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tester/bench.h"
#include "../tester/pattern.h"
#include "../tester/report.h"
#include "../tester/results.h"
#include "fakes.h"
#include "vm_test.h"

/* ---- the fake console --------------------------------------------------------- */

typedef struct Fake {
	uint8_t *data;
	size_t   size, cap;
	int      exists, open_handles;
	uint64_t now;
	uint64_t cpu;                /* the testing thread's CPU time, us           */

	int      on;                 /* the device's setting (the tester must not change it) */
	int      initial_on;
	int      status_override;    /* -1: derive from `on`                       */
	int      disabled, unavailable, verify;
	int      verify_ready;       /* the plugin's check buffer is there         */
	int      verify_dead;        /* verify on and ready, but nothing is checked */
	int      mismatch_at_read;   /* the Nth faster read fails the plugin check */
	uint32_t reads, writes, errors, mismatches, verified;
	uint32_t passthru;           /* faster path declined: buffer busy           */
	int      busy_every;         /* every Nth large read finds the buffer busy  */
	int      small;              /* the plugin's "small transfers too"          */
	int      pipeline;           /* ... and its "overlapped reads"              */
	int      offload;            /* ... and its "copy on the system core"       */
	int      gpusafe;            /* ... and "GPU-safe reads"                    */
	int      sf_corrupt;         /* small files read back wrong                 */
	int      sf_fail_remove;     /* deleting a small file fails                 */
	int      busy_seen;
	int      no_timing;          /* kernel module without vmGetTiming          */
	uint32_t rs, ri, rc, ws, wi, wc;   /* faster-path timing counters          */

	int      corrupt_read_on, corrupt_write_on;
	int      no_counters;        /* ON, but nothing takes the faster path      */
	int      counts_when_off;    /* OFF, but the counters move anyway          */
	int      flip_at_write;      /* the user flips the setting at this write   */
	int      fail_write_at;      /* the Nth write call fails (0 = never)       */
	int      write_calls;
	int      cancel_after_polls, polls;
	int      mkdirs_fail;
	int      get_calls, remove_calls;
} Fake;

static uint64_t f_now(void *c) { return ((Fake *)c)->now; }
static uint64_t f_cpu(void *c) { return ((Fake *)c)->cpu; }

static void ensure(Fake *f, size_t n)
{
	if (n > f->cap) {
		f->data = realloc(f->data, n);
		memset(f->data + f->cap, 0, n - f->cap);
		f->cap = n;
	}
}

/* The small files (vmsfNNN.bin) live apart from the one big test file. */
#define SF_MAX 1000
static int     g_sf_exists[SF_MAX];
static uint8_t g_sf_data[SF_MAX][4096];

static int sf_index(const char *path)
{
	const char *p = strstr(path, "vmsf");

	return p ? atoi(p + 4) : -1;
}

static int sf_left(void)
{
	int n = 0;

	for (int i = 0; i < SF_MAX; i++)
		n += g_sf_exists[i];
	return n;
}

static int f_open(void *c, const char *path, int trunc)
{
	Fake *f = c;
	int sf = sf_index(path);

	if (sf >= 0) {
		if (trunc)
			g_sf_exists[sf] = 1;
		else if (!g_sf_exists[sf])
			return -1;
		f->open_handles++;
		f->now += 300;
		return 1000 + sf;
	}
	if (trunc) {
		f->exists = 1;
		f->size = 0;
		if (f->cap)                        /* memset(NULL, 0, 0) is undefined behaviour */
			memset(f->data, 0, f->cap);
	} else if (!f->exists) {
		return -1;
	}
	f->open_handles++;
	return 1;
}

static int f_close(void *c, int h)
{
	Fake *f = c;

	(void)h;
	f->open_handles--;
	return 0;
}

static int counts(const Fake *f, uint32_t n)
{
	return n >= 32768 && ((f->on && !f->no_counters) || (!f->on && f->counts_when_off));
}

static int f_pread(void *c, int h, void *buf, uint32_t n, uint64_t off)
{
	Fake *f = c;

	if (h >= 1000) {
		if (off + n > 4096)
			return -1;
		memcpy(buf, g_sf_data[h - 1000] + off, n);
		if (f->sf_corrupt)
			((uint8_t *)buf)[7] ^= 1;
		if (f->on && f->small)
			f->reads++;                            /* a small transfer, faster path */
		f->now += 200;
		return (int)n;
	}
	if (off + n > f->size)
		return -1;
	memcpy(buf, f->data + off, n);
	f->cpu += (uint64_t)n * 1000 / 1048576;        /* 1.0 ms per MiB */
	f->now += 200 + (uint64_t)(n * (f->on ? 0.06 : 0.08));        /* ON reads ~33% faster */
	if (counts(f, n) && f->busy_every && ++f->busy_seen % f->busy_every == 0) {
		f->passthru++;                             /* this one found the buffer busy */
	} else if (counts(f, n)) {
		f->reads++;
		if (f->verify && f->verify_ready && !f->verify_dead)
			f->verified++;
		if (f->mismatch_at_read && f->reads == (uint32_t)f->mismatch_at_read) {
			f->mismatches++;
			f->status_override = VM_ST_FALLBACK;   /* the plugin goes back to stock */
		}
		f->rs += n / 512;                          /* 20 us io + 4 us copy per block */
		f->ri += (n / 512) * 20;
		f->rc += (n / 512) * 4;
	}
	if (f->on && f->corrupt_read_on && n >= 32768)
		((uint8_t *)buf)[n / 2] ^= 0x10;
	return (int)n;
}

static int f_pwrite(void *c, int h, const void *buf, uint32_t n, uint64_t off)
{
	Fake *f = c;

	if (h >= 1000) {
		if (off + n > 4096)
			return -1;
		memcpy(g_sf_data[h - 1000] + off, buf, n);
		if (f->on && f->small)
			f->writes++;
		f->now += 400;
		return (int)n;
	}
	f->write_calls++;
	if (f->fail_write_at && f->write_calls == f->fail_write_at)
		return -1;
	if (f->flip_at_write && f->write_calls == f->flip_at_write)
		f->on = !f->on;                            /* changed in Settings mid-test */
	ensure(f, off + n);
	memcpy(f->data + off, buf, n);
	if (off + n > f->size)
		f->size = off + n;
	f->now += 400 + (uint64_t)(n * (f->on ? 0.15 : 0.17));
	if (counts(f, n)) {
		f->writes++;
		if (f->verify && f->verify_ready && !f->verify_dead)
			f->verified++;
		f->ws += n / 512;                          /* 30 us io + 2 us copy per block */
		f->wi += (n / 512) * 30;
		f->wc += (n / 512) * 2;
	}
	if (f->on && f->corrupt_write_on && n >= 32768)
		f->data[off + n / 2] ^= 0x01;              /* lands on the "card" wrong */
	return (int)n;
}

static int f_remove(void *c, const char *path)
{
	Fake *f = c;

	int sf = sf_index(path);

	if (sf >= 0) {
		if (f->sf_fail_remove || !g_sf_exists[sf])
			return -1;
		g_sf_exists[sf] = 0;
		f->now += 250;
		return 0;
	}
	f->remove_calls++;
	f->exists = 0;
	return 0;
}

static int f_mkdirs(void *c, const char *dir)
{
	(void)dir;
	return ((Fake *)c)->mkdirs_fail ? -1 : 0;
}

static int f_get(void *c, BenchPluginState *o)
{
	Fake *f = c;

	f->get_calls++;
	memset(o, 0, sizeof(*o));
	if (f->unavailable)
		return -1;
	o->available = 1;
	o->disabled = f->disabled;
	o->status = f->disabled ? VM_ST_UNAVAIL
	          : f->on ? (f->status_override >= 0 ? f->status_override : VM_ST_ACTIVE)
	          : VM_ST_OFF;
	o->reads = f->reads;
	o->writes = f->writes;
	o->errors = f->errors;
	o->mismatches = f->mismatches;
	o->passthru = f->passthru;
	o->small = f->small;
	o->pipeline = f->pipeline;
	o->offload = f->offload;
	o->gpusafe = f->gpusafe;
	o->fingerprint = 0x2e7c52f7;
	o->verify = f->verify;
	o->verify_ready = f->verify_ready;
	o->verified = f->verified;
	if (!f->no_timing) {
		o->timing_ok = 1;
		o->rd_sectors = f->rs; o->rd_io_us = f->ri; o->rd_copy_us = f->rc;
		o->wr_sectors = f->ws; o->wr_io_us = f->wi; o->wr_copy_us = f->wc;
	}
	return 0;
}

static int f_cancelled(void *c)
{
	Fake *f = c;

	return f->cancel_after_polls && ++f->polls > f->cancel_after_polls;
}

static int g_progress_calls, g_progress_bad;
static void f_progress(void *c, const struct BenchProgress *p)
{
	(void)c;
	g_progress_calls++;
	if (p->permille > 1000 || p->run_index < 0 || p->run_total < 1)
		g_progress_bad++;
}

static BenchEnv env_for(Fake *f)
{
	BenchEnv e = { f, f_now, f_open, f_pread, f_pwrite, f_close, f_remove, f_mkdirs,
	               f_get, f_progress, f_cancelled, NULL, NULL, NULL, NULL, NULL, NULL };
	return e;
}

static void fake_init(Fake *f, int on)
{
	free(f->data);
	memset(f, 0, sizeof(*f));
	f->status_override = -1;
	f->initial_on = f->on = on;
	g_progress_calls = g_progress_bad = 0;
	memset(g_sf_exists, 0, sizeof(g_sf_exists));
}

/* the plugin setting is exactly what the user left, and nothing leaked */
#define CHECK_UNTOUCHED(f) \
	CHECK((f).on == (f).initial_on && !(f).exists && (f).open_handles == 0 && sf_left() == 0)

/* a small profile so the tests stay quick under the sanitizers */
static const BenchProfile tiny = { "tiny", 2, 3, { 20, 20, 10, 4 }, { 6, 6, 4, 2 }, 0 };
static const BenchProfile one_round = { "one", 2, 1, { 8, 8, 4, 2 }, { 4, 4, 2, 2 }, 0 };

static BenchResult *new_result(void) { return calloc(1, sizeof(BenchResult)); }

/* ---- the engine --------------------------------------------------------------- */

static void test_full_run_on(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchAgg a;
	BenchEnv e;

	fake_init(&f, 1);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(bench_valid(r) && r->nruns == 3 && r->plugin_mode == BPM_ON);
	for (int i = 0; i < 3; i++) {
		CHECK(r->runs[i].complete && r->runs[i].plugin_ok && r->runs[i].final_done);
		CHECK(r->runs[i].round == i + 1);
		CHECK(r->runs[i].after.reads > r->runs[i].before.reads);
	}
	CHECK(bench_integrity_ok(r) && bench_plugin_ok(r));
	CHECK_UNTOUCHED(f);
	CHECK(f.remove_calls == 1);
	CHECK(g_progress_calls > 50 && g_progress_bad == 0);

	bench_aggregate(r, &a);
	CHECK(a.n == 3);
	/* 1 MiB at 0.06 us/byte + 200 us = ~63 ms */
	CHECK(r->runs[0].med_us[BM_SEQ_R_1M] > 62000 && r->runs[0].med_us[BM_SEQ_R_1M] < 64000);
	CHECK(a.med_ms[BM_SEQ_R_1M] > 62.0 && a.med_ms[BM_SEQ_R_1M] < 64.0);
	CHECK(r->runs[0].ops[BM_SEQ_R_1M] == 2 && r->runs[0].ops[BM_SEQ_R_128K] == 16);
	CHECK(r->runs[0].ops[BM_RR_4K] == 20 && r->runs[0].ops[BM_RW_1M] == 2);
	free(r);
	free(f.data);
}

/* The NC reads: measured into the platform's non-cacheable memory, checked
 * like every other read, the memory given back on every path; without such
 * memory they are not measured and the tables say "-". */
static int g_nc_allocs, g_nc_frees;
static void *g_nc_last;
static void *f_nc_alloc(void *c, uint32_t size)
{
	(void)c;
	g_nc_allocs++;
	return g_nc_last = malloc(size);
}
static void f_nc_free(void *c, void *p)
{
	(void)c;
	CHECK(p == g_nc_last);
	g_nc_frees++;
	free(p);
}
static void *f_nc_none(void *c, uint32_t size) { (void)c; (void)size; return NULL; }

static void test_nc_reads(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;
	char *text = malloc(40000);
	char (*lines)[REPORT_LINE_MAX] = malloc(REPORT_LINES_MAX * REPORT_LINE_MAX);
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };
	int n, found;

	/* with the memory */
	fake_init(&f, 1);
	e = env_for(&f);
	e.nc_alloc = f_nc_alloc;
	e.nc_free = f_nc_free;
	g_nc_allocs = g_nc_frees = 0;
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_integrity_ok(r) && bench_plugin_ok(r));
	CHECK(g_nc_allocs == 1 && g_nc_frees == 1);
	CHECK(r->runs[0].ops[BM_NC_R_1M] == 2 && r->runs[0].ops[BM_NC_R_128K] == 16);
	CHECK(r->runs[0].value[BM_NC_R_1M] > 0 && bench_metric_measured(r, BM_NC_R_1M));
	CHECK(!bench_metric_is_iops(BM_NC_R_1M) && !strcmp(bench_metric_unit(BM_NC_R_128K), "MiB/s"));
	CHECK_UNTOUCHED(f);
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "\nNC read 1M       MiB/s     ") && strstr(text, "(NC: into non-cacheable"));
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	CHECK(n < REPORT_LINES_MAX);                     /* still fits the viewer */
	found = 0;
	for (int i = 0; i < n; i++)
		found |= !strncmp(lines[i], "NC: into non-cacheable", 22);
	CHECK(found);

	/* corrupted data read into it is caught like any other */
	fake_init(&f, 1);
	f.corrupt_write_on = 1;
	e = env_for(&f);
	e.nc_alloc = f_nc_alloc;
	e.nc_free = f_nc_free;
	g_nc_allocs = g_nc_frees = 0;
	bench_run(&e, &cfg, r);
	CHECK(!bench_integrity_ok(r) && g_nc_frees == g_nc_allocs);

	/* cancelled half way: the memory is still given back */
	fake_init(&f, 1);
	f.cancel_after_polls = 30;
	e = env_for(&f);
	e.nc_alloc = f_nc_alloc;
	e.nc_free = f_nc_free;
	g_nc_allocs = g_nc_frees = 0;
	CHECK(bench_run(&e, &cfg, r) == 1 && g_nc_allocs == 1 && g_nc_frees == 1);

	/* no such memory (or no callback): not measured, "-" in the tables */
	for (int k = 0; k < 2; k++) {
		fake_init(&f, 1);
		e = env_for(&f);
		if (k) {
			e.nc_alloc = f_nc_none;
			e.nc_free = f_nc_free;
		}
		g_nc_frees = 0;
		CHECK(bench_run(&e, &cfg, r) == 0 && bench_valid(r) && g_nc_frees == 0);
		CHECK(r->runs[0].ops[BM_NC_R_1M] == 0 && !bench_metric_measured(r, BM_NC_R_128K));
		report_format(r, &m, text, 40000);
		CHECK(strstr(text, "\nNC read 1M       MiB/s             -\n"));
		CHECK(!strstr(text, "(NC: into"));
	}
	free(lines);
	free(text);
	free(r);
	free(f.data);
}

static void test_full_run_off(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_MEMCARD, &tiny, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f, 0);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(bench_valid(r) && r->plugin_mode == BPM_OFF);
	CHECK(bench_integrity_ok(r) && bench_plugin_ok(r));
	CHECK(r->runs[0].med_us[BM_SEQ_R_1M] > 83000 && r->runs[0].med_us[BM_SEQ_R_1M] < 85000);
	CHECK(f.reads == 0 && f.writes == 0);
	CHECK_UNTOUCHED(f);
	free(r);
	free(f.data);
}

static void test_write_corruption_is_caught(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_MEMCARD, &one_round, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f, 1);
	f.corrupt_write_on = 1;                           /* ON writes land wrong */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);               /* it still runs to the end */
	CHECK(!bench_integrity_ok(r));
	CHECK(r->runs[0].verify_bad > 0 && r->runs[0].final_bad > 0);
	CHECK_UNTOUCHED(f);
	free(r);
	free(f.data);
}

static void test_read_corruption_is_caught(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &one_round, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f, 1);
	f.corrupt_read_on = 1;                            /* ON reads come back wrong */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(!bench_integrity_ok(r));
	CHECK(r->runs[0].verify_bad > 0 && r->runs[0].final_bad > 0);
	free(r);
	free(f.data);
}

static void test_plugin_counters(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &one_round, "ux0:data/t/" };
	BenchEnv e;
	char *text = malloc(40000);
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };

	/* ON, but nothing took the faster path */
	fake_init(&f, 1);
	f.no_counters = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(bench_valid(r) && !bench_plugin_ok(r) && !r->runs[0].plugin_ok);
	CHECK(strstr(r->runs[0].note, "no transfer"));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin behaved as its state says: FAIL"));
	CHECK(strstr(text, "NOT as expected"));

	/* OFF, but the faster path was used */
	fake_init(&f, 0);
	f.counts_when_off = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(!bench_plugin_ok(r) && strstr(r->runs[0].note, "although the plugin is OFF"));
	CHECK_UNTOUCHED(f);
	free(text);
	free(r);
	free(f.data);
}

static void test_plugin_verify(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &one_round, "ux0:data/t/" };
	BenchEnv e;
	BenchVerify v;
	char *text = malloc(40000);
	char lines[REPORT_LINES_MAX][REPORT_LINE_MAX];
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };
	int n, found;

	/* on and working: every faster transfer checked, PASS */
	fake_init(&f, 1);
	f.verify = f.verify_ready = 1;
	f.pipeline = 1;                    /* overlapped reads set, but verify wins */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_plugin_ok(r));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin overlapped reads: auto (not used: verify is on)\n"));
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	found = 0;
	for (int i = 0; i < n; i++) {
		found |= !strcmp(lines[i], "overlapped reads: auto (off: verify on)");
		CHECK(strlen(lines[i]) < 60);
	}
	CHECK(found);
	f.pipeline = 0;
	bench_verify(r, &v);
	CHECK(v.on && v.ready && v.checked > 0 && v.mismatches == 0);
	CHECK(v.checked == (r->runs[0].after.reads - r->runs[0].before.reads) +
	                   (r->runs[0].after.writes - r->runs[0].before.writes));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin verify: PASS - "));
	CHECK(strstr(text, ", plugin verify ") && strstr(text, " / 0 mismatches"));
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	found = 0;
	for (int i = 0; i < n; i++)
		found |= !strncmp(lines[i], "plugin verify: PASS", 19);
	CHECK(found);

	/* a mismatch: the run fails with its own note, the report says FAIL */
	fake_init(&f, 1);
	f.verify = f.verify_ready = 1;
	f.mismatch_at_read = 2;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && !bench_plugin_ok(r));
	CHECK(strstr(r->runs[0].note, "plugin verify: 1 mismatch(es)"));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin verify: FAIL - 1 of "));

	/* on, but the check buffer is not there: said so, not a failure */
	fake_init(&f, 1);
	f.verify = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_plugin_ok(r));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin verify: on, but no check buffer"));

	/* on and ready, but nothing got checked: a failure */
	fake_init(&f, 1);
	f.verify = f.verify_ready = f.verify_dead = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && !bench_plugin_ok(r));
	CHECK(strstr(r->runs[0].note, "checked no transfer"));

	/* off: a plain line, the counters are not read into anything */
	fake_init(&f, 1);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_plugin_ok(r));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin verify: off"));
	CHECK_UNTOUCHED(f);
	free(text);
	free(r);
	free(f.data);
}

static void test_passthru_counted(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &one_round, "ux0:data/t/" };
	BenchEnv e;
	char *text = malloc(40000);
	char lines[REPORT_LINES_MAX][REPORT_LINE_MAX];
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };
	char want[64];
	int n, found = 0;

	fake_init(&f, 1);
	f.busy_every = 4;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_plugin_ok(r));
	CHECK(bench_passthru(r) > 0 && bench_passthru(r) == f.passthru);
	report_format(r, &m, text, 40000);
	snprintf(want, sizeof(want), "; %u stock, buffer busy)", (unsigned)f.passthru);
	CHECK(strstr(text, want));
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	snprintf(want, sizeof(want), "buffer busy: %u large transfers went stock", (unsigned)f.passthru);
	for (int i = 0; i < n; i++)
		found |= !strcmp(lines[i], want);
	CHECK(found);
	free(text);
	free(r);
	free(f.data);
}

static const BenchProfile with_files = { "files", 2, 2, { 4, 4, 2, 1 }, { 2, 2, 2, 1 }, 12 };

static void test_small_files(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &with_files, "ux0:data/t/" };
	BenchEnv e;
	char *text = malloc(40000);
	char lines[REPORT_LINES_MAX][REPORT_LINE_MAX];
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };
	int n, found = 0;

	/* all three measured, checked, and nothing left behind */
	fake_init(&f, 1);
	f.small = 1;
	f.pipeline = 1;
	f.offload = VM_OFFLOAD_GPU;
	f.gpusafe = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_valid(r) && bench_integrity_ok(r));
	for (int k = 0; k < r->nruns; k++)
		for (int mm = BM_SF_CREATE; mm <= BM_SF_DELETE; mm++)
			CHECK(r->runs[k].ops[mm] == 12 && r->runs[k].value[mm] > 0);
	CHECK(r->runs[0].value[BM_SF_CREATE] < r->runs[0].value[BM_SF_DELETE]);   /* 700 vs 250 us */
	CHECK(r->plugin0.small == 1);
	CHECK_UNTOUCHED(f);
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "files create     files/s"));
	CHECK(strstr(text, "plugin small transfers too: on"));
	CHECK(strstr(text, "plugin overlapped reads: auto\n"));
	CHECK(strstr(text, "plugin copy on the system core: with GPU-safe reads only\n"));
	CHECK(strstr(text, "plugin GPU-safe reads: on\n"));
	CHECK(r->plugin0.offload == VM_OFFLOAD_GPU);
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	for (int i = 0; i < n; i++)
		found |= !strcmp(lines[i], "small transfers too: on");
	CHECK(found);
	found = 0;
	for (int i = 0; i < n; i++)
		found |= !strcmp(lines[i], "copy on the system core: GPU-safe only");
	CHECK(found);
	found = 0;
	for (int i = 0; i < n; i++)
		found |= !strcmp(lines[i], "GPU-safe reads: on");
	CHECK(found);
	CHECK(strstr(lines[n - 5], "files delete"));      /* then the NC reads */
	CHECK(strstr(lines[n - 3], "NC read 128K"));
	CHECK(strstr(lines[n - 1], "CDRAM read 1M"));    /* the other kinds */

	/* a small file that reads back wrong fails the data check */
	fake_init(&f, 1);
	f.sf_corrupt = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && !bench_integrity_ok(r));
	CHECK_UNTOUCHED(f);

	/* a delete that fails stops the test, and the files are still cleaned up
	 * as far as possible (here the fake refuses every delete) */
	fake_init(&f, 1);
	f.sf_fail_remove = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) < 0 && strstr(r->fail_reason, "delete a small file"));
	f.sf_fail_remove = 0;
	memset(g_sf_exists, 0, sizeof(g_sf_exists));

	/* stopped during the small files: the ones made so far are removed */
	fake_init(&f, 1);
	f.cancel_after_polls = 1000000;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	{
		int polls = f.polls;

		fake_init(&f, 1);
		f.cancel_after_polls = polls - 30;        /* inside the last run's small files */
		e = env_for(&f);
		CHECK(bench_run(&e, &cfg, r) == 1 && r->cancelled);
		CHECK_UNTOUCHED(f);
	}

	/* the shipped profiles test small files; an absurd count is refused */
	CHECK(bench_profile_quick.sf_files > 0 && bench_profile_standard.sf_files > 0);
	{
		BenchProfile bad = with_files;

		bad.sf_files = 1000;
		cfg.profile = &bad;
		CHECK(bench_run(&e, &cfg, r) < 0);
	}
	free(text);
	free(r);
	free(f.data);
}

static void test_state_changed_mid_test(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f, 1);
	f.flip_at_write = 3;                              /* the user switched it in Settings */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(r->plugin_mode == BPM_ON && !bench_plugin_ok(r));
	CHECK(strstr(r->runs[0].note, "state changed"));
	CHECK(f.on == 0);                                 /* the tester did not switch it back */
	free(r);
	free(f.data);
}

static void test_other_plugin_states_still_run(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &one_round, "ux0:data/t/" };
	BenchEnv e;

	/* no kernel module */
	fake_init(&f, 1);
	f.unavailable = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_valid(r));
	CHECK(r->plugin_mode == BPM_UNKNOWN && !r->plugin0.available && bench_plugin_ok(r));

	/* safe mode */
	fake_init(&f, 1);
	f.disabled = VM_DIS_SAFE;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && r->plugin_mode == BPM_INACTIVE && bench_plugin_ok(r));

	/* fell back to stock / no buffer */
	fake_init(&f, 1);
	f.status_override = VM_ST_FALLBACK;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && r->plugin_mode == BPM_TROUBLE);
	fake_init(&f, 1);
	f.status_override = VM_ST_NO_BUFFER;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && r->plugin_mode == BPM_TROUBLE);
	CHECK_UNTOUCHED(f);

	/* an env without plugin_get at all */
	fake_init(&f, 0);
	e = env_for(&f);
	e.plugin_get = NULL;
	CHECK(bench_run(&e, &cfg, r) == 0 && r->plugin_mode == BPM_UNKNOWN);
	free(r);
	free(f.data);
}

static void test_plugin_mode_names(void)
{
	BenchPluginState st = { 0 };

	CHECK(bench_plugin_mode(&st) == BPM_UNKNOWN);
	st.available = 1;
	st.status = VM_ST_OFF;      CHECK(bench_plugin_mode(&st) == BPM_OFF);
	st.status = VM_ST_ACTIVE;   CHECK(bench_plugin_mode(&st) == BPM_ON);
	st.status = VM_ST_FALLBACK; CHECK(bench_plugin_mode(&st) == BPM_TROUBLE);
	st.status = VM_ST_UNAVAIL;  CHECK(bench_plugin_mode(&st) == BPM_TROUBLE);
	st.disabled = VM_DIS_HOOKS; CHECK(bench_plugin_mode(&st) == BPM_INACTIVE);
	CHECK(!strcmp(bench_plugin_mode_name(BPM_ON), "ON"));
	CHECK(!strcmp(bench_plugin_mode_name(BPM_OFF), "OFF"));
	CHECK(!strcmp(bench_plugin_mode_name(99), "UNKNOWN"));
}

static void test_cancel_cleans_up(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;

	int total;

	/* how many times a full run asks "stop?", so every cancel point is inside it */
	fake_init(&f, 1);
	f.cancel_after_polls = 1 << 30;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	total = f.polls;
	CHECK(total > 100);

	for (int after = 1; after < total; after += total / 9) {
		fake_init(&f, 1);
		f.cancel_after_polls = after;
		e = env_for(&f);
		CHECK(bench_run(&e, &cfg, r) == 1);
		CHECK(r->cancelled && !r->failed && !bench_valid(r));
		CHECK_UNTOUCHED(f);
	}
	free(r);
	free(f.data);
}

static void test_io_errors_clean_up(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f, 1);
	f.fail_write_at = 3;                              /* the card fills up */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == -1);
	CHECK(r->failed && strstr(r->fail_reason, "write error"));
	CHECK(r->runs[0].io_errors == 1 && !bench_integrity_ok(r));
	CHECK_UNTOUCHED(f);

	fake_init(&f, 1);
	f.mkdirs_fail = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == -1 && strstr(r->fail_reason, "folder"));
	CHECK(f.remove_calls == 0 && f.write_calls == 0);
	free(r);
	free(f.data);
}

static void test_bad_profiles(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchEnv e;
	BenchProfile bad[] = {
		{ "r0", 2, 0, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0 },
		{ "r17", 2, BENCH_MAX_RUNS + 1, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0 },
		{ "s0", 0, 1, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0 },
		{ "big", 2000, 1, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, 0 },
	};

	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		BenchConfig cfg = { VM_DEV_SD2VITA, &bad[i], "ux0:data/t/" };

		fake_init(&f, 1);
		e = env_for(&f);
		CHECK(bench_run(&e, &cfg, r) == -1 && strstr(r->fail_reason, "profile"));
		CHECK(f.write_calls == 0 && f.get_calls == 0);
	}
	free(r);
	free(f.data);
}

static void test_shipped_profiles(void)
{
	CHECK(bench_profile_quick.rounds >= 1 && bench_profile_quick.rounds <= BENCH_MAX_RUNS);
	CHECK(bench_profile_standard.rounds >= 1 && bench_profile_standard.rounds <= BENCH_MAX_RUNS);
}

static void test_timing_math(void)
{
	BenchResult *r = new_result();
	BenchTiming t;

	r->nruns = 3;
	for (int i = 0; i < 3; i++) {
		BenchRun *x = &r->runs[i];

		x->complete = i != 2;                    /* the third run does not count */
		x->before.timing_ok = x->after.timing_ok = 1;
		x->before.rd_sectors = 0xFFFFFFF0u;      /* wraps during the run */
		x->after.rd_sectors = 0xFFFFFFF0u + 1000;
		x->before.rd_io_us = 5;
		x->after.rd_io_us = 5 + 21000;
		x->after.rd_copy_us = 6500;
	}
	bench_timing(r, &t);
	CHECK(t.rd_ok && !t.wr_ok && t.rd_sectors == 2000);
	CHECK(t.rd_io_us > 20.99 && t.rd_io_us < 21.01);
	CHECK(t.rd_copy_us > 6.49 && t.rd_copy_us < 6.51);
	r->runs[0].before.timing_ok = 0;             /* a reading without timing is skipped */
	bench_timing(r, &t);
	CHECK(t.rd_sectors == 1000);
	free(r);
}

static void test_aggregate_skips_incomplete(void)
{
	BenchResult *r = new_result();
	BenchAgg a;

	r->nruns = 3;
	r->runs[0].complete = 1; r->runs[0].value[0] = 10;
	r->runs[1].complete = 1; r->runs[1].value[0] = 20;
	r->runs[2].complete = 0; r->runs[2].value[0] = 999;
	bench_aggregate(r, &a);
	CHECK(a.n == 2 && a.v[0] == 15);
	r->nruns = 0;
	bench_aggregate(r, &a);
	CHECK(a.n == 0 && a.v[0] == 0);                  /* no division by zero */
	free(r);
}

/* ---- report and screen -------------------------------------------------------- */

static int count(const char *text, const char *needle)
{
	int n = 0;

	for (const char *p = text; (p = strstr(p, needle)); p += strlen(needle))
		n++;
	return n;
}

static void test_report_text(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "uma0:data/t/" };
	BenchEnv e;
	ReportMeta m = { "2026-10-28 12:30:11", "0.2.0-alpha", "uma0:data/t/" };
	char *text = malloc(40000);

	fake_init(&f, 1);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(report_format(r, &m, text, 40000) == strlen(text));
	CHECK(strstr(text, "vita-memio tester " TESTER_BUILD "   2026-10-28 12:30:11"));
	CHECK(strstr(text, "plugin 0.2.0-alpha   storage driver id 0x2e7c52f7"));
	CHECK(strstr(text, "plugin during the test: ON for SD2Vita"));
	CHECK(strstr(text, "device: SD2Vita   folder: uma0:data/t/"));
	CHECK(strstr(text, "profile: tiny (2 MiB file, 3 runs)"));
	CHECK(strstr(text, "status: COMPLETE\n"));
	CHECK(strstr(text, "integrity: PASS"));
	CHECK(strstr(text, "plugin behaved as its state says: PASS"));
	CHECK(strstr(text, "seq read 1M"));
	CHECK(strstr(text, "latency per call"));
	CHECK(count(text, "\ncsv,") == 3 * BM_COUNT);
	CHECK(count(text, ",SD2Vita,tiny,ON,") == 3 * BM_COUNT);
	CHECK(count(text, "whole file: ok") == 3);
	CHECK(!strstr(text, "WARNING"));
	CHECK(!strstr(text, " OFF"));                     /* no OFF/ON pair any more */
	CHECK(strstr(text, "  reads:  io 20.0 us, copy 4.0 us"));
	CHECK(strstr(text, "  writes: io 30.0 us, copy 2.0 us"));
	CHECK(count(text, "\ncsvt,") == 3);
	CHECK(strstr(text, ",1,") && strstr(text, ",20.0,4.0,"));

	/* every size: always terminated, never past the buffer */
	for (size_t cap = 1; cap < 600; cap += 7) {
		char *small = malloc(cap);
		size_t n = report_format(r, &m, small, cap);

		CHECK(n < cap && small[n] == 0 && strlen(small) == n);
		free(small);
	}

	/* ON with verify mode on: said in the header */
	fake_init(&f, 1);
	f.verify = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "WARNING: the plugin's verify mode is on"));

	/* OFF: verify mode does not matter then, so no warning */
	fake_init(&f, 0);
	f.verify = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin during the test: OFF for SD2Vita"));
	CHECK(!strstr(text, "WARNING"));
	CHECK(!strstr(text, "time per 512-byte block") && !strstr(text, "csvt"));   /* ON only */
	CHECK(count(text, ",SD2Vita,tiny,OFF,") == 3 * BM_COUNT);

	/* ON with an older kernel module: timing is n/a, nothing else changes */
	fake_init(&f, 1);
	f.no_timing = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_valid(r));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "n/a (the kernel module is older than this tester)"));
	CHECK(!strstr(text, "csvt"));

	/* no kernel module: no plugin verdict line at all */
	fake_init(&f, 1);
	f.unavailable = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin during the test: UNKNOWN"));
	CHECK(!strstr(text, "plugin behaved"));

	/* a failed test says why */
	fake_init(&f, 1);
	f.fail_write_at = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == -1);
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "status: FAILED - write error"));
	CHECK(strstr(text, "no complete run"));
	free(text);
	free(r);
	free(f.data);
}

static void test_screen_lines(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { VM_DEV_INTERNAL, &one_round, "ur0:data/t/" };
	BenchEnv e;
	char (*lines)[REPORT_LINE_MAX] = malloc(REPORT_LINES_MAX * REPORT_LINE_MAX);
	int n;

	fake_init(&f, 1);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	CHECK(n > 25 && n <= REPORT_LINES_MAX);
	CHECK(strcmp(lines[0], "device: Internal    plugin: ON") == 0);
	CHECK(strcmp(lines[2], "data check: PASS") == 0);
	CHECK(strcmp(lines[3], "plugin as expected: PASS") == 0);
	CHECK(strcmp(lines[4], "plugin verify: off") == 0);
	CHECK(strcmp(lines[5], "small transfers too: off") == 0);
	CHECK(strcmp(lines[6], "overlapped reads: off") == 0);
	CHECK(strcmp(lines[7], "buffer busy: 0 large transfers went stock") == 0);
	CHECK(strcmp(lines[8], "read  us/block: io 20.0  copy 4.0") == 0);
	CHECK(strcmp(lines[9], "write us/block: io 30.0  copy 2.0") == 0);
	for (int i = 0; i < n; i++)
		CHECK(strlen(lines[i]) < 60);                /* fits the screen */
	for (int max = 0; max < 6; max++) {              /* a short array is never overrun */
		char (*s)[REPORT_LINE_MAX] = malloc((size_t)(max ? max : 1) * REPORT_LINE_MAX);

		CHECK(report_screen_lines(r, s, max) == max);
		free(s);
	}
	free(lines);
	free(r);
	free(f.data);
}

static void test_formatters(void)
{
	char b[16];

	report_fmt1(b, sizeof(b), 12.34);  CHECK(strcmp(b, "12.3") == 0);
	report_fmt1(b, sizeof(b), 12.36);  CHECK(strcmp(b, "12.4") == 0);
	report_fmt1(b, sizeof(b), 0.04);   CHECK(strcmp(b, "0.0") == 0);
	report_fmt1(b, sizeof(b), -5);     CHECK(strcmp(b, "0.0") == 0);
	report_fmt1(b, sizeof(b), 1e12);   CHECK(strlen(b) > 0);               /* no overflow */
	report_fmt_change(b, sizeof(b), 100, 126);   CHECK(strcmp(b, "+26%") == 0);
	report_fmt_change(b, sizeof(b), 100, 50);    CHECK(strcmp(b, "-50%") == 0);
	report_fmt_change(b, sizeof(b), 100, 100);   CHECK(strcmp(b, "+0%") == 0);
	report_fmt_change(b, sizeof(b), 0, 5);       CHECK(strcmp(b, "n/a") == 0);
	report_fmt1(b, 3, 123.4);                    CHECK(strlen(b) == 2);    /* cut, terminated */
}

/* ---- the results file --------------------------------------------------------- */

static void test_results_file(void)
{
	const char *dir = fakes_tmpdir_new();
	char path[300];
	char (*L)[RESULTS_LINE_MAX] = malloc(RESULTS_LINES_MAX * RESULTS_LINE_MAX);
	char big[40000];
	int n;

	snprintf(path, sizeof(path), "%s/results.txt", dir);

	CHECK(results_size(path) == -1);
	CHECK(results_load(path, L, RESULTS_LINES_MAX) == 0);
	CHECK(results_clear(path) == 0);                 /* clearing nothing is fine */

	CHECK(results_append(path, "first run\nline two\n") == 0);
	CHECK(results_append(path, "second run\n") == 0);   /* appended, not replaced */
	CHECK(results_size(path) == (long)strlen("first run\nline two\nsecond run\n"));
	n = results_load(path, L, RESULTS_LINES_MAX);
	CHECK(n == 3 && strcmp(L[0], "first run") == 0 && strcmp(L[2], "second run") == 0);

	CHECK(results_clear(path) == 0);                 /* and the app can clear it */
	CHECK(results_size(path) == -1 && results_load(path, L, RESULTS_LINES_MAX) == 0);
	CHECK(results_append(path, "after clear\n") == 0);
	CHECK(results_size(path) == (long)strlen("after clear\n"));
	results_clear(path);

	/* long lines wrap, CRLF is dropped, odd bytes are shown as '?' */
	memset(big, 'x', 200);
	strcpy(big + 200, "\r\nplain\n\x01\xC3\xA9z\n");
	CHECK(results_append(path, big) == 0);
	n = results_load(path, L, RESULTS_LINES_MAX);
	CHECK(n == 5);                                   /* 89 + 89 + 22, "plain", "???z" */
	CHECK(strlen(L[0]) == RESULTS_LINE_MAX - 1 && strlen(L[1]) == RESULTS_LINE_MAX - 1);
	CHECK(strlen(L[2]) == 200 - 2 * (RESULTS_LINE_MAX - 1));
	CHECK(strcmp(L[3], "plain") == 0 && strcmp(L[4], "???z") == 0);
	results_clear(path);

	/* a long file keeps the end, and a smaller viewer buffer works too */
	for (int i = 0; i < 1000; i++) {
		char ln[32];

		snprintf(ln, sizeof(ln), "line %d\n", i);
		results_append(path, ln);
	}
	n = results_load(path, L, RESULTS_LINES_MAX);
	CHECK(n == RESULTS_LINES_MAX && strcmp(L[n - 1], "line 999") == 0 && strcmp(L[0], "line 300") == 0);
	{
		char (*S)[RESULTS_LINE_MAX] = malloc(5 * RESULTS_LINE_MAX);

		n = results_load(path, S, 5);
		CHECK(n == 5 && strcmp(S[4], "line 999") == 0 && strcmp(S[0], "line 995") == 0);
		free(S);
	}

	/* bigger than the tail limit: the cut-off first line is dropped */
	results_clear(path);
	for (int i = 0; i < 20000; i++) {
		char ln[40];

		snprintf(ln, sizeof(ln), "row %05d\n", i);
		results_append(path, ln);
	}
	n = results_load(path, L, RESULTS_LINES_MAX);
	CHECK(n == RESULTS_LINES_MAX && strcmp(L[n - 1], "row 19999") == 0);
	for (int i = 0; i < n; i++)
		CHECK(strncmp(L[i], "row ", 4) == 0);        /* no half lines */

	CHECK(results_append("/nonexistent-dir/x/results.txt", "x") == -1);
	free(L);
	fakes_tmpdir_remove();
}

/* ---- the test pattern --------------------------------------------------------- */

static void test_pattern(void)
{
	uint32_t *a = malloc(4096), *b = malloc(4096);
	uint64_t bad_at = 0;

	tp_fill(a, 8192, 4096);
	CHECK(tp_verify(a, 8192, 4096, NULL) == 0);
	CHECK(tp_verify(a, 8192 + 512, 4096, NULL) > 1000);      /* the right data at the wrong place */

	a[100] ^= 1u << 7;
	CHECK(tp_verify(a, 8192, 4096, &bad_at) == 1 && bad_at == 8192 + 400);
	a[100] ^= 1u << 7;

	tp_fill(b, 8192 + 4, 4096);                              /* shifted by one word */
	CHECK(tp_verify(b, 8192, 4096, NULL) > 1000);

	memset(a, 0, 4096);                                      /* a hole of zeros */
	CHECK(tp_verify(a, 8192, 4096, NULL) > 1000);

	/* any offset and size pieces fit together */
	tp_fill(a, 0, 4096);
	tp_fill(b, 0, 4096);
	CHECK(memcmp(a, b, 4096) == 0);
	{
		uint32_t *whole = malloc(8192);

		tp_fill(whole, 4096, 8192);
		CHECK(tp_verify(whole + 512, 4096 + 2048, 6144, NULL) == 0);
		free(whole);
	}
	free(a);
	free(b);
}

/* The calling thread's CPU time: in the report when the platform gives it. */
static void test_cpu_time(void)
{
	Fake f = { 0 };
	BenchEnv e;
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchResult *r = new_result();
	char *text = malloc(40000);
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };
	char lines[REPORT_LINES_MAX][REPORT_LINE_MAX];
	double v;
	int n, found = 0;

	fake_init(&f, 1);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(!bench_cpu_ms_per_mib(r, BM_SEQ_R_1M, &v) && v == 0);
	report_format(r, &m, text, 40000);
	CHECK(!strstr(text, "CPU time of the calling thread"));

	fake_init(&f, 1);
	e = env_for(&f);
	e.cpu_us = f_cpu;
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(bench_cpu_ms_per_mib(r, BM_SEQ_R_1M, &v) && v > 0.99 && v < 1.01);
	CHECK(bench_cpu_ms_per_mib(r, BM_SEQ_R_128K, &v) && v > 0.99 && v < 1.01);
	CHECK(!bench_cpu_ms_per_mib(r, BM_SEQ_W_1M, &v) || v < 0.01);   /* writes cost nothing here */
	CHECK(!bench_cpu_ms_per_mib(r, -1, &v) && !bench_cpu_ms_per_mib(r, BM_COUNT, &v));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "CPU time of the calling thread, ms per MiB:\n  seq read 1M     1.0\n"));
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	for (int i = 0; i < n; i++)
		found |= !strcmp(lines[i], "caller CPU per MiB read: 1.0 ms");
	CHECK(found);
	CHECK_UNTOUCHED(f);
	free(f.data);
	free(text);
	free(r);
}

/* the reads into the other kinds of non-cacheable memory, when
 * the platform gives it; each block given back. */
static int g_mem_allocs, g_mem_frees;
static void *f_mem_alloc(void *c, uint32_t size, int kind)
{
	(void)c;
	if (kind == BENCH_MEM_CDRAM)
		return NULL;                     /* not given: that one is skipped */
	g_mem_allocs++;
	return malloc(size);
}

static void f_mem_free(void *c, void *p, int kind)
{
	(void)c; (void)kind;
	g_mem_frees++;
	free(p);
}

static void test_other_memory(void)
{
	Fake f = { 0 };
	BenchEnv e;
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchResult *r = new_result();

	fake_init(&f, 1);
	e = env_for(&f);
	e.mem_alloc = f_mem_alloc;
	e.mem_free = f_mem_free;
	e.cpu_us = f_cpu;
	g_mem_allocs = g_mem_frees = 0;
	CHECK(bench_run(&e, &cfg, r) == 0 && bench_integrity_ok(r));
	CHECK(g_mem_allocs == 1 && g_mem_frees == 1);
	CHECK(bench_metric_measured(r, BM_NCPC_R_1M) && !bench_metric_measured(r, BM_CDRAM_R_1M));
	CHECK(r->runs[0].ops[BM_NCPC_R_1M] == 2 && r->runs[0].value[BM_NCPC_R_1M] > 0);
	CHECK(!strcmp(bench_metric_name[BM_NCPC_R_1M], "NCphy read 1M"));
	CHECK_UNTOUCHED(f);
	free(f.data);
	free(r);
}

/* the cores' busy time over the calls, when the platform gives
 * it: here core 1 is busy 2 ms per MiB read, the others never. */
static int f_core_busy(void *c, uint64_t out[3])
{
	const Fake *f = c;

	out[0] = 0;
	out[1] = f->cpu * 2;
	out[2] = 0;
	return 0;
}

static void test_core_meter(void)
{
	Fake f = { 0 };
	BenchEnv e;
	BenchConfig cfg = { VM_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchResult *r = new_result();
	char *text = malloc(40000);
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };
	double v;

	fake_init(&f, 1);
	e = env_for(&f);
	e.cpu_us = f_cpu;
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(!bench_core_ms_per_mib(r, BM_SEQ_R_1M, 1, &v));
	report_format(r, &m, text, 40000);
	CHECK(!strstr(text, "busy time of cores"));
	fake_init(&f, 1);
	e = env_for(&f);
	e.cpu_us = f_cpu;
	e.core_busy_us = f_core_busy;
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(bench_core_ms_per_mib(r, BM_SEQ_R_1M, 1, &v) && v > 1.99 && v < 2.01);
	CHECK(bench_core_ms_per_mib(r, BM_SEQ_R_1M, 0, &v) && v == 0);
	CHECK(!bench_core_ms_per_mib(r, BM_SEQ_R_1M, 3, &v));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "busy time of cores 0/1/2 during the calls, ms per MiB:\n"
	                   "  seq read 1M     0.0 / 2.0 / 0.0\n"));
	CHECK_UNTOUCHED(f);
	free(f.data);
	free(text);
	free(r);
}

int main(void)
{
	RUN(test_full_run_on);
	RUN(test_full_run_off);
	RUN(test_nc_reads);
	RUN(test_write_corruption_is_caught);
	RUN(test_read_corruption_is_caught);
	RUN(test_plugin_counters);
	RUN(test_plugin_verify);
	RUN(test_passthru_counted);
	RUN(test_small_files);
	RUN(test_state_changed_mid_test);
	RUN(test_other_plugin_states_still_run);
	RUN(test_plugin_mode_names);
	RUN(test_cancel_cleans_up);
	RUN(test_io_errors_clean_up);
	RUN(test_bad_profiles);
	RUN(test_shipped_profiles);
	RUN(test_timing_math);
	RUN(test_aggregate_skips_incomplete);
	RUN(test_report_text);
	RUN(test_screen_lines);
	RUN(test_formatters);
	RUN(test_results_file);
	RUN(test_pattern);
	RUN(test_cpu_time);
	RUN(test_other_memory);
	RUN(test_core_meter);

	printf(vm_failures ? "test_bench: %d FAILED\n" : "test_bench: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
