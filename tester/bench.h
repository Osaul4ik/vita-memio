/*
 * bench.h - the vita-memio benchmark engine (tester/bench.c).
 *
 * Runs one set of storage tests several times on one device, in whatever state the
 * plugin is in. The tester NEVER changes a plugin setting: the user switches the
 * plugin on or off in Settings > vita-memio and runs the test again; every run
 * is saved with the plugin state it was made in, so the two can be compared.
 * Everything that touches the console goes through BenchEnv, so this file and
 * bench.c have no Vita dependencies and run unchanged in the host tests
 * (tests/test_bench.c).
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
 *   - after the last test of each run the whole file is read back and compared
 *     once more, so the random writes are checked too;
 *   - the plugin state (read only): with the plugin ON for the device, its
 *     read/write counters must grow during a run and it must stay ACTIVE; with
 *     it OFF the counters must not move. Otherwise the run is flagged.
 */
#ifndef VITAMEMIO_TESTER_BENCH_H
#define VITAMEMIO_TESTER_BENCH_H

#include <stddef.h>
#include <stdint.h>

#include "../include/vitamemio.h"
#include "../include/vitamemio_kernel_api.h"     /* VM_DIS_* */

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

/* Display name of a device (enum vm_dev). */
const char *bench_device_name(int dev);

/* ---- what the engine needs from the platform ------------------------------- */

typedef struct BenchPluginState {
	int      available;      /* the kernel module answered                      */
	int      disabled;       /* VmState.disabled (VM_DIS_*); 0 = plugin active  */
	int      status;         /* VM_ST_* of the device under test                */
	uint32_t reads, writes;  /* that device's faster-path transfer counters     */
	uint32_t errors, mismatches;
	uint32_t passthru;       /* large transfers left stock: the buffer was busy */
	uint32_t fingerprint;    /* storage driver fingerprint                      */
	int      verify;         /* the plugin's verify mode is on (slows transfers) */
	int      verify_ready;   /* ... and its check buffer is allocated           */
	uint32_t verified;       /* faster transfers the plugin re-read and compared */
	/* Where the faster path's time goes (vmGetTiming; an older kernel module
	 * does not have it: timing_ok = 0). Counters only grow and wrap. */
	int      timing_ok;
	uint32_t rd_sectors, rd_io_us, rd_copy_us;
	uint32_t wr_sectors, wr_io_us, wr_copy_us;
} BenchPluginState;

/* What the plugin does for the device under test, read from its state. */
enum bench_plugin_mode {
	BPM_UNKNOWN = 0,         /* the kernel module does not answer               */
	BPM_OFF,                 /* switched off for this device                    */
	BPM_ON,                  /* ACTIVE                                          */
	BPM_INACTIVE,            /* safe mode, unknown firmware or no hooks         */
	BPM_TROUBLE              /* fell back to stock, or has no staging buffer    */
};

int bench_plugin_mode(const BenchPluginState *st);
const char *bench_plugin_mode_name(int mode);

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

	/* The plugin, read only: nothing here can change a setting. Returns 0 when
	 * the kernel module answered; on -1 (or when NULL) the test still runs and
	 * the plugin state is "unknown". */
	int (*plugin_get)(void *ctx, BenchPluginState *out);

	/* Optional. */
	void (*progress)(void *ctx, const struct BenchProgress *p);
	int (*cancelled)(void *ctx);
} BenchEnv;

/* ---- configuration and results ---------------------------------------------- */

typedef struct BenchProfile {
	const char *name;
	uint32_t    file_mib;       /* size of the test file, MiB (>= 1)            */
	uint32_t    rounds;         /* 1..BENCH_MAX_RUNS runs, one after another    */
	uint32_t    rr_ops[4];      /* random reads for 4K, 32K, 128K, 1M           */
	uint32_t    rw_ops[4];      /* random writes, same sizes                    */
} BenchProfile;

extern const BenchProfile bench_profile_quick;
extern const BenchProfile bench_profile_standard;

typedef struct BenchConfig {
	int                 device;     /* enum vm_dev                              */
	const BenchProfile *profile;
	const char         *dir;        /* test folder with a trailing '/'          */
} BenchConfig;

typedef struct BenchRun {
	int      round;                 /* 1-based                                  */
	int      complete;
	double   value[BM_COUNT];       /* MiB/s or IOPS                            */
	uint32_t med_us[BM_COUNT];      /* median time of one call                  */
	uint32_t p99_us[BM_COUNT];
	uint32_t ops[BM_COUNT];
	uint32_t verify_bad;            /* words that differed when read back       */
	uint32_t final_bad;             /* ... in the read-back of the whole file   */
	int      final_done;
	uint32_t io_errors;
	int      plugin_ok;             /* the plugin behaved as its state says     */
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
	int          plugin_mode;       /* enum bench_plugin_mode, from plugin0     */
} BenchResult;

enum { BP_SETUP = 0, BP_METRIC, BP_FINAL, BP_CLEANUP };

typedef struct BenchProgress {
	int phase;
	int run_index, run_total;       /* run_index is 0-based                     */
	int round;
	int plugin_mode;                /* enum bench_plugin_mode                   */
	int metric;                     /* BM_*, or -1                              */
	uint32_t permille;              /* progress of the current phase, 0..1000   */
	const BenchRun *run;            /* the run so far                           */
} BenchProgress;

/* Runs the whole test. Returns 0 when it ran to the end, 1 when cancelled and
 * -1 on failure (res->fail_reason says why). The test file is removed on every
 * path. No plugin setting is ever touched. */
int bench_run(const BenchEnv *env, const BenchConfig *cfg, BenchResult *res);

/* Mean over the completed runs. */
typedef struct BenchAgg {
	int    n;
	double v[BM_COUNT];
	double med_ms[BM_COUNT];
	double p99_ms[BM_COUNT];
} BenchAgg;

void bench_aggregate(const BenchResult *res, BenchAgg *out);

/* The faster path's time per 512-byte block, over the complete runs: inside the
 * system read/write ("io") and in the CPU copy. ok = 0 when there is nothing to
 * show (no timing from the kernel module, or no faster-path transfer). */
typedef struct BenchTiming {
	int      rd_ok, wr_ok;
	uint64_t rd_sectors, wr_sectors;
	double   rd_io_us, rd_copy_us;     /* per block */
	double   wr_io_us, wr_copy_us;
} BenchTiming;

void bench_timing_run(const BenchRun *r, BenchTiming *out);
void bench_timing(const BenchResult *res, BenchTiming *out);

/* Verdicts. */
int bench_integrity_ok(const BenchResult *res);    /* nothing differed, no I/O error */
int bench_plugin_ok(const BenchResult *res);       /* the plugin did what its state says */

/* The plugin's own verify mode over the completed runs: transfers it re-read
 * and compared, and how many of those differed. 0 if verify mode was off. */
typedef struct BenchVerify {
	int      on;             /* verify mode was on when the test started       */
	int      ready;          /* its check buffer was there                     */
	uint32_t checked, mismatches;
} BenchVerify;
void bench_verify(const BenchResult *res, BenchVerify *out);
void bench_verify_run(const BenchRun *r, BenchVerify *out);

/* Large transfers that went stock because the device's one staging buffer was
 * in use, over the completed runs (ON only). Near 0: a second buffer would not
 * help. */
uint32_t bench_passthru(const BenchResult *res);
int bench_valid(const BenchResult *res);           /* ran to the end */

#endif /* VITAMEMIO_TESTER_BENCH_H */
