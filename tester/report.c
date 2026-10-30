/*
 * report.c - see report.h.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "report.h"

void report_fmt1(char *out, size_t cap, double v)
{
	uint32_t t;

	if (v < 0)
		v = 0;
	if (v > 4e8)
		v = 4e8;
	t = (uint32_t)(v * 10.0 + 0.5);
	snprintf(out, cap, "%u.%u", (unsigned)(t / 10), (unsigned)(t % 10));
}

void report_fmt_change(char *out, size_t cap, double off, double on)
{
	double pct;

	if (off <= 0.0 || on < 0.0) {
		snprintf(out, cap, "n/a");
		return;
	}
	pct = (on - off) * 100.0 / off;
	snprintf(out, cap, "%+d%%", (int)(pct < 0 ? pct - 0.5 : pct + 0.5));
}

/* ---- a bounded text builder -------------------------------------------------- */

typedef struct { char *p; size_t cap, len; } Out;

static void put(Out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (o->len + 1 >= o->cap)
		return;
	va_start(ap, fmt);
	n = vsnprintf(o->p + o->len, o->cap - o->len, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	o->len += (size_t)n < o->cap - o->len ? (size_t)n : o->cap - o->len - 1;
}

static const char *verdict(const BenchResult *r)
{
	return r->failed ? "FAILED" : r->cancelled ? "CANCELLED" : "COMPLETE";
}

static int total_bad(const BenchResult *r)
{
	uint32_t bad = 0;

	for (int i = 0; i < r->nruns; i++)
		bad += r->runs[i].verify_bad + r->runs[i].final_bad;
	return (int)bad;
}

static int checkable(const BenchResult *r)
{
	return r->plugin_mode == BPM_ON || r->plugin_mode == BPM_OFF;
}

/* One line that says what the plugin was doing during the test. */
static void plugin_text(const BenchResult *r, char *out, size_t cap)
{
	const char *dev = bench_device_name(r->device);

	switch (r->plugin_mode) {
	case BPM_ON:
		snprintf(out, cap, "ON for %s (faster path)", dev);
		break;
	case BPM_OFF:
		snprintf(out, cap, "OFF for %s (stock storage)", dev);
		break;
	case BPM_INACTIVE:
		snprintf(out, cap, "INACTIVE (safe mode, unknown firmware or no hooks)");
		break;
	case BPM_TROUBLE:
		snprintf(out, cap, "TROUBLE for %s (fell back to stock or no buffer)", dev);
		break;
	default:
		snprintf(out, cap, "UNKNOWN (the kernel module does not answer)");
		break;
	}
}

/* "Small transfers too" when the test started. */
static const char *onoff(int v)
{
	return v == 1 ? "on" : v == 0 ? "off" : "unknown";
}

static const char *pipe_text(int v)
{
	return v == VM_PIPE_OFF ? "off" : v == VM_PIPE_AUTO ? "auto"
	     : v == VM_PIPE_FIXED ? "fixed (2 parts, 3 with GPU-safe)" : "unknown";
}

static const char *small_text(const BenchResult *r)
{
	return onoff(r->plugin0.small);
}

/* The plugin's verify mode in one line; empty when it does not apply. */
static void verify_text(const BenchResult *r, char *out, size_t cap)
{
	BenchVerify v;

	out[0] = 0;
	if (r->plugin_mode != BPM_ON)
		return;
	bench_verify(r, &v);
	if (!v.on)
		snprintf(out, cap, "plugin verify: off");
	else if (!v.ready)
		snprintf(out, cap, "plugin verify: on, but no check buffer - nothing was checked");
	else if (v.mismatches)
		snprintf(out, cap, "plugin verify: FAIL - %u of %u checked transfers differed",
		         (unsigned)v.mismatches, (unsigned)v.checked);
	else if (v.checked)
		snprintf(out, cap, "plugin verify: PASS - %u transfers checked, no mismatch",
		         (unsigned)v.checked);
	else
		snprintf(out, cap, "plugin verify: on, no transfer checked");
}

/* ---- the file block ---------------------------------------------------------- */

/* One value of the tables; "-" for a metric that was not measured. */
static void cell(char *b, size_t cap, const BenchResult *res, int m, double v)
{
	if (bench_metric_measured(res, m))
		report_fmt1(b, cap, v);
	else
		snprintf(b, cap, "-");
}

#define NC_LEGEND "NC: into non-cacheable memory, as games load data for the graphics chip"

static void metric_table(Out *o, const BenchResult *res, const BenchAgg *a, int latency)
{
	char x[16], y[16];

	if (latency)
		put(o, "%-16s %-9s %9s %9s\n", "latency per call", "", "median ms", "p99 ms");
	else
		put(o, "%-16s %-9s %9s\n", "metric", "unit", "average");
	for (int m = 0; m < BM_COUNT; m++) {
		if (latency) {
			cell(x, sizeof(x), res, m, a->med_ms[m]);
			cell(y, sizeof(y), res, m, a->p99_ms[m]);
			put(o, "%-16s %-9s %9s %9s\n", bench_metric_name[m], "", x, y);
		} else {
			cell(x, sizeof(x), res, m, a->v[m]);
			put(o, "%-16s %-9s %9s\n", bench_metric_name[m], bench_metric_unit(m), x);
		}
	}
}

size_t report_format(const BenchResult *res, const ReportMeta *meta, char *out, size_t cap)
{
	Out o = { out, cap, 0 };
	BenchAgg a;
	char pl[96], v[16], vt[128];

	if (!cap)
		return 0;
	out[0] = 0;
	bench_aggregate(res, &a);
	plugin_text(res, pl, sizeof(pl));

	put(&o, "================================================================\n");
	put(&o, "vita-memio tester %s   %s\n", TESTER_VERSION, meta->timestamp);
	put(&o, "plugin %s   storage driver id 0x%08x\n", meta->plugin_version,
	    (unsigned)res->plugin0.fingerprint);
	put(&o, "plugin during the test: %s\n", pl);
	if (res->plugin_mode == BPM_ON && res->plugin0.verify)
		put(&o, "WARNING: the plugin's verify mode is on, so the timings are slower\n");
	verify_text(res, vt, sizeof(vt));
	if (vt[0])
		put(&o, "%s\n", vt);
	if (res->plugin_mode == BPM_ON) {
		put(&o, "plugin small transfers too: %s\n", small_text(res));
		put(&o, "plugin overlapped reads: %s\n", pipe_text(res->plugin0.pipeline));
	}
	put(&o, "device: %s   folder: %s\n", bench_device_name(res->device), meta->folder);
	put(&o, "profile: %s (%u MiB file, %u runs)\n", res->profile.name,
	    (unsigned)res->profile.file_mib, (unsigned)res->profile.rounds);
	put(&o, "status: %s", verdict(res));
	if (res->failed)
		put(&o, " - %s", res->fail_reason);
	put(&o, "\n");
	put(&o, "integrity: %s", bench_integrity_ok(res) ? "PASS" : "FAIL");
	if (!bench_integrity_ok(res))
		put(&o, " (%d differing words, I/O errors possible; see the run lines)", total_bad(res));
	put(&o, "\n");
	if (checkable(res))
		put(&o, "plugin behaved as its state says: %s\n", bench_plugin_ok(res) ? "PASS" : "FAIL");
	put(&o, "----------------------------------------------------------------\n");
	if (a.n) {
		metric_table(&o, res, &a, 0);
		if (bench_metric_measured(res, BM_NC_R_1M))
			put(&o, "(%s)\n", NC_LEGEND);
		put(&o, "\n");
		metric_table(&o, res, &a, 1);
		put(&o, "\n");
	} else {
		put(&o, "(no complete run, so no result table)\n\n");
	}

	if (res->plugin_mode == BPM_ON) {
		BenchTiming t;
		char a[16], b[16];

		bench_timing(res, &t);
		put(&o, "faster path, time per 512-byte block (system call / CPU copy):\n");
		if (!res->plugin0.timing_ok) {
			put(&o, "  n/a (the kernel module is older than this tester)\n");
		} else {
			if (t.rd_ok) {
				report_fmt1(a, sizeof(a), t.rd_io_us);
				report_fmt1(b, sizeof(b), t.rd_copy_us);
				put(&o, "  reads:  io %s us, copy %s us   (%u MiB)\n", a, b,
				    (unsigned)(t.rd_sectors / 2048));
			} else {
				put(&o, "  reads:  no faster-path transfer\n");
			}
			if (t.wr_ok) {
				report_fmt1(a, sizeof(a), t.wr_io_us);
				report_fmt1(b, sizeof(b), t.wr_copy_us);
				put(&o, "  writes: io %s us, copy %s us   (%u MiB)\n", a, b,
				    (unsigned)(t.wr_sectors / 2048));
			} else {
				put(&o, "  writes: no faster-path transfer\n");
			}
		}
		put(&o, "\n");
	}

	put(&o, "runs (value per metric, in the order of the table above):\n");
	for (int i = 0; i < res->nruns; i++) {
		const BenchRun *r = &res->runs[i];

		put(&o, " #%d %s", i + 1, r->complete ? "" : "(incomplete) ");
		if (r->complete) {
			for (int m = 0; m < BM_COUNT; m++) {
				report_fmt1(v, sizeof(v), r->value[m]);
				put(&o, "%s%s", m ? " " : "", v);
			}
		}
		put(&o, "\n");
		put(&o, "    check: read-back %s", r->verify_bad ? "FAIL" : "ok");
		if (r->final_done)
			put(&o, ", whole file: %s", r->final_bad ? "FAIL" : "ok");
		if (checkable(res))
			put(&o, ", plugin %s", r->plugin_ok ? "ok" : "NOT as expected");
		if (res->plugin_mode == BPM_ON && r->complete)
			put(&o, " (faster-path transfers: %u reads, %u writes; %u stock, buffer busy)",
			    (unsigned)(r->after.reads - r->before.reads),
			    (unsigned)(r->after.writes - r->before.writes),
			    (unsigned)(r->after.passthru - r->before.passthru));
		if (res->plugin_mode == BPM_ON && r->complete && r->before.verify) {
			BenchVerify rv;

			bench_verify_run(r, &rv);
			put(&o, ", plugin verify %u checked / %u mismatches",
			    (unsigned)rv.checked, (unsigned)rv.mismatches);
		}
		put(&o, "\n");
		if (r->note[0])
			put(&o, "    note: %s\n", r->note);
	}

	if (res->plugin_mode == BPM_ON && res->plugin0.timing_ok) {
		put(&o, "\ncsvt: version,time,device,profile,run,rd_sectors,rd_io_us_per_blk,"
		        "rd_copy_us_per_blk,wr_sectors,wr_io_us_per_blk,wr_copy_us_per_blk\n");
		for (int i = 0; i < res->nruns; i++) {
			BenchTiming t;
			char ri[16], rc[16], wi[16], wc[16];

			if (!res->runs[i].complete)
				continue;
			bench_timing_run(&res->runs[i], &t);
			report_fmt1(ri, sizeof(ri), t.rd_io_us);
			report_fmt1(rc, sizeof(rc), t.rd_copy_us);
			report_fmt1(wi, sizeof(wi), t.wr_io_us);
			report_fmt1(wc, sizeof(wc), t.wr_copy_us);
			put(&o, "csvt,%s,%s,%s,%s,%d,%u,%s,%s,%u,%s,%s\n", TESTER_VERSION, meta->timestamp,
			    bench_device_name(res->device), res->profile.name, i + 1,
			    (unsigned)t.rd_sectors, ri, rc, (unsigned)t.wr_sectors, wi, wc);
		}
	}

	put(&o, "\ncsv: version,time,device,profile,plugin,run,metric,value,median_us,p99_us\n");
	for (int i = 0; i < res->nruns; i++) {
		const BenchRun *r = &res->runs[i];

		if (!r->complete)
			continue;
		for (int m = 0; m < BM_COUNT; m++) {
			report_fmt1(v, sizeof(v), r->value[m]);
			put(&o, "csv,%s,%s,%s,%s,%s,%d,%s,%s,%u,%u\n", TESTER_VERSION, meta->timestamp,
			    bench_device_name(res->device), res->profile.name,
			    bench_plugin_mode_name(res->plugin_mode), i + 1,
			    bench_metric_name[m], v, (unsigned)r->med_us[m], (unsigned)r->p99_us[m]);
		}
	}
	put(&o, "\n");
	return o.len;
}

/* ---- the screen -------------------------------------------------------------- */

typedef struct { char (*l)[REPORT_LINE_MAX]; int max, n; } Lines;

static void line(Lines *s, const char *fmt, ...)
{
	va_list ap;

	if (s->n >= s->max)
		return;
	va_start(ap, fmt);
	vsnprintf(s->l[s->n], REPORT_LINE_MAX, fmt, ap);
	va_end(ap);
	s->n++;
}

int report_screen_lines(const BenchResult *res, char lines[][REPORT_LINE_MAX], int max)
{
	Lines s = { lines, max, 0 };
	BenchAgg a;
	char x[16], y[16], vt[128];

	bench_aggregate(res, &a);
	line(&s, "device: %s    plugin: %s", bench_device_name(res->device),
	     bench_plugin_mode_name(res->plugin_mode));
	line(&s, "status: %s%s%s", verdict(res), res->failed ? " - " : "",
	     res->failed ? res->fail_reason : "");
	line(&s, "data check: %s", bench_integrity_ok(res) ? "PASS" : "FAIL");
	if (checkable(res))
		line(&s, "plugin as expected: %s", bench_plugin_ok(res) ? "PASS" : "FAIL");
	if (res->plugin_mode == BPM_ON && res->plugin0.verify)
		line(&s, "WARNING: verify mode is on, timings are slower");
	verify_text(res, vt, sizeof(vt));
	if (vt[0])
		line(&s, "%s", vt);
	if (res->plugin_mode == BPM_ON) {
		line(&s, "small transfers too: %s", small_text(res));
		line(&s, "overlapped reads: %s", pipe_text(res->plugin0.pipeline));
		line(&s, "buffer busy: %u large transfers went stock", (unsigned)bench_passthru(res));
	}
	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].note[0])
			line(&s, "run %d: %s", i + 1, res->runs[i].note);
	if (res->plugin_mode == BPM_ON && res->plugin0.timing_ok) {
		BenchTiming t;

		bench_timing(res, &t);
		if (t.rd_ok) {
			report_fmt1(x, sizeof(x), t.rd_io_us);
			report_fmt1(y, sizeof(y), t.rd_copy_us);
			line(&s, "read  us/block: io %s  copy %s", x, y);
		}
		if (t.wr_ok) {
			report_fmt1(x, sizeof(x), t.wr_io_us);
			report_fmt1(y, sizeof(y), t.wr_copy_us);
			line(&s, "write us/block: io %s  copy %s", x, y);
		}
	}
	if (!a.n) {
		line(&s, "(no complete run)");
		return s.n;
	}
	line(&s, "");
	line(&s, "%-15s %10s", "metric", "average");
	for (int m = 0; m < BM_COUNT; m++) {
		cell(x, sizeof(x), res, m, a.v[m]);
		line(&s, "%-15s %10s %s", bench_metric_name[m], x, bench_metric_unit(m));
	}
	if (bench_metric_measured(res, BM_NC_R_1M)) {
		line(&s, "NC: into non-cacheable memory,");
		line(&s, "    as games load data for the GPU");
	}
	line(&s, "");
	line(&s, "%-15s %10s %10s", "ms per call", "median", "p99");
	for (int m = 0; m < BM_COUNT; m++) {
		cell(x, sizeof(x), res, m, a.med_ms[m]);
		cell(y, sizeof(y), res, m, a.p99_ms[m]);
		line(&s, "%-15s %10s %10s", bench_metric_name[m], x, y);
	}
	return s.n;
}
