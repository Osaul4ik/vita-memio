/*
 * report.h - turns a BenchResult into text: the block that is appended to
 * results.txt, and the shorter lines shown on the tester's result screen.
 * No Vita calls.
 */
#ifndef VITAMEMIO_TESTER_REPORT_H
#define VITAMEMIO_TESTER_REPORT_H

#include <stddef.h>

#include "bench.h"

#define TESTER_VERSION  "0.4.0"

typedef struct ReportMeta {
	const char *timestamp;        /* "2026-10-28 12:30:11"                      */
	const char *plugin_version;   /* VM_VERSION_STR of the app build            */
	const char *folder;           /* where the test file was                    */
} ReportMeta;

/* "12.3" from a value, without floating-point printf. */
void report_fmt1(char *out, size_t cap, double v);
/* "+17%" for on versus off, "n/a" when off is zero. */
void report_fmt_change(char *out, size_t cap, double off, double on);

/* The text appended to results.txt: header (with the plugin state the test ran
 * in), result table, latency table, one line per run, and CSV lines. Returns the length (always NUL-terminated, cut to
 * cap). */
size_t report_format(const BenchResult *res, const ReportMeta *meta, char *out, size_t cap);

#define REPORT_LINE_MAX  90
#define REPORT_LINES_MAX 80

/* Short lines for the screen (at most `max`). Returns the number of lines. */
int report_screen_lines(const BenchResult *res, char lines[][REPORT_LINE_MAX], int max);

#endif /* VITAMEMIO_TESTER_REPORT_H */
