/*
 * test_bench.c - host tests for the tester's engine (tester/bench.c), report
 * (report.c), results file (results.c) and test pattern (pattern.h).
 *
 * The "console" is a fake: one file in memory, a clock that advances by a simple
 * speed model (reads and writes are faster with the plugin ON), a fake plugin
 * with the same states and counters as the real one, and switches that inject
 * trouble: corrupted data, a plugin that does not switch on, failing writes, a
 * cancel part-way. Built with AddressSanitizer + UBSan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tester/bench.h"
#include "../tester/pattern.h"
#include "../tester/report.h"
#include "../tester/results.h"
#include "fakes.h"
#include "ts_test.h"

/* ---- the fake console --------------------------------------------------------- */

typedef struct Fake {
	uint8_t *data;
	size_t   size, cap;
	int      exists, open_handles;
	uint64_t now;

	int      on;                 /* the device's setting right now             */
	int      initial_on;         /* what the user had before the test          */
	int      status_override;    /* -1: derive from `on`                       */
	int      disabled, unavailable;
	uint32_t reads, writes, errors, mismatches;

	int      corrupt_read_on, corrupt_write_on;
	int      no_counters;
	int      fail_write_at;      /* the Nth write call fails (0 = never)       */
	int      write_calls;
	int      cancel_after_polls, polls;
	int      mkdirs_fail, refuse_set;

	int      begin_calls, end_calls, remove_calls, set_calls;
} Fake;

static uint64_t f_now(void *c) { return ((Fake *)c)->now; }

static void ensure(Fake *f, size_t n)
{
	if (n > f->cap) {
		f->data = realloc(f->data, n);
		memset(f->data + f->cap, 0, n - f->cap);
		f->cap = n;
	}
}

static int f_open(void *c, const char *path, int trunc)
{
	Fake *f = c;

	(void)path;
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

static int f_pread(void *c, int h, void *buf, uint32_t n, uint64_t off)
{
	Fake *f = c;

	(void)h;
	if (off + n > f->size)
		return -1;
	memcpy(buf, f->data + off, n);
	f->now += 200 + (uint64_t)(n * (f->on ? 0.06 : 0.08));        /* ON reads ~33% faster */
	if (f->on && n >= 32768 && !f->no_counters)
		f->reads++;
	if (f->on && f->corrupt_read_on && n >= 32768)
		((uint8_t *)buf)[n / 2] ^= 0x10;
	return (int)n;
}

static int f_pwrite(void *c, int h, const void *buf, uint32_t n, uint64_t off)
{
	Fake *f = c;

	(void)h;
	f->write_calls++;
	if (f->fail_write_at && f->write_calls == f->fail_write_at)
		return -1;
	ensure(f, off + n);
	memcpy(f->data + off, buf, n);
	if (off + n > f->size)
		f->size = off + n;
	f->now += 400 + (uint64_t)(n * (f->on ? 0.15 : 0.17));
	if (f->on && n >= 32768 && !f->no_counters)
		f->writes++;
	if (f->on && f->corrupt_write_on && n >= 32768)
		f->data[off + n / 2] ^= 0x01;              /* lands on the "card" wrong */
	return (int)n;
}

static int f_remove(void *c, const char *path)
{
	Fake *f = c;

	(void)path;
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

	memset(o, 0, sizeof(*o));
	o->available = !f->unavailable;
	o->disabled = f->disabled;
	o->status = f->disabled ? TS_ST_UNAVAIL
	          : f->on ? (f->status_override >= 0 ? f->status_override : TS_ST_ACTIVE)
	          : TS_ST_OFF;
	o->reads = f->reads;
	o->writes = f->writes;
	o->errors = f->errors;
	o->mismatches = f->mismatches;
	o->fingerprint = 0x2e7c52f7;
	return f->unavailable ? -1 : 0;
}

static int f_begin(void *c) { ((Fake *)c)->begin_calls++; return 0; }

static int f_set(void *c, int on)
{
	Fake *f = c;

	f->set_calls++;
	if (f->refuse_set)
		return -1;
	f->on = on;
	return 0;
}

static void f_end(void *c)
{
	Fake *f = c;

	f->end_calls++;
	f->on = f->initial_on;                     /* put the user's setting back */
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
	               f_get, f_begin, f_set, f_end, f_progress, f_cancelled };
	return e;
}

static void fake_init(Fake *f)
{
	free(f->data);
	memset(f, 0, sizeof(*f));
	f->status_override = -1;
	f->initial_on = 1;                          /* the user had it on */
	f->on = 1;
	g_progress_calls = g_progress_bad = 0;
}

/* a small profile so the tests stay quick under the sanitizers */
static const BenchProfile tiny = { "tiny", 2, 3, { 20, 20, 10, 4 }, { 6, 6, 4, 2 } };
static const BenchProfile one_round = { "one", 2, 1, { 8, 8, 4, 2 }, { 4, 4, 2, 2 } };

static BenchResult *new_result(void) { return calloc(1, sizeof(BenchResult)); }

/* ---- the engine --------------------------------------------------------------- */

static void test_full_run(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchAgg a;
	BenchEnv e;
	static const int order[6] = { 0, 1, 1, 0, 0, 1 };

	fake_init(&f);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(bench_valid(r) && r->nruns == 6);
	for (int i = 0; i < 6; i++) {
		CHECK(r->runs[i].mode_on == order[i]);        /* OFF ON | ON OFF | OFF ON */
		CHECK(r->runs[i].complete && r->runs[i].active_ok);
		CHECK(r->runs[i].round == i / 2 + 1);
		CHECK(r->runs[i].cross_done == (i > 0));
		if (i > 0)
			CHECK(r->runs[i].cross_from_on == order[i - 1]);
	}
	CHECK(bench_integrity_ok(r) && bench_active_ok(r));
	CHECK(r->final_cross_done && r->final_cross_from_on == 1 && r->final_cross_bad == 0);
	CHECK(f.begin_calls == 1 && f.end_calls == 1);
	CHECK(f.on == f.initial_on);                      /* the user's setting is back */
	CHECK(!f.exists && f.open_handles == 0);          /* test file removed, nothing leaked */
	CHECK(g_progress_calls > 50 && g_progress_bad == 0);

	bench_aggregate(r, &a);
	CHECK(a.n_off == 3 && a.n_on == 3);
	CHECK(a.on[BM_SEQ_R_1M] > a.off[BM_SEQ_R_1M] * 1.25 && a.on[BM_SEQ_R_1M] < a.off[BM_SEQ_R_1M] * 1.45);
	CHECK(a.on[BM_SEQ_W_1M] > a.off[BM_SEQ_W_1M]);
	CHECK(a.on_med_ms[BM_SEQ_R_1M] < a.off_med_ms[BM_SEQ_R_1M]);   /* faster = shorter */
	/* 1 MiB at 0.08 us/byte + 200 us = ~84 ms */
	CHECK(r->runs[0].med_us[BM_SEQ_R_1M] > 83000 && r->runs[0].med_us[BM_SEQ_R_1M] < 85000);
	CHECK(r->runs[0].ops[BM_SEQ_R_1M] == 2 && r->runs[0].ops[BM_SEQ_R_128K] == 16);
	CHECK(r->runs[0].ops[BM_RR_4K] == 20 && r->runs[0].ops[BM_RW_1M] == 2);
	free(r);
	free(f.data);
}

static void test_write_corruption_is_caught(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_MEMCARD, &one_round, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f);
	f.corrupt_write_on = 1;                           /* ON writes land wrong */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);               /* it still runs to the end */
	CHECK(!bench_integrity_ok(r));
	CHECK(r->runs[1].verify_bad > 0);                 /* ON read-back sees it */
	CHECK(r->final_cross_done && r->final_cross_bad > 0);   /* and so does the OFF read */
	CHECK(r->runs[0].verify_bad == 0 && r->runs[0].cross_bad == 0);   /* OFF run is clean */
	free(r);
	free(f.data);
}

static void test_read_corruption_is_caught(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_SD2VITA, &one_round, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f);
	f.corrupt_read_on = 1;                            /* ON reads come back wrong */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(!bench_integrity_ok(r));
	CHECK(r->runs[1].verify_bad > 0);
	CHECK(r->runs[1].cross_done && r->runs[1].cross_bad > 0);   /* OFF-written file read ON */
	CHECK(r->runs[0].verify_bad == 0);
	CHECK(r->final_cross_bad == 0);                   /* OFF reads are fine */
	free(r);
	free(f.data);
}

static void test_plugin_does_not_become_active(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f);
	f.status_override = TS_ST_FALLBACK;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == -1);
	CHECK(r->failed && strstr(r->fail_reason, "did not become active"));
	CHECK(r->nruns == 2 && r->runs[0].complete && !r->runs[1].complete);
	CHECK(strstr(r->runs[1].note, "FALLBACK"));
	CHECK(!bench_valid(r));
	CHECK(f.end_calls == 1 && f.on == f.initial_on && !f.exists && f.open_handles == 0);
	free(r);
	free(f.data);
}

static void test_counters_must_grow(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_SD2VITA, &one_round, "ux0:data/t/" };
	BenchEnv e;
	char *text = malloc(40000);
	ReportMeta m = { "2026-01-01 00:00:00", "9.9", "ux0:data/t/" };

	fake_init(&f);
	f.no_counters = 1;                                /* ACTIVE but nothing took the path */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(bench_valid(r) && !bench_active_ok(r));
	CHECK(r->runs[0].active_ok && !r->runs[1].active_ok);
	CHECK(strstr(r->runs[1].note, "no transfer"));
	report_format(r, &m, text, 40000);
	CHECK(strstr(text, "plugin really used in ON runs: FAIL"));
	CHECK(strstr(text, "NOT as expected"));
	free(text);
	free(r);
	free(f.data);
}

static void test_unusable_plugin_at_start(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f);
	e = env_for(&f);
	f.disabled = TS_DIS_SAFE;
	CHECK(bench_run(&e, &cfg, r) == -1 && strstr(r->fail_reason, "inactive"));
	CHECK(f.begin_calls == 0 && f.end_calls == 0 && f.set_calls == 0);   /* nothing touched */

	fake_init(&f);
	e = env_for(&f);
	f.unavailable = 1;
	CHECK(bench_run(&e, &cfg, r) == -1 && strstr(r->fail_reason, "does not answer"));
	CHECK(f.begin_calls == 0);

	fake_init(&f);
	e = env_for(&f);
	f.refuse_set = 1;
	CHECK(bench_run(&e, &cfg, r) == -1 && strstr(r->fail_reason, "refused"));
	CHECK(f.end_calls == 1 && f.on == f.initial_on);
	free(r);
	free(f.data);
}

static void test_cancel_cleans_up(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;

	for (int after = 1; after <= 200; after += 37) {
		fake_init(&f);
		f.cancel_after_polls = after;
		e = env_for(&f);
		CHECK(bench_run(&e, &cfg, r) == 1);
		CHECK(r->cancelled && !r->failed && !bench_valid(r));
		CHECK(f.end_calls == 1 && f.on == f.initial_on);
		CHECK(!f.exists && f.open_handles == 0);
	}
	free(r);
	free(f.data);
}

static void test_io_errors_clean_up(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_SD2VITA, &tiny, "ux0:data/t/" };
	BenchEnv e;

	fake_init(&f);
	f.fail_write_at = 3;                              /* the card fills up */
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == -1);
	CHECK(r->failed && strstr(r->fail_reason, "write error"));
	CHECK(r->runs[0].io_errors == 1 && !bench_integrity_ok(r));
	CHECK(f.end_calls == 1 && f.on == f.initial_on && !f.exists && f.open_handles == 0);

	fake_init(&f);
	f.mkdirs_fail = 1;
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == -1 && strstr(r->fail_reason, "folder"));
	CHECK(f.begin_calls == 0 && f.end_calls == 0);
	free(r);
	free(f.data);
}

static void test_bad_profiles(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchEnv e;
	BenchProfile bad[] = {
		{ "r0", 2, 0, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } },
		{ "r9", 2, 9, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } },
		{ "s0", 0, 1, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } },
		{ "big", 2000, 1, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } },
	};

	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		BenchConfig cfg = { TS_DEV_SD2VITA, &bad[i], "ux0:data/t/" };

		fake_init(&f);
		e = env_for(&f);
		CHECK(bench_run(&e, &cfg, r) == -1 && strstr(r->fail_reason, "profile"));
		CHECK(f.begin_calls == 0 && f.set_calls == 0);
	}
	free(r);
	free(f.data);
}

static void test_aggregate_skips_incomplete(void)
{
	BenchResult *r = new_result();
	BenchAgg a;

	r->nruns = 3;
	r->runs[0].complete = 1; r->runs[0].mode_on = 0; r->runs[0].value[0] = 10;
	r->runs[1].complete = 1; r->runs[1].mode_on = 1; r->runs[1].value[0] = 20;
	r->runs[2].complete = 0; r->runs[2].mode_on = 1; r->runs[2].value[0] = 999;
	bench_aggregate(r, &a);
	CHECK(a.n_off == 1 && a.n_on == 1 && a.off[0] == 10 && a.on[0] == 20);
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
	BenchConfig cfg = { TS_DEV_SD2VITA, &tiny, "uma0:data/t/" };
	BenchEnv e;
	ReportMeta m = { "2026-10-28 12:30:11", "0.2.0-alpha", "uma0:data/t/" };
	char *text = malloc(40000);

	fake_init(&f);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	CHECK(report_format(r, &m, text, 40000) == strlen(text));
	CHECK(strstr(text, "Turbo Storage tester " TESTER_VERSION "   2026-10-28 12:30:11"));
	CHECK(strstr(text, "plugin 0.2.0-alpha   storage driver id 0x2e7c52f7"));
	CHECK(strstr(text, "device: SD2Vita   folder: uma0:data/t/"));
	CHECK(strstr(text, "profile: tiny (2 MiB file, 3 rounds)"));
	CHECK(strstr(text, "run order: OFF ON | ON OFF | OFF ON"));
	CHECK(strstr(text, "status: COMPLETE\n"));
	CHECK(strstr(text, "integrity: PASS"));
	CHECK(strstr(text, "plugin really used in ON runs: PASS"));
	CHECK(strstr(text, "seq read 1M"));
	CHECK(strstr(text, "latency per call"));
	CHECK(count(text, "\ncsv,") == 6 * BM_COUNT);
	CHECK(count(text, " (written ") >= 6);
	CHECK(strstr(text, "final: last file (written ON) read OFF: ok"));
	CHECK(strstr(text, "+3") != NULL);                /* a ~+33% line */

	/* every size: always terminated, never past the buffer */
	for (size_t cap = 1; cap < 600; cap += 7) {
		char *small = malloc(cap);
		size_t n = report_format(r, &m, small, cap);

		CHECK(n < cap && small[n] == 0 && strlen(small) == n);
		free(small);
	}
	free(text);

	/* a failed test says why */
	{
		BenchResult *bad = new_result();
		char *t2 = malloc(8000);
		Fake g = { 0 };

		fake_init(&g);
		g.status_override = TS_ST_NO_BUFFER;
		g.on = g.initial_on = 0;           /* the user had it off; switching on finds no buffer */
		e = env_for(&g);
		CHECK(bench_run(&e, &cfg, bad) == -1);
		report_format(bad, &m, t2, 8000);
		CHECK(strstr(t2, "status: FAILED - the plugin did not become active"));
		CHECK(strstr(t2, "no complete OFF and ON pair"));
		CHECK(g.end_calls == 1 && g.on == g.initial_on);

		/* device already on and without a buffer: refused before anything is touched */
		fake_init(&g);
		g.status_override = TS_ST_NO_BUFFER;
		e = env_for(&g);
		CHECK(bench_run(&e, &cfg, bad) == -1 && strstr(bad->fail_reason, "no staging buffer"));
		CHECK(g.begin_calls == 0 && g.set_calls == 0);
		free(t2);
		free(bad);
		free(g.data);
	}
	free(r);
	free(f.data);
}

static void test_screen_lines(void)
{
	Fake f = { 0 };
	BenchResult *r = new_result();
	BenchConfig cfg = { TS_DEV_INTERNAL, &one_round, "ur0:data/t/" };
	BenchEnv e;
	char (*lines)[REPORT_LINE_MAX] = malloc(REPORT_LINES_MAX * REPORT_LINE_MAX);
	int n;

	fake_init(&f);
	e = env_for(&f);
	CHECK(bench_run(&e, &cfg, r) == 0);
	n = report_screen_lines(r, lines, REPORT_LINES_MAX);
	CHECK(n > 25 && n <= REPORT_LINES_MAX);
	CHECK(strcmp(lines[0], "device: Internal") == 0);
	CHECK(strstr(lines[2], "data check: PASS"));
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
	CHECK(n == 5);                                   /* 71 + 71 + 58, "plain", "???z" */
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

int main(void)
{
	RUN(test_full_run);
	RUN(test_write_corruption_is_caught);
	RUN(test_read_corruption_is_caught);
	RUN(test_plugin_does_not_become_active);
	RUN(test_counters_must_grow);
	RUN(test_unusable_plugin_at_start);
	RUN(test_cancel_cleans_up);
	RUN(test_io_errors_clean_up);
	RUN(test_bad_profiles);
	RUN(test_aggregate_skips_incomplete);
	RUN(test_report_text);
	RUN(test_screen_lines);
	RUN(test_formatters);
	RUN(test_results_file);
	RUN(test_pattern);

	printf(ts_failures ? "test_bench: %d FAILED\n" : "test_bench: ok\n", ts_failures);
	return ts_failures ? 1 : 0;
}
