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

static const char *mode_name(int on) { return on ? "ON" : "OFF"; }

static const char *verdict(const BenchResult *r)
{
	return r->failed ? "FAILED" : r->cancelled ? "CANCELLED" : "COMPLETE";
}

static const char *order_text(const BenchResult *r, char *buf, size_t cap)
{
	size_t n = 0;

	buf[0] = 0;
	for (int i = 0; i < r->nruns && n + 8 < cap; i++)
		n += (size_t)snprintf(buf + n, cap - n, "%s%s", i ? (i % 2 ? " " : " | ") : "",
		                      mode_name(r->runs[i].mode_on));
	return buf;
}

static int total_bad(const BenchResult *r)
{
	uint32_t bad = r->final_cross_bad;

	for (int i = 0; i < r->nruns; i++)
		bad += r->runs[i].verify_bad + r->runs[i].cross_bad;
	return (int)bad;
}

/* ---- the file block ---------------------------------------------------------- */

static void metric_table(Out *o, const BenchAgg *a, int latency)
{
	char x[16], y[16], c[16];

	if (latency)
		put(o, "%-16s %-9s %9s %9s %7s\n", "latency per call", "", "OFF ms", "ON ms", "change");
	else
		put(o, "%-16s %-9s %9s %9s %7s\n", "metric", "unit", "OFF", "ON", "change");
	for (int m = 0; m < BM_COUNT; m++) {
		double off = latency ? a->off_med_ms[m] : a->off[m];
		double on = latency ? a->on_med_ms[m] : a->on[m];

		report_fmt1(x, sizeof(x), off);
		report_fmt1(y, sizeof(y), on);
		report_fmt_change(c, sizeof(c), off, on);
		if (latency)
			put(o, "%-16s %-9s %9s %9s %7s\n", bench_metric_name[m], "median", x, y, c);
		else
			put(o, "%-16s %-9s %9s %9s %7s\n", bench_metric_name[m],
			    bench_metric_is_iops(m) ? "IOPS" : "MiB/s", x, y, c);
	}
}

size_t report_format(const BenchResult *res, const ReportMeta *meta, char *out, size_t cap)
{
	Out o = { out, cap, 0 };
	BenchAgg a;
	char order[96], v[16];

	if (!cap)
		return 0;
	out[0] = 0;
	bench_aggregate(res, &a);

	put(&o, "================================================================\n");
	put(&o, "Turbo Storage tester %s   %s\n", TESTER_VERSION, meta->timestamp);
	put(&o, "plugin %s   storage driver id 0x%08x\n", meta->plugin_version,
	    (unsigned)res->plugin0.fingerprint);
	put(&o, "device: %s   folder: %s\n", bench_device_name(res->device), meta->folder);
	put(&o, "profile: %s (%u MiB file, %u rounds)\n", res->profile.name,
	    (unsigned)res->profile.file_mib, (unsigned)res->profile.rounds);
	put(&o, "run order: %s\n", order_text(res, order, sizeof(order)));
	put(&o, "status: %s", verdict(res));
	if (res->failed)
		put(&o, " - %s", res->fail_reason);
	put(&o, "\n");
	put(&o, "integrity: %s", bench_integrity_ok(res) ? "PASS" : "FAIL");
	if (!bench_integrity_ok(res))
		put(&o, " (%d differing words, I/O errors possible; see the run lines)", total_bad(res));
	put(&o, "\n");
	put(&o, "plugin really used in ON runs: %s\n", bench_active_ok(res) ? "PASS" : "FAIL");
	put(&o, "----------------------------------------------------------------\n");
	if (a.n_off && a.n_on) {
		metric_table(&o, &a, 0);
		put(&o, "\n");
		metric_table(&o, &a, 1);
		put(&o, "\n");
	} else {
		put(&o, "(no complete OFF and ON pair, so no comparison table)\n\n");
	}

	put(&o, "runs (value per metric, in the order of the table above):\n");
	for (int i = 0; i < res->nruns; i++) {
		const BenchRun *r = &res->runs[i];

		put(&o, " #%d round %d %-3s %s", i + 1, r->round, mode_name(r->mode_on),
		    r->complete ? "" : "(incomplete) ");
		if (r->complete) {
			for (int m = 0; m < BM_COUNT; m++) {
				report_fmt1(v, sizeof(v), r->value[m]);
				put(&o, "%s%s", m ? " " : "", v);
			}
		}
		put(&o, "\n");
		put(&o, "    check: same-mode read-back %s", r->verify_bad ? "FAIL" : "ok");
		if (r->cross_done)
			put(&o, ", previous file (written %s) read %s: %s", mode_name(r->cross_from_on),
			    mode_name(r->mode_on), r->cross_bad ? "FAIL" : "ok");
		put(&o, ", plugin %s", r->active_ok ? "ok" : "NOT as expected");
		if (r->mode_on && r->complete)
			put(&o, " (faster-path transfers: %u reads, %u writes)",
			    (unsigned)(r->after.reads - r->before.reads),
			    (unsigned)(r->after.writes - r->before.writes));
		put(&o, "\n");
		if (r->note[0])
			put(&o, "    note: %s\n", r->note);
	}
	if (res->final_cross_done)
		put(&o, " final: last file (written %s) read %s: %s\n",
		    mode_name(res->final_cross_from_on), mode_name(!res->final_cross_from_on),
		    res->final_cross_bad ? "FAIL" : "ok");

	put(&o, "\ncsv: version,time,device,profile,run,round,mode,metric,value,median_us,p99_us\n");
	for (int i = 0; i < res->nruns; i++) {
		const BenchRun *r = &res->runs[i];

		if (!r->complete)
			continue;
		for (int m = 0; m < BM_COUNT; m++) {
			report_fmt1(v, sizeof(v), r->value[m]);
			put(&o, "csv,%s,%s,%s,%s,%d,%d,%s,%s,%s,%u,%u\n", TESTER_VERSION, meta->timestamp,
			    bench_device_name(res->device), res->profile.name, i + 1, r->round,
			    mode_name(r->mode_on), bench_metric_name[m], v,
			    (unsigned)r->med_us[m], (unsigned)r->p99_us[m]);
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
	char x[16], y[16], c[16];

	bench_aggregate(res, &a);
	line(&s, "device: %s", bench_device_name(res->device));
	line(&s, "status: %s%s%s", verdict(res), res->failed ? " - " : "",
	     res->failed ? res->fail_reason : "");
	line(&s, "data check: %s    plugin used in ON runs: %s",
	     bench_integrity_ok(res) ? "PASS" : "FAIL", bench_active_ok(res) ? "PASS" : "FAIL");
	for (int i = 0; i < res->nruns; i++)
		if (res->runs[i].note[0])
			line(&s, "run %d (%s): %s", i + 1, mode_name(res->runs[i].mode_on),
			     res->runs[i].note);
	if (!(a.n_off && a.n_on)) {
		line(&s, "(no complete OFF and ON pair to compare)");
		return s.n;
	}
	line(&s, "");
	line(&s, "%-15s %8s %8s %7s", "MiB/s or IOPS", "OFF", "ON", "change");
	for (int m = 0; m < BM_COUNT; m++) {
		report_fmt1(x, sizeof(x), a.off[m]);
		report_fmt1(y, sizeof(y), a.on[m]);
		report_fmt_change(c, sizeof(c), a.off[m], a.on[m]);
		line(&s, "%-15s %8s %8s %7s", bench_metric_name[m], x, y, c);
	}
	line(&s, "");
	line(&s, "%-15s %8s %8s %7s", "median ms/call", "OFF", "ON", "change");
	for (int m = 0; m < BM_COUNT; m++) {
		report_fmt1(x, sizeof(x), a.off_med_ms[m]);
		report_fmt1(y, sizeof(y), a.on_med_ms[m]);
		report_fmt_change(c, sizeof(c), a.off_med_ms[m], a.on_med_ms[m]);
		line(&s, "%-15s %8s %8s %7s", bench_metric_name[m], x, y, c);
	}
	return s.n;
}
