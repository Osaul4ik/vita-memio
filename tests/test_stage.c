/*
 * test_stage.c - host tests for kernel/stage.c, the code that sits between the
 * system's storage driver and every read and write.
 *
 * stage.c is included (not linked) so the tests can reach its statics. The
 * "driver" is an in-memory disk behind fake_io(); the memory card slot
 * (VM_DEV_MEMCARD, port -1) is used because it needs no card context. Built with
 * AddressSanitizer + UBSan, so an out-of-range copy fails the run.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

/* The staging copy, with a time cost the tests can set (0 by default, so the
 * other tests' timings are io only). */
extern uint32_t fake_now_us;
static uint32_t copy_cost_us;
/* Which thread copied: the test's own (the caller) or another (the copier). */
static pthread_t test_thread;
static int copies_here, copies_elsewhere;
static void *fake_copy(void *d, const void *s, size_t n)
{
	__sync_fetch_and_add(&fake_now_us, copy_cost_us);
	if (pthread_equal(pthread_self(), test_thread))
		__sync_fetch_and_add(&copies_here, 1);
	else
		__sync_fetch_and_add(&copies_elsewhere, 1);
	return memcpy(d, s, n);
}
#define vm_copy_from_stage fake_copy
#define vm_copy_to_stage fake_copy

#include "../kernel/stage.c"

#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include <stdlib.h>

#include "fakes.h"
#include "vm_test.h"

#define DISK_SECTORS 4096
#define REF_RD       1
#define REF_WR       2
#define M            VM_DEV_MEMCARD

static uint8_t disk[DISK_SECTORS * 512];
static pthread_mutex_t disk_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *staging;

static int io_calls, fail_at_call, corrupt_staged;
static int io_here;                       /* card reads by the test's own thread */
static void *last_buf;
static void *io_bufs[16];                 /* the buffer and size of each call */
static uint32_t io_ns[16];

/* The "system driver": ref says whether this is the read or the write function. */
static int fake_io(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	(void)part;
	int call = __sync_add_and_fetch(&io_calls, 1);

	__atomic_store_n(&last_buf, buf, __ATOMIC_RELAXED);   /* threads in the stress test */
	if (pthread_equal(pthread_self(), test_thread))
		__atomic_fetch_add(&io_here, 1, __ATOMIC_RELAXED);
	if (call <= (int)(sizeof(io_bufs) / sizeof(io_bufs[0]))) {
		io_bufs[call - 1] = buf;
		io_ns[call - 1] = n;
	}
	__sync_fetch_and_add(&fake_now_us, 2500);
	if (fail_at_call && call == fail_at_call)
		return -5;
	if (sector > DISK_SECTORS || n > DISK_SECTORS - sector)
		return -5;                            /* past the card's end */
	pthread_mutex_lock(&disk_lock);           /* the stress test writes meanwhile */
	if (ref == REF_RD) {
		memcpy(buf, disk + (size_t)sector * 512, (size_t)n * 512);
		if (corrupt_staged && buf == staging)
			((uint8_t *)buf)[0] ^= 0xFF;
	} else {
		memcpy(disk + (size_t)sector * 512, buf, (size_t)n * 512);
		if (corrupt_staged && buf == staging)
			disk[(size_t)sector * 512] ^= 0xFF;
	}
	pthread_mutex_unlock(&disk_lock);
	return 0;
}

static void reset(void)
{
	Dev *d = &g_dev[M];

	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (g_dev[i].pipe_made)
			pipe_release(&g_dev[i]);       /* a test that left its helper */
	fakes_reset();
	for (size_t i = 0; i < sizeof(disk); i++)
		disk[i] = (uint8_t)(i * 31 + (i >> 9));
	memset(d, 0, sizeof(*d));
	d->port = -1;
	d->uid = -1;
	d->uid2 = d->th = d->sem_go = d->sem_done = d->sem_free = -1;
	d->th2 = d->sem_cgo = d->sem_fin = -1;
	d->ra_th = d->ra_go = d->ra_done = -1;
	g_readahead = 0;
	g_ra_hits = g_ra_kib = g_ra_drops = g_ra_waste_kib = g_ra_bad = 0;
	g_ra_nc_kib = 0;
	g_ra_mode = VM_RA_OFF;
	g_ra_pid = g_ra_seen = 0;
	g_ra_thr[0] = g_ra_thr[1] = 0;
	g_ra_learning = 0;
	g_ra_title[0] = 0;
	g_ra_auto_logged = 0;
	g_title_fn = NULL;
	ra_hist_clear();
	g_nc_off_refused = 0;
	g_nc_off_type = 0;
	g_offload = VM_OFFLOAD_OFF;
	g_offload_state = g_offload_logged = 0;
	g_offload_type = 0;
	g_offload_err = 0;
	g_off_reads = g_off_kib = g_off_maperr = 0;
	g_log_diag = 0;
	memset(g_cp_core_kib, 0, sizeof(g_cp_core_kib));
	g_cp_sys_kib = g_cp_game_kib = 0;
	test_thread = pthread_self();
	copies_here = copies_elsewhere = 0;
	d->buf = staging;
	d->enabled = 1;
	g_verify = 0;
	g_min_sectors = MIN_SECTORS;
	g_stats = 1;                              /* most tests look at the figures */
	g_gpu_safe = 0;
	g_dcache_clean = NULL;
	g_gpu_cleaned_kib = g_nc_stock_kib = 0;
	g_pipeline = 0;
	g_pipe_reads = g_pipe_reads3 = g_rd_max = g_pipe_waits = g_pipe_wait_us = 0;
	memset(io_bufs, 0, sizeof(io_bufs));
	memset(io_ns, 0, sizeof(io_ns));
	g_count_all = 1;
	g_shadow = NULL;
	g_shadow_uid = -1;
	g_shadow_busy = 0;
	g_shadow_failed = 0;
	io_calls = fail_at_call = corrupt_staged = 0;
	last_buf = NULL;
	copy_cost_us = 0;
}

static uint8_t *user_buf(size_t sectors)
{
	uint8_t *b = malloc(sectors * 512);       /* exact size: ASan sees any overrun */
	memset(b, 0xEE, sectors * 512);
	return b;
}

static int rd(uint8_t *u, uint32_t sector, uint32_t n)
{
	return stage_xfer(&g_dev[M], 0, fake_io, REF_RD, 0, NULL, sector, u, n);
}

static int wr(const uint8_t *u, uint32_t sector, uint32_t n, tai_hook_ref_t rd_ref)
{
	return stage_xfer(&g_dev[M], 1, fake_io, REF_WR, rd_ref, NULL, sector, (void *)u, n);
}

/* div_by (the planner's division by a multiplication) is exact. */
static void test_div_by(void)
{
	static const uint64_t ds[] = { 1, 2, 3, 7, 1000, 4096, 4200, 11100, 30999, 65536, 1000003,
	                               0x7FFFFFFFu, 0xFFFFFFFFu, 0x100000000ull };
	uint32_t r = 12345;

	CHECK(div_by(5, 0, recip(0)) == ~(uint64_t)0);
	for (unsigned j = 0; j < sizeof(ds) / sizeof(ds[0]); j++) {
		const uint64_t d = ds[j];
		const uint32_t inv = recip(d);
		static const uint64_t xs[] = { 0, 1, 4095, 4096, 4097, 0xFFFFFFFEu, 0xFFFFFFFFu,
		                               0x100000000ull, 0x123456789ull };

		for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++)
			CHECK(div_by(xs[i], d, inv) == xs[i] / d);
		for (int i = 0; i < 200000; i++) {
			uint64_t x;

			r = r * 1664525u + 1013904223u;
			x = r;
			CHECK(div_by(x, d, inv) == x / d);
			x = (x / d) * d;                    /* exact multiples and their neighbours */
			CHECK(div_by(x, d, inv) == x / d);
			if (x)
				CHECK(div_by(x - 1, d, inv) == (x - 1) / d);
		}
	}
}

static void test_small_goes_stock(void)
{
	uint8_t *u = user_buf(8);

	reset();
	CHECK(rd(u, 10, 8) == 0);
	CHECK(io_calls == 1 && last_buf == u);
	CHECK(memcmp(u, disk + 10 * 512, 8 * 512) == 0);
	CHECK(g_dev[M].st.reads == 0);
	CHECK(g_dev[M].all_rd == 8 * 512);        /* still counted for the HUD */
	free(u);
}

/* "Small transfers too": every transfer takes the faster path, down to one
 * sector; switching it off again restores the 32 KiB minimum. */
static void test_small_transfers_switch(void)
{
	uint8_t *u = user_buf(8);
	VmSettings s;

	reset();
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.small = 1;
	vm_stage_apply(&s);
	CHECK(rd(u, 10, 8) == 0);
	CHECK(last_buf == staging && g_dev[M].st.reads == 1);
	CHECK(memcmp(u, disk + 10 * 512, 8 * 512) == 0);
	CHECK(rd(u, 3, 1) == 0 && last_buf == staging);          /* one sector */
	CHECK(memcmp(u, disk + 3 * 512, 512) == 0);
	memset(u, 0x5A, 8 * 512);
	CHECK(wr(u, 20, 8, REF_RD) == 0 && last_buf == staging);
	CHECK(memcmp(disk + 20 * 512, u, 8 * 512) == 0);
	CHECK(rd(u, 0, 0) == 0 && last_buf == u);                /* nothing to move: stock */

	s.small = 0;
	vm_stage_apply(&s);
	CHECK(rd(u, 10, 8) == 0 && last_buf == u);
	free(u);
}

/* Statistics off: the clock is never read and the timers and the data
 * counters stay still; what safety needs (requests, errors) is still counted.
 * The HUD alone turns the all-transfers counter back on. */
static void test_statistics_switch(void)
{
	uint8_t *u = user_buf(600);
	VmSettings s;

	reset();
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	vm_stage_apply(&s);                       /* stats 0, hud off */
	CHECK(rd(u, 0, 600) == 0 && wr(u, 0, 600, REF_RD) == 0);
	CHECK(memcmp(u, disk, 600 * 512) == 0);
	CHECK(fake_time_calls == 0);
	CHECK(g_dev[M].tm.rd_sectors == 0 && g_dev[M].tm.wr_io_us == 0);
	CHECK(g_dev[M].rd_bytes == 0 && g_dev[M].wr_bytes == 0);
	CHECK(g_dev[M].all_rd == 0 && g_dev[M].all_wr == 0);
	CHECK(g_dev[M].st.reads == 1 && g_dev[M].st.writes == 1);

	s.hud = VM_HUD_COMPACT;                   /* the HUD needs its byte counts */
	vm_stage_apply(&s);
	CHECK(rd(u, 0, 100) == 0);
	CHECK(g_dev[M].all_rd == 100 * 512 && g_dev[M].tm.rd_sectors == 0 && fake_time_calls == 0);

	s.stats = 1;
	vm_stage_apply(&s);
	CHECK(rd(u, 0, 100) == 0);
	CHECK(g_dev[M].tm.rd_sectors == 100 && g_dev[M].rd_bytes == 100 * 512 && fake_time_calls > 0);
	free(u);
}

/* The fake cache clean: records what it was asked to write out. */
static int clean_calls;
static uintptr_t clean_lo, clean_hi;
static uint32_t clean_copy_seen;          /* the copy had run before the clean */
static uint8_t *clean_user;
static void fake_clean(void *start, unsigned int size)
{
	uintptr_t a = (uintptr_t)start;

	clean_calls++;
	if (!clean_lo || a < clean_lo) clean_lo = a;
	if (a + size > clean_hi) clean_hi = a + size;
	if (clean_user && clean_user[0] == disk[10 * 512])
		clean_copy_seen++;
}

/* A caller buffer in non-cacheable memory (games load data for the graphics
 * chip into it): the standard path, which has no cache work there and needs no
 * copy, is the faster one - reads and writes, GPU-safe on or off. Cached,
 * write-back-L1 or unknown memory, or only one end non-cacheable: staged as
 * before. Looked up only for a transfer that would be staged. */
static void test_noncacheable_buffers_go_stock(void)
{
	uint8_t *u = user_buf(1100);
	VmSettings s;

	for (int gpu = 0; gpu < 2; gpu++) {
		static const int stock[] = { 0x8000, 0x0800 };
		static const int staged[] = { 0xD000, 0x4000, -1 };

		reset();
		memset(&s, 0, sizeof(s));
		s.memcard = 1;
		s.gpusafe = gpu;
		s.stats = 1;
		vm_stage_apply(&s);
		g_dcache_clean = fake_clean;
		for (int i = 0; i < 2; i++) {
			fake_memtype = stock[i];
			fake_memtype_calls = 0;
			clean_calls = 0;
			memset(u, 0, 1100 * 512);
			CHECK(rd(u, 10, 1100) == 0 && last_buf == u && io_calls >= 1);
			CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
			CHECK(fake_memtype_calls == 2 && clean_calls == 0);   /* both ends */
			memset(u, 0x3C, 100 * 512);
			CHECK(wr(u, 50, 100, REF_RD) == 0 && last_buf == u);
			CHECK(memcmp(disk + 50 * 512, u, 100 * 512) == 0);
		}
		CHECK(g_dev[M].st.reads == 0 && g_dev[M].st.writes == 0);
		CHECK(g_nc_stock_kib == 2 * (550 + 50));
		CHECK(g_dev[M].all_rd == 2 * 1100 * 512);            /* the HUD still counts */
		for (int i = 0; i < 3; i++) {
			fake_memtype = staged[i];
			CHECK(rd(u, 10, 1100) == 0 && last_buf == staging);
			CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
		}
		/* only the start non-cacheable, the end cached: staged */
		fake_memtype = 0x8000;
		fake_memtype_from = u + 600 * 512;
		fake_memtype_above = 0xD000;
		CHECK(rd(u, 10, 1100) == 0 && last_buf == staging);
		/* ... and the other way round */
		fake_memtype = 0xD000;
		fake_memtype_above = 0x8000;
		CHECK(rd(u, 10, 1100) == 0 && last_buf == staging);
		fake_memtype_from = NULL;
		CHECK(g_dev[M].st.reads == 5);
	}

	/* nothing looked up for what stays stock anyway */
	reset();
	fake_memtype = 0x8000;
	CHECK(rd(u, 0, 8) == 0 && fake_memtype_calls == 0);           /* small */
	g_dev[M].enabled = 0;
	CHECK(rd(u, 0, 100) == 0 && fake_memtype_calls == 0 && last_buf == u);   /* off */
	free(u);
}

/* GPU-safe reads: every faster read's copy is cleaned out of the cache, the
 * whole destination in 32-byte lines; writes are not; off (or no clean
 * function on this firmware) changes nothing. */
static void test_gpu_safe_reads(void)
{
	uint8_t *u = user_buf(1100);
	uint8_t *odd = u + 3;                     /* a caller buffer off a line */
	VmSettings s;

	reset();
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.gpusafe = 1;
	s.stats = 1;
	vm_stage_apply(&s);
	g_dcache_clean = fake_clean;
	clean_calls = 0; clean_lo = clean_hi = 0; clean_copy_seen = 0; clean_user = u;
	CHECK(rd(u, 10, 1100) == 0);                    /* 512 + 512 + 76: three chunks */
	CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
	CHECK(clean_calls == 3 && clean_copy_seen == 3);
	CHECK(clean_lo <= (uintptr_t)u && clean_hi >= (uintptr_t)u + 1100 * 512);
	CHECK((clean_lo & 31) == 0 && (clean_hi & 31) == 0);

	clean_calls = 0; clean_lo = clean_hi = 0; clean_user = NULL;
	CHECK(rd(odd, 0, 64) == 0);
	CHECK(clean_calls == 1 && clean_lo <= (uintptr_t)odd &&
	      clean_hi >= (uintptr_t)odd + 64 * 512 && (clean_lo & 31) == 0);

	/* a non-cacheable buffer goes the standard way: nothing to clean */
	clean_calls = 0;
	fake_memtype = 0x8000;
	CHECK(rd(u, 10, 1100) == 0 && clean_calls == 0 && last_buf == u);
	CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
	fake_memtype = 0xD000;

	clean_calls = 0;
	CHECK(wr(u, 0, 100, REF_RD) == 0 && clean_calls == 0);   /* writes: nothing to do */
	CHECK(rd(u, 0, 8) == 0 && clean_calls == 0);             /* small, stock: not ours */

	s.gpusafe = 0;
	vm_stage_apply(&s);
	CHECK(rd(u, 0, 100) == 0 && clean_calls == 0);

	s.gpusafe = 1;                                  /* on, but no clean function: */
	vm_stage_apply(&s);
	g_dcache_clean = NULL;
	memset(u, 0, 100 * 512);
	CHECK(rd(u, 0, 100) == 0 && memcmp(u, disk, 100 * 512) == 0);
	CHECK(last_buf == u);                           /* the standard path, not staged */
	CHECK(wr(u, 0, 100, REF_RD) == 0 && last_buf == staging);   /* writes as usual */
	s.gpusafe = 0;
	vm_stage_apply(&s);
	CHECK(rd(u, 0, 100) == 0 && last_buf == staging);           /* off: staged again */
	free(u);
}

static void test_staged_read(void)
{
	uint8_t *u = user_buf(100);

	reset();
	CHECK(rd(u, 10, 100) == 0);
	CHECK(io_calls == 1 && last_buf == staging);
	CHECK(memcmp(u, disk + 10 * 512, 100 * 512) == 0);
	CHECK(g_dev[M].st.reads == 1 && g_dev[M].rd_bytes == 100 * 512);
	free(u);
}

static void test_large_read_is_split(void)
{
	uint8_t *u = user_buf(1000);

	reset();
	CHECK(rd(u, 5, 1000) == 0);
	CHECK(io_calls == 2);                     /* 512 + 488 sectors */
	CHECK(memcmp(u, disk + 5 * 512, 1000 * 512) == 0);
	CHECK(g_dev[M].st.reads == 1 && g_dev[M].tm.rd_sectors == 1000);
	free(u);
}

static void test_read_error_finishes_stock(void)
{
	uint8_t *u = user_buf(1000);

	reset();
	fail_at_call = 2;                         /* the second staged chunk fails */
	CHECK(rd(u, 5, 1000) == 0);
	CHECK(io_calls == 3);
	CHECK(last_buf == u + 512 * 512);         /* rest done stock, at the right offset */
	CHECK(memcmp(u, disk + 5 * 512, 1000 * 512) == 0);
	CHECK(g_dev[M].fallback == 1 && g_dev[M].st.errors == 1);
	free(u);

	/* once fallen back, the next request is stock from the start */
	u = user_buf(100);
	io_calls = fail_at_call = 0;
	CHECK(rd(u, 0, 100) == 0);
	CHECK(io_calls == 1 && last_buf == u);
	free(u);
}

static void test_staged_write(void)
{
	uint8_t *u = user_buf(100);

	reset();
	for (int i = 0; i < 100 * 512; i++)
		u[i] = (uint8_t)(i * 7 + 3);
	CHECK(wr(u, 20, 100, 0) == 0);
	CHECK(last_buf == staging);
	CHECK(memcmp(disk + 20 * 512, u, 100 * 512) == 0);
	CHECK(g_dev[M].st.writes == 1 && g_dev[M].wr_bytes == 100 * 512);
	free(u);
}

static void test_write_error_finishes_stock(void)
{
	uint8_t *u = user_buf(1000);

	reset();
	for (int i = 0; i < 1000 * 512; i++)
		u[i] = (uint8_t)(i * 13 + 1);
	fail_at_call = 2;
	CHECK(wr(u, 40, 1000, 0) == 0);
	CHECK(memcmp(disk + 40 * 512, u, 1000 * 512) == 0);   /* nothing lost or torn */
	CHECK(g_dev[M].fallback == 1 && g_dev[M].st.errors == 1);
	free(u);
}

static void test_read_verify_mismatch(void)
{
	uint8_t *u = user_buf(100);

	reset();
	g_verify = 1;
	g_shadow = calloc(1, STAGE_BYTES);
	corrupt_staged = 1;                       /* the staged copy comes back wrong */
	CHECK(rd(u, 10, 100) == 0);
	CHECK(memcmp(u, disk + 10 * 512, 100 * 512) == 0);    /* caller still gets good data */
	CHECK(g_dev[M].st.verified == 1 && g_dev[M].st.mismatches == 1);
	CHECK(g_dev[M].fallback == 1);
	CHECK(g_shadow_busy == 0 && g_dev[M].busy == 0);
	free(g_shadow);
	g_shadow = NULL;
	free(u);
}

static void test_write_verify_mismatch(void)
{
	uint8_t *u = user_buf(100);

	reset();
	for (int i = 0; i < 100 * 512; i++)
		u[i] = (uint8_t)(i * 5 + 9);
	g_verify = 1;
	g_shadow = calloc(1, STAGE_BYTES);
	corrupt_staged = 1;                       /* the staged write lands corrupted */
	CHECK(wr(u, 20, 100, REF_RD) == 0);
	CHECK(memcmp(disk + 20 * 512, u, 100 * 512) == 0);    /* rewritten the stock way */
	CHECK(g_dev[M].st.mismatches == 1 && g_dev[M].fallback == 1);
	CHECK(g_shadow_busy == 0);
	free(g_shadow);
	g_shadow = NULL;
	free(u);
}

/* Regression: verify on, but no read hook yet -> the shadow buffer must not stay
 * locked, or verify would be dead until reboot. */
static void test_shadow_not_leaked_without_read_ref(void)
{
	uint8_t *u = user_buf(100);

	reset();
	g_verify = 1;
	g_shadow = calloc(1, STAGE_BYTES);
	CHECK(wr(u, 0, 100, 0) == 0);
	CHECK(g_shadow_busy == 0);
	free(g_shadow);
	g_shadow = NULL;
	free(u);
}

static void test_busy_and_disabled_go_stock(void)
{
	uint8_t *u = user_buf(100);

	reset();
	g_dev[M].busy = 1;                        /* another transfer holds the buffer */
	CHECK(rd(u, 0, 100) == 0);
	CHECK(last_buf == u && g_dev[M].st.passthru == 1 && g_dev[M].busy == 1);

	reset();
	g_dev[M].enabled = 0;
	CHECK(rd(u, 0, 100) == 0);
	CHECK(last_buf == u && g_dev[M].busy == 0);

	reset();
	g_dev[M].buf = NULL;                      /* no staging buffer: always stock */
	CHECK(rd(u, 0, 100) == 0);
	CHECK(last_buf == u);
	free(u);
}

/* Regression: a failed verify-buffer allocation used to be retried every tick. */
static void test_shadow_alloc_not_retried(void)
{
	VmSettings s;

	reset();
	memset(&s, 0, sizeof(s));
	s.verify = 1;
	fake_alloc_fail = 1;
	g_verify = 1;
	for (int i = 0; i < 5; i++)
		vm_stage_worker_tick();
	CHECK(fake_alloc_calls == 1);
	CHECK(g_shadow == NULL && g_shadow_uid < 0);

	/* turning verify off and on again is a fresh attempt */
	s.verify = 0;
	vm_stage_apply(&s);
	s.verify = 1;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(fake_alloc_calls == 2);

	fake_alloc_fail = 0;
	s.verify = 0;
	vm_stage_apply(&s);
	s.verify = 1;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(g_shadow != NULL && vm_stage_verify_ready());
	vm_stage_fini();
	CHECK(g_shadow == NULL);
}

static void test_get_reports_state(void)
{
	VmDevState out[VM_DEV_COUNT];
	uint8_t *u = user_buf(100);

	reset();
	CHECK(rd(u, 0, 100) == 0);
	vm_stage_get(out, 1);
	CHECK(out[M].status == VM_ST_ACTIVE && out[M].reads == 1);
	CHECK(out[M].time_ms == 0 && out[M].max_ms == 0);     /* no longer measured */
	vm_stage_get(out, 0);
	CHECK(out[M].status == VM_ST_UNAVAIL);
	free(u);
}

/* io and copy time are counted apart, per chunk that succeeded, and the
 * verify-mode re-read is in neither. */
static void test_timing_split(void)
{
	VmTimingDev t[VM_DEV_COUNT];
	uint8_t *u = user_buf(1100);

	reset();
	copy_cost_us = 300;
	CHECK(rd(u, 0, 1100) == 0);                    /* 512 + 512 + 76: 3 chunks */
	CHECK(wr(u, 0, 600, REF_RD) == 0);             /* 512 + 88: 2 chunks */
	vm_stage_timing(t);
	CHECK(t[M].rd_sectors == 1100 && t[M].rd_io_us == 3 * 2500 && t[M].rd_copy_us == 3 * 300);
	CHECK(t[M].wr_sectors == 600 && t[M].wr_io_us == 2 * 2500 && t[M].wr_copy_us == 2 * 300);
	CHECK(t[VM_DEV_SD2VITA].rd_sectors == 0 && t[VM_DEV_INTERNAL].wr_io_us == 0);

	/* small transfers stay stock and are not counted */
	CHECK(rd(u, 0, 8) == 0);
	vm_stage_timing(t);
	CHECK(t[M].rd_sectors == 1100);

	/* a failing chunk is not counted (the rest is finished stock) */
	reset();
	copy_cost_us = 300;
	fail_at_call = 2;
	CHECK(rd(u, 0, 1100) == 0);
	vm_stage_timing(t);
	CHECK(t[M].rd_sectors == 512 && t[M].rd_io_us == 2500 && t[M].rd_copy_us == 300);

	/* verify mode: the stock re-read is not in io time */
	reset();
	g_verify = 1;
	g_shadow = calloc(1, STAGE_BYTES);
	CHECK(rd(u, 0, 100) == 0);
	CHECK(io_calls == 2);
	vm_stage_timing(t);
	CHECK(t[M].rd_sectors == 100 && t[M].rd_io_us == 2500 && t[M].rd_copy_us == 0);
	free(g_shadow);
	g_shadow = NULL;

	/* wrap at 2^32: a difference of two readings stays right */
	reset();
	g_dev[M].tm.rd_io_us = 0xFFFFFF00u;
	CHECK(rd(u, 0, 100) == 0);
	vm_stage_timing(t);
	CHECK((uint32_t)(t[M].rd_io_us - 0xFFFFFF00u) == 2500);
	free(u);
}

static void count_free_bufs(int *n)
{
	*n = 0;
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (g_dev[i].buf)
			(*n)++;
}

/* Every device back to the boot state: nothing allocated, nothing switched on. */
static void boot_state(void)
{
	static const int port[VM_DEV_COUNT] = { 1, 0, -1 };

	reset();
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		memset(&g_dev[i], 0, sizeof(g_dev[i]));
		g_dev[i].port = port[i];
		g_dev[i].uid = -1;
	}
	for (int i = 0; i < H_COUNT; i++) {
		g_hook[i] = -1;
		g_ref[i] = 0;
	}
	g_live = 0;
}

static VmSettings devices_on(int sd2vita, int internal, int memcard)
{
	VmSettings s;

	memset(&s, 0, sizeof(s));
	s.sd2vita = sd2vita;
	s.internal = internal;
	s.memcard = memcard;
	return s;
}

static void test_init_all_hooks(void)
{
	VmSettings s = devices_on(1, 1, 1);
	int n;

	boot_state();
	vm_stage_apply(&s);                       /* at boot, before the hooks: */
	CHECK(fake_alloc_calls == 0);             /* nothing yet (could be safe mode) */
	CHECK(vm_stage_init() == VM_OK);
	count_free_bufs(&n);
	CHECK(n == 3 && fake_alloc_calls == 3 && fake_hook_calls == 6);
	vm_stage_fini();
	CHECK(fake_hook_releases == 6 && fake_free_calls == 3);
	count_free_bufs(&n);
	CHECK(n == 0);
	vm_stage_apply(&s);                       /* after fini: nothing allocated again */
	count_free_bufs(&n);
	CHECK(n == 0 && fake_alloc_calls == 3);
}

static void test_init_sdif_hook_fails_releases_everything(void)
{
	VmSettings s = devices_on(1, 1, 1);
	int n;

	boot_state();
	vm_stage_apply(&s);
	fake_hook_fail_at = 2;                    /* the second SDIF hook fails */
	CHECK(vm_stage_init() == VM_ERR_RESOLVE);
	count_free_bufs(&n);
	CHECK(n == 0 && fake_free_calls == 3);    /* all buffers given back */
	CHECK(fake_hook_releases == 1);           /* and the one hook that was live */
}

static void test_init_memcard_hook_fails_only_memcard_stock(void)
{
	VmSettings s = devices_on(1, 1, 1);
	VmDevState out[VM_DEV_COUNT];

	boot_state();
	vm_stage_apply(&s);
	fake_hook_fail_at = 6;                    /* the memory card's write hook fails */
	CHECK(vm_stage_init() == VM_OK);
	CHECK(g_dev[VM_DEV_MEMCARD].buf == NULL && fake_free_calls == 1);
	CHECK(g_dev[VM_DEV_SD2VITA].buf != NULL && g_dev[VM_DEV_INTERNAL].buf != NULL);
	CHECK(fake_hook_releases == 1);           /* the memory card's read hook */
	s.memcard = 0;                            /* switching it again allocates nothing */
	vm_stage_apply(&s);
	s.memcard = 1;
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_MEMCARD].buf == NULL && fake_alloc_calls == 3);
	vm_stage_get(out, 1);
	CHECK(out[VM_DEV_MEMCARD].status == VM_ST_NO_BUFFER);
	vm_stage_fini();
	CHECK(fake_hook_releases == 5);
}

/* A buffer only for the devices that are on; one switched on later gets its
 * buffer then and keeps it; a failed allocation is retried only when the
 * device is switched off and on again. */
static void test_buffers_on_demand(void)
{
	VmSettings s = devices_on(1, 0, 0);
	VmDevState out[VM_DEV_COUNT];
	int n;

	boot_state();
	vm_stage_apply(&s);
	CHECK(vm_stage_init() == VM_OK);
	count_free_bufs(&n);
	CHECK(n == 1 && g_dev[VM_DEV_SD2VITA].buf && fake_alloc_calls == 1);

	s.internal = 1;                           /* switched on in Settings */
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_INTERNAL].buf != NULL && fake_alloc_calls == 2);
	s.internal = 0;                           /* off again: the buffer is kept */
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_INTERNAL].buf != NULL && fake_free_calls == 0);
	s.internal = 1;
	vm_stage_apply(&s);
	CHECK(fake_alloc_calls == 2);

	fake_alloc_fail = 1;                      /* the memory card: no memory left */
	s.memcard = 1;
	vm_stage_apply(&s);
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_MEMCARD].buf == NULL && fake_alloc_calls == 3);   /* tried once */
	vm_stage_get(out, 1);
	CHECK(out[VM_DEV_MEMCARD].status == VM_ST_NO_BUFFER);
	vm_stage_worker_tick();
	CHECK(g_dev[VM_DEV_MEMCARD].alloc_logged == 1);
	fake_alloc_fail = 0;
	s.memcard = 0;
	vm_stage_apply(&s);
	s.memcard = 1;
	vm_stage_apply(&s);                       /* off and on: a fresh try */
	CHECK(g_dev[VM_DEV_MEMCARD].buf != NULL && fake_alloc_calls == 4);
	vm_stage_get(out, 1);
	CHECK(out[VM_DEV_MEMCARD].status == VM_ST_ACTIVE);
	vm_stage_fini();
	CHECK(fake_free_calls == 3);
}

/* ---- overlapped reads ------------------------------------------------------ */

static void pipe_on(VmSettings *s, int gpu)
{
	memset(s, 0, sizeof(*s));
	s->memcard = 1;
	s->stats = 1;
	s->gpusafe = gpu;
	s->pipeline = 1;
	vm_stage_apply(s);
	g_live = 1;
	vm_stage_worker_tick();               /* the helper is made by the worker */
}

/* A large read is split: the head into the staging buffer by the caller, the
 * tail into the second buffer by the helper; the data is exactly the disk's,
 * and nothing is split below 64 KiB. */
static void test_pipeline_reads(void)
{
	uint8_t *u = user_buf(1100);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	reset();
	pipe_on(&s, 0);
	CHECK(d->pipe_made && d->pipe_ready && fake_threads_live == 2 && fake_semas_live == 5);
	CHECK(fake_thread_last_mask == PIPE_CPU_SYSTEM);   /* on the system core */
	g_pipeline = VM_PIPE_FIXED;                     /* its 2 parts per chunk */
	CHECK(rd(u, 10, 1100) == 0);                    /* 512 + 512 + 76 */
	CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
	CHECK(io_calls == 5);                           /* split, split, not split */
	CHECK(io_bufs[0] == staging && io_bufs[1] == d->buf2);
	CHECK(io_ns[0] + io_ns[1] == 512 && io_ns[1] >= PIPE_MIN_TAIL && io_ns[1] <= 256);
	CHECK(io_bufs[2] == staging && io_bufs[3] == d->buf2 && io_ns[2] + io_ns[3] == 512);
	CHECK(io_bufs[4] == staging && io_ns[4] == 76);
	CHECK(g_pipe_reads == 2 && d->st.reads == 1 && d->st.errors == 0);
	CHECK(d->tm.rd_sectors == 1100);                /* all counted */
	g_pipeline = VM_PIPE_AUTO;

	/* many reads in a row, different sizes and places: always the disk's data */
	for (int k = 0; k < 200; k++) {
		uint32_t n = 128 + (uint32_t)(k * 37) % 900, at = (uint32_t)(k * 101) % 2000;

		memset(u, 0, n * 512);
		CHECK(rd(u, at, n) == 0);
		CHECK(memcmp(u, disk + (size_t)at * 512, (size_t)n * 512) == 0);
	}
	CHECK(d->st.errors == 0 && !d->fallback);

	/* Fixed: 64 KiB and up are split, smaller is not */
	g_pipeline = VM_PIPE_FIXED;
	io_calls = 0;
	CHECK(rd(u, 0, 127) == 0 && io_calls == 1);
	io_calls = 0;
	CHECK(rd(u, 0, 128) == 0 && io_calls == 2);
	/* Auto: only when the plan beats one piece. With the built-in costs a
	 * plain 64 KiB read stays whole (an extra command costs more than the
	 * short copy it hides). With GPU-safe and a card command of 0.42 ms (as
	 * measured on the SD2Vita) 32 KiB is split, 16 KiB not; at 0.6 ms not. */
	g_pipeline = VM_PIPE_AUTO;
	d->est_io_ns = 25000; d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100; d->est_cmd_ns = 600000;
	io_calls = 0;
	CHECK(rd(u, 0, 128) == 0 && io_calls == 1 && memcmp(u, disk, 128 * 512) == 0);
	g_gpu_safe = 1;
	g_dcache_clean = fake_clean;
	d->est_io_ns = 25000; d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100; d->est_cmd_ns = 420000;
	io_calls = 0;
	CHECK(rd(u, 0, 64) == 0 && io_calls == 2 && memcmp(u, disk, 64 * 512) == 0);
	d->est_io_ns = 25000; d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100; d->est_cmd_ns = 420000;
	io_calls = 0;
	CHECK(rd(u, 0, 32) == 0 && io_calls == 1 && memcmp(u, disk, 32 * 512) == 0);
	d->est_io_ns = 25000; d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100; d->est_cmd_ns = 600000;
	io_calls = 0;
	CHECK(rd(u, 0, 64) == 0 && io_calls == 1 && memcmp(u, disk, 64 * 512) == 0);
	g_gpu_safe = 0;
	g_dcache_clean = NULL;

	/* the plan: with the console's costs, 2 parts without GPU-safe and 3 with */
	{
		uint32_t e = 0, sz[PIPE_MAX_PARTS];

		d->est_io_ns = 25000; d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100; d->est_cmd_ns = 600000;
		CHECK(pipe_plan(d, 512, 0, sz) == 2);
		CHECK(sz[0] + sz[1] == 512 && sz[1] >= 50 && sz[1] <= 56);        /* ~54 */
		d->est_cp_ns[1] = 11100;
		CHECK(pipe_plan(d, 512, 1, sz) == 3);
		CHECK(sz[0] + sz[1] + sz[2] == 512 && sz[2] >= 30 && sz[2] <= 40); /* ~35 */
		CHECK(sz[1] <= PIPE_TAIL_SECTORS);
		d->est_cmd_ns = 5000000;                       /* commands dear: fewer parts */
		CHECK(pipe_plan(d, 512, 1, sz) == 2);
		d->est_cmd_ns = 600000;
		d->est_cp_ns[0] = 40000;                       /* a copy slower than the card */
		CHECK(pipe_plan(d, 512, 0, sz) == 2 && sz[0] == 256 && sz[1] == 256);
		d->est_io_ns = d->est_cp_ns[0] = d->est_cp_ns[1] = d->est_cmd_ns = 0;   /* nothing learnt: defaults */
		CHECK(pipe_plan(d, 512, 0, sz) == 2 && pipe_plan(d, 512, 1, sz) == 3);
		CHECK(pipe_plan(d, 256, 0, sz) >= 2 && pipe_plan(d, 256, 1, sz) >= 2);
		CHECK(pipe_plan(d, 64, 1, sz) == 0 && pipe_plan(d, 128, 0, sz) == 0);
		d->est_cmd_ns = 420000;
		CHECK(pipe_plan(d, 64, 1, sz) == 2 && sz[0] + sz[1] == 64);
		d->est_cmd_ns = 0;
		CHECK(pipe_plan(d, 32, 1, sz) == 0 && pipe_plan(d, 64, 0, sz) == 0);
		g_pipeline = VM_PIPE_FIXED;                    /* Fixed: always its parts */
		CHECK(pipe_plan(d, 128, 0, sz) == 2);
		g_pipeline = VM_PIPE_AUTO;
		/* the two copy costs are kept apart: switching GPU-safe needs no relearning */
		d->est_io_ns = 25000; d->est_cmd_ns = 600000;
		d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100;
		pipe_learn(&d->est_cp_ns[1], 1110, 100, PIPE_DEF_CPCL_NS);   /* a clean read learnt */
		CHECK(d->est_cp_ns[0] == 4200);                              /* the plain one untouched */
		CHECK(pipe_plan(d, 512, 0, sz) == 2 && pipe_plan(d, 512, 1, sz) == 3);
		d->est_io_ns = d->est_cp_ns[0] = d->est_cp_ns[1] = d->est_cmd_ns = 0;

		/* any costs and sizes: the parts cover n, fit their buffers, every
		 * part's copy is hidden by the next read, and no x or k is cheaper */
		int planned = 0, three = 0;

		for (uint32_t r = 1; r < 3000; r++) {
			uint32_t n = 128 + (r * 7919u) % 385, k;
			uint64_t io = 5000 + (r * 104729u) % 40000, cp = 1000 + (r * 1299709u) % 30000;
			uint64_t cmd = (r * 15485863u) % 2000000, best = ~(uint64_t)0, sum = 0, cost;

			d->est_io_ns = (uint32_t)io; d->est_cp_ns[0] = (uint32_t)cp; d->est_cmd_ns = (uint32_t)(cmd ? cmd : 1);
			cmd = d->est_cmd_ns;
			k = (uint32_t)pipe_plan(d, n, 0, sz);
			CHECK(k == 0 || k == 2 || k == 3);
			if (!k)
				continue;
			for (uint32_t i = 0; i < k; i++) {
				sum += sz[i];
				CHECK(sz[i] >= PIPE_MIN_TAIL && sz[i] <= pipe_cap((int)i));
			}
			CHECK(sum == n);
			if (cp >= io)                               /* the halves fallback */
				continue;
			for (uint32_t i = 0; i + 1 < k; i++)
				CHECK(sz[i] * cp <= cmd + sz[i + 1] * io);
			cost = k * cmd + sz[k - 1] * cp;
			planned++;
			three += k == 3;
			for (uint32_t kk = 2; kk <= PIPE_MAX_PARTS; kk++) {
				uint32_t t[PIPE_MAX_PARTS];

				for (uint32_t x = PIPE_MIN_TAIL; x <= n / kk; x++)
					if (pipe_fit(n, (int)kk, x, io, cp, recip(cp), cmd, t) && kk * cmd + x * cp < best)
						best = kk * cmd + x * cp;
			}
			CHECK(cost == best);
		}
		CHECK(planned > 1000 && three > 100 && planned - three > 100);   /* both kinds seen */
		d->est_io_ns = 25000; d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100; d->est_cmd_ns = 600000;
		/* learning both from one split read: 25 us per sector, 600 us per command */
		d->est_io_ns = 25000; d->est_cmd_ns = 600000;
		for (int k = 0; k < 60; k++)
			pipe_learn_io(d, 600 + 450 * 25, 450, 600 + 62 * 25, 62);
		CHECK(d->est_io_ns > 24900 && d->est_io_ns < 25100);
		CHECK(d->est_cmd_ns > 590000 && d->est_cmd_ns < 610000);
		d->est_io_ns = 0; d->est_cmd_ns = 0;
		for (int k = 0; k < 60; k++)                               /* from nothing */
			pipe_learn_io(d, 600 + 450 * 25, 450, 600 + 62 * 25, 62);
		CHECK(d->est_io_ns > 24500 && d->est_io_ns < 25500 && d->est_cmd_ns > 550000);
		{
			uint32_t io = d->est_io_ns, cmd = d->est_cmd_ns;

			pipe_learn_io(d, 100, 450, 5000, 62);          /* head faster than tail: noise */
			pipe_learn_io(d, 9000, 70, 100, 62);           /* too close to tell */
			CHECK(d->est_io_ns == io && d->est_cmd_ns == cmd);
		}
		d->est_io_ns = d->est_cp_ns[0] = d->est_cp_ns[1] = d->est_cmd_ns = 0;
		pipe_learn(&e, 0, 0, 5000);
		CHECK(e == 0);                                             /* nothing to learn from */
		pipe_learn(&e, 8, 1, 5000);                                /* 8000 ns: 5000 + 3000/8 */
		CHECK(e == 5000 - 625 + 1000);
		for (int k = 0; k < 100; k++)
			pipe_learn(&e, 8, 1, 5000);
		CHECK(e > 7900 && e <= 8000);                              /* converges */
	}

	/* off again: not split at once; the next worker tick gives the helper,
	 * its semaphores and its buffer back */
	s.pipeline = 0;
	vm_stage_apply(&s);
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 1 && last_buf == staging);
	CHECK(d->pipe_made && fake_threads_live == 2);
	{
		int frees = fake_free_calls;

		vm_stage_worker_tick();
		CHECK(!d->pipe_made && !d->pipe_ready && fake_threads_live == 0 && fake_semas_live == 0);
		CHECK(fake_free_calls == frees + 1 && d->buf2 == NULL && d->uid2 < 0);
	}
	CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0);   /* reads as before */
	/* on again: made again at the next tick */
	s.pipeline = VM_PIPE_AUTO;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(d->pipe_made && fake_threads_live == 2);
	/* the device switched off: released too, and the staging buffer kept */
	s.memcard = 0;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(!d->pipe_made && fake_threads_live == 0 && d->buf == staging);
	s.memcard = 1;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(d->pipe_made && fake_threads_live == 2);
	vm_stage_fini();
	CHECK(!d->pipe_made && fake_threads_live == 0 && fake_semas_live == 0);
	g_live = saved_live;
	free(u);
}

/* GPU-safe with overlapped reads: both parts cleaned, after their copy. */
static void test_pipeline_gpu_safe(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	int saved_live = g_live;

	reset();
	pipe_on(&s, 1);
	g_dcache_clean = fake_clean;
	clean_calls = 0; clean_lo = clean_hi = 0; clean_copy_seen = 0; clean_user = NULL;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 3);              /* three parts */
	CHECK(io_bufs[0] == staging && io_bufs[1] == g_dev[M].buf2 && io_bufs[2] == staging);
	CHECK(io_ns[0] + io_ns[1] + io_ns[2] == 512 && io_ns[2] < io_ns[1]);
	CHECK(memcmp(u, disk, 512 * 512) == 0);
	CHECK(clean_calls == 3);
	CHECK(clean_lo <= (uintptr_t)u && clean_hi >= (uintptr_t)u + 512 * 512);
	CHECK(g_pipe_reads == 1 && g_pipe_reads3 == 1);

	/* many three-part reads: always the disk's data */
	{
		uint8_t *v = user_buf(1100);

		g_stats = 0;                            /* the tester's figures off ... */
		for (int k = 0; k < 150; k++) {
			uint32_t n = 128 + (uint32_t)(k * 53) % 900, at = (uint32_t)(k * 97) % 2000;
			Dev *dm = &g_dev[M];

			/* ... the plan still learns, from the fake clock: put the
			 * console's costs back before each read */
			dm->est_io_ns = 25000; dm->est_cmd_ns = 600000;
			dm->est_cp_ns[0] = 4200; dm->est_cp_ns[1] = 11100;

			memset(v, 0, n * 512);
			CHECK(rd(v, at, n) == 0);
			CHECK(memcmp(v, disk + (size_t)at * 512, (size_t)n * 512) == 0);
		}
		CHECK(g_pipe_reads3 > 100);
		CHECK(g_dev[M].st.errors == 0);
		CHECK(g_dev[M].tm.rd_sectors == 512);   /* only the first read, with Statistics on */
		free(v);
	}
	vm_stage_fini();
	g_live = saved_live;
	free(u);
}

/* An error in either part: the device goes back to stock and the whole part
 * is read again the standard way, so the caller still gets the disk's data. */
static void test_pipeline_errors(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	for (int at = 1; at <= 5; at++) {          /* 2 parts: 1, 2; 3 parts: 3..5 */
		reset();
		pipe_on(&s, at > 2);
		if (at > 2) {
			g_dcache_clean = fake_clean;
			fail_at_call = at - 2;
		}
		if (at <= 2)
			fail_at_call = at;
		memset(u, 0, 512 * 512);
		CHECK(rd(u, 0, 512) == 0);
		CHECK(memcmp(u, disk, 512 * 512) == 0);
		CHECK(d->st.errors == 1 && d->fallback);
		/* finished stock from the failed part on; the parts before it,
		 * already copied, stay (the first part's error: all of it) */
		{
			uint32_t before = 0;

			for (int i = 0; i + 1 < fail_at_call; i++)
				before += io_ns[i];
			CHECK(last_buf == u + (size_t)before * 512);
			CHECK(io_calls == fail_at_call + 1);
			CHECK(fake_sema_count(d->sem_free) == 0 && fake_sema_count(d->sem_done) == 0);
		}
		CHECK(g_pipe_reads == 0);
		vm_stage_fini();
		CHECK(fake_threads_live == 0);
	}
	g_live = saved_live;
	free(u);
}

/* The system core refused: the helper runs on any core instead. */
static void test_pipeline_any_core(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	reset();
	fake_thread_fail_masked = 1;
	pipe_on(&s, 0);
	CHECK(d->pipe_made && fake_thread_last_mask == 0 && fake_threads_live == 2);
	CHECK(rd(u, 0, 512) == 0 && io_calls == 2 && memcmp(u, disk, 512 * 512) == 0);
	vm_stage_fini();
	g_live = saved_live;
	free(u);
}

/* Verify mode keeps the plain path (one read, one re-read per chunk). */
static void test_pipeline_not_with_verify(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	int saved_live = g_live;

	reset();
	pipe_on(&s, 0);
	g_shadow = malloc(STAGE_BYTES);
	g_verify = 1;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 2 && io_bufs[1] == g_shadow);
	CHECK(g_pipe_reads == 0);
	g_verify = 0;
	free(g_shadow);
	g_shadow = NULL;
	vm_stage_fini();
	g_live = saved_live;
	free(u);
}

/* Setting it up can fail at every step: nothing is kept, reads go as before,
 * and it is not tried again every second. */
static void test_pipeline_setup_fails(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	for (int step = 0; step < 8; step++) {
		reset();
		if (step == 0) fake_alloc_fail = 1;
		if (step >= 1 && step <= 5) fake_sema_fail = step;   /* each of the five */
		if (step == 6) fake_thread_fail = 1;                 /* the helper */
		if (step == 7) fake_thread_fail_from = 2;            /* the copier (masked, any) */
		pipe_on(&s, 0);
		fake_alloc_fail = 0;
		CHECK(!d->pipe_made && !d->pipe_ready && d->pipe_failed);
		CHECK(fake_threads_live == 0 && fake_semas_live == 0);
		CHECK(d->uid2 < 0 && d->th < 0 && d->sem_go < 0 && d->sem_done < 0);
		CHECK(d->th2 < 0 && d->sem_free < 0 && d->sem_cgo < 0 && d->sem_fin < 0);
		vm_stage_worker_tick();
		CHECK(!d->pipe_made);                    /* not retried */
		io_calls = 0;
		CHECK(rd(u, 0, 512) == 0 && io_calls == 1 && memcmp(u, disk, 512 * 512) == 0);
		/* switched off and on again: a fresh try, which now works */
		fake_thread_fail = fake_sema_fail = fake_thread_fail_from = 0;
		s.pipeline = 0;
		vm_stage_apply(&s);
		s.pipeline = 1;
		vm_stage_apply(&s);
		vm_stage_worker_tick();
		CHECK(d->pipe_made && d->pipe_ready && !d->pipe_failed);
		io_calls = 0;
		CHECK(rd(u, 0, 512) == 0 && io_calls == 2 && memcmp(u, disk, 512 * 512) == 0);
		vm_stage_fini();
		CHECK(fake_threads_live == 0 && fake_semas_live == 0);
	}
	g_live = saved_live;
	free(u);
}

/* Every setting takes effect on the next transfer, in both directions, with
 * no restart: the devices, small transfers, statistics, GPU-safe reads,
 * verify (its buffer within a worker tick) and overlapped reads (its helper
 * within a worker tick). */
static void test_every_switch_on_the_fly(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	reset();
	g_live = 1;
	memset(&s, 0, sizeof(s));
	vm_stage_apply(&s);
	CHECK(rd(u, 0, 512) == 0 && last_buf == u);                     /* device off */
	s.memcard = 1; vm_stage_apply(&s);
	CHECK(rd(u, 0, 512) == 0 && last_buf == staging);               /* on */
	s.memcard = 0; vm_stage_apply(&s);
	CHECK(rd(u, 0, 512) == 0 && last_buf == u);                     /* off again */
	s.memcard = 1; vm_stage_apply(&s);

	CHECK(rd(u, 0, 8) == 0 && last_buf == u);                       /* small: stock */
	s.small = 1; vm_stage_apply(&s);
	CHECK(rd(u, 0, 8) == 0 && last_buf == staging);
	s.small = 0; vm_stage_apply(&s);
	CHECK(rd(u, 0, 8) == 0 && last_buf == u);

	fake_time_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && fake_time_calls == 0);              /* stats off */
	s.stats = 1; vm_stage_apply(&s);
	CHECK(rd(u, 0, 512) == 0 && fake_time_calls > 0);
	s.stats = 0; vm_stage_apply(&s);
	fake_time_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && fake_time_calls == 0);

	g_dcache_clean = fake_clean;
	clean_calls = 0; clean_user = NULL;
	CHECK(rd(u, 0, 512) == 0 && clean_calls == 0);                  /* GPU-safe off */
	s.gpusafe = 1; vm_stage_apply(&s);
	CHECK(rd(u, 0, 512) == 0 && clean_calls == 1);
	s.gpusafe = 0; vm_stage_apply(&s);
	CHECK(rd(u, 0, 512) == 0 && clean_calls == 1);

	s.verify = 1; vm_stage_apply(&s);
	vm_stage_worker_tick();                                         /* its buffer */
	io_calls = 0;
	CHECK(g_shadow && rd(u, 0, 512) == 0 && io_calls == 2 && d->st.verified == 1);
	s.verify = 0; vm_stage_apply(&s);
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 1);

	s.pipeline = 1; vm_stage_apply(&s);
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 1);                     /* no helper yet */
	vm_stage_worker_tick();
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 2);                     /* split */
	s.pipeline = 0; vm_stage_apply(&s);
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 1);
	CHECK(memcmp(u, disk, 512 * 512) == 0 && d->st.errors == 0);

	vm_stage_fini();
	CHECK(fake_threads_live == 0 && g_shadow == NULL);
	g_live = saved_live;
	free(u);
}

/* Overlapped reads learn their costs with Statistics off too (they read the
 * clock themselves); Statistics only adds the tester's figures. Before
 * anything is learnt the defaults give 2 parts without GPU-safe, 3 with. */
static void test_pipeline_learns_without_statistics(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	Dev *d = &g_dev[M];
	uint32_t sz[PIPE_MAX_PARTS];
	int saved_live = g_live;

	reset();
	pipe_on(&s, 0);
	s.stats = 0;
	vm_stage_apply(&s);
	CHECK(d->est_cp_ns[0] == 0 && d->est_cp_ns[1] == 0 && d->est_cmd_ns == 0);
	CHECK(pipe_plan(d, 512, 0, sz) == 2 && pipe_plan(d, 512, 1, sz) == 3);
	copy_cost_us = 300;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 2 && memcmp(u, disk, 512 * 512) == 0);
	CHECK(d->est_cp_ns[0] != 0 && d->est_cp_ns[1] == 0);   /* learnt, the plain one only */
	CHECK(d->tm.rd_sectors == 0 && d->rd_bytes == 0);        /* no tester figures */
	copy_cost_us = 0;
	vm_stage_fini();
	g_live = saved_live;
	free(u);
}

/* The three choices: off, auto (learns), fixed (built-in sizes: 2 parts, 3
 * with GPU-safe; learns nothing and, without Statistics, reads no clock).
 * 1, which was "on", is auto; anything past fixed is refused. */
static void test_pipeline_modes(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	Dev *d = &g_dev[M];
	uint32_t sz[PIPE_MAX_PARTS], want[PIPE_MAX_PARTS];
	int saved_live = g_live;

	CHECK(vm_setting_valid(VM_KEY_PIPELINE, VM_PIPE_OFF) && vm_setting_valid(VM_KEY_PIPELINE, VM_PIPE_AUTO));
	CHECK(vm_setting_valid(VM_KEY_PIPELINE, VM_PIPE_FIXED) && !vm_setting_valid(VM_KEY_PIPELINE, 3));
	CHECK(!vm_setting_valid(VM_KEY_PIPELINE, -1) && VM_PIPE_AUTO == 1);

	reset();
	pipe_on(&s, 0);                               /* pipeline = 1: auto */
	CHECK(g_pipeline == VM_PIPE_AUTO);
	s.pipeline = VM_PIPE_FIXED;
	s.stats = 0;
	vm_stage_apply(&s);
	CHECK(g_pipeline == VM_PIPE_FIXED && d->pipe_made);   /* the same helper */

	/* fixed ignores whatever was learnt: built-in sizes */
	d->est_io_ns = 9000; d->est_cp_ns[0] = 9000; d->est_cp_ns[1] = 30000; d->est_cmd_ns = 50000;
	CHECK(pipe_plan(d, 512, 0, sz) == 2);
	CHECK(pipe_plan_costs(512, PIPE_DEF_IO_NS, PIPE_DEF_CP_NS, PIPE_DEF_CMD_NS, 2, 2, want) == 2);
	CHECK(sz[0] == want[0] && sz[1] == want[1]);
	CHECK(pipe_plan(d, 512, 1, sz) == 3);
	CHECK(pipe_plan_costs(512, PIPE_DEF_IO_NS, PIPE_DEF_CPCL_NS, PIPE_DEF_CMD_NS, 3, 3, want) == 3);
	CHECK(sz[0] == want[0] && sz[1] == want[1] && sz[2] == want[2]);
	for (uint32_t n = 128; n <= 512; n++) {      /* 3 where they fit, else 2 */
		int k = pipe_plan(d, n, 1, sz);

		CHECK(k == 2 || k == 3);
		CHECK(k == 3 || !pipe_plan_costs(n, PIPE_DEF_IO_NS, PIPE_DEF_CPCL_NS, PIPE_DEF_CMD_NS, 3, 3, want));
	}

	/* fixed learns nothing and, with Statistics off, reads no clock at all */
	d->est_io_ns = d->est_cp_ns[0] = d->est_cp_ns[1] = d->est_cmd_ns = 0;
	copy_cost_us = 300;
	fake_time_calls = 0;
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 2 && memcmp(u, disk, 512 * 512) == 0);
	CHECK(fake_time_calls == 0);
	CHECK(d->est_cp_ns[0] == 0 && d->est_io_ns == 0 && d->est_cmd_ns == 0);
	copy_cost_us = 0;

	/* off: not split */
	s.pipeline = VM_PIPE_OFF;
	vm_stage_apply(&s);
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 1);
	vm_stage_fini();
	g_live = saved_live;
	free(u);
}

/* The helper given back and made again while another thread keeps reading:
 * every read still gets the disk's data, nothing is used after it is freed
 * (AddressSanitizer), and the hand-over is race-free (ThreadSanitizer). */
static volatile int g_reader_stop, g_reader_bad, g_reader_reads;

static void *reader_main(void *arg)
{
	uint8_t *u = arg;

	for (int k = 0; !__atomic_load_n(&g_reader_stop, __ATOMIC_ACQUIRE); k++) {
		uint32_t n = 128 + (uint32_t)(k * 61) % 380, at = (uint32_t)(k * 89) % 2000;

		if (rd(u, at, n) != 0 || memcmp(u, disk + (size_t)at * 512, (size_t)n * 512) != 0)
			__sync_fetch_and_add(&g_reader_bad, 1);
		__atomic_fetch_add(&g_reader_reads, 1, __ATOMIC_RELEASE);
	}
	return NULL;
}

static void test_pipeline_released_while_reading(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	pthread_t th;
	int saved_live = g_live;

	reset();
	pipe_on(&s, 0);
	g_stats = 0;
	g_reader_stop = g_reader_bad = g_reader_reads = 0;
	CHECK(pthread_create(&th, NULL, reader_main, u) == 0);
	for (int i = 0; i < 300; i++) {
		s.pipeline = (i & 1) ? VM_PIPE_AUTO : VM_PIPE_OFF;
		vm_stage_apply(&s);
		vm_stage_worker_tick();               /* releases or makes the helper */
		while (__atomic_load_n(&g_reader_reads, __ATOMIC_ACQUIRE) < i)
			sched_yield();
	}
	__atomic_store_n(&g_reader_stop, 1, __ATOMIC_RELEASE);
	pthread_join(th, NULL);
	CHECK(g_reader_bad == 0 && g_reader_reads >= 300);
	CHECK(g_dev[M].st.errors == 0);
	vm_stage_fini();
	CHECK(fake_threads_live == 0 && fake_semas_live == 0);
	g_live = saved_live;
	free(u);
}

/* An error or a verify mismatch wakes the worker (to log it at once); a
 * transfer that went well does not. */
static int g_problem_calls;
static void count_problem(void) { g_problem_calls++; }

static void test_problems_wake_the_worker(void)
{
	uint8_t *u = user_buf(100);

	reset();
	g_problem_calls = 0;
	vm_stage_on_problem(count_problem);
	CHECK(rd(u, 0, 100) == 0 && wr(u, 0, 100, REF_RD) == 0 && g_problem_calls == 0);
	fail_at_call = io_calls + 1;                     /* the next read fails */
	CHECK(rd(u, 0, 100) == 0 && g_problem_calls == 1 && g_dev[M].fallback);
	reset();
	g_problem_calls = 0;
	vm_stage_on_problem(count_problem);
	g_shadow = malloc(STAGE_BYTES);
	g_verify = 1;
	corrupt_staged = 1;                              /* a mismatch */
	CHECK(rd(u, 0, 100) == 0 && g_dev[M].st.mismatches == 1 && g_problem_calls == 1);
	corrupt_staged = 0;
	g_verify = 0;
	free(g_shadow);
	g_shadow = NULL;
	vm_stage_on_problem(NULL);                       /* none: nothing called */
	fail_at_call = io_calls + 1;
	g_dev[M].fallback = 0;
	CHECK(rd(u, 0, 100) == 0 && g_problem_calls == 1);
	free(u);
}
/* The helper reads the parts back to back on its own: one job (one go) and
 * the buffer of part 2 handed back, it reads parts 1 and 2 in order, into the
 * right buffers, without the caller asking for each. Before, every part was a
 * separate job posted after the caller woke up. A late hand-back (the
 * caller's thread slow to copy) is counted for the log; a prompt one is not. */
static void test_helper_runs_ahead(void)
{
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;
	static const uint32_t sz[3] = { 300, 180, 32 };

	for (int late = 0; late <= 1; late++) {
		reset();
		pipe_on(&s, 1);
		d->job_io = fake_io;
		d->job_ref = REF_RD;
		d->job_part = NULL;
		d->job_k = 3;
		d->job_first = 1;                       /* part 0: the caller's (pipe_run) */
		memcpy(d->job_sz, sz, sizeof(sz));
		d->job_sector = 100 + sz[0];
		d->job_fail = 0;
		__sync_synchronize();
		if (!late)
			CHECK(ksceKernelSignalSema(d->sem_free, 1) == 0);   /* part 0 copied */
		CHECK(ksceKernelSignalSema(d->sem_go, 1) == 0);
		CHECK(ksceKernelWaitSema(d->sem_done, 1, NULL) == 0);
		if (late) {
			/* the copy of part 0 is late by 1 ms of the (fake) clock; the
			 * helper is waiting for it by then (50 ms of real time) */
			struct timespec ts = { 0, 50 * 1000 * 1000 };

			nanosleep(&ts, NULL);
			__sync_fetch_and_add(&fake_now_us, 1000);
			CHECK(ksceKernelSignalSema(d->sem_free, 1) == 0);
		}
		CHECK(ksceKernelWaitSema(d->sem_done, 1, NULL) == 0);
		CHECK(io_calls == 2 && d->job_fail == 0);
		CHECK(io_ns[0] == sz[1] && io_bufs[0] == d->buf2);
		CHECK(io_ns[1] == sz[2] && io_bufs[1] == staging);
		CHECK(d->job_big_n == sz[1] && d->job_io_us == 2 * 2500 && d->job_last_us == 2500);
		CHECK(fake_sema_count(d->sem_free) == 0 && fake_sema_count(d->sem_done) == 0);
		if (late)
			CHECK(g_pipe_waits == 1 && g_pipe_wait_us >= 1000);
		else
			CHECK(g_pipe_waits == 0 && g_pipe_wait_us == 0);
		vm_stage_fini();
	}
	g_live = saved_live;
}

/* Auto's quick "one piece wins" check never turns down a split the full plan
 * would make: for every size and a range of costs, the decision is the same
 * as working the plan out. */
static void test_plan_quick_check(void)
{
	static const uint32_t costs[][3] = {   /* io, cp, cmd (ns) */
		{ 25000, 4200, 600000 }, { 25000, 11100, 600000 }, { 27000, 11100, 420000 },
		{ 23000, 4200, 420000 }, { 23000, 4200, 150000 }, { 15000, 11100, 300000 },
		{ 30000, 20000, 900000 }, { 25000, 4200, 0 },
	};
	Dev *d = &g_dev[M];
	uint32_t quick = 0, split = 0;

	reset();
	g_pipeline = VM_PIPE_AUTO;
	for (size_t c = 0; c < sizeof(costs) / sizeof(costs[0]); c++)
		for (int clean = 0; clean <= 1; clean++)
			for (uint32_t n = PIPE_MIN_AUTO; n <= STAGE_SECTORS; n++) {
				const uint64_t io = costs[c][0], cp = costs[c][1], cmd = costs[c][2];
				uint32_t sz[PIPE_MAX_PARTS], full[PIPE_MAX_PARTS];
				int k, kf;

				d->est_io_ns = costs[c][0];
				d->est_cp_ns[clean] = costs[c][1];
				d->est_cmd_ns = costs[c][2] ? costs[c][2] : 1;   /* 0: the default */
				k = pipe_plan(d, n, clean, sz);
				kf = pipe_plan_costs(n, io, cp, d->est_cmd_ns, 2, PIPE_MAX_PARTS, full);
				if (kf >= 2 && (uint64_t)kf * d->est_cmd_ns + (uint64_t)full[kf - 1] * cp +
				               PIPE_HANDOFF_NS >= d->est_cmd_ns + (uint64_t)n * cp)
					kf = 0;
				CHECK(k == kf);
				if ((uint64_t)n * cp <= d->est_cmd_ns + (uint64_t)PIPE_MIN_TAIL * cp +
				                        PIPE_HANDOFF_NS)
					quick++;
				if (k >= 2)
					split++;
				(void)cmd;
			}
	CHECK(quick > 500 && split > 5000);        /* both cases well covered */
}

/* Where the copy ran (log on only): the KiB per core of the thread that
 * copied, and per kind of caller (a system thread or a game). Nothing with
 * the log off. */
static void test_copy_diagnostics(void)
{
	uint8_t *u = user_buf(128);

	reset();
	CHECK(rd(u, 0, 128) == 0);
	CHECK(g_cp_core_kib[0] == 0 && g_cp_sys_kib == 0 && g_cp_game_kib == 0);   /* log off */
	g_log_diag = 1;
	CHECK(rd(u, 0, 128) == 0);                                 /* a system caller */
	CHECK(g_cp_core_kib[0] == 64 && g_cp_sys_kib == 64 && g_cp_game_kib == 0);
	fake_pid = 0x42;                                           /* a game */
	CHECK(rd(u, 0, 128) == 0 && memcmp(u, disk, 128 * 512) == 0);
	CHECK(g_cp_game_kib == 64 && g_cp_core_kib[0] == 128);
	CHECK(fake_usermap_calls == 0);                            /* nothing mapped */
	free(u);
}

/* Copy on the system core: a game's read is copied by the copier thread
 * through a kernel mapping of the game's buffer; the game's thread copies
 * nothing. As the setting says (with GPU-safe only, or always); never for a
 * system thread. The mapping is checked once for write access and every
 * time for its memory type; refused, failed or not, the caller still gets
 * the disk's data. */
static void offload_on(VmSettings *s, int gpu, int mode, int pipeline)
{
	memset(s, 0, sizeof(*s));
	s->memcard = 1;
	s->stats = 1;
	s->gpusafe = gpu;
	s->pipeline = pipeline;
	s->offload = mode;
	vm_stage_apply(s);
	g_live = 1;
	vm_stage_worker_tick();               /* helper and copier made by the worker */
	if (gpu)
		g_dcache_clean = fake_clean;
}

static int sems_empty(const Dev *d)
{
	return fake_sema_count(d->sem_go) == 0 && fake_sema_count(d->sem_done) == 0 &&
	       fake_sema_count(d->sem_free) == 0 && fake_sema_count(d->sem_cgo) == 0 &&
	       fake_sema_count(d->sem_fin) == 0;
}

static void test_offload(void)
{
	static const uint32_t sizes[] = { 1, 8, 32, 64, 100, 128, 256, 300, 512, 777, 1100 };
	uint8_t *u = user_buf(1100);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	/* made with overlapped reads off too: the helper reads, the copier copies */
	for (int pipeline = 0; pipeline <= 1; pipeline++) {
		reset();
		offload_on(&s, 1, VM_OFFLOAD_GPU, pipeline);
		CHECK(d->pipe_made && fake_threads_live == 2 && fake_semas_live == 5);
		fake_pid = 0x42;
		g_log_diag = 1;
		g_min_sectors = 1;                      /* small transfers too */
		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			const uint32_t n = sizes[i], at = (uint32_t)(i * 97) % 2000;
			const int maps = fake_usermap_calls;

			d->est_io_ns = 25000; d->est_cmd_ns = 420000;
			d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100;
			memset(u, 0, n * 512);
			copies_here = copies_elsewhere = 0;
			clean_calls = 0; clean_lo = clean_hi = 0; clean_copy_seen = 0; clean_user = NULL;
			CHECK(rd(u, at, n) == 0);
			CHECK(memcmp(u, disk + (size_t)at * 512, (size_t)n * 512) == 0);
			if (n >= OFFLOAD_MIN_SECTORS) {
				CHECK(copies_here == 0 && copies_elsewhere >= 1);    /* all on the copier */
				CHECK(fake_usermap_calls == maps + 1 && fake_usermap_live == 0);   /* one, unmapped */
				/* mapped and unmapped by the copier, not the caller's thread */
				CHECK(!pthread_equal(fake_usermap_thread, test_thread));
				CHECK(!pthread_equal(fake_unmap_thread, test_thread));
			} else {
				/* under 64 KiB: copied by the caller, nothing mapped */
				CHECK(copies_here >= 1 && copies_elsewhere == 0);
				CHECK(fake_usermap_calls == maps);
			}
			CHECK(clean_lo <= (uintptr_t)u && clean_hi >= (uintptr_t)u + (size_t)n * 512);
			CHECK(sems_empty(d));
		}
		/* every mapping checked on its own (no request relies on another's) */
		CHECK(g_offload_state == 1 && fake_meminfo_calls == fake_usermap_calls);
		CHECK(g_off_reads >= 6 && d->st.errors == 0);           /* the six of 64 KiB and up */
		CHECK(g_cp_game_kib > 0 && g_cp_core_kib[0] > 0);       /* noted by the copier */
		if (pipeline)
			CHECK(g_pipe_reads > 0);                            /* parts, overlapped */
		vm_stage_fini();
		CHECK(fake_threads_live == 0 && fake_semas_live == 0);
	}

	/* the helper could not be made while copy on the system core was on:
	 * a fresh try once it is wanted again after neither setting wanted it,
	 * whichever of the two is switched on */
	reset();
	fake_alloc_fail = 1;
	offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 0);
	fake_alloc_fail = 0;
	CHECK(!d->pipe_made && d->pipe_failed);
	s.offload = VM_OFFLOAD_OFF;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(!d->pipe_made);                           /* not wanted */
	s.offload = VM_OFFLOAD_ALWAYS;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(d->pipe_made && fake_threads_live == 2);
	vm_stage_fini();
	reset();
	fake_alloc_fail = 1;
	offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
	fake_alloc_fail = 0;
	CHECK(!d->pipe_made && d->pipe_failed);
	s.pipeline = VM_PIPE_OFF;
	s.offload = VM_OFFLOAD_OFF;
	vm_stage_apply(&s);                             /* both off ... */
	s.pipeline = VM_PIPE_AUTO;
	vm_stage_apply(&s);                             /* ... one of them on again */
	vm_stage_worker_tick();
	CHECK(d->pipe_made);
	vm_stage_fini();

	/* "with GPU-safe only": a plain read stays with the caller; "always" not */
	reset();
	offload_on(&s, 0, VM_OFFLOAD_GPU, 1);
	fake_pid = 0x42;
	CHECK(rd(u, 0, 256) == 0 && memcmp(u, disk, 256 * 512) == 0);
	CHECK(copies_elsewhere == 0 && fake_usermap_calls == 0 && g_off_reads == 0);
	s.offload = VM_OFFLOAD_ALWAYS;
	vm_stage_apply(&s);
	copies_here = copies_elsewhere = 0;
	CHECK(rd(u, 0, 256) == 0 && memcmp(u, disk, 256 * 512) == 0);
	CHECK(copies_here == 0 && copies_elsewhere >= 1 && g_off_reads == 1);
	/* a request inside the last one's buffer: mapped afresh (nothing kept) */
	CHECK(rd(u + 512, 1, 128) == 0 && memcmp(u + 512, disk + 512, 128 * 512) == 0);
	CHECK(fake_usermap_calls == 2 && fake_usermap_live == 0);
	CHECK(rd(u, 0, 300) == 0 && fake_usermap_calls == 3);
	/* a system thread's read: copied where it is, nothing mapped */
	fake_pid = KERNEL_PID;
	copies_here = copies_elsewhere = 0;
	CHECK(rd(u, 0, 256) == 0 && memcmp(u, disk, 256 * 512) == 0);
	CHECK(copies_elsewhere == 0 && fake_usermap_calls == 3);
	/* off: as before */
	fake_pid = 0x42;
	s.offload = VM_OFFLOAD_OFF;
	vm_stage_apply(&s);
	copies_here = copies_elsewhere = 0;
	CHECK(rd(u, 0, 256) == 0 && copies_elsewhere == 0 && fake_usermap_calls == 3);
	vm_stage_fini();

	/* the mapping fails: the caller copies; tried again on the next request */
	reset();
	offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
	fake_pid = 0x42;
	fake_usermap_err = (int)0x80020001;
	fake_usermap_deny = 1 << VM_USERMAP_RW;
	CHECK(rd(u, 3, 700) == 0 && memcmp(u, disk + 3 * 512, 700 * 512) == 0);
	CHECK(copies_elsewhere == 0 && g_off_maperr == 1 && g_offload_state == 0);
	CHECK(g_offload_err == (int)0x80020001 && sems_empty(d) && g_off_reads == 0);
	fake_usermap_err = 0;
	CHECK(rd(u, 3, 700) == 0 && memcmp(u, disk + 3 * 512, 700 * 512) == 0);
	CHECK(g_off_reads >= 1 && g_offload_state == 1);
	vm_stage_fini();

	/* not writable: refused until switched again; the caller copies */
	reset();
	offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
	fake_pid = 0x42;
	fake_meminfo_type = 0x1020D004;           /* kernel read-only */
	CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0);
	CHECK(g_offload_state == -1 && copies_elsewhere == 0 && fake_usermap_live == 0);
	CHECK(rd(u, 0, 512) == 0 && fake_usermap_calls == 1);  /* not tried again */
	vm_stage_worker_tick();
	CHECK(g_offload_logged == -1);
	fake_meminfo_type = 0x1020D006;
	s.offload = VM_OFFLOAD_OFF;
	vm_stage_apply(&s);
	s.offload = VM_OFFLOAD_ALWAYS;
	vm_stage_apply(&s);                       /* switched again: checked afresh */
	CHECK(g_offload_state == 0);
	CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0 && g_offload_state == 1);
	/* a type word that does not match the memory at the address (the info
	 * layout read wrong): refused, even with R and W in it */
	s.offload = VM_OFFLOAD_OFF;
	vm_stage_apply(&s);
	s.offload = VM_OFFLOAD_ALWAYS;
	vm_stage_apply(&s);
	fake_meminfo_type = 0x10208006;           /* non-cacheable, but the buffer is cached */
	copies_elsewhere = 0;
	CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0);
	CHECK(g_offload_state == -1 && copies_elsewhere == 0 && fake_usermap_live == 0);
	fake_meminfo_type = 0x1020D006;
	/* the type cannot be read: refused too */
	s.offload = VM_OFFLOAD_OFF;
	vm_stage_apply(&s);
	s.offload = VM_OFFLOAD_ALWAYS;
	vm_stage_apply(&s);
	fake_meminfo_err = -5;
	copies_elsewhere = 0;
	CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0);
	CHECK(g_offload_state == -1 && copies_elsewhere == 0);
	vm_stage_fini();

	/* a mapping of another memory type than the game's buffer: refused */
	{
		static uint8_t elsewhere[600 * 512];

		reset();
		offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
		fake_pid = 0x42;
		fake_usermap_elsewhere = elsewhere;
		fake_memtype_from = elsewhere;        /* the mapping: non-cacheable */
		fake_memtype_to = elsewhere + sizeof(elsewhere);
		fake_memtype_above = 0x8000;
		fake_memtype = 0xD000;                /* the game's buffer: cached */
		memset(elsewhere, 0x5A, sizeof(elsewhere));
		CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0);
		CHECK(g_offload_state == -2 && copies_elsewhere == 0 && fake_usermap_live == 0);
		CHECK(elsewhere[0] == 0x5A && elsewhere[sizeof(elsewhere) - 1] == 0x5A);
		vm_stage_worker_tick();
		CHECK(g_offload_logged == -2);
		fake_memtype_from = fake_memtype_to = NULL;
		vm_stage_fini();
	}

	/* an error at any part: the parts before it copied, the rest stock;
	 * nothing left in the semaphores, the mapping removed */
	for (int at = 1; at <= 3; at++) {
		uint32_t before = 0;

		reset();
		offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
		fake_pid = 0x42;
		d->est_io_ns = 25000; d->est_cmd_ns = 600000;
		d->est_cp_ns[0] = 4200; d->est_cp_ns[1] = 11100;
		fail_at_call = at;
		memset(u, 0, 512 * 512);
		CHECK(rd(u, 0, 512) == 0);
		CHECK(memcmp(u, disk, 512 * 512) == 0);
		CHECK(d->st.errors == 1 && d->fallback);
		for (int i = 0; i + 1 < at; i++)
			before += io_ns[i];
		CHECK(last_buf == u + (size_t)before * 512);
		CHECK(io_calls == at + 1 && sems_empty(d) && fake_usermap_live == 0);
		vm_stage_fini();
		CHECK(fake_threads_live == 0);
	}

	/* with Verify: copied on the system core in one piece, then compared */
	reset();
	offload_on(&s, 0, VM_OFFLOAD_ALWAYS, 1);
	fake_pid = 0x42;
	g_shadow = malloc(STAGE_BYTES);
	g_verify = 1;
	CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0);
	CHECK(io_calls == 2 && io_bufs[1] == g_shadow && copies_elsewhere == 1);
	CHECK(d->st.verified == 1 && d->st.mismatches == 0 && g_pipe_reads == 0);
	g_verify = 0;
	free(g_shadow);
	g_shadow = NULL;
	vm_stage_fini();

	g_live = saved_live;
	free(u);
}

/* pipe_retire does not wait for ever: a request stuck in the driver (here:
 * `busy` held) leaves the helper, copier and buffer untouched for a later
 * tick, the worker goes on, vm_stage_pending says so; wanted again
 * meanwhile, they are simply used again; once the request is done they go. */
static void test_pipe_retire_bounded(void)
{
	uint8_t *u = user_buf(512);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;
	uint32_t t0;

	reset();
	pipe_on(&s, 0);
	CHECK(d->pipe_made && d->pipe_ready && !vm_stage_pending());
	d->busy = 1;                                    /* a request that does not end */
	s.pipeline = VM_PIPE_OFF;
	vm_stage_apply(&s);
	t0 = (uint32_t)time(NULL);
	vm_stage_worker_tick();                         /* returns: does not hang */
	CHECK((uint32_t)time(NULL) - t0 <= 2);
	CHECK(d->pipe_made && !d->pipe_ready && fake_threads_live == 2 && vm_stage_pending());
	vm_stage_worker_tick();                         /* still stuck: still kept */
	CHECK(d->pipe_made && vm_stage_pending());
	s.pipeline = VM_PIPE_AUTO;                      /* wanted again meanwhile */
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(d->pipe_made && d->pipe_ready && !vm_stage_pending() && fake_threads_live == 2);
	d->busy = 0;
	CHECK(rd(u, 0, 512) == 0 && memcmp(u, disk, 512 * 512) == 0 && io_calls == 2);
	s.pipeline = VM_PIPE_OFF;
	vm_stage_apply(&s);
	vm_stage_worker_tick();                         /* nothing in flight: released */
	CHECK(!d->pipe_made && fake_threads_live == 0 && fake_semas_live == 0 && !vm_stage_pending());
	vm_stage_fini();
	g_live = saved_live;
	free(u);
}

/* EXPERIMENT "Read ahead": while a stream is read (five staged reads in a row,
 * each where the last one ended), the read-ahead thread reads the next
 * 384 KiB in pieces of 128 KiB; the stream's next reads take their data from
 * them. Anything that does not continue the stream drops them; a write (any
 * way) makes them unusable; switching the device too; a failed piece (past
 * the card's end) is not used and is no error. The data is always the disk's. */
static void ra_settings(VmSettings *s, int gpu, int offload, int pipeline)
{
	memset(s, 0, sizeof(*s));
	s->memcard = 1;
	s->stats = 1;
	s->gpusafe = gpu;
	s->pipeline = pipeline;
	s->offload = offload;
	s->readahead = 1;
	vm_stage_apply(s);
	g_live = 1;
	if (gpu)
		g_dcache_clean = fake_clean;
	vm_stage_worker_tick();               /* the helper and copier ... */
	vm_stage_worker_tick();               /* ... then the read-ahead thread */
}

static int ra_idle(const Dev *d)
{
	for (int i = 0; i < RA_PIECES; i++)
		if (__atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) != RA_FREE)
			return 0;
	return 1;
}

/* Read [at, at + n) and check it against the disk. */
static int rd_ok(uint8_t *u, uint32_t at, uint32_t n)
{
	memset(u, 0xEE, (size_t)n * 512);
	return rd(u, at, n) == 0 && memcmp(u, disk + (size_t)at * 512, (size_t)n * 512) == 0;
}

static void test_readahead(void)
{
	uint8_t *u = user_buf(1100), *w = user_buf(256);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	/* off by default, and nothing made for it */
	CHECK(VM_DEF_READAHEAD == VM_RA_OFF && vm_setting_valid(VM_KEY_READAHEAD, VM_RA_ON) &&
	      vm_setting_valid(VM_KEY_READAHEAD, VM_RA_AUTO) && !vm_setting_valid(VM_KEY_READAHEAD, 3));
	reset();
	offload_on(&s, 0, VM_OFFLOAD_OFF, 1);
	vm_stage_worker_tick();
	CHECK(d->pipe_made && !d->ra_made && fake_threads_live == 2);
	for (uint32_t at = 0; at < 8 * 256; at += 256)
		CHECK(rd_ok(u, at, 256));
	CHECK(g_ra_hits == 0 && ra_idle(d));
	vm_stage_fini();

	/* a stream of 128 KiB reads: from the sixth on, from the pieces */
	for (int gpu = 0; gpu <= 1; gpu++) {
		reset();
		ra_settings(&s, gpu, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
		CHECK(d->pipe_made && d->ra_made && d->ra_ready && fake_threads_live == 3);
		for (uint32_t i = 0; i < 12; i++) {
			const int here = __atomic_load_n(&io_here, __ATOMIC_RELAXED);

			CHECK(rd_ok(u, i * 256, 256));
			if (i == 4)
				CHECK(!ra_idle(d));               /* the fifth starts it */
			if (i >= 5)                           /* from the pieces: no card read here */
				CHECK(__atomic_load_n(&io_here, __ATOMIC_RELAXED) == here);
		}
		CHECK(g_ra_hits >= 6 && g_ra_kib >= 6 * 128 && g_ra_bad == 0);
		CHECK(d->st.errors == 0 && !d->fallback && d->st.reads == 12);
		if (gpu)
			CHECK(clean_calls > 0);               /* cleaned as any GPU-safe read */
		/* elsewhere: the pieces dropped (waiting for the one being read) */
		CHECK(rd_ok(u, 3000, 300));
		CHECK(g_ra_drops >= 1 && ra_idle(d) && fake_sema_count(d->ra_done) == 0);
		vm_stage_fini();
		CHECK(fake_threads_live == 0 && fake_semas_live == 0);
	}

	/* reads of other sizes (64, 300, 1100 sectors) in a stream: still the
	 * disk's data, partly from the pieces */
	{
		static const uint32_t sizes[] = { 128, 300, 600 };

		for (size_t k = 0; k < 3; k++) {
			uint32_t at = 0;

			reset();
			ra_settings(&s, 1, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
			while (at + sizes[k] <= DISK_SECTORS) {
				CHECK(rd_ok(u, at, sizes[k]));
				at += sizes[k];
			}
			CHECK(g_ra_hits > 0 && d->st.errors == 0 && !d->fallback);
			vm_stage_fini();
		}
	}

	/* a stream into non-cacheable memory (a game's graphics data): the first
	 * reads straight into the buffer as before, then from the pieces, copied
	 * by the copier through its non-cacheable mapping, nothing cleaned */
	for (int offl = 0; offl <= 1; offl++) {
		reset();
		ra_settings(&s, 1, offl ? VM_OFFLOAD_ALWAYS : VM_OFFLOAD_OFF, VM_PIPE_AUTO);
		fake_pid = 0x42;
		fake_memtype = MEMTYPE_NC;
		fake_meminfo_type = 0x10208006;           /* the mapping: non-cacheable, RW */
		for (uint32_t i = 0; i < 12; i++) {
			const int here = __atomic_load_n(&io_here, __ATOMIC_RELAXED);

			copies_here = copies_elsewhere = 0;
			clean_calls = 0;
			CHECK(rd_ok(u, i * 256, 256));
			CHECK(clean_calls == 0 && fake_usermap_live == 0);
			if (i < 5) {                          /* from the sixth read in a row */
				CHECK(copies_here == 0 && copies_elsewhere == 0);
			} else {
				CHECK(__atomic_load_n(&io_here, __ATOMIC_RELAXED) == here);
				CHECK(offl ? copies_here == 0 && copies_elsewhere >= 1
				           : copies_here >= 1 && copies_elsewhere == 0);
			}
		}
		CHECK(g_ra_hits == 7 && g_ra_nc_kib == 7 * 128 && g_nc_stock_kib == 5 * 128);
		CHECK(d->st.reads == 0 && d->st.errors == 0 && !d->fallback);
		CHECK(g_nc_off_refused == 0 && g_offload_state >= 0);
		/* elsewhere: dropped, straight in */
		CHECK(rd_ok(u, 3500, 300) && ra_idle(d) && __atomic_load_n(&last_buf, __ATOMIC_RELAXED) == u);
		vm_stage_fini();
	}

	/* one piece ahead after the fifth read, two after the sixth, then
	 * three: little read past the end of a file */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_ALWAYS, VM_PIPE_AUTO);
	fake_pid = 0x42;
	fake_memtype = MEMTYPE_NC;
	fake_meminfo_type = 0x10208006;
	for (uint32_t i = 0; i < 7; i++) {
		static const int ahead[] = { 0, 0, 0, 0, 1, 2, 3 };
		int live = 0;

		CHECK(rd_ok(u, 100 + i * 512, 512));
		for (int k = 0; k < RA_PIECES; k++)
			live += __atomic_load_n(&d->ra[k].state, __ATOMIC_ACQUIRE) != RA_FREE;
		CHECK(live == ahead[i]);
	}
	/* random 1 MiB reads (four 256 KiB requests each, as the file system
	 * issues them): never start it, the right data */
	{
		uint32_t x = 99;

		for (int r = 0; r < 12; r++) {
			uint32_t at;

			x = x * 1103515245u + 12345u;
			at = (x >> 8) % (DISK_SECTORS - 2048);
			for (uint32_t q = 0; q < 4; q++)
				CHECK(rd_ok(u, at + q * 512, 512));
		}
		const uint32_t hits = g_ra_hits;

		CHECK(g_ra_drops == 1 && d->st.errors == 0 && !d->fallback);   /* the stream's */
		(void)hits;
	}
	vm_stage_fini();

	/* the non-cacheable mapping does not fit (here: cached): only those are
	 * refused - the caller copies them; a cached read still goes to the copier */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_ALWAYS, VM_PIPE_AUTO);
	fake_pid = 0x42;
	fake_memtype = MEMTYPE_NC;
	fake_meminfo_type = 0x1020D006;
	for (uint32_t i = 0; i < 8; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(g_ra_hits >= 2 && g_nc_off_refused == 1 && g_offload_state == 0);
	CHECK(fake_usermap_live == 0 && sems_empty(d));
	fake_memtype = 0xD000;
	copies_here = copies_elsewhere = 0;
	CHECK(rd_ok(u, 3000, 256) && copies_here == 0 && copies_elsewhere >= 1);
	CHECK(g_offload_state == 1);
	vm_stage_fini();

	/* device memory, small reads, read ahead off: straight in as before,
	 * nothing read ahead */
	{
		static const int kinds[] = { MEMTYPE_DEVICE, MEMTYPE_NC, MEMTYPE_NC };

		for (int k = 0; k < 3; k++) {
			reset();
			ra_settings(&s, 0, VM_OFFLOAD_ALWAYS, VM_PIPE_AUTO);
			fake_memtype = kinds[k];
			if (k == 2) {
				s.readahead = 0;
				vm_stage_apply(&s);
				vm_stage_worker_tick();
			}
			for (uint32_t i = 0; i < 10; i++) {
				const uint32_t n = k == 1 ? 64 : 256;

				g_min_sectors = 1;
				CHECK(rd_ok(u, i * n, n) && __atomic_load_n(&last_buf, __ATOMIC_RELAXED) == u);
			}
			CHECK(ra_idle(d) && g_ra_hits == 0 && g_nc_stock_kib == (k == 1 ? 10 * 32 : 10 * 128));
			vm_stage_fini();
		}
	}

	/* small transfers too (as the tester's small files): reads under 64 KiB
	 * never start a stream ... */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	g_min_sectors = 1;
	for (uint32_t i = 0; i < 40; i++)
		CHECK(rd_ok(u, i * 64, 64));
	CHECK(ra_idle(d) && g_ra_hits == 0 && g_ra_drops == 0);
	/* ... nor break one, and go the standard way while pieces are kept,
	 * which stay (no drop) for the stream's next read */
	for (uint32_t i = 0; i < 5; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(!ra_idle(d));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);                            /* the thread is quiet now */
	{
		const int st0 = d->st.reads;

		CHECK(rd_ok(u, 3000, 8) && __atomic_load_n(&last_buf, __ATOMIC_RELAXED) == u);       /* stock, into the caller's buffer */
		memset(w, 0x3C, 8 * 512);
		CHECK(wr(w, 3500, 8, REF_RD) == 0);              /* a small write: stock too */
		CHECK((int)d->st.reads == st0 && g_ra_drops == 0 && !ra_idle(d));
	}
	CHECK(rd_ok(u, 5 * 256, 256) && g_ra_hits == 1 && g_ra_bad == 0);
	/* the stream over (nothing read for a while): what it left behind is
	 * dropped by the next staged transfer, which goes the faster way */
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	CHECK(!ra_idle(d));
	fake_now_us += RA_LIVE_US;
	g_min_sectors = MIN_SECTORS;
	CHECK(rd_ok(u, 3000, 64) && __atomic_load_n(&last_buf, __ATOMIC_RELAXED) == staging);
	CHECK(ra_idle(d) && g_ra_drops == 1);
	CHECK(rd_ok(u, 3100, 64) && __atomic_load_n(&last_buf, __ATOMIC_RELAXED) == staging);
	vm_stage_fini();

	/* copy on the system core: a game's stream is copied by the copier from
	 * the pieces, through its mapping, unmapped after */
	reset();
	ra_settings(&s, 1, VM_OFFLOAD_ALWAYS, VM_PIPE_AUTO);
	fake_pid = 0x42;
	CHECK(fake_threads_live == 3);
	for (uint32_t i = 0; i < 7; i++) {
		copies_here = copies_elsewhere = 0;
		CHECK(rd_ok(u, 100 + i * 512, 512));
		CHECK(copies_here == 0 && copies_elsewhere >= 1);
		CHECK(fake_usermap_live == 0 && sems_empty(d));
	}
	CHECK(g_ra_hits >= 2 && g_off_reads >= 7);
	vm_stage_fini();

	/* a staged write in the stream: the pieces dropped, the new data read */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 6; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(!ra_idle(d));
	memset(w, 0x5A, 256 * 512);
	CHECK(wr(w, 6 * 256, 256, REF_RD) == 0);
	CHECK(ra_idle(d) && d->ra_streak == 0);
	CHECK(rd_ok(u, 6 * 256, 256) && u[0] == 0x5A);
	vm_stage_fini();

	/* a write that goes stock (small) into a piece: the piece is not used */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 6; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(!ra_idle(d));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);                            /* read already */
	memset(w, 0xA5, 8 * 512);
	CHECK(wr(w, 6 * 256 + 10, 8, REF_RD) == 0);   /* stock: under 32 KiB */
	CHECK(d->ra_wbusy == 0 && !ra_idle(d));
	CHECK(rd_ok(u, 6 * 256, 256) && u[10 * 512] == 0xA5);
	CHECK(g_ra_bad == 1 && g_ra_hits == 1);           /* the sixth was one */
	/* ... and the stream goes on */
	for (uint32_t i = 7; i < 14; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(g_ra_hits >= 4);
	vm_stage_fini();

	/* a write elsewhere leaves the pieces usable; one into a piece marks
	 * only that piece (the stream's read then goes the standard way) */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 5; i++)
		CHECK(rd_ok(u, i * 256, 256));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	memset(w, 0x77, 256 * 512);
	CHECK(wr(w, 3000, 8, REF_RD) == 0);           /* elsewhere, stock */
	CHECK(wr(w, 3300, 256, REF_RD) == 0);         /* elsewhere, staged: drops them */
	CHECK(ra_idle(d) && g_ra_drops == 1);
	for (uint32_t i = 5; i < 10; i++)
		CHECK(rd_ok(u, i * 256, 256));            /* a stream again from here */
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	CHECK(wr(w, 3900, 8, REF_RD) == 0);           /* elsewhere: nothing marked */
	{
		int dirty = 0;

		for (int i = 0; i < RA_PIECES; i++)
			dirty += d->ra[i].dirty;
		CHECK(dirty == 0);
		CHECK(wr(w, 10 * 256 + 300, 8, REF_RD) == 0);   /* into the second piece */
		for (int i = 0; i < RA_PIECES; i++)
			dirty += d->ra[i].dirty;
		CHECK(dirty == 1);
	}
	{
		const uint32_t hits = g_ra_hits;

		CHECK(rd_ok(u, 10 * 256, 256) && g_ra_hits == hits + 1);   /* the clean piece */
		CHECK(rd_ok(u, 11 * 256, 256) && u[44 * 512] == 0x77 && g_ra_bad == 1);
	}
	vm_stage_fini();

	/* a read larger than the pieces hold: their part, then the rest as
	 * before */
	reset();
	ra_settings(&s, 1, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 5; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(rd_ok(u, 5 * 256, 1100));
	CHECK(g_ra_hits == 1 && g_ra_kib == 3 * 128 && ra_idle(d) == 0);
	CHECK(rd_ok(u, 5 * 256 + 1100, 256) && g_ra_hits == 2);
	vm_stage_fini();

	/* a write while a piece is being read: that piece is not used */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 5; i++)
		CHECK(rd_ok(u, i * 256, 256));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	d->ra[0].ok = 0;                              /* as if a write had been running */
	CHECK(rd_ok(u, 5 * 256, 256) && g_ra_bad == 1 && g_ra_hits == 0);
	vm_stage_fini();

	/* the device switched off and on: nothing read before is used */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 5; i++)
		CHECK(rd_ok(u, i * 256, 256));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	s.memcard = 0;
	vm_stage_apply(&s);
	s.memcard = 1;
	vm_stage_apply(&s);
	CHECK(rd_ok(u, 5 * 256, 256) && g_ra_bad == 1 && g_ra_hits == 0);
	/* too old: not used either */
	for (uint32_t i = 6; i < 11; i++)
		CHECK(rd_ok(u, i * 256, 256));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	fake_now_us += RA_MAX_AGE_US;
	{
		const uint32_t bad = g_ra_bad;

		CHECK(rd_ok(u, 11 * 256, 256) && g_ra_bad == bad + 1);
	}
	vm_stage_fini();

	/* past the card's end: those pieces fail, no error, the device stays */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t at = DISK_SECTORS - 8 * 256; at < DISK_SECTORS; at += 256)
		CHECK(rd_ok(u, at, 256));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	CHECK(d->st.errors == 0 && !d->fallback && g_ra_hits >= 1);
	CHECK(rd_ok(u, 0, 256) && ra_idle(d));       /* dropped, the failed ones too */
	vm_stage_fini();

	/* a card error in the stream itself: as without read ahead */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 5; i++)
		CHECK(rd_ok(u, i * 256, 256));
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	fail_at_call = io_calls + 1;                  /* the next card read */
	CHECK(rd_ok(u, 2000, 256));                   /* drop, then fails, done stock */
	CHECK(d->fallback && d->st.errors == 1 && ra_idle(d));
	vm_stage_fini();

	/* verify on: read ahead too, and every read from the pieces is read
	 * again the standard way and compared; a piece gone wrong (here: its
	 * bytes changed in the buffer) falls back, the caller gets the disk's */
	for (int nc = 0; nc <= 1; nc++) {
		reset();
		ra_settings(&s, 0, VM_OFFLOAD_ALWAYS, VM_PIPE_AUTO);
		fake_pid = 0x42;
		if (nc) {
			fake_memtype = MEMTYPE_NC;
			fake_meminfo_type = 0x10208006;
		}
		g_verify = 1;
		g_shadow = malloc(STAGE_BYTES);
		for (uint32_t i = 0; i < 10; i++)
			CHECK(rd_ok(u, i * 256, 256));
		CHECK(g_ra_hits >= 4 && d->st.verified >= g_ra_hits && d->st.mismatches == 0);
		CHECK(!d->fallback && g_shadow_busy == 0);
		for (int i = 0; i < RA_PIECES; i++)
			ra_wait(d, i);
		for (int i = 0; i < RA_PIECES; i++)
			if (d->ra[i].state == RA_READY && d->ra[i].sector == 10 * 256)
				ra_ptr(d, i)[100] ^= 0xFF;
		CHECK(rd_ok(u, 10 * 256, 256));
		CHECK(d->st.mismatches == 1 && d->fallback == 1 && g_shadow_busy == 0);
		{
			const uint32_t hits = g_ra_hits;

			CHECK(rd_ok(u, 11 * 256, 256) && g_ra_hits == hits);   /* stock from now on */
		}
		vm_stage_worker_tick();
		CHECK(!d->ra_made);                       /* given back after the fallback */
		free(g_shadow);
		g_shadow = NULL;
		g_verify = 0;
		vm_stage_fini();
	}

	/* switched off in the middle of a stream: the thread is given back
	 * once nothing is being read, the reads go on as before */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 6; i++)
		CHECK(rd_ok(u, i * 256, 256));
	s.readahead = 0;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(!d->ra_made && !d->ra_ready && ra_idle(d) && d->pipe_made && fake_threads_live == 2);
	CHECK(d->ra_go == -1 && d->ra_done == -1);
	for (uint32_t i = 6; i < 12; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(ra_idle(d));
	/* on again: made again; overlapped reads and copy both off still keep
	 * the helper for it */
	s.readahead = 1;
	s.pipeline = VM_PIPE_OFF;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(d->pipe_made && d->ra_made && fake_threads_live == 3);
	for (uint32_t i = 0; i < 10; i++)
		CHECK(rd_ok(u, i * 256, 256));
	CHECK(g_ra_hits >= 4);
	/* everything off: all of it given back */
	s.readahead = 0;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(!d->ra_made && !d->pipe_made && fake_threads_live == 0 && fake_semas_live == 0);
	vm_stage_fini();

	/* the thread cannot be made: reads as before, not tried every tick */
	reset();
	offload_on(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	CHECK(d->pipe_made);
	fake_thread_fail = 1;
	s.readahead = 1;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(!d->ra_made && d->ra_failed && d->ra_go == -1 && fake_threads_live == 2);
	fake_thread_fail = 0;
	vm_stage_worker_tick();
	CHECK(!d->ra_made);
	for (uint32_t i = 0; i < 8; i++)
		CHECK(rd_ok(u, i * 256, 256));
	s.readahead = 0;
	vm_stage_apply(&s);
	s.readahead = 1;
	vm_stage_apply(&s);                           /* switched again: tried afresh */
	vm_stage_worker_tick();
	CHECK(d->ra_made);
	vm_stage_fini();
	CHECK(fake_threads_live == 0 && fake_semas_live == 0);

	/* the request in flight when it is switched off: released only after */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	d->busy = 1;
	s.readahead = 0;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(d->ra_made && !d->ra_ready && vm_stage_pending());
	d->busy = 0;
	vm_stage_worker_tick();
	CHECK(!d->ra_made && !vm_stage_pending());
	vm_stage_fini();

	g_live = saved_live;
	free(u);
	free(w);
}

/* Read ahead "Auto": per game (title id), the streams' lengths are learnt
 * and the threshold that pays most is chosen per kind of memory, kept in
 * ur0:data/vita-memio/ra/<title>.bin and used when the game runs again. */
static const char *fake_title_of(SceUID pid)
{
	return pid == 0x42 ? "PCSE00001" : pid == 0x43 ? "PCSE00001"
	     : pid == 0x44 ? "PCSB00777" : pid == 0x45 ? "../bad" : NULL;
}

static int fake_title(SceUID pid, char *t, SceSize len)
{
	const char *s = fake_title_of(pid);

	if (!s)
		return -1;
	snprintf(t, len, "%s", s);
	return 0;
}

/* `count` streams of `len` reads of `n` sectors each, one after another on
 * the disk (wrapping), each starting somewhere new. */
static void streams(uint8_t *u, uint32_t count, uint32_t len, uint32_t n)
{
	static uint32_t at = 0;

	for (uint32_t k = 0; k < count; k++) {
		at += 3 * n + 64;                         /* not where the last one ended */
		if (at + len * n > DISK_SECTORS)
			at = 0;
		for (uint32_t i = 0; i < len; i++)
			CHECK(rd_ok(u, at + i * n, n));
		at += len * n;
	}
}

static void test_readahead_auto(void)
{
	uint8_t *u = user_buf(1100);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;
	uint32_t thr[2];

	/* the choice: long streams start early; short ones (as a game reading
	 * files of 1 MiB and less) never; a mix by what pays */
	{
		uint32_t c[RA_HBINS] = { 0 };

		c[16] = 50;                                   /* 4 MiB streams */
		CHECK(ra_choose(c) == 2);
		memset(c, 0, sizeof(c));
		c[2] = 300;
		c[3] = 200;
		c[4] = 100;                                   /* 1 MiB files */
		CHECK(ra_choose(c) == RA_THR_NONE);
		c[12] = 40;                                   /* and some 3 MiB ones */
		CHECK(ra_choose(c) == 5);                     /* after the 1 MiB files */
		memset(c, 0, sizeof(c));
		CHECK(ra_choose(c) == RA_THR_NONE);           /* nothing seen */
		c[1] = 1000;                                  /* single reads: nothing to gain */
		CHECK(ra_choose(c) == RA_THR_NONE);
	}

	fakes_tmpdir_new();

	/* a game of long streams: learnt (fixed threshold meanwhile), kept,
	 * served early from then on */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	s.readahead = VM_RA_AUTO;
	vm_stage_apply(&s);
	g_title_fn = fake_title;
	fake_pid = 0x42;
	streams(u, 1, 8, 256);
	vm_stage_worker_tick();                           /* sees the game: learning */
	CHECK(g_ra_pid == 0x42 && g_ra_learning && !strcmp(g_ra_title, "PCSE00001"));
	CHECK(ra_threshold(0x42, 0) == RA_STREAK && ra_threshold(0x42, 1) == RA_STREAK_NC);
	streams(u, RA_LEARN_STREAMS, 8, 256);
	vm_stage_worker_tick();
	CHECK(!g_ra_learning && g_ra_thr[0] == 2 && g_ra_thr[1] == RA_THR_NONE);   /* none seen */
	CHECK(ra_profile_load("PCSE00001", thr) && thr[0] == 2 && thr[1] == RA_THR_NONE);
	CHECK(ra_threshold(0x42, 0) == 2 && ra_threshold(0x99, 0) == RA_STREAK);  /* only it */
	{
		const uint32_t hits = g_ra_hits;

		streams(u, 4, 8, 256);
		CHECK(g_ra_hits >= hits + 4 * 5);              /* from the third read on */
	}
	/* never: not even a long stream starts it (but stays correct) */
	g_ra_thr[0] = RA_THR_NONE;
	{
		const uint32_t hits = g_ra_hits;

		streams(u, 2, 12, 256);
		CHECK(g_ra_hits == hits && ra_idle(d));
	}
	vm_stage_fini();

	/* the same title again (another process): kept, not learnt again */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	s.readahead = VM_RA_AUTO;
	vm_stage_apply(&s);
	g_title_fn = fake_title;
	fake_pid = 0x43;
	streams(u, 1, 8, 256);
	vm_stage_worker_tick();
	CHECK(g_ra_pid == 0x43 && !g_ra_learning && g_ra_thr[0] == 2);
	/* another game: learns its own; a stream of the first one meanwhile is
	 * not counted for it */
	fake_pid = 0x44;
	streams(u, 1, 8, 256);
	vm_stage_worker_tick();
	CHECK(g_ra_pid == 0x44 && g_ra_learning && !strcmp(g_ra_title, "PCSB00777"));
	CHECK(g_ra_thr[0] == 0 && g_ra_hstreams == 0);
	fake_pid = 0x44;
	streams(u, 10, 2, 256);                           /* short ones */
	CHECK(g_ra_hstreams >= 9);
	/* a process with no usable title id: the fixed threshold, nothing saved */
	fake_pid = 0x45;
	streams(u, 1, 8, 256);
	vm_stage_worker_tick();
	CHECK(g_ra_pid == 0x45 && !g_ra_learning && !g_ra_title[0]);
	CHECK(ra_threshold(0x45, 0) == RA_STREAK);
	/* system threads are never a game */
	fake_pid = KERNEL_PID;
	streams(u, 1, 8, 256);
	vm_stage_worker_tick();
	CHECK(g_ra_pid == 0x45);
	vm_stage_fini();

	/* a fresh folder (no profiles): learnt again; a profile out of range or
	 * damaged is not used */
	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	CHECK(!ra_profile_load("PCSE00001", thr));       /* the new folder has none */
	reset();
	g_title_fn = fake_title;
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	s.readahead = VM_RA_AUTO;
	vm_stage_apply(&s);
	g_title_fn = fake_title;
	fake_pid = 0x42;
	streams(u, 1, 8, 256);
	vm_stage_worker_tick();
	CHECK(g_ra_learning);
	{
		uint32_t t2[2] = { 7, 9 };                    /* 9: out of range */

		ra_profile_save("PCSE00002", t2, 1);
		CHECK(!ra_profile_load("PCSE00002", thr));
		t2[1] = 3;
		ra_profile_save("PCSE00002", t2, 1);
		CHECK(ra_profile_load("PCSE00002", thr) && thr[0] == 7 && thr[1] == 3);
	}
	vm_stage_fini();

	/* auto without title ids (an unknown firmware): as on, said once */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	s.readahead = VM_RA_AUTO;
	vm_stage_apply(&s);
	fake_pid = 0x42;
	streams(u, 3, 8, 256);
	vm_stage_worker_tick();
	vm_stage_worker_tick();
	CHECK(g_ra_pid == 0 && g_ra_auto_logged == 1 && g_ra_hits > 0);
	vm_stage_fini();

	fakes_tmpdir_remove();
	g_live = saved_live;
	free(u);
}

/* Everything at once, for ThreadSanitizer: two readers on the memory card
 * (one of them mostly finds the buffer busy and goes stock), one on the
 * internal storage, the settings switched on the fly (overlapped reads, copy
 * on the system core, statistics, small transfers), the worker making and
 * releasing the helpers, and the counters read meanwhile. The data is always
 * the disk's and nothing fails. */
static volatile int g_stress_stop, g_stress_bad;
static void *g_null_ctx;                          /* the fake SD context's first word */

typedef struct { int dev; int seed; int reads; int seq; int pause; } StressArg;
/* The readers stay below this; a writer writes above it (into what read
 * ahead may have read: the pieces it touches are marked). */
#define STRESS_RD_END  3584

static volatile int g_stress_wpause;              /* the writer holds off */

static void *stress_writer(void *argp)
{
	uint8_t *w = malloc(256 * 512);
	uint32_t x = 7;

	(void)argp;
	while (!__atomic_load_n(&g_stress_stop, __ATOMIC_ACQUIRE)) {
		uint32_t n, at;

		if (__atomic_load_n(&g_stress_wpause, __ATOMIC_ACQUIRE)) {
			sched_yield();
			continue;
		}

		x = x * 1103515245u + 12345u;
		n = (x >> 8) & 1 ? 8 : 256;                /* stock or staged */
		at = STRESS_RD_END + (x >> 4) % (DISK_SECTORS - STRESS_RD_END - n);
		memset(w, (int)(x >> 24), (size_t)n * 512);
		stage_xfer(&g_dev[M], 1, fake_io, REF_WR, REF_RD, NULL, at, w, n);
		sched_yield();
	}
	free(w);
	return NULL;
}

static void *stress_reader(void *argp)
{
	StressArg *a = argp;
	uint8_t *u = malloc(1100 * 512);
	uint32_t x = (uint32_t)a->seed * 2654435761u + 1;

	while (!__atomic_load_n(&g_stress_stop, __ATOMIC_ACQUIRE)) {
		uint32_t n, at;
		int r;

		if (__atomic_load_n(&a->pause, __ATOMIC_ACQUIRE)) {
			sched_yield();
			continue;
		}

		x = x * 1103515245u + 12345u;
		n = 64 + (x >> 8) % 1037;
		at = (x >> 4) % (STRESS_RD_END - n);
		if (a->seq) {                               /* a stream: read ahead's case */
			static __thread uint32_t pos;

			n = 256;
			if (pos + n > STRESS_RD_END)
				pos = 0;
			at = pos;
			pos += n;
		}
		if (a->dev == M)
			r = stage_xfer(&g_dev[M], 0, fake_io, REF_RD, 0, NULL, at, u, n);
		else
			r = stage_xfer(&g_dev[a->dev], 0, fake_io, REF_RD, 0, &g_null_ctx, at, u, n);
		if (r != 0 || memcmp(u, disk + (size_t)at * 512, (size_t)n * 512) != 0)
			__atomic_fetch_add(&g_stress_bad, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&a->reads, 1, __ATOMIC_RELAXED);
	}
	free(u);
	return NULL;
}

static void *stress_stats(void *argp)
{
	(void)argp;
	while (!__atomic_load_n(&g_stress_stop, __ATOMIC_ACQUIRE)) {
		VmDevState st[VM_DEV_COUNT];
		VmTimingDev tm[VM_DEV_COUNT];
		uint64_t rdb, wrb;

		vm_stage_get(st, 1);
		vm_stage_timing(tm);
		vm_stage_bytes(M, &rdb, &wrb);
		(void)vm_stage_verify_ready();
		(void)vm_stage_pending();
		sched_yield();
	}
	return NULL;
}

static void test_stress_concurrency(void)
{
	VmSettings s;
	Dev *in = &g_dev[VM_DEV_INTERNAL];
	uint8_t *staging2 = calloc(1, STAGE_BYTES);
	pthread_t th[5];
	StressArg args[3] = { { M, 1, 0, 1, 0 }, { M, 2, 0, 0, 0 }, { VM_DEV_INTERNAL, 3, 0, 1, 0 } };
	int saved_live = g_live;

	reset();
	fake_pid = 0x42;                                /* a game: copy on the system core too */
	g_title_fn = fake_title;                        /* read ahead's auto learns it */
	in->buf = staging2;
	in->enabled = 1;
	in->fallback = 0;
	in->unusable = 0;                               /* an earlier vm_stage_fini set it */
	in->ra_th = in->ra_go = in->ra_done = -1;
	in->uid = -1;
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.internal = 1;
	s.pipeline = VM_PIPE_AUTO;
	vm_stage_apply(&s);
	g_live = 1;
	vm_stage_worker_tick();
	g_stress_stop = g_stress_bad = 0;
	g_stress_wpause = 0;
	for (int i = 0; i < 3; i++)
		CHECK(pthread_create(&th[i], NULL, stress_reader, &args[i]) == 0);
	CHECK(pthread_create(&th[3], NULL, stress_stats, NULL) == 0);
	CHECK(pthread_create(&th[4], NULL, stress_writer, NULL) == 0);
	/* switch until every reader has read a fair amount (slow under TSan) */
	for (int i = 0; i < 240 || ((__atomic_load_n(&args[0].reads, __ATOMIC_RELAXED) < 40 ||
	                             __atomic_load_n(&args[1].reads, __ATOMIC_RELAXED) < 40 ||
	                             __atomic_load_n(&args[2].reads, __ATOMIC_RELAXED) < 40) &&
	                            i < 20000); i++) {
		s.pipeline = i % 3;                         /* off, auto, fixed */
		s.offload = (i / 3) % 3 == 1 ? VM_OFFLOAD_ALWAYS : VM_OFFLOAD_OFF;
		s.stats = (i / 9) & 1;
		s.small = (i / 18) & 1;
		s.readahead = (i / 5) % 4 == 0 ? VM_RA_OFF : (i / 5) % 4 == 2 ? VM_RA_AUTO : VM_RA_ON;
		vm_stage_apply(&s);
		vm_stage_worker_tick();
		sched_yield();
	}
	/* read ahead left on until the card's stream has used it (the other
	 * card reader paused: it would break the stream); never on the
	 * internal storage */
	s.readahead = 1;
	vm_stage_apply(&s);
	__atomic_store_n(&args[1].pause, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&g_stress_wpause, 1, __ATOMIC_RELEASE);   /* its writes break streams */
	for (int i = 0; i < 30000 && !__atomic_load_n(&g_ra_hits, __ATOMIC_RELAXED); i++) {
		vm_stage_worker_tick();
		usleep(1000);                               /* up to 30 s, however slow the runner */
	}
	__atomic_store_n(&g_stress_stop, 1, __ATOMIC_RELEASE);
	for (int i = 0; i < 5; i++)
		pthread_join(th[i], NULL);
	CHECK(g_stress_bad == 0);
	CHECK(args[0].reads > 20 && args[1].reads > 20 && args[2].reads > 20);
	CHECK(g_dev[M].st.errors == 0 && in->st.errors == 0 && !g_dev[M].fallback && !in->fallback);
	CHECK(g_dev[M].st.passthru > 0);                /* the two card readers met */
	CHECK(g_ra_hits > 0);                           /* the stream read ahead */
	CHECK(!in->ra_made && in->ra_th == -1 && in->pipe_made);
	vm_stage_fini();
	CHECK(fake_threads_live == 0 && fake_semas_live == 0);
	in->unusable = 0;
	in->buf = NULL;
	g_live = saved_live;
	free(staging2);
}

int main(void)
{
	staging = calloc(1, STAGE_BYTES);

	RUN(test_div_by);
	RUN(test_small_goes_stock);
	RUN(test_small_transfers_switch);
	RUN(test_statistics_switch);
	RUN(test_gpu_safe_reads);
	RUN(test_noncacheable_buffers_go_stock);
	RUN(test_pipeline_reads);
	RUN(test_helper_runs_ahead);
	RUN(test_plan_quick_check);
	RUN(test_copy_diagnostics);
	RUN(test_offload);
	RUN(test_pipe_retire_bounded);
	RUN(test_stress_concurrency);
	RUN(test_pipeline_gpu_safe);
	RUN(test_pipeline_errors);
	RUN(test_pipeline_not_with_verify);
	RUN(test_pipeline_any_core);
	RUN(test_pipeline_setup_fails);
	RUN(test_every_switch_on_the_fly);
	RUN(test_pipeline_learns_without_statistics);
	RUN(test_readahead);
	RUN(test_readahead_auto);
	RUN(test_pipeline_modes);
	RUN(test_pipeline_released_while_reading);
	RUN(test_problems_wake_the_worker);
	RUN(test_staged_read);
	RUN(test_large_read_is_split);
	RUN(test_read_error_finishes_stock);
	RUN(test_staged_write);
	RUN(test_write_error_finishes_stock);
	RUN(test_read_verify_mismatch);
	RUN(test_write_verify_mismatch);
	RUN(test_shadow_not_leaked_without_read_ref);
	RUN(test_busy_and_disabled_go_stock);
	RUN(test_shadow_alloc_not_retried);
	RUN(test_get_reports_state);
	RUN(test_timing_split);
	RUN(test_init_all_hooks);
	RUN(test_init_sdif_hook_fails_releases_everything);
	RUN(test_init_memcard_hook_fails_only_memcard_stock);
	RUN(test_buffers_on_demand);

	free(staging);
	printf(vm_failures ? "test_stage: %d FAILED\n" : "test_stage: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
