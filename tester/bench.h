/*
 * bench.h - the Turbo Storage benchmark engine (tester/bench.c).
 *
 * Runs the same set of storage tests several times, alternating the plugin OFF
 * and ON for the device under test (a setting that takes effect at once, so both
 * modes run on the same card in the same session). Everything that touches the
 * console goes through BenchEnv, so this file and bench.c have no Vita
 * dependencies and run unchanged in the host tests (tests/test_bench.c).
 *
 * What a run measures (MiB/s for sequential, IOPS for random, plus the median
 * and 99th percentile time of a single call):
 *   seq write 1M | seq read 1M | seq read 128K
 *   random read 4K/32K/128K/1M | random write 4K/32K/128K/1M
 * Only the time spent inside the read/write calls counts: filling and checking
 * the data happens outside the timer.
 *
 * What it checks:
 *   - every byte read back is compared with the known pattern (pattern.h);
 *   - cross check: each run first reads, in its own mode, the file written by the
 *     previous run in the other mode, and after the last run the opposite mode
 *     reads the last file. So data written with the plugin ON is read with it OFF
 *     and the other way round;
 *   - in an ON run the plugin must be ACTIVE and its read/write counters must
 *     grow, otherwise the comparison is not valid and is flagged.
 */
#ifndef STORAGETURBO_TESTER_BENCH_H
#define STORAGETURBO_TESTER_BENCH_H

#include <stddef.h>
#include <stdint.h>

#include "../include/storageturbo.h"
#include "../include/storageturbo_kernel_api.h"     /* TS_DIS_* */

#define BENCH_MAX_RUNS    16
#define BENCH_PATH_MAX    160
#define BENCH_REASON_MAX  96

enum bench_metric {
	BM_SEQ_W_1M = 0, BM_SEQ_R_1M, BM_SEQ_R_128K,
	BM_RR_4K, BM_RR_32K, BM_RR_128K, BM_RR_1M,
	BM_RW_4K, BM_RW_32K, BM_RW_128K, BM_RW_1M,
	BM_COUNT
};

extern const char *const bench_metric_name[BM_COUNT];
/* 0: the value is MiB/s, 1: the value is operations per second. */
int bench_metric_is_iops(int metric);

/* Setting key of a device (enum ts_dev) and its display name. */
int bench_device_key(int dev);
const char *bench_device_name(int dev);

/* ---- what the engine needs from the platform ------------------------------- */

typedef struct BenchPluginState {
	int      available;      /* the kernel module answered                      */
	int      disabled;       /* TsState.disabled (TS_DIS_*); 0 = plugin active  */
	int      status;         /* TS_ST_* of the device under test                */
	uint32_t reads, writes;  /* that device's faster-path transfer counters     */
	uint32_t errors, mismatches;
	uint32_t fingerprint;    /* storage driver fingerprint                      */
} BenchPluginState;

struct BenchProgress;

typedef struct BenchEnv {
	void *ctx;
	uint64_t (*now_us)(void *ctx);

	/* Files. open: create_trunc = 1 creates/empties the file, 0 opens an existing
	 * one; always read/write. Handles are >= 0; errors are < 0. read/write return
	 * the number of bytes moved. */
	int (*open)(void *ctx, const char *path, int create_trunc);
	int (*pread)(void *ctx, int h, void *buf, uint32_t n, uint64_t off);
	int (*pwrite)(void *ctx, int h, const void *buf, uint32_t n, uint64_t off);
	int (*close)(void *ctx, int h);
	int (*remove)(void *ctx, const char *path);
	int (*mkdirs)(void *ctx, const char *dir);

	/* The plugin. begin remembers the current settings, end puts them back (the
	 * engine calls end on every way out); set switches the device under test. */
	int (*plugin_get)(void *ctx, BenchPluginState *out);
	int (*plugin_begin)(void *ctx);
	int (*plugin_set)(void *ctx, int on);
	void (*plugin_end)(void *ctx);

	/* Optional. */
	void (*progress)(void *ctx, const struct BenchProgress *p);
	int (*cancelled)(void *ctx);
} BenchEnv;

/* ---- configuration and results ---------------------------------------------- */

typedef struct BenchProfile {
	const char *name;
	uint32_t    file_mib;       /* size of the test file, MiB (>= 1)            */
	uint32_t    rounds;         /* 1..8; each round runs OFF and ON, order flips */
	uint32_t    rr_ops[4];      /* random reads for 4K, 32K, 128K, 1M           */
	uint32_t    rw_ops[4];      /* random writes, same sizes                    */
} BenchProfile;

extern const BenchProfile bench_profile_quick;
extern const BenchProfile bench_profile_standard;

typedef struct BenchConfig {
	int                 device;     /* enum ts_dev                              */
	const BenchProfile *profile;
	const char         *dir;        /* test folder with a trailing '/'          */
} BenchConfig;

typedef struct BenchRun {
	int      mode_on;
	int      round;
	int      complete;
	double   value[BM_COUNT];       /* MiB/s or IOPS                            */
	uint32_t med_us[BM_COUNT];      /* median time of one call                  */
	uint32_t p99_us[BM_COUNT];
	uint32_t ops[BM_COUNT];
	uint32_t verify_bad;            /* words that differed when read back       */
	uint32_t cross_bad;             /* ... in the file the previous run wrote   */
	int      cross_done;
	int      cross_from_on;
	uint32_t io_errors;
	int      active_ok;             /* ON: ACTIVE and counters grew; OFF: idle  */
	char     note[BENCH_REASON_MAX];
	BenchPluginState before, after;
} BenchRun;

typedef struct BenchResult {
	BenchProfile profile;
	int          device;
	char         dir[BENCH_PATH_MAX];
	BenchRun     runs[BENCH_MAX_RUNS];
	int          nruns;
	int          cancelled;
	int          failed;
	char         fail_reason[BENCH_REASON_MAX];
	BenchPluginState plugin0;       /* state when the test started              */
	int          final_cross_done;  /* the last file read in the opposite mode  */
	int          final_cross_from_on;
	uint32_t     final_cross_bad;
} BenchResult;

enum { BP_SETUP = 0, BP_CROSS, BP_METRIC, BP_FINAL, BP_CLEANUP };

typedef struct BenchProgress {
	int phase;
	int run_index, run_total;       /* run_index is 0-based                     */
	int round;
	int mode_on;
	int metric;                     /* BM_*, or -1                              */
	uint32_t permille;              /* progress of the current phase, 0..1000   */
	const BenchRun *run;            /* the run so far                           */
} BenchProgress;

/* Runs the whole test. Returns 0 when it ran to the end, 1 when cancelled and
 * -1 on failure (res->fail_reason says why). The plugin settings are restored and
 * the test file removed on every path. */
int bench_run(const BenchEnv *env, const BenchConfig *cfg, BenchResult *res);

/* Mean over the completed runs of each mode. */
typedef struct BenchAgg {
	int    n_off, n_on;
	double off[BM_COUNT], on[BM_COUNT];
	double off_med_ms[BM_COUNT], on_med_ms[BM_COUNT];
	double off_p99_ms[BM_COUNT], on_p99_ms[BM_COUNT];
} BenchAgg;

void bench_aggregate(const BenchResult *res, BenchAgg *out);

/* Verdicts. */
int bench_integrity_ok(const BenchResult *res);    /* nothing differed, no I/O error */
int bench_active_ok(const BenchResult *res);       /* every ON run really used the plugin */
int bench_valid(const BenchResult *res);           /* ran to the end */

#endif /* STORAGETURBO_TESTER_BENCH_H */
