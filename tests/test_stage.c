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
/* 1: the staging copies go through the plugin's own choice (DMA or CPU), as on
 * the console; the DMA tests set it. */
static int use_copy_out;
static int stage_copy_out(void *dst, const void *src, size_t n);
/* 1 if the DMA controller made the copy (as stage_copy_out says) */
static int fake_copy(void *d, const void *s, size_t n)
{
	if (use_copy_out && s != NULL)
		return stage_copy_out(d, s, n);
	__sync_fetch_and_add(&fake_now_us, copy_cost_us);
	if (pthread_equal(pthread_self(), test_thread))
		__sync_fetch_and_add(&copies_here, 1);
	else
		__sync_fetch_and_add(&copies_elsewhere, 1);
	memcpy(d, s, n);
	return 0;
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
	memset(g_mapfail, 0, sizeof(g_mapfail));
	g_mapfail_n = g_mapfail_logged = 0;
	for (size_t i = 0; i < sizeof(disk); i++)
		disk[i] = (uint8_t)(i * 31 + (i >> 9));
	memset(d, 0, sizeof(*d));
	d->port = -1;
	d->uid = -1;
	d->uid2 = d->th = d->sem_go = d->sem_done = d->sem_free = -1;
	d->th2 = d->sem_cgo = d->sem_fin = -1;
	d->ra_th = d->ra_go = d->ra_done = d->t_uid = -1;
	g_readahead = 0;
	memset(g_cal, 0, sizeof(g_cal));
	g_cal_on = g_cal_dirty = g_cal_loaded = 0;
	g_ra_hits = g_ra_kib = g_ra_drops = g_ra_waste_kib = g_ra_bad = 0;
	g_rs_kib = g_rs_waste_kib = g_rs_drops = 0;
	g_ra_nc_kib = 0;
	g_ra_mode = VM_RA_OFF;
	g_ra_fg = 0;
	g_count_cores = 0;
	g_title_fn = NULL;
	g_ra_tiny_reads = 0;
	g_nc_off_refused = 0;
	g_nc_off_type = 0;
	g_offload = VM_OFFLOAD_OFF;
	g_offload_state = g_offload_logged = 0;
	g_offload_type = 0;
	g_offload_err = 0;
	g_off_reads = g_off_kib = g_off_maperr = 0;
	g_ncd_state = g_ncd_logged = g_ncd_err = 0;
	g_ncd_type = 0;
	g_ncd_reads = g_ncd_kib = g_ncd_maperr = g_ncd_busy = g_ncd_fail = 0;
	g_log_diag = 0;
	memset(g_cp_core_kib, 0, sizeof(g_cp_core_kib));
	g_ra_nc_sys_kib = g_ra_nc_own_kib = 0;
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
	use_copy_out = 0;
	g_dmac_memcpy = NULL;
	g_dma_want = g_dma_stop = g_dma_logged = 0;
	g_dma_copies = g_dma_kib = g_dma_us = g_dma_errors = g_dma_mid = g_dma_mid_us = 0;
	g_dma_small = g_dma_small_kib = g_dma_small_half = g_dma_unal = g_dma_unal_kib = 0;
	g_dma_last_err = 0;
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

	s.hud = VM_HUD_EXTENDED;                  /* the setting alone counts nothing */
	vm_stage_apply(&s);
	CHECK(rd(u, 0, 100) == 0);
	CHECK(g_dev[M].all_rd == 0);
	vm_stage_count_hud(1);                    /* the HUD on screen: its byte counts */
	CHECK(rd(u, 0, 100) == 0);
	CHECK(g_dev[M].all_rd == 100 * 512 && g_dev[M].tm.rd_sectors == 0 && fake_time_calls == 0);
	vm_stage_count_hud(0);

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

/* ---- DMA copy (VM_KEY_DMACOPY) ------------------------------------------------- */

/* The system's ksceDmacMemcpy, faked: copies, or refuses (having written junk
 * into part of dst first), or "copies" wrong data (as a controller that missed
 * the CPU's cache would leave it). */
static int dmac_calls, dmac_fail, dmac_corrupt;
static int fake_dmac(void *dst, const void *src, SceSize n)
{
	dmac_calls++;
	if (dmac_fail) {
		memset(dst, 0xA5, n / 2);
		return (int)0x80020005;
	}
	memcpy(dst, src, n);
	if (dmac_corrupt)
		((uint8_t *)dst)[n / 3] ^= 0x40;
	return 0;
}

/* A buffer on a cache line, as the DMA copy needs (exact size for ASan). */
static uint8_t *line_buf(size_t n)
{
	void *p = NULL;

	return posix_memalign(&p, 64, n) == 0 ? p : NULL;
}

static VmSettings dma_settings(int on)
{
	VmSettings s;

	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.dmacopy = on;
	return s;
}

static void fill(uint8_t *p, size_t n, unsigned seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)(i * 13 + seed);
}

static void test_dma_copy_choice(void)
{
	const size_t big = 64 * 1024;
	uint8_t *src = line_buf(big + 64), *dst = line_buf(big + 64);
	VmSettings s;
	VmDma st;

	reset();
	dmac_calls = dmac_fail = dmac_corrupt = 0;
	fill(src, big + 64, 7);

	/* off: never the controller, even when there is one */
	g_dmac_memcpy = fake_dmac;
	s = dma_settings(0);
	vm_stage_apply(&s);
	stage_copy_out(dst, src, big);
	stage_copy_out(dst, src, 4096);                   /* not counted: DMA copy is off */
	stage_copy_out(dst + 4, src, big);
	CHECK(dmac_calls == 0 && memcmp(dst + 4, src, big) == 0 && dma_state() == VM_DMA_OFF);
	CHECK(g_dma_small == 0 && g_dma_unal == 0);

	/* on: whole cache lines from 16 KiB up go to the controller */
	s = dma_settings(1);
	vm_stage_apply(&s);
	CHECK(dma_state() == VM_DMA_ON);
	memset(dst, 0, big);
	stage_copy_out(dst, src, big);
	CHECK(dmac_calls == 1 && memcmp(dst, src, big) == 0);
	CHECK(g_dma_copies == 1 && g_dma_kib == 64 && g_dma_small == 0 && g_dma_unal == 0);
	CHECK(g_dma_mid == 0);                          /* 64 KiB: not a 16-32 KiB one */

	/* the rest is the CPU's, each counted by why: too small (and of them the
	 * ones from 8 KiB up), an odd start, an odd end */
	memset(dst, 0, big + 64);
	stage_copy_out(dst, src, VM_DMA_MIN_BYTES - 512);   /* 15.5 KiB */
	stage_copy_out(dst, src, 4096);
	stage_copy_out(dst + 4, src, 4096);                 /* small first, whatever else */
	CHECK(g_dma_small == 3 && g_dma_small_half == 1 && g_dma_small_kib == 15 + 4 + 4);
	stage_copy_out(dst + 4, src, big);
	stage_copy_out(dst, src + 512 + 8, big - 512);
	stage_copy_out(dst, src, big + 32);
	CHECK(g_dma_unal == 3 && g_dma_unal_kib == 64 + 63 + 64);
	CHECK(dmac_calls == 1 && g_dma_small == 3);
	CHECK(memcmp(dst, src, big + 32) == 0);           /* the last one, all of it */
	stage_copy_out(dst, src, VM_DMA_MIN_BYTES);       /* exactly the smallest: DMA */
	CHECK(dmac_calls == 2 && g_dma_mid == 1);
	stage_copy_out(dst, src, 2 * VM_DMA_MIN_BYTES - 512);   /* 31.5 KiB: still 16-32 */
	stage_copy_out(dst, src, 2 * VM_DMA_MIN_BYTES);         /* 32 KiB: no longer */
	CHECK(dmac_calls == 4 && g_dma_mid == 2 && g_dma_copies == 4);
	CHECK(memcmp(dst, src, 2 * VM_DMA_MIN_BYTES) == 0);

	/* refused: the CPU copies all of it, junk and all overwritten */
	dmac_fail = 1;
	memset(dst, 0, big);
	stage_copy_out(dst, src, big);
	CHECK(memcmp(dst, src, big) == 0 && g_dma_errors == 1 && dma_state() == VM_DMA_ON);
	CHECK(g_dma_last_err == (int32_t)0x80020005);
	for (unsigned i = 1; i < VM_DMA_MAX_ERRORS; i++)
		stage_copy_out(dst, src, big);
	CHECK(dma_state() == VM_DMA_STOPPED && g_dma_stop == VM_DMA_WHY_ERRORS);
	{
		const int calls = dmac_calls;

		dmac_fail = 0;
		memset(dst, 0, big);
		stage_copy_out(dst, src, big);              /* stopped: the CPU, not tried */
		CHECK(dmac_calls == calls && memcmp(dst, src, big) == 0);
	}
	vm_stage_dma(&st);
	CHECK(st.size == sizeof(st) && st.magic == VM_DMA_MAGIC && st.state == VM_DMA_STOPPED &&
	      st.stop_why == VM_DMA_WHY_ERRORS && st.errors == VM_DMA_MAX_ERRORS && st.copies == 4 &&
	      st.mid == 2);

	/* the worker says why, once */
	g_live = 1;
	vm_stage_worker_tick();
	CHECK(g_dma_logged == 1);
	g_live = 0;

	/* still on in Settings: stays stopped; off and on: a fresh start */
	vm_stage_apply(&s);
	CHECK(dma_state() == VM_DMA_STOPPED);
	s = dma_settings(0);
	vm_stage_apply(&s);
	s = dma_settings(1);
	vm_stage_apply(&s);
	CHECK(dma_state() == VM_DMA_ON && g_dma_errors == 0 && g_dma_last_err == 0 &&
	      !g_dma_logged);

	/* no ksceDmacMemcpy on this system: on in Settings, the CPU copies */
	g_dmac_memcpy = NULL;
	CHECK(dma_state() == VM_DMA_UNAVAIL);
	memset(dst, 0, big);
	stage_copy_out(dst, src, big);
	CHECK(memcmp(dst, src, big) == 0);
	free(src);
	free(dst);
}

/* Through a real read: the controller makes the copy, and with verify on a
 * copy that came out wrong is caught - the caller gets the standard data and
 * DMA copy stops at once. */
static void test_dma_copy_reads(void)
{
	const uint32_t n = 128;                           /* 64 KiB: one staged chunk */
	uint8_t *u = line_buf(n * 512);
	VmSettings s;

	reset();
	dmac_calls = dmac_fail = dmac_corrupt = 0;
	use_copy_out = 1;
	g_dmac_memcpy = fake_dmac;
	s = dma_settings(1);
	vm_stage_apply(&s);
	CHECK(rd(u, 30, n) == 0);
	CHECK(memcmp(u, disk + 30 * 512, n * 512) == 0 && dmac_calls == 1 && g_dma_copies == 1);

	g_verify = 1;
	g_shadow = calloc(1, STAGE_BYTES);
	dmac_corrupt = 1;
	memset(u, 0, n * 512);
	CHECK(rd(u, 50, n) == 0);
	CHECK(memcmp(u, disk + 50 * 512, n * 512) == 0);  /* the standard data */
	CHECK(g_dev[M].st.mismatches == 1);
	CHECK(dma_state() == VM_DMA_STOPPED && g_dma_stop == VM_DMA_WHY_MISMATCH);
	free(g_shadow);
	g_shadow = NULL;
	g_verify = 0;
	free(u);
}

/* Every default at once, as a fresh install has them (with one storage
 * switched on): Auto overlapped reads, copy on the system core, read ahead,
 * small transfers, DMA copy, GPU-safe Non-DMA, Statistics off. Streams,
 * large, small and scattered reads all give the disk's data; DMA makes the
 * large copies, Auto learns without Statistics, nothing is left behind. */
static void test_defaults_together(void)
{
	uint8_t *u = line_buf(1100 * 512);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;

	reset();
	dmac_calls = dmac_fail = dmac_corrupt = 0;
	use_copy_out = 1;
	g_dmac_memcpy = fake_dmac;
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.verify = VM_DEF_VERIFY;
	s.hud = VM_DEF_HUD;
	s.log = VM_DEF_LOG;
	s.small = VM_DEF_SMALL;
	s.stats = VM_DEF_STATS;
	s.gpusafe = VM_DEF_GPUSAFE;
	s.pipeline = VM_DEF_PIPELINE;
	s.offload = VM_DEF_OFFLOAD;
	s.readahead = VM_DEF_READAHEAD;
	s.overlay = VM_DEF_OVERLAY;
	s.dmacopy = VM_DEF_DMACOPY;
	CHECK(s.stats == 0 && s.dmacopy == 1 && s.readahead == VM_RA_ADAPTIVE && s.small == 1 &&
	      s.gpusafe == VM_GPUSAFE_NODMA && s.pipeline == VM_PIPE_AUTO &&
	      s.offload == VM_OFFLOAD_ALL);
	vm_stage_apply(&s);
	g_live = 1;
	vm_stage_worker_tick();
	g_dcache_clean = fake_clean;
	clean_calls = 0;
	CHECK(d->pipe_made && d->ra_made && d->ra_ready && dma_state() == VM_DMA_ON);

	for (uint32_t i = 0; i < 12; i++) {           /* a stream of 128 KiB reads */
		memset(u, 0, 256 * 512);
		CHECK(rd(u, i * 256, 256) == 0 && memcmp(u, disk + (size_t)i * 256 * 512, 256 * 512) == 0);
	}
	CHECK(g_ra_hits >= 6 && g_ra_bad == 0);
	memset(u, 0, 1100 * 512);                     /* elsewhere, large: overlapped */
	copy_cost_us = 300;                           /* a copy that takes time: Auto learns it */
	CHECK(rd(u, 2900, 1100) == 0 && memcmp(u, disk + 2900 * 512, 1100 * 512) == 0);
	copy_cost_us = 0;
	for (uint32_t i = 0; i < 40; i++) {           /* scattered 4 KiB (small transfers) */
		const uint32_t at = (i * 977u) % (DISK_SECTORS - 8);

		memset(u, 0, 8 * 512);
		CHECK(rd(u, at, 8) == 0 && memcmp(u, disk + (size_t)at * 512, 8 * 512) == 0);
	}
	for (uint32_t i = 0; i < 4; i++) {            /* writes, read back */
		memcpy(u, disk + 100 * 512, 64 * 512);
		CHECK(wr(u, 1000 + i * 64, 64, REF_RD) == 0);
	}
	memset(u, 0, 256 * 512);
	CHECK(rd(u, 1000, 256) == 0);
	for (uint32_t i = 0; i < 4; i++)
		CHECK(memcmp(u + (size_t)i * 64 * 512, disk + 100 * 512, 64 * 512) == 0);

	CHECK(dmac_calls > 0 && g_dma_copies > 0 && g_dma_errors == 0 && dma_state() == VM_DMA_ON);
	CHECK(d->est_cp_ns[0] != 0 || d->est_cp_ns[1] != 0);   /* Auto learnt, Statistics off */
	CHECK(d->tm.rd_sectors == 0 && d->rd_bytes == 0);   /* no tester figures */
	CHECK(d->st.errors == 0 && d->st.mismatches == 0 && !d->fallback);
	vm_stage_fini();
	CHECK(fake_threads_live == 0 && fake_semas_live == 0);
	g_dmac_memcpy = NULL;
	use_copy_out = 0;
	g_live = saved_live;
	free(u);
}

/* GPU-safe "Non-DMA": the copies the DMA controller made are not cleaned, the
 * CPU's still are; "All" cleans both. */
static void test_gpu_safe_non_dma(void)
{
	uint8_t *u = line_buf(1100 * 512);
	VmSettings s;

	reset();
	dmac_calls = dmac_fail = dmac_corrupt = 0;
	use_copy_out = 1;
	g_dmac_memcpy = fake_dmac;
	s = dma_settings(1);
	s.gpusafe = VM_GPUSAFE_NODMA;
	vm_stage_apply(&s);
	g_dcache_clean = fake_clean;
	clean_calls = 0; clean_lo = clean_hi = 0; clean_copy_seen = 0; clean_user = NULL;
	CHECK(rd(u, 10, 1100) == 0);                     /* 256 + 256 + 38 KiB: all DMA */
	CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
	CHECK(dmac_calls == 3 && clean_calls == 0 && g_gpu_dma_kib == 256 + 256 + 38);
	CHECK(rd(u, 0, 512 + 20) == 0);                  /* 256 KiB DMA, 10 KiB the CPU's */
	CHECK(memcmp(u, disk, (512 + 20) * 512) == 0);
	CHECK(dmac_calls == 4 && clean_calls == 1);       /* only the CPU's copy */

	/* DMA copy off: every copy is the CPU's, every one cleaned */
	s.dmacopy = 0;
	vm_stage_apply(&s);
	g_dcache_clean = fake_clean;
	clean_calls = 0;
	CHECK(rd(u, 10, 1100) == 0 && clean_calls == 3 && dmac_calls == 4);

	/* "All": the DMA copies cleaned too */
	s.dmacopy = 1;
	s.gpusafe = VM_GPUSAFE_ALL;
	vm_stage_apply(&s);
	g_dcache_clean = fake_clean;
	clean_calls = 0;
	CHECK(rd(u, 10, 1100) == 0 && clean_calls == 3 && dmac_calls == 7);
	CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
	g_dcache_clean = NULL;
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
	/* the internal storage asked for too: no buffer, no hook of its own (the
	 * SceSdif MMC functions are never hooked) */
	CHECK(n == 2 && fake_alloc_calls == 2 && fake_hook_calls == 5);   /* 4 + the app events */
	CHECK(g_dev[VM_DEV_INTERNAL].buf == NULL && !g_dev[VM_DEV_INTERNAL].enabled);
	vm_stage_fini();
	CHECK(fake_hook_releases == 5 && fake_free_calls == 2);
	count_free_bufs(&n);
	CHECK(n == 0);
	vm_stage_apply(&s);                       /* after fini: nothing allocated again */
	count_free_bufs(&n);
	CHECK(n == 0 && fake_alloc_calls == 2);
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
	CHECK(n == 0 && fake_free_calls == 2);    /* all buffers given back */
	CHECK(fake_hook_releases == 1);           /* and the one hook that was live */
}

static void test_init_memcard_hook_fails_only_memcard_stock(void)
{
	VmSettings s = devices_on(1, 1, 1);
	VmDevState out[VM_DEV_COUNT];

	boot_state();
	vm_stage_apply(&s);
	fake_hook_fail_at = 4;                    /* the memory card's write hook fails */
	CHECK(vm_stage_init() == VM_OK);
	CHECK(g_dev[VM_DEV_MEMCARD].buf == NULL && fake_free_calls == 1);
	CHECK(g_dev[VM_DEV_SD2VITA].buf != NULL && g_dev[VM_DEV_INTERNAL].buf == NULL);
	CHECK(fake_hook_releases == 1);           /* the memory card's read hook */
	s.memcard = 0;                            /* switching it again allocates nothing */
	vm_stage_apply(&s);
	s.memcard = 1;
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_MEMCARD].buf == NULL && fake_alloc_calls == 2);
	vm_stage_get(out, 1);
	CHECK(out[VM_DEV_MEMCARD].status == VM_ST_NO_BUFFER);
	vm_stage_fini();
	CHECK(fake_hook_releases == 4);   /* 1 + the two SDIF hooks + the app events */
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

	s.internal = 1;                           /* the internal storage: never */
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_INTERNAL].buf == NULL && !g_dev[VM_DEV_INTERNAL].enabled &&
	      fake_alloc_calls == 1);
	s.internal = 0;
	s.sd2vita = 0;                            /* off again: the buffer is kept */
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_SD2VITA].buf != NULL && fake_free_calls == 0);
	s.sd2vita = 1;
	vm_stage_apply(&s);
	CHECK(fake_alloc_calls == 1);

	fake_alloc_fail = 1;                      /* the memory card: no memory left */
	s.memcard = 1;
	vm_stage_apply(&s);
	vm_stage_apply(&s);
	CHECK(g_dev[VM_DEV_MEMCARD].buf == NULL && fake_alloc_calls == 2);   /* tried once */
	vm_stage_get(out, 1);
	CHECK(out[VM_DEV_MEMCARD].status == VM_ST_NO_BUFFER);
	vm_stage_worker_tick();
	CHECK(g_dev[VM_DEV_MEMCARD].alloc_logged == 1);
	fake_alloc_fail = 0;
	s.memcard = 0;
	vm_stage_apply(&s);
	s.memcard = 1;
	vm_stage_apply(&s);                       /* off and on: a fresh try */
	CHECK(g_dev[VM_DEV_MEMCARD].buf != NULL && fake_alloc_calls == 3);
	vm_stage_get(out, 1);
	CHECK(out[VM_DEV_MEMCARD].status == VM_ST_ACTIVE);
	vm_stage_fini();
	CHECK(fake_free_calls == 2);
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
		while (__atomic_load_n(&g_reader_reads, __ATOMIC_ACQUIRE) <= i)
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

/* "C + NC": a game's read into non-cacheable memory is
 * read by the helper straight into the game's buffer (mapped, non-cacheable
 * too); the game's thread reads nothing itself. Small reads, system threads,
 * other settings, verify, a busy device, a cached or unwritable mapping: the
 * standard path, as before. A failed read is read again the standard way. */
static pthread_t ncd_io_thread;
static int ncd_io(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	ncd_io_thread = pthread_self();
	return fake_io(ref, part, sector, buf, n);
}

static int ncd_rd(uint8_t *u, uint32_t sector, uint32_t n)
{
	return stage_xfer(&g_dev[M], 0, ncd_io, REF_RD, 0, NULL, sector, u, n);
}

static void test_ncd(void)
{
	uint8_t *u = user_buf(2048), *other = user_buf(2048);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;
	uint32_t nc;

	reset();
	offload_on(&s, 0, VM_OFFLOAD_FULL, 1);
	CHECK(d->pipe_made && d->pipe_ready);
	fake_pid = 0x42;
	fake_memtype = MEMTYPE_NC;                 /* the game's buffer and its mapping */
	fake_meminfo_type = 0x10208006;            /* non-cacheable, read and write */

	/* 128 KiB, 256 KiB and 1 MiB: on the helper, into the buffer itself */
	{
		static const uint32_t sizes[] = { 128, 256, 512, 2048 };

		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			const uint32_t n = sizes[i], at = (uint32_t)(i * 211) % 1000;

			memset(u, 0, (size_t)n * 512);
			io_calls = 0;
			ncd_io_thread = pthread_self();
			CHECK(ncd_rd(u, at, n) == 0);
			CHECK(memcmp(u, disk + (size_t)at * 512, (size_t)n * 512) == 0);
			CHECK(io_calls == 1 && last_buf == u);            /* one read, no staging */
			CHECK(!pthread_equal(ncd_io_thread, pthread_self()));   /* by the helper */
			CHECK(fake_usermap_live == 0 && sems_empty(d) && d->busy == 0);
		}
		CHECK(g_ncd_reads == 4 && g_ncd_state == 1 && g_ncd_kib == (128 + 256 + 512 + 2048) / 2);
		CHECK(g_nc_stock_kib == 0 && d->st.reads == 0 && d->st.errors == 0);
		CHECK(fake_meminfo_calls == fake_usermap_calls && fake_usermap_calls == 4);
	}

	/* under 64 KiB, a system thread, verify on: the standard path, nothing mapped */
	nc = g_nc_stock_kib;
	io_calls = 0;
	CHECK(ncd_rd(u, 5, 64) == 0 && memcmp(u, disk + 5 * 512, 64 * 512) == 0);
	CHECK(io_calls == 1 && last_buf == u && pthread_equal(ncd_io_thread, pthread_self()));
	fake_pid = KERNEL_PID;
	CHECK(ncd_rd(u, 7, 256) == 0 && memcmp(u, disk + 7 * 512, 256 * 512) == 0);
	CHECK(pthread_equal(ncd_io_thread, pthread_self()));
	fake_pid = 0x42;
	g_verify = 1;
	CHECK(ncd_rd(u, 9, 256) == 0 && pthread_equal(ncd_io_thread, pthread_self()));
	g_verify = 0;
	CHECK(g_nc_stock_kib == nc + (64 + 256 + 256) / 2 && fake_usermap_calls == 4 && g_ncd_reads == 4);

	/* the device busy (another request): this one standard, counted */
	d->busy = 1;
	CHECK(ncd_rd(u, 11, 256) == 0 && memcmp(u, disk + 11 * 512, 256 * 512) == 0);
	CHECK(g_ncd_busy == 1 && pthread_equal(ncd_io_thread, pthread_self()));
	d->busy = 0;

	/* the read fails on the helper: read again the standard way, all of it */
	io_calls = 0;
	fail_at_call = 1;
	memset(u, 0, 256 * 512);
	CHECK(ncd_rd(u, 13, 256) == 0 && memcmp(u, disk + 13 * 512, 256 * 512) == 0);
	CHECK(io_calls == 2 && g_ncd_fail == 1 && fake_usermap_live == 0 && sems_empty(d));
	fail_at_call = 0;

	/* device memory (not non-cacheable): standard, nothing mapped, not refused */
	fake_memtype = MEMTYPE_DEVICE;
	CHECK(ncd_rd(u, 15, 256) == 0 && pthread_equal(ncd_io_thread, pthread_self()));
	CHECK(fake_usermap_calls == 5 && g_ncd_state == 1);
	fake_memtype = MEMTYPE_NC;

	/* the mapping cached: refused until the setting changes; reads standard */
	fake_usermap_elsewhere = other;
	fake_memtype_from = other;
	fake_memtype_to = other + 2048 * 512;
	fake_memtype_above = 0xD000;
	g_ncd_state = 0;
	CHECK(ncd_rd(u, 17, 256) == 0 && memcmp(u, disk + 17 * 512, 256 * 512) == 0);
	CHECK(g_ncd_state == -2 && fake_usermap_live == 0);
	{
		const int maps = fake_usermap_calls;

		CHECK(ncd_rd(u, 19, 256) == 0 && fake_usermap_calls == maps);   /* not tried again */
	}
	fake_usermap_elsewhere = NULL;
	fake_memtype_from = fake_memtype_to = NULL;
	fake_memtype_above = 0xD000;
	vm_stage_worker_tick();                       /* logs the refusal once */
	CHECK(g_ncd_logged == -2);

	/* switching the setting checks afresh; a mapping without write: refused */
	s.offload = VM_OFFLOAD_ALWAYS;
	vm_stage_apply(&s);
	CHECK(g_ncd_state == 0);
	CHECK(ncd_rd(u, 21, 256) == 0 && g_ncd_reads == 4);   /* Always: uncached stays standard */
	s.offload = VM_OFFLOAD_FULL;
	vm_stage_apply(&s);
	fake_meminfo_type = 0x10208004;               /* no read bit */
	CHECK(ncd_rd(u, 23, 256) == 0 && memcmp(u, disk + 23 * 512, 256 * 512) == 0);
	CHECK(g_ncd_state == -1 && fake_usermap_live == 0);

	/* cached reads with "C + NC": copied on the system core */
	fake_meminfo_type = 0x1020D006;
	fake_memtype = 0xD000;
	copies_here = copies_elsewhere = 0;
	CHECK(rd(u, 25, 256) == 0 && memcmp(u, disk + 25 * 512, 256 * 512) == 0);
	CHECK(copies_here == 0 && copies_elsewhere >= 1 && g_off_reads >= 1);

	/* C + NC R/A: cached reads on the copier; without read ahead non-cacheable
	 * reads stay standard */
	s.offload = VM_OFFLOAD_ALL;
	vm_stage_apply(&s);
	copies_here = copies_elsewhere = 0;
	CHECK(rd(u, 27, 256) == 0 && memcmp(u, disk + 27 * 512, 256 * 512) == 0);
	CHECK(copies_here == 0 && copies_elsewhere >= 1);
	fake_memtype = MEMTYPE_NC;
	fake_meminfo_type = 0x10208006;
	{
		const uint32_t reads = g_ncd_reads;

		CHECK(ncd_rd(u, 29, 256) == 0 && memcmp(u, disk + 29 * 512, 256 * 512) == 0);
		CHECK(g_ncd_reads == reads && fake_usermap_live == 0 && sems_empty(d) && d->busy == 0);
	}

	vm_stage_fini();
	CHECK(fake_threads_live == 0 && fake_semas_live == 0 && fake_usermap_live == 0);
	g_live = saved_live;
	free(u);
	free(other);
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
		offload_on(&s, 1, VM_OFFLOAD_ALWAYS, pipeline);
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
	offload_on(&s, 0, VM_OFFLOAD_OFF, 1);
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
	/* ... and described for the log: once, by the worker */
	CHECK(g_mapfail_n == 1 && g_mapfail[0].ready && g_mapfail[0].which == 0 &&
	      g_mapfail[0].err == (int)0x80020001 && g_mapfail[0].pid == 0x42 &&
	      g_mapfail[0].len == 700 * 512 && g_mapfail[0].nseg >= 1 && g_mapfail[0].nblk >= 1 &&
	      g_mapfail[0].blk[0].info == 0 && g_mapfail[0].seg[0].off == 0);
	{                                                 /* the stretches cover the buffer */
		uint32_t cov = 0;

		for (int k = 0; k < g_mapfail[0].nseg; k++) {
			CHECK(g_mapfail[0].seg[k].off == cov && g_mapfail[0].seg[k].uid >= 0);
			CHECK(g_mapfail[0].seg[k].map == (int)0x80020001);   /* the fake refuses them all */
			cov += g_mapfail[0].seg[k].len;
		}
		CHECK(cov == 700 * 512 && !g_mapfail[0].more && fake_usermap_live == 0);
	}

	vm_stage_worker_tick();
	CHECK(g_mapfail_logged == 1);
	vm_stage_worker_tick();
	CHECK(g_mapfail_logged == 1);
	fake_usermap_err = 0;
	CHECK(rd(u, 3, 700) == 0 && memcmp(u, disk + 3 * 512, 700 * 512) == 0);
	CHECK(g_off_reads >= 1 && g_offload_state == 1);
	vm_stage_fini();

	/* as PCSB00724's: the buffer starts in no block - described stretch by
	 * stretch: the start with no block (does not map), the rest in a block
	 * (maps alone); the data is the caller's copy */
	reset();
	offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
	fake_pid = 0x42;
	fake_noblock_lo = (uintptr_t)u;
	fake_noblock_hi = ((uintptr_t)u & ~(uintptr_t)4095) + 8192;   /* its first page or two */
	CHECK(rd(u, 3, 700) == 0 && memcmp(u, disk + 3 * 512, 700 * 512) == 0);
	CHECK(g_off_maperr == 1 && g_mapfail_n == 1 && g_mapfail[1 - 1].ready);
	{
		const MapFail *f = &g_mapfail[0];

		CHECK(f->nseg == 2 && f->seg[0].off == 0 && f->seg[0].uid < 0 && f->seg[0].map < 0);
		CHECK(f->seg[1].off == f->seg[0].len && f->seg[1].uid >= 0 && f->seg[1].map == 0);
		CHECK(f->seg[0].len + f->seg[1].len == 700 * 512 && f->nblk == 1 &&
		      f->blk[0].uid == f->seg[1].uid);
	}
	vm_stage_worker_tick();
	CHECK(g_mapfail_logged == 1 && fake_usermap_live == 0);
	fake_noblock_lo = fake_noblock_hi = 0;
	vm_stage_fini();

	/* ... and as on the console, where the caller sees no memory type on
	 * that first page either: mapped from the next page, nothing refused;
	 * the chunk that takes the first page is the caller's, the rest the
	 * copier's; every byte right; the mapping gone after */
	reset();
	offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
	fake_pid = 0x42;
	{
		const uintptr_t pg = ((uintptr_t)u + 4095) & ~(uintptr_t)4095;   /* u's next page */

		fake_noblock_lo = (uintptr_t)u;
		fake_noblock_hi = pg;
		fake_memtype_from = u;
		fake_memtype_to = (const void *)pg;
		fake_memtype_above = -1;
		if (pg == (uintptr_t)u) {                      /* u on a page: its first page */
			fake_noblock_hi = pg + 4096;
			fake_memtype_to = (const void *)(pg + 4096);
		}
	}
	copies_here = copies_elsewhere = 0;
	memset(u, 0, 700 * 512);
	CHECK(rd(u, 3, 700) == 0 && memcmp(u, disk + 3 * 512, 700 * 512) == 0);
	CHECK(g_off_maperr == 0 && g_mapfail_n == 0 && g_off_headed == 1 && g_offload_state == 1);
	CHECK(copies_here > 0 && copies_elsewhere > 0 && g_off_reads >= 1);
	CHECK(fake_usermap_live == 0 && sems_empty(d));
	/* a read that is one chunk: all the caller's, the mapping still unmapped */
	copies_here = copies_elsewhere = 0;
	memset(u, 0, 64 * 512);
	CHECK(rd(u, 9, 64) == 0 && memcmp(u, disk + 9 * 512, 64 * 512) == 0);
	CHECK(copies_elsewhere == 0 && fake_usermap_live == 0 && sems_empty(d));
	fake_noblock_lo = fake_noblock_hi = 0;
	fake_memtype_from = fake_memtype_to = NULL;
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

/* "Read ahead": while a stream is read (five staged reads in a row,
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

	/* on by default; switched off here: nothing made for it */
	CHECK(VM_DEF_READAHEAD == VM_RA_ADAPTIVE && vm_setting_valid(VM_KEY_READAHEAD, VM_RA_ON) &&
	      vm_setting_valid(VM_KEY_READAHEAD, VM_RA_ON) && !vm_setting_valid(VM_KEY_READAHEAD, 3));
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
	 * by the copier through its non-cacheable mapping (only with an NC
	 * setting: "Always" covers cached reads only), nothing cleaned; with an
	 * NC setting the first reads go to the system core too, straight in */
	for (int mode = 0; mode < 4; mode++) {
		static const int modes[] = { VM_OFFLOAD_OFF, VM_OFFLOAD_ALWAYS, VM_OFFLOAD_ALL,
		                             VM_OFFLOAD_FULL };
		const int offl = vm_offload_nc(modes[mode]), direct = vm_offload_direct(modes[mode]);

		reset();
		ra_settings(&s, 1, modes[mode], VM_PIPE_AUTO);
		g_count_cores = 1;                        /* the overlay open: where they ran */
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
		CHECK(g_ra_hits == 7 && g_ra_nc_kib == 7 * 128);
		CHECK(direct ? g_nc_stock_kib == 0 && g_ncd_reads == 5 && g_ncd_kib == 5 * 128
		           : g_nc_stock_kib == 5 * 128 && g_ncd_reads == 0);
		CHECK(d->st.reads == 0 && d->st.errors == 0 && !d->fallback);
		CHECK(g_nc_off_refused == 0 && g_offload_state >= 0);
		/* graphics data counts as NC, in "Core 3 NC", not as copies (C) */
		{
			uint32_t core[4], ncs, nco;

			vm_stage_core_stats(core, &ncs, &nco);
			CHECK(core[0] + core[1] + core[2] + core[3] == 0);
			CHECK(offl ? g_ra_nc_sys_kib == 7 * 128 && g_ra_nc_own_kib == 0
			           : g_ra_nc_own_kib == 7 * 128 && g_ra_nc_sys_kib == 0);
			CHECK(ncs == (direct ? 5u * 128 : 0u) + g_ra_nc_sys_kib &&
			      nco == (direct ? 0u : 5u * 128) + g_ra_nc_own_kib);
		}
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
	ra_settings(&s, 0, VM_OFFLOAD_ALL, VM_PIPE_AUTO);
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

	/* small transfers too (as the tester's small files): reads under 32 KiB
	 * never start a stream ... */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	g_min_sectors = 1;
	for (uint32_t i = 0; i < 40; i++)
		CHECK(rd_ok(u, i * 32, 32));
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
	__sync_fetch_and_add(&fake_now_us, RA_LIVE_US);
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
	for (uint32_t i = 5; i < 12; i++)
		CHECK(rd_ok(u, i * 256, 256));            /* a stream again from here: three pieces */
	for (int i = 0; i < RA_PIECES; i++)
		ra_wait(d, i);
	CHECK(wr(w, 3900, 8, REF_RD) == 0);           /* elsewhere: nothing marked */
	{
		int dirty = 0;

		for (int i = 0; i < RA_PIECES; i++)
			dirty += d->ra[i].dirty;
		CHECK(dirty == 0);
		CHECK(wr(w, 12 * 256 + 300, 8, REF_RD) == 0);   /* into the second piece */
		for (int i = 0; i < RA_PIECES; i++)
			dirty += d->ra[i].dirty;
		CHECK(dirty == 1);
	}
	{
		const uint32_t hits = g_ra_hits;

		CHECK(rd_ok(u, 12 * 256, 256) && g_ra_hits == hits + 1);   /* the clean piece */
		CHECK(rd_ok(u, 13 * 256, 256) && u[44 * 512] == 0x77 && g_ra_bad == 1);
	}
	vm_stage_fini();

	/* a read larger than the pieces hold: their part, then the rest as
	 * before */
	reset();
	ra_settings(&s, 1, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 7; i++)                  /* step by step: three pieces by now */
		CHECK(rd_ok(u, i * 256, 256));
	{
		const uint32_t hits = g_ra_hits, kib = g_ra_kib;

		CHECK(rd_ok(u, 7 * 256, 1100));
		CHECK(g_ra_hits == hits + 1 && g_ra_kib == kib + 3 * 128 && ra_idle(d) == 0);
		CHECK(rd_ok(u, 7 * 256 + 1100, 256) && g_ra_hits == hits + 2);
	}
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
	__sync_fetch_and_add(&fake_now_us, RA_MAX_AGE_US);
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

/* The app in front. */
static const char *fake_title_of(SceUID pid)
{
	return pid == 0x42 ? "PCSE00001" : pid == 0x43 ? "PCSE00001"
	     : pid == 0x44 ? "PCSB00777" : pid == 0x45 ? "../bad" : pid == 0x46 ? "VITASHELL"
	     : pid == 0x47 ? "NPXS10015" : pid == 0x48 ? "main"
	     : pid == (SceUID)0x80410023 ? "PCSE00042" : NULL;
}

static int fake_title(SceUID pid, char *t, SceSize len)
{
	const char *s = fake_title_of(pid);

	if (!s)
		return -1;
	snprintf(t, len, "%s", s);
	return 0;
}


/* `pid` comes to the front (started / resumed: its process event) and
 * reads from now on */
static void as_app(SceUID pid)
{
	fake_pid = pid;
	ra_proc_event(pid, 1);
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
	Dev *in = &g_dev[VM_DEV_SD2VITA];              /* the second storage */
	uint8_t *staging2 = calloc(1, STAGE_BYTES);
	pthread_t th[5];
	StressArg args[3] = { { M, 1, 0, 1, 0 }, { M, 2, 0, 0, 0 }, { VM_DEV_SD2VITA, 3, 0, 1, 0 } };
	int saved_live = g_live;
	const Dev saved_in = *in;                       /* put back as it was, for the next tests */

	reset();
	as_app(0x42);                                /* a game: copy on the system core too */
	g_title_fn = fake_title;
	in->buf = staging2;
	in->enabled = 1;
	in->fallback = 0;
	in->unusable = 0;                               /* an earlier vm_stage_fini set it */
	in->ra_th = in->ra_go = in->ra_done = -1;
	in->uid = -1;
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.sd2vita = 1;
	s.internal = 1;                                 /* asked for: never on */
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
		s.readahead = (i / 5) % 4 == 0 ? VM_RA_OFF : VM_RA_ON;
		vm_stage_apply(&s);
		vm_stage_worker_tick();
		sched_yield();
	}
	/* read ahead left on until the card's stream has used it (the other
	 * card reader paused: it would break the stream) */
	s.readahead = 1;
	vm_stage_apply(&s);
	vm_stage_worker_tick();                         /* at least once: g_ra_hits may be > 0 already */
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
	CHECK(in->pipe_made);
	CHECK(!g_dev[VM_DEV_INTERNAL].enabled && !g_dev[VM_DEV_INTERNAL].pipe_made &&
	      !g_dev[VM_DEV_INTERNAL].ra_made && g_dev[VM_DEV_INTERNAL].buf == NULL);
	vm_stage_fini();
	CHECK(fake_threads_live == 0 && fake_semas_live == 0);
	*in = saved_in;
	g_live = saved_live;
	free(staging2);
}

/* What a review found. */
static void on_game(VmSettings *s, SceUID pid)
{
	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	ra_settings(s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	g_title_fn = fake_title;
	as_app(pid);
	vm_stage_worker_tick();
}

/* A tiny read in order, from their block once it has started. */
static void test_tiny_reads(void)
{
	static uint8_t u[64 * 512];
	VmSettings s;
	Dev *const d = &g_dev[M];
	uint32_t at = 8;

	on_game(&s, 0x44);
	CHECK(d->t_buf != NULL);
	for (int k = 0; k < RA_TINY_STREAK - 1; k++, at++)
		CHECK(rd_ok(u, at, 1));
	CHECK(g_ra_tiny_reads == 0);
	{
		const int calls = __atomic_load_n(&io_calls, __ATOMIC_RELAXED);

		CHECK(rd_ok(u, at, 1));                       /* the 8th: one read of the block */
		at++;
		CHECK(g_ra_tiny_reads == 1 && io_calls == calls + 1);
		for (int k = 1; k < RA_TINY_SECTORS; k++, at++)
			CHECK(rd_ok(u, at, 1));
		CHECK(g_ra_tiny_reads == RA_TINY_SECTORS && io_calls == calls + 1);   /* no more reads */
		CHECK(rd_ok(u, at, 1));                       /* past it: the next block */
		at++;
		CHECK(io_calls == calls + 2);
		/* of several sectors, and across its end: the next block */
		CHECK(rd_ok(u, at, 8));
		at += 8;
		CHECK(io_calls == calls + 2);
	}
	/* elsewhere: as before, its block is of no use now; a stream again */
	{
		const uint32_t t = g_ra_tiny_reads;

		CHECK(rd_ok(u, 2000, 1));
		CHECK(g_ra_tiny_reads == t && d->t_streak == 1 && LD(d->t_end) == LD(d->t_start));
	}
	/* a write into it: not used, and the new data is read */
	at = 2100;
	for (int k = 0; k < RA_TINY_STREAK; k++, at++)
		CHECK(rd_ok(u, at, 1));
	{
		static uint8_t w[512];

		memset(w, 0x5A, sizeof(w));
		CHECK(wr(w, at + 4, 1, REF_RD) >= 0);
		disk[(at + 4) * 512] = 0x5A;
		CHECK(rd_ok(u, at, 1));
		for (int k = 1; k < 8; k++)
			CHECK(rd_ok(u, at + k, 1));
		CHECK(rd(u, at + 4, 1) == 0 && u[0] == 0x5A);
	}
	/* too old: not used */
	at = 2300;
	for (int k = 0; k < RA_TINY_STREAK; k++, at++)
		CHECK(rd_ok(u, at, 1));
	{
		const int calls = io_calls;

		__sync_fetch_and_add(&fake_now_us, RA_MAX_AGE_US + 1);
		CHECK(rd_ok(u, at, 1));
		CHECK(io_calls == calls + 1);
	}
	/* a read of its block fails (past the card's end): this one the standard
	 * way, the device stays on the faster path */
	at = 2400;
	for (int k = 0; k < RA_TINY_STREAK - 1; k++, at++)
		CHECK(rd_ok(u, at, 1));
	fail_at_call = io_calls + 1;
	{
		const int calls = io_calls;

		CHECK(rd_ok(u, at, 1) && io_calls == calls + 2 && !d->fallback && d->st.errors == 0);
	}
	fail_at_call = 0;
	vm_stage_fini();

	/* verify mode: what it gave is read again and compared */
	on_game(&s, 0x44);
	s.verify = 1;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	at = 300;
	for (int k = 0; k < RA_TINY_STREAK + 8; k++, at++)
		CHECK(rd_ok(u, at, 1));
	CHECK(d->st.verified >= 8 && d->st.mismatches == 0 && !d->fallback);
	vm_stage_fini();

	/* read ahead off: never */
	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	s.readahead = VM_RA_OFF;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	at = 300;
	for (int k = 0; k < RA_TINY_STREAK + 8; k++, at++)
		CHECK(rd_ok(u, at, 1));
	CHECK(g_ra_tiny_reads == 0 && !d->ra_made && !d->t_buf && d->t_uid < 0);
	vm_stage_fini();
}

/* The app in front's own settings, kept in its profile. */
/* The same without the worker's look after each run. */
static void adapt_runs_raw(int nc, uint32_t len, int count)
{
	for (int r = 0; r < count; r++) {
		for (uint32_t i = 1; i <= len; i++)
			ra_adapt_read(0, nc, i, 256);
		ra_adapt_end(&g_dev[M], 0, nc, len);
	}
}

/* `count` runs of `len` reads of 128 KiB in a row into cached (nc 0) or
 * non-cacheable memory, as read ahead's streams tell Adaptive of them. */
static void adapt_runs(int nc, uint32_t len, int count)
{
	for (int r = 0; r < count; r++) {
		for (uint32_t i = 1; i <= len; i++)
			ra_adapt_read(0, nc, i, 256);
		ra_adapt_end(&g_dev[M], 0, nc, len);
		ra_adapt_tick();                                 /* the worker, at once */
	}
}

/* Read ahead Adaptive: every RA_ADAPT_RUNS runs of a kind, the threshold
 * 2..8 that would have brought the most over them (as the R/A Test weighs
 * it), if better than the one in effect by more than the margin; each kind
 * on its own, at once rather than step by step, down as readily as up.
 * "never" stays never, On never moves, and what is in effect changing starts
 * afresh. Then the same from reads. */
static void test_readahead_adaptive(void)
{
	static uint8_t u[256 * 512];
	VmSettings s;
	VmApp a;
	Dev *d = &g_dev[M];

	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	CHECK(vm_setting_valid(VM_KEY_READAHEAD, VM_RA_ADAPTIVE) && !vm_setting_valid(VM_KEY_READAHEAD, 3));
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);   /* On */
	CHECK(ra_threshold(0) == 5 && ra_threshold(1) == 5);

	s.readahead = VM_RA_ADAPTIVE;
	vm_stage_apply(&s);
	CHECK(g_ra_mode == VM_RA_ADAPTIVE && g_readahead && ra_threshold(0) == 5);
	/* long runs: the sooner the better - down to 2 at once */
	adapt_runs(0, 20, RA_ADAPT_RUNS - 1);
	CHECK(ra_threshold(0) == 5);                         /* a round not yet over */
	adapt_runs(0, 20, 2);
	CHECK(ra_threshold(0) == 2 && ra_threshold(1) == 5 && g_ra_moves[0] == 1);   /* NC as it was */
	/* runs of 4: a read ahead that begins before their end only costs; from 5
	 * on nothing either way */
	adapt_runs(0, 4, 4 * RA_ADAPT_RUNS);
	CHECK(ra_threshold(0) == 5);
	/* runs of 5: at 5 a piece read for nothing each - one later */
	adapt_runs(0, 5, 4 * RA_ADAPT_RUNS);
	CHECK(ra_threshold(0) == 6);
	/* runs of 2 or 3 alone: nothing either way from 4 on - it stays */
	adapt_runs(0, 3, 4 * RA_ADAPT_RUNS);
	CHECK(ra_threshold(0) == 6);
	/* a run or two of another length moves nothing (the margin) */
	adapt_runs(0, 20, 1);
	adapt_runs(0, 3, RA_ADAPT_RUNS);
	CHECK(ra_threshold(0) == 6);
	/* as Gravity Rush reads graphics data: mostly single reads, now and then
	 * a long run - 3 pays best (the long run's tail read for nothing is
	 * outweighed); a rare run is not lost when the counts are halved */
	for (int r = 0; r < 6; r++) {
		adapt_runs(1, 1, 26);
		adapt_runs(1, 2, 5);
		adapt_runs(1, 50, 1);
	}
	CHECK(ra_threshold(1) == 3 && g_ad_runs[1][10] == 6);
	CHECK(g_ad.run[1][10] > 0);                          /* still weighed after the halving */
	/* NC on its own: runs of 8 - from 2 */
	adapt_runs(1, 8, 2 * RA_ADAPT_RUNS);
	CHECK(ra_threshold(1) == 2 && ra_threshold(0) == 6);
	vm_stage_apply(&s);                                  /* in effect again: afresh */
	CHECK(ra_threshold(0) == 5 && ra_threshold(1) == 5 && g_ad.ended[0] == 0 && g_ad.run[1][8] == 0);
	/* followed at no extra cost: from read ahead's own stream, no lookup of
	 * its own (the non-cacheable check: both ends, two) */
	fake_memtype = 0x8000;
	fake_memtype_calls = 0;
	CHECK(rd(u, 0, 256) == 0 && fake_memtype_calls == 2 && g_ad.total_sectors == 256);
	fake_memtype = 0xD000;
	ra_adapt_reset();
	/* On: nothing followed, nothing moves */
	s.readahead = 1;
	vm_stage_apply(&s);
	CHECK(rd(u, 0, 256) == 0);
	CHECK(g_ad.total_sectors == 0 && ra_threshold(0) == 5);
	s.readahead = VM_RA_ADAPTIVE;
	vm_stage_apply(&s);

	/* an app's own thresholds are where it starts; its "never" stays */
	g_title_fn = fake_title;
	as_app(0x44);
	vm_stage_worker_tick();
	ra_app_global(&a);
	a.ra = VM_RA_ADAPTIVE;
	a.thr[0] = 3;
	a.thr[1] = VM_RA_NEVER;
	CHECK(vm_stage_app_set(&a) == VM_OK && g_ra_mode == VM_RA_ADAPTIVE);
	CHECK(ra_threshold(0) == 3 && ra_threshold(1) == VM_RA_NEVER);
	adapt_runs(0, 5, 2 * RA_ADAPT_RUNS);
	adapt_runs(1, 20, 2 * RA_ADAPT_RUNS);
	CHECK(ra_threshold(0) == 6 && ra_threshold(1) == VM_RA_NEVER);
	/* while the R/A Test runs: the standard 5 and 5, nothing followed */
	CHECK(vm_stage_ratest_start() == VM_OK);
	CHECK(ra_threshold(0) == 5 && ra_threshold(1) == 5);
	{
		const uint32_t before = g_ad.total_sectors;

		CHECK(rd(u, 0, 256) == 0);
		CHECK(g_ad.total_sectors == before);
	}
	vm_stage_ratest_cancel();
	CHECK(ra_threshold(0) == 6 && ra_threshold(1) == VM_RA_NEVER);   /* back as it was */
	as_app(0x42);                                        /* another in front: afresh */
	vm_stage_worker_tick();
	CHECK(ra_threshold(0) == 5 && g_ra_adj[0] == 0);
	vm_stage_fini();

	/* from reads: runs of 5 reads in a row, each elsewhere, leave the piece
	 * read after the fifth unused - it goes to 6 */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	s.readahead = VM_RA_ADAPTIVE;
	vm_stage_apply(&s);
	CHECK(d->ra_ready && ra_threshold(0) == 5);
	/* the runs are read ahead's own streams: 7 reads in a row, a small one
	 * between (neither part of a stream nor a break), then a read elsewhere */
	for (uint32_t i = 0; i < 7; i++) {
		CHECK(rd(u, i * 256, 256) == 0);
		if (i == 3)
			CHECK(rd(u, 3500, 8) == 0);
	}
	CHECK(rd(u, 2048, 256) == 0);
	CHECK(g_ad.run[0][7] == RA_ADAPT_ONE && g_ad.ended[0] == 1 && g_ad.pos[0][7] == 256 * RA_ADAPT_ONE &&
	      g_ad_runs[0][7] == 1);
	ra_adapt_reset();
	for (uint32_t run = 0; run < 3 * RA_ADAPT_RUNS && ra_threshold(0) == 5; run++) {
		const uint32_t at = (run % 2) * 2048;

		for (uint32_t i = 0; i < 5; i++)
			CHECK(rd(u, at + i * 256, 256) == 0 && memcmp(u, disk + (size_t)(at + i * 256) * 512, 256 * 512) == 0);
		for (int k = 0; k < RA_PIECES; k++)               /* the pieces read, then ... */
			for (int t = 0; t < 2000 && ra_pending(__atomic_load_n(&d->ra[k].state, __ATOMIC_ACQUIRE)); t++)
				usleep(500);
		vm_stage_worker_tick();                          /* the worker weighs a round due */
	}
	CHECK(ra_threshold(0) == 6 && g_ra_moves[0] > 0);
	/* a round due is weighed by the worker, not by the request that ended it */
	ra_adapt_reset();
	adapt_runs_raw(0, 20, RA_ADAPT_RUNS);
	CHECK(g_ad_due[0] && ra_threshold(0) == 5);              /* not yet */
	vm_stage_worker_tick();
	CHECK(!g_ad_due[0] && ra_threshold(0) == 2);
	CHECK(d->st.errors == 0 && !d->fallback);
	vm_stage_fini();
	g_title_fn = NULL;
}

/* Calibration: off, nothing measured and the fixed figures in use; on, a
 * stream's reads before read ahead began count as from the card, those from
 * the pieces as from them, and a piece dropped unused as one read for
 * nothing; with enough of each, the medians' difference and the mean are
 * what Adaptive weighs by - a storage where the pieces save nothing leaves
 * read ahead as late as it goes. Kept in a file, its switch too; a damaged
 * file is not used. */
static void test_calibration(void)
{
	static uint8_t u[256 * 512];
	VmSettings s;
	VmCal c;
	CalDev *k = &g_cal[M];
	const char *dir;

	fakes_tmpdir_remove();
	dir = fakes_tmpdir_new();
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	vm_stage_cal(M, &c);
	CHECK(!c.on && !c.have[0] && !c.have[1] && !c.have_waste && g_cal_loaded);
	CHECK(c.gain16[0] == RT_GAIN_C_US * 16 && c.gain16[1] == RT_GAIN_NC_US * 16 &&
	      c.waste_us == RT_WASTE_US);
	for (uint32_t i = 0; i < 8; i++)                      /* off: nothing counted */
		CHECK(rd(u, i * 256, 256) == 0);
	CHECK(!k->n_plain[0] && !k->n_served[0] && !g_cal_dirty);

	vm_stage_cal_set(1);
	CHECK(g_cal_on);
	/* a stream: the first read starts it, the 2nd .. 5th from the card, the
	 * pieces read after the 5th; then a read elsewhere drops one unused */
	for (uint32_t i = 0; i < 9; i++) {
		CHECK(rd(u, i * 256, 256) == 0 && memcmp(u, disk + (size_t)(i * 256) * 512, 256 * 512) == 0);
		for (int j = 0; j < RA_PIECES; j++)
			for (int t = 0; t < 2000 && ra_pending(__atomic_load_n(&g_dev[M].ra[j].state, __ATOMIC_ACQUIRE)); t++)
				usleep(500);
	}
	CHECK(k->n_plain[0] == 4 && k->n_served[0] >= 3 && !k->n_plain[1]);
	CHECK(rd(u, 3584, 256) == 0);
	CHECK(k->waste_n >= 1 && g_cal_dirty);
	/* the clock only when needed: a small request with no piece being read
	 * costs a look at the pieces; with one being read, that piece notes it */
	{
		Dev *d = &g_dev[M];
		int held = 1, calls = fake_time_calls;

		CHECK(cal_start(d, 8, &held) == 0 && !held && fake_time_calls == calls);
		CHECK(cal_start(d, 256, &held) != 0 && !held && fake_time_calls == calls + 1);
		__atomic_store_n(&d->ra[1].state, RA_READING, __ATOMIC_RELEASE);   /* as the thread */
		d->ra[1].wait_from = 0;
		CHECK(cal_start(d, 8, &held) == 0 && held && d->ra[1].wait_from != 0);
		__atomic_store_n(&d->ra[1].state, RA_FREE, __ATOMIC_RELEASE);
		d->ra[1].wait_from = 0;
	}

	/* enough measured: what it is in use (us per 128 KiB in the middle of
	 * their buckets: 7125 from the card, 4125 from the pieces) */
	memset(k, 0, sizeof(*k));
	k->plain[0][28] = VM_CAL_MIN_PLAIN;
	k->n_plain[0] = VM_CAL_MIN_PLAIN;
	k->served[0][16] = VM_CAL_MIN_SERVED - 1;
	k->n_served[0] = VM_CAL_MIN_SERVED - 1;
	k->waste_us = 1500 * VM_CAL_MIN_WASTE;
	k->waste_n = VM_CAL_MIN_WASTE;
	vm_stage_cal(M, &c);
	CHECK(!c.have[0] && c.gain16[0] == RT_GAIN_C_US * 16);   /* one short */
	CHECK(c.have_waste && c.waste_us == 1500);
	k->served[0][16]++;
	k->n_served[0]++;
	vm_stage_cal(M, &c);
	CHECK(c.have[0] && c.gain16[0] == (7125 - 4125) / 16 && !c.have[1] &&
	      c.gain16[1] == RT_GAIN_NC_US * 16);
	vm_stage_cal(VM_DEV_SD2VITA, &c);                    /* another storage: its own */
	CHECK(!c.have[0] && !c.have_waste);

	/* written only when worth it (internal storage): what is in use now is
	 * what was saved - more of the same, nothing to write */
	vm_stage_cal_set(1);                                 /* saved: the snapshot */
	CHECK(!cal_worth_saving());
	k->plain[0][28] += 40;                               /* the same medians */
	k->n_plain[0] += 40;
	k->served[0][16] += 40;
	k->n_served[0] += 40;
	CHECK(!cal_worth_saving());
	k->served[0][15] += 400;                             /* served median 3875: 8.6 % more gain */
	k->n_served[0] += 400;
	CHECK(cal_worth_saving());
	vm_stage_cal_set(1);
	CHECK(!cal_worth_saving());
	/* still learning: a count grown by half is kept, a little is not */
	k->n_plain[1] = 20;
	k->plain[1][20] = 20;
	CHECK(cal_worth_saving());
	vm_stage_cal_set(1);
	k->n_plain[1] += 5;
	k->plain[1][20] += 5;
	CHECK(!cal_worth_saving());
	/* a figure coming into use: written */
	k->n_plain[1] = VM_CAL_MIN_PLAIN;
	k->plain[1][20] = VM_CAL_MIN_PLAIN;
	k->n_served[1] = VM_CAL_MIN_SERVED;
	k->served[1][2] = VM_CAL_MIN_SERVED;
	CHECK(cal_worth_saving());
	memset(k->plain[1], 0, sizeof(k->plain[1]));
	memset(k->served[1], 0, sizeof(k->served[1]));
	k->n_plain[1] = k->n_served[1] = 0;
	k->served[0][15] -= 400;                             /* back as the checks below want it */
	k->n_served[0] -= 400;
	k->plain[0][28] -= 40;
	k->n_plain[0] -= 40;
	k->served[0][16] -= 40;
	k->n_served[0] -= 40;

	/* kept: the file, the switch with it */
	vm_stage_cal_set(1);
	memset(g_cal, 0, sizeof(g_cal));
	g_cal_on = 0;
	g_cal_loaded = 0;
	cal_load();
	CHECK(g_cal_on && k->n_plain[0] == VM_CAL_MIN_PLAIN && k->served[0][16] == VM_CAL_MIN_SERVED);
	/* a damaged file: not used */
	{
		char path[256];
		FILE *f;

		snprintf(path, sizeof(path), "%s/calib.bin", dir);
		f = fopen(path, "r+b");
		CHECK(f != NULL);
		if (f) {
			fseek(f, 40, SEEK_SET);
			fputc(0x5A, f);
			fclose(f);
		}
		memset(g_cal, 0, sizeof(g_cal));
		g_cal_on = 0;
		g_cal_loaded = 0;
		cal_load();
		CHECK(!g_cal_on && !k->n_plain[0]);
	}

	/* runs of 6, the pieces measured to save nothing here: Adaptive starts
	 * read ahead where no piece is read past a run's end (7); by the fixed
	 * figures it would start soonest */
	s.readahead = VM_RA_ADAPTIVE;
	vm_stage_apply(&s);
	memset(k, 0, sizeof(*k));
	k->plain[0][16] = k->served[0][16] = 200;
	k->n_plain[0] = k->n_served[0] = 200;
	adapt_runs(0, 6, 2 * RA_ADAPT_RUNS);
	CHECK(ra_threshold(0) == 7);
	memset(k, 0, sizeof(*k));                            /* the fixed figures again */
	adapt_runs(0, 6, 2 * RA_ADAPT_RUNS);
	CHECK(ra_threshold(0) == 2);
	/* the margin is three pieces at what one costs here: one long run among
	 * single reads saves 8.4 ms sooner - under three fixed pieces (9 ms),
	 * over three measured at 1 ms */
	vm_stage_apply(&s);
	memset(k, 0, sizeof(*k));
	adapt_runs(0, 1, RA_ADAPT_RUNS - 1);
	adapt_runs(0, 20, 1);
	CHECK(ra_threshold(0) == 5);
	vm_stage_apply(&s);
	k->waste_us = 1000 * VM_CAL_MIN_WASTE;
	k->waste_n = VM_CAL_MIN_WASTE;
	adapt_runs(0, 1, RA_ADAPT_RUNS - 1);
	adapt_runs(0, 20, 1);
	CHECK(ra_threshold(0) == 2);
	/* reset: nothing measured any more, the fixed figures, the file too */
	vm_stage_cal_reset();
	vm_stage_cal(M, &c);
	CHECK(!c.have_waste && c.waste_us == RT_WASTE_US && !k->waste_n);
	k->waste_n = 99;
	g_cal_loaded = 0;
	cal_load();
	CHECK(!k->waste_n);
	CHECK(g_dev[M].st.errors == 0 && !g_dev[M].fallback);
	vm_stage_cal_set(0);
	CHECK(!g_cal_on);
	vm_stage_fini();
}

/* The log's split of read ahead by stream: 32 KiB reads in a row take the
 * pieces after 16 of them; what they use and what is read for nothing when
 * the run ends are counted apart from the large reads'. */
static void test_readahead_by_size(void)
{
	static uint8_t u[256 * 512];
	VmSettings s;
	Dev *d = &g_dev[M];

	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	for (uint32_t i = 0; i < 24; i++) {
		CHECK(rd(u, i * 64, 64) == 0 && memcmp(u, disk + (size_t)(i * 64) * 512, 64 * 512) == 0);
		for (int k = 0; k < RA_PIECES; k++)
			for (int t = 0; t < 2000 && ra_pending(__atomic_load_n(&d->ra[k].state, __ATOMIC_ACQUIRE)); t++)
				usleep(500);
	}
	CHECK(rd(u, 3500, 256) == 0);                        /* elsewhere: the pieces dropped */
	CHECK(g_rs_kib > 0 && g_rs_kib == g_ra_kib);         /* all of it the 32 KiB reads' */
	CHECK(g_rs_drops == 1 && g_rs_waste_kib > 0 && g_rs_waste_kib == g_ra_waste_kib);
	CHECK(d->st.errors == 0 && !d->fallback);
	vm_stage_fini();
}

/* Adaptive on tiny reads: runs of one-sector reads in a row move (T, B)
 * from the fixed (8, 32 KiB) to what pays most - long runs from the 2nd read
 * with the biggest block, runs of four a small one - and the data stay
 * right; On keeps the fixed ones. With calibration on, reads from the block
 * and block reads are timed. */
static void tiny_run(uint32_t at, uint32_t reads)
{
	static uint8_t u[512];

	for (uint32_t i = 0; i < reads; i++)
		CHECK(rd(u, at + i, 1) == 0 && memcmp(u, disk + (size_t)(at + i) * 512, 512) == 0);
	ra_adapt_tick();                                     /* the worker, at once */
}

static void test_tiny_adaptive(void)
{
	VmSettings s;

	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);   /* On */
	CHECK(g_dev[M].t_buf && tiny_thr() == RA_TINY_STREAK && tiny_blk() == RA_TINY_SECTORS);
	s.readahead = VM_RA_ADAPTIVE;
	vm_stage_apply(&s);
	vm_stage_cal_set(1);
	for (uint32_t r = 0; r <= RA_ADAPT_RUNS; r++)
		tiny_run(r * 100, 40);
	CHECK(g_tiny_thr == 2 && g_tiny_blk == 64 && g_tn_moves == 1 && tiny_thr() == 2);
	CHECK(g_tn_runs[rt_tiny_bucket(40)] == RA_ADAPT_RUNS);
	CHECK(g_cal[M].tiny_hit_n > 0 && g_cal[M].tiny_blk_n > 0 &&
	      g_cal[M].tiny_blk_sec == g_cal[M].tiny_blk_n * 64u);
	/* afresh, runs of four - weighed by the worker, not the reads */
	vm_stage_apply(&s);
	CHECK(g_tiny_thr == RA_TINY_STREAK && g_tiny_blk == RA_TINY_SECTORS);
	for (uint32_t r = 0; r <= RA_ADAPT_RUNS; r++) {
		static uint8_t u1[512];

		for (uint32_t i = 0; i < 4; i++)
			CHECK(rd(u1, r * 100 + i, 1) == 0);
	}
	CHECK(g_tn_due && g_tiny_thr == RA_TINY_STREAK);
	vm_stage_worker_tick();
	CHECK(!g_tn_due && g_tiny_thr == 2 && g_tiny_blk == 16);
	tiny_run(3900, 20);                                   /* a run now read in 8 KiB blocks */
	/* On: the fixed ones, whatever Adaptive had */
	s.readahead = 1;
	vm_stage_apply(&s);
	CHECK(tiny_thr() == RA_TINY_STREAK && tiny_blk() == RA_TINY_SECTORS);
	/* a version 1 file (before the tiny reads' figures): its counts kept,
	 * the tiny ones from 0; saved again as version 2 */
	{
		char path[256];
		CalHead h = { CAL_MAGIC, 1u, 1u, VM_DEV_COUNT };
		CalDev one[VM_DEV_COUNT];
		uint32_t c = 0x811C9DC5u;
		FILE *f;

		memset(one, 0, sizeof(one));
		one[M].n_plain[0] = 70;
		one[M].plain[0][20] = 70;
		snprintf(path, sizeof(path), "%s/calib.bin", fakes_tmpdir_new());
		f = fopen(path, "wb");
		fwrite(&h, sizeof(h), 1, f);
		c = cal_fnv(c, &h, sizeof(h));
		for (int i = 0; i < VM_DEV_COUNT; i++) {
			fwrite(&one[i], CAL_DEV_V1, 1, f);
			c = cal_fnv(c, &one[i], CAL_DEV_V1);
		}
		c ^= 0xA5C3A5C3u;
		fwrite(&c, sizeof(c), 1, f);
		fclose(f);
		memset(g_cal, 0xEE, sizeof(g_cal));
		g_cal_on = 0;
		g_cal_loaded = 0;
		cal_load();
		CHECK(g_cal_on && g_cal[M].n_plain[0] == 70 && g_cal[M].plain[0][20] == 70 &&
		      !g_cal[M].tiny_hit_n && !g_cal[M].tiny_blk_us);
		g_cal[M].tiny_hit_n = 5;
		cal_save();
		memset(g_cal, 0, sizeof(g_cal));
		g_cal_loaded = 0;
		cal_load();
		CHECK(g_cal[M].n_plain[0] == 70 && g_cal[M].tiny_hit_n == 5);
	}
	vm_stage_fini();
}

/* "B" in the HUD and the overlay: the app in front's buffers for the system
 * core since it was started - none yet (or the setting off), all mapped,
 * some from their 2nd page, some refused; another app starts afresh. */
static void test_bufmap_state(void)
{
	VmSettings s;
	VmRaNow rn;

	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	offload_on(&s, 1, VM_OFFLOAD_ALWAYS, 1);
	g_title_fn = fake_title;
	as_app(0x42);
	vm_stage_worker_tick();
	vm_stage_ra_now(&rn);
	CHECK(rn.bufmap == VM_BUFMAP_NONE);              /* nothing mapped yet */
	ADD(g_off_reads, 1);
	vm_stage_ra_now(&rn);
	CHECK(rn.bufmap == VM_BUFMAP_OK);
	ADD(g_off_headed, 1);
	vm_stage_ra_now(&rn);
	CHECK(rn.bufmap == VM_BUFMAP_WORKAROUND);
	ADD(g_ncd_maperr, 1);
	vm_stage_ra_now(&rn);
	CHECK(rn.bufmap == VM_BUFMAP_REFUSED);
	as_app(0x44);                                    /* another app: afresh */
	vm_stage_worker_tick();
	vm_stage_ra_now(&rn);
	CHECK(rn.bufmap == VM_BUFMAP_NONE);
	ADD(g_off_reads, 1);
	vm_stage_ra_now(&rn);
	CHECK(rn.bufmap == VM_BUFMAP_OK);
	s.offload = VM_OFFLOAD_OFF;                       /* the setting off: nothing shown */
	vm_stage_apply(&s);
	vm_stage_ra_now(&rn);
	CHECK(rn.bufmap == VM_BUFMAP_NONE);
	vm_stage_fini();
	g_title_fn = NULL;
	as_app(0x42);                                    /* as test_app_settings expects to change */
}

static void test_app_settings(void)
{
	VmSettings s;
	VmApp a;
	VmAppNow an;
	RaProf pf;

	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	g_title_fn = fake_title;
	ra_app_global(&a);
	CHECK(vm_stage_app_set(&a) == VM_ERR_NOT_READY && vm_stage_app_save() == VM_ERR_NOT_READY);
	as_app(0x44);
	vm_stage_worker_tick();
	vm_stage_app_now(&an);
	CHECK(!strcmp(an.title, "PCSB00777") && an.cur.ra == VM_APP_GLOBAL &&
	      !memcmp(&an.cur, &an.saved, sizeof(an.cur)));
	a.ra = 3;                                         /* past Adaptive: no value */
	CHECK(vm_stage_app_set(&a) == VM_ERR_ARG);
	ra_app_global(&a);
	a.ra = VM_RA_ON;
	a.thr[0] = 3;
	a.thr[1] = VM_RA_NEVER;
	CHECK(vm_stage_app_set(&a) == VM_OK && vm_stage_app_save() == VM_OK);
	CHECK(ra_threshold(0) == 3 && ra_threshold(1) == VM_RA_NEVER);
	CHECK(ra_prof_load("PCSB00777", &pf) && pf.app.thr[0] == 3);
	/* suspended: the global ones; its profile deleted by hand keeps its title */
	ra_proc_event(0x44, 4);
	vm_stage_worker_tick();
	CHECK(ra_threshold(0) == RA_STREAK);
	ra_proc_event(0x44, 5);
	vm_stage_worker_tick();
	vm_stage_app_now(&an);
	CHECK(!strcmp(an.title, "PCSB00777") && app_in_front() && ra_threshold(0) == 3);
	/* all global again: no profile */
	ra_app_global(&a);
	CHECK(vm_stage_app_set(&a) == VM_OK && vm_stage_app_save() == VM_OK);
	CHECK(!ra_prof_load("PCSB00777", &pf));
	/* system apps have none */
	as_app(0x47);
	vm_stage_worker_tick();
	vm_stage_app_now(&an);
	CHECK(!strcmp(an.title, "PCSB00777"));            /* it stays the app in front */
	vm_stage_fini();
}

/* Profiles: versions, checks, and never half written. */
static void test_profiles(void)
{
	VmSettings s;
	RaProf pf;
	char path[96];

	on_game(&s, 0x44);
	memset(&pf, 0, sizeof(pf));
	ra_app_global(&pf.app);
	pf.app.pipe = VM_PIPE_FIXED;
	ra_prof_save("PCSB00777", &pf);
	fake_write_short = 1;
	pf.app.pipe = VM_PIPE_OFF;
	ra_prof_save("PCSB00777", &pf);
	fake_write_short = 0;
	CHECK(ra_prof_load("PCSB00777", &pf) && pf.app.pipe == VM_PIPE_FIXED);
	pf.app.pipe = VM_PIPE_OFF;
	fake_rename_fail = 1;
	ra_prof_save("PCSB00777", &pf);
	fake_rename_fail = 0;
	CHECK(ra_prof_load("PCSB00777", &pf) && pf.app.pipe == VM_PIPE_OFF);
	CHECK(ra_prof_remove("PCSB00777") < 0 && !ra_prof_load("PCSB00777", &pf));
	/* one that is not right: not used */
	{
		RaProfileFile v;
		SceUID fd;

		memset(&v, 0, sizeof(v));
		v.magic = RA_PROFILE_MAGIC;
		v.version = RA_PROFILE_VERSION;
		v.ra = VM_RA_ON;
		v.offload = v.pipe = v.gpusafe = -1;
		v.check = ra_profile_check(&v);
		ra_profile_path(path, sizeof(path), "PCSE00001");
		ksceIoMkdir("ur0:data", 0777);
		ksceIoMkdir(VM_DATA_DIR, 0777);
		ksceIoMkdir(RA_PROFILE_DIR, 0777);
		fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		ksceIoWrite(fd, &v, sizeof(v));
		ksceIoClose(fd);
		CHECK(ra_prof_load("PCSE00001", &pf) && pf.app.ra == VM_RA_ON);
		v.check ^= 1;
		fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		ksceIoWrite(fd, &v, sizeof(v));
		ksceIoClose(fd);
		CHECK(!ra_prof_load("PCSE00001", &pf));
		v.check ^= 1;
		v.ra = 3;                                     /* out of range */
		v.check = ra_profile_check(&v);
		fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		ksceIoWrite(fd, &v, sizeof(v));
		ksceIoClose(fd);
		CHECK(!ra_prof_load("PCSE00001", &pf));
	}
	vm_stage_fini();
}

/* What a review found. */
static void test_review_fixes(void)
{
	static uint8_t u[64 * 512 * 16];
	VmSettings s;
	Dev *const d = &g_dev[M];

	/* read ahead switched on with no helper: both made on the one wake-up,
	 * the worker may sleep after it */
	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.readahead = VM_RA_ON;
	vm_stage_apply(&s);
	g_live = 1;
	ensure_buf(d);
	vm_stage_worker_tick();
	CHECK(d->pipe_made && d->ra_ready && !vm_stage_pending());
	vm_stage_fini();

	/* the HUD by the overlay's keys, Statistics and the HUD's setting off */
	reset();
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.overlay = 1;
	vm_stage_apply(&s);
	CHECK(!g_count_all);
	vm_stage_count_hud(1);
	CHECK(g_count_all);
	vm_stage_apply(&s);                               /* a setting changed meanwhile */
	CHECK(g_count_all);
	vm_stage_count_hud(0);
	CHECK(!g_count_all);
	s.stats = 1;
	vm_stage_apply(&s);
	vm_stage_count_hud(0);
	CHECK(g_count_all);

	/* 32 KiB reads: a stream of their own. Sixteen in a row start read ahead,
	 * every read still gets the disk's data; one elsewhere ends it */
	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	{
		uint32_t at = 64;
		const uint32_t h0 = g_ra_hits;

		for (int k = 0; k < RA_SMALL_STREAK - 1; k++, at += 64)
			CHECK(rd_ok(u, at, 64));
		CHECK(!ra_any(d) && d->rs_streak == RA_SMALL_STREAK - 1 && !d->ra_streak);
		CHECK(rd_ok(u, at, 64));                      /* it starts */
		at += 64;
		CHECK(ra_any(d) && d->ra_small);
		for (int k = 0; k < 40; k++, at += 64)
			CHECK(rd_ok(u, at, 64));
		CHECK(g_ra_hits > h0 + 20);
		CHECK(rd_ok(u, 3000, 64));
	}
	/* ... and the large reads' stream is not broken by one, nor loses its
	 * pieces to one (the cached 32 KiB read goes the standard way, live) */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	{
		uint32_t at = 0;

		for (int k = 0; k < RA_STREAK; k++, at += 256)
			CHECK(rd_ok(u, at, 256));
		CHECK(ra_any(d) && !d->ra_small && d->ra_streak == RA_STREAK);
		CHECK(rd_ok(u, 3500, 64));
		CHECK(ra_any(d) && d->ra_streak == RA_STREAK);
		{
			const uint32_t h = g_ra_hits;

			CHECK(rd_ok(u, at, 256));
			CHECK(g_ra_hits == h + 1 && d->ra_streak == RA_STREAK + 1);
		}
	}
	vm_stage_fini();

	/* a 32 KiB read with the device busy goes the standard way, and its
	 * stream goes on; and small reads between them leave the pieces */
	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	{
		uint32_t at = 64;

		for (int k = 0; k < 5; k++, at += 64)
			CHECK(rd_ok(u, at, 64));
		d->busy = 1;
		CHECK(rd_ok(u, at, 64));
		d->busy = 0;
		at += 64;
		CHECK(d->rs_streak == 6);
		for (int k = 0; k < RA_SMALL_STREAK; k++, at += 64)
			CHECK(rd_ok(u, at, 64));
		CHECK(ra_any(d) && d->ra_small);
		{
			const uint32_t drops = g_ra_drops;

			__sync_fetch_and_add(&fake_now_us, RA_LIVE_US / 2);
			g_min_sectors = 1;
			CHECK(rd_ok(u, 3900, 1));                 /* live: not dropped */
			CHECK(g_ra_drops == drops && ra_any(d));
			g_min_sectors = MIN_SECTORS;
		}
	}
	vm_stage_fini();

	/* the verify buffer given back once verify is off, not while it is used;
	 * a request that looked just before gets none */
	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	reset();
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	s.verify = 1;
	vm_stage_apply(&s);
	g_live = 1;
	vm_stage_worker_tick();
	CHECK(g_shadow != NULL);
	s.verify = 0;
	vm_stage_apply(&s);
	g_shadow_busy = 1;
	vm_stage_worker_tick();
	CHECK(g_shadow != NULL && vm_stage_pending());
	g_shadow_busy = 0;
	vm_stage_worker_tick();
	CHECK(g_shadow == NULL && g_shadow_uid < 0 && !vm_stage_pending());
	s.verify = 1;
	vm_stage_apply(&s);
	vm_stage_worker_tick();
	CHECK(g_shadow != NULL && take_shadow() != NULL);
	g_shadow_busy = 0;
	vm_stage_fini();
}

/* The overlay's read ahead test: only the app it was started for, nothing
 * changed by it; a stream gets read ahead picked whatever read ahead is set to. */
static void test_ra_test_hook(void)
{
	static uint8_t u[256 * 512];
	VmSettings s;
	VmRaTestPick p;
	Dev *const d = &g_dev[M];
	uint32_t mib = 0;

	reset();
	memset(&s, 0, sizeof(s));
	s.memcard = 1;
	vm_stage_apply(&s);
	g_title_fn = fake_title;
	/* no app in front: not started */
	CHECK(vm_stage_ratest_start() == VM_ERR_NOT_READY && !vm_stage_ratest_running(NULL));
	as_app(0x42);
	vm_stage_worker_tick();
	CHECK(vm_stage_ratest_start() == VM_OK && vm_stage_ratest_running(&mib) && mib == 0);
	/* read ahead Off for it, reads as ever: a long stream, cached */
	for (uint32_t i = 0; i < 12; i++)
		CHECK(rd(u, i * 256, 256) == 0);
	CHECK(!d->ra_made);
	CHECK(vm_stage_ratest_running(&mib) && mib > 0);
	CHECK(!vm_stage_ratest_stale());
	vm_stage_ratest_stop(&p);
	CHECK(!vm_stage_ratest_running(NULL));
	CHECK(p.little && p.mib == mib);                  /* too little to tell */
	vm_stage_ratest_start();
	for (uint32_t k = 0; k < 60; k++)
		for (uint32_t i = 0; i < 12; i++) {
			__sync_fetch_and_add(&fake_now_us, 1000);
			CHECK(rd(u, k * 7 + i * 256, 256) == 0);
		}
	vm_stage_ratest_stop(&p);
	CHECK(!p.little && p.ra == VM_RA_ON && p.thr[0] >= 2 && p.thr[0] <= 8 && p.offload == VM_OFFLOAD_ALWAYS);
	/* another app in front: its reads do not count, and the test is stale */
	vm_stage_ratest_start();
	as_app(0x44);
	vm_stage_worker_tick();
	CHECK(vm_stage_ratest_stale());
	CHECK(rd(u, 0, 256) == 0);
	CHECK(g_rt.total_sectors == 0);
	vm_stage_ratest_cancel();
	CHECK(!vm_stage_ratest_running(NULL) && !vm_stage_ratest_stale());
	/* off: its look costs nothing, and counts nothing */
	CHECK(rd(u, 0, 256) == 0 && g_rt.total_sectors == 0);
	g_title_fn = NULL;
	vm_stage_fini();
}

/* A slow stream: a read every second gets what was read for it, nothing is
 * dropped for its age; a pause too long ends the stream, and nothing is read
 * again until it has started afresh. */
static void test_ra_slow_stream(void)
{
	uint8_t *u = user_buf(256);
	VmSettings s;
	Dev *d = &g_dev[M];
	int saved_live = g_live;
	uint32_t at = 0, hits;

	reset();
	ra_settings(&s, 0, VM_OFFLOAD_OFF, VM_PIPE_AUTO);
	fake_pid = 0x42;
	fake_memtype = MEMTYPE_NC;
	fake_meminfo_type = 0x10208006;
	for (int i = 0; i < 8; i++, at += 256) {
		__sync_fetch_and_add(&fake_now_us, 1000000);
		CHECK(rd_ok(u, at, 256));
	}
	CHECK(g_ra_hits >= 2 && g_ra_bad == 0 && g_ra_drops == 0 && d->ra_streak == 8);
	/* a pause longer than the limit: over; the pieces left are not used */
	hits = g_ra_hits;
	for (int k = 0; k < RA_PIECES; k++)
		ra_wait(d, k);
	__sync_fetch_and_add(&fake_now_us, RA_MAX_AGE_US + 1000);
	CHECK(rd_ok(u, at, 256));
	at += 256;
	CHECK(d->ra_streak == 1 && g_ra_hits == hits && g_ra_bad == 1 && ra_idle(d));
	/* ... and nothing is read ahead for a stream read this slowly */
	{
		const int calls = __atomic_load_n(&io_calls, __ATOMIC_RELAXED);

		for (int i = 0; i < 6; i++, at += 256) {
			__sync_fetch_and_add(&fake_now_us, RA_MAX_AGE_US + 1000);
			CHECK(rd_ok(u, at, 256));
			CHECK(d->ra_streak == 1 && ra_idle(d));
		}
		CHECK(__atomic_load_n(&io_calls, __ATOMIC_RELAXED) == calls + 6);
	}
	vm_stage_fini();
	g_live = saved_live;
	free(u);
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
	RUN(test_ncd);
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
	RUN(test_ra_slow_stream);
	RUN(test_ra_test_hook);
	RUN(test_tiny_reads);
	RUN(test_readahead_adaptive);
	RUN(test_calibration);
	RUN(test_tiny_adaptive);
	RUN(test_readahead_by_size);
	RUN(test_bufmap_state);
	RUN(test_app_settings);
	RUN(test_profiles);
	RUN(test_review_fixes);
	RUN(test_pipeline_modes);
	RUN(test_pipeline_released_while_reading);
	RUN(test_problems_wake_the_worker);
	RUN(test_staged_read);
	RUN(test_large_read_is_split);
	RUN(test_read_error_finishes_stock);
	RUN(test_staged_write);
	RUN(test_write_error_finishes_stock);
	RUN(test_read_verify_mismatch);
	RUN(test_dma_copy_choice);
	RUN(test_dma_copy_reads);
	RUN(test_gpu_safe_non_dma);
	RUN(test_defaults_together);
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
