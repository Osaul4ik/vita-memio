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

/* The staging copy, with a time cost the tests can set (0 by default, so the
 * other tests' timings are io only). */
extern uint32_t fake_now_us;
static uint32_t copy_cost_us;
static void *fake_copy(void *d, const void *s, size_t n)
{
	__sync_fetch_and_add(&fake_now_us, copy_cost_us);
	return memcpy(d, s, n);
}
#define vm_copy_from_stage fake_copy
#define vm_copy_to_stage fake_copy

#include "../kernel/stage.c"

#include <stdio.h>
#include <stdlib.h>

#include "fakes.h"
#include "vm_test.h"

#define DISK_SECTORS 4096
#define REF_RD       1
#define REF_WR       2
#define M            VM_DEV_MEMCARD

static uint8_t disk[DISK_SECTORS * 512];
static uint8_t *staging;

static int io_calls, fail_at_call, corrupt_staged;
static void *last_buf;
static void *io_bufs[16];                 /* the buffer and size of each call */
static uint32_t io_ns[16];

/* The "system driver": ref says whether this is the read or the write function. */
static int fake_io(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	(void)part;
	int call = __sync_add_and_fetch(&io_calls, 1);

	last_buf = buf;
	if (call <= (int)(sizeof(io_bufs) / sizeof(io_bufs[0]))) {
		io_bufs[call - 1] = buf;
		io_ns[call - 1] = n;
	}
	__sync_fetch_and_add(&fake_now_us, 2500);
	if (fail_at_call && call == fail_at_call)
		return -5;
	if (ref == REF_RD) {
		memcpy(buf, disk + (size_t)sector * 512, (size_t)n * 512);
		if (corrupt_staged && buf == staging)
			((uint8_t *)buf)[0] ^= 0xFF;
	} else {
		memcpy(disk + (size_t)sector * 512, buf, (size_t)n * 512);
		if (corrupt_staged && buf == staging)
			disk[(size_t)sector * 512] ^= 0xFF;
	}
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
	d->uid2 = d->th = d->sem_go = d->sem_done = -1;
	d->buf = staging;
	d->enabled = 1;
	g_verify = 0;
	g_min_sectors = MIN_SECTORS;
	g_stats = 1;                              /* most tests look at the figures */
	g_gpu_safe = 0;
	g_dcache_clean = NULL;
	g_gpu_cleaned_kib = g_nc_stock_kib = 0;
	g_pipeline = 0;
	g_pipe_reads = g_pipe_reads3 = 0;
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

	s.gpusafe = 1;                                  /* on, but no clean function */
	vm_stage_apply(&s);
	g_dcache_clean = NULL;
	CHECK(rd(u, 0, 100) == 0 && memcmp(u, disk, 100 * 512) == 0);
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
	CHECK(d->pipe_made && d->pipe_ready && fake_threads_live == 1 && fake_semas_live == 2);
	CHECK(fake_thread_last_mask == PIPE_CPU_SYSTEM);   /* on the system core */
	CHECK(rd(u, 10, 1100) == 0);                    /* 512 + 512 + 76 */
	CHECK(memcmp(u, disk + 10 * 512, 1100 * 512) == 0);
	CHECK(io_calls == 5);                           /* split, split, not split */
	CHECK(io_bufs[0] == staging && io_bufs[1] == d->buf2);
	CHECK(io_ns[0] + io_ns[1] == 512 && io_ns[1] >= PIPE_MIN_TAIL && io_ns[1] <= 256);
	CHECK(io_bufs[2] == staging && io_bufs[3] == d->buf2 && io_ns[2] + io_ns[3] == 512);
	CHECK(io_bufs[4] == staging && io_ns[4] == 76);
	CHECK(g_pipe_reads == 2 && d->st.reads == 1 && d->st.errors == 0);
	CHECK(d->tm.rd_sectors == 1100);                /* all counted */

	/* many reads in a row, different sizes and places: always the disk's data */
	for (int k = 0; k < 200; k++) {
		uint32_t n = 128 + (uint32_t)(k * 37) % 900, at = (uint32_t)(k * 101) % 2000;

		memset(u, 0, n * 512);
		CHECK(rd(u, at, n) == 0);
		CHECK(memcmp(u, disk + (size_t)at * 512, (size_t)n * 512) == 0);
	}
	CHECK(d->st.errors == 0 && !d->fallback);

	/* 64 KiB and up are split, smaller is not */
	io_calls = 0;
	CHECK(rd(u, 0, 127) == 0 && io_calls == 1);
	io_calls = 0;
	CHECK(rd(u, 0, 128) == 0 && io_calls == 2);

	/* the plan: with the console's costs, 2 parts without GPU-safe and 3 with */
	{
		uint32_t e = 0, sz[PIPE_MAX_PARTS];

		d->est_io_ns = 25000; d->est_cp_ns = 4200; d->est_cmd_ns = 600000;
		CHECK(pipe_plan(d, 512, 0, sz) == 2);
		CHECK(sz[0] + sz[1] == 512 && sz[1] >= 50 && sz[1] <= 56);        /* ~54 */
		d->est_cp_ns = 11100;
		CHECK(pipe_plan(d, 512, 1, sz) == 3);
		CHECK(sz[0] + sz[1] + sz[2] == 512 && sz[2] >= 30 && sz[2] <= 40); /* ~35 */
		CHECK(sz[1] <= PIPE_TAIL_SECTORS);
		d->est_cmd_ns = 5000000;                       /* commands dear: fewer parts */
		CHECK(pipe_plan(d, 512, 1, sz) == 2);
		d->est_cmd_ns = 600000;
		d->est_cp_ns = 40000;                          /* a copy slower than the card */
		CHECK(pipe_plan(d, 512, 0, sz) == 2 && sz[0] == 256 && sz[1] == 256);
		d->est_io_ns = d->est_cp_ns = d->est_cmd_ns = 0;   /* nothing learnt: defaults */
		CHECK(pipe_plan(d, 512, 0, sz) == 2 && pipe_plan(d, 512, 1, sz) == 3);
		CHECK(pipe_plan(d, 128, 0, sz) >= 2 && pipe_plan(d, 256, 1, sz) >= 2);

		/* any costs and sizes: the parts cover n, fit their buffers, every
		 * part's copy is hidden by the next read, and no x or k is cheaper */
		int planned = 0, three = 0;

		for (uint32_t r = 1; r < 3000; r++) {
			uint32_t n = 128 + (r * 7919u) % 385, k;
			uint64_t io = 5000 + (r * 104729u) % 40000, cp = 1000 + (r * 1299709u) % 30000;
			uint64_t cmd = (r * 15485863u) % 2000000, best = ~(uint64_t)0, sum = 0, cost;

			d->est_io_ns = (uint32_t)io; d->est_cp_ns = (uint32_t)cp; d->est_cmd_ns = (uint32_t)(cmd ? cmd : 1);
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
					if (pipe_fit(n, (int)kk, x, io, cp, cmd, t) && kk * cmd + x * cp < best)
						best = kk * cmd + x * cp;
			}
			CHECK(cost == best);
		}
		CHECK(planned > 1000 && three > 100 && planned - three > 100);   /* both kinds seen */
		d->est_io_ns = 25000; d->est_cp_ns = 4200; d->est_cmd_ns = 600000;
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
		d->est_io_ns = d->est_cp_ns = d->est_cmd_ns = 0;
		pipe_learn(&e, 0, 0, 5000);
		CHECK(e == 0);                                             /* nothing to learn from */
		pipe_learn(&e, 8, 1, 5000);                                /* 8000 ns: 5000 + 3000/8 */
		CHECK(e == 5000 - 625 + 1000);
		for (int k = 0; k < 100; k++)
			pipe_learn(&e, 8, 1, 5000);
		CHECK(e > 7900 && e <= 8000);                              /* converges */
	}

	/* off again: not split, the helper stays (until the module stops) */
	s.pipeline = 0;
	vm_stage_apply(&s);
	io_calls = 0;
	CHECK(rd(u, 0, 512) == 0 && io_calls == 1 && last_buf == staging);
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

		g_stats = 0;                            /* the fake timings teach nothing useful */
		for (int k = 0; k < 150; k++) {
			uint32_t n = 128 + (uint32_t)(k * 53) % 900, at = (uint32_t)(k * 97) % 2000;

			memset(v, 0, n * 512);
			CHECK(rd(v, at, n) == 0);
			CHECK(memcmp(v, disk + (size_t)at * 512, (size_t)n * 512) == 0);
		}
		CHECK(g_pipe_reads3 > 100 && g_dev[M].st.errors == 0);
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
		CHECK(last_buf == u);                    /* finished stock */
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
	CHECK(d->pipe_made && fake_thread_last_mask == 0 && fake_threads_live == 1);
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

	for (int step = 0; step < 4; step++) {
		reset();
		if (step == 0) fake_alloc_fail = 1;
		if (step == 1) fake_sema_fail = 1;
		if (step == 2) fake_sema_fail = 2;
		if (step == 3) fake_thread_fail = 1;
		pipe_on(&s, 0);
		fake_alloc_fail = 0;
		CHECK(!d->pipe_made && !d->pipe_ready && d->pipe_failed);
		CHECK(fake_threads_live == 0 && fake_semas_live == 0);
		CHECK(d->uid2 < 0 && d->th < 0 && d->sem_go < 0 && d->sem_done < 0);
		vm_stage_worker_tick();
		CHECK(!d->pipe_made);                    /* not retried */
		io_calls = 0;
		CHECK(rd(u, 0, 512) == 0 && io_calls == 1 && memcmp(u, disk, 512 * 512) == 0);
		/* switched off and on again: a fresh try, which now works */
		fake_thread_fail = fake_sema_fail = 0;
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

int main(void)
{
	staging = calloc(1, STAGE_BYTES);

	RUN(test_small_goes_stock);
	RUN(test_small_transfers_switch);
	RUN(test_statistics_switch);
	RUN(test_gpu_safe_reads);
	RUN(test_noncacheable_buffers_go_stock);
	RUN(test_pipeline_reads);
	RUN(test_pipeline_gpu_safe);
	RUN(test_pipeline_errors);
	RUN(test_pipeline_not_with_verify);
	RUN(test_pipeline_any_core);
	RUN(test_pipeline_setup_fails);
	RUN(test_every_switch_on_the_fly);
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
