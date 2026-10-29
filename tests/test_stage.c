/*
 * test_stage.c - host tests for kernel/stage.c, the code that sits between the
 * system's storage driver and every read and write.
 *
 * stage.c is included (not linked) so the tests can reach its statics. The
 * "driver" is an in-memory disk behind fake_io(); the memory card slot
 * (TS_DEV_MEMCARD, port -1) is used because it needs no card context. Built with
 * AddressSanitizer + UBSan, so an out-of-range copy fails the run.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The staging copy, with a time cost the tests can set (0 by default, so the
 * other tests' timings are io only). */
extern uint32_t fake_now_us;
static uint32_t copy_cost_us;
static const void *last_copy_src;
static void *fake_copy(void *d, const void *s, size_t n)
{
	fake_now_us += copy_cost_us;
	last_copy_src = s;
	return memcpy(d, s, n);
}
#define ts_copy_from_stage fake_copy
#define ts_copy_to_stage fake_copy

#include "../kernel/stage.c"

#include <stdlib.h>

#include "fakes.h"
#include "ts_test.h"

#define DISK_SECTORS 4096
#define REF_RD       1
#define REF_WR       2
#define M            TS_DEV_MEMCARD

static uint8_t disk[DISK_SECTORS * 512];
static uint8_t *staging;

static int io_calls, fail_at_call, corrupt_staged;
static void *last_buf;

/* The "system driver": ref says whether this is the read or the write function. */
static int fake_io(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	(void)part;
	io_calls++;
	last_buf = buf;
	fake_now_us += 2500;
	if (fail_at_call && io_calls == fail_at_call)
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

	fakes_reset();
	for (size_t i = 0; i < sizeof(disk); i++)
		disk[i] = (uint8_t)(i * 31 + (i >> 9));
	memset(d, 0, sizeof(*d));
	d->port = -1;
	d->uid = -1;
	d->alias_uid = -1;
	d->buf = staging;
	d->enabled = 1;
	g_cached = 0;
	g_dc_tried = 0;
	g_dc_inv = NULL;
	g_dc_clean_inv = NULL;
	last_copy_src = NULL;
	g_verify = 0;
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
	return stage_read(&g_dev[M], fake_io, REF_RD, NULL, sector, u, n);
}

static int wr(const uint8_t *u, uint32_t sector, uint32_t n, tai_hook_ref_t rd_ref)
{
	return stage_write(&g_dev[M], fake_io, REF_WR, rd_ref, NULL, sector, (void *)u, n);
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

static void test_staged_read(void)
{
	uint8_t *u = user_buf(100);

	reset();
	CHECK(rd(u, 10, 100) == 0);
	CHECK(io_calls == 1 && last_buf == staging);
	CHECK(memcmp(u, disk + 10 * 512, 100 * 512) == 0);
	CHECK(g_dev[M].st.reads == 1 && g_dev[M].rd_bytes == 100 * 512);
	CHECK(g_dev[M].us_total == 2500 && g_dev[M].us_max == 2500);
	free(u);
}

static void test_large_read_is_split(void)
{
	uint8_t *u = user_buf(1000);

	reset();
	CHECK(rd(u, 5, 1000) == 0);
	CHECK(io_calls == 2);                     /* 512 + 488 sectors */
	CHECK(memcmp(u, disk + 5 * 512, 1000 * 512) == 0);
	CHECK(g_dev[M].us_total == 5000);
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
	TsSettings s;

	reset();
	memset(&s, 0, sizeof(s));
	s.verify = 1;
	fake_alloc_fail = 1;
	g_verify = 1;
	for (int i = 0; i < 5; i++)
		ts_stage_worker_tick();
	CHECK(fake_alloc_calls == 1);
	CHECK(g_shadow == NULL && g_shadow_uid < 0);

	/* turning verify off and on again is a fresh attempt */
	s.verify = 0;
	ts_stage_apply(&s);
	s.verify = 1;
	ts_stage_apply(&s);
	ts_stage_worker_tick();
	CHECK(fake_alloc_calls == 2);

	fake_alloc_fail = 0;
	s.verify = 0;
	ts_stage_apply(&s);
	s.verify = 1;
	ts_stage_apply(&s);
	ts_stage_worker_tick();
	CHECK(g_shadow != NULL && ts_stage_verify_ready());
	ts_stage_fini();
	CHECK(g_shadow == NULL);
}

static void test_get_reports_time(void)
{
	TsDevState out[TS_DEV_COUNT];
	uint8_t *u = user_buf(100);

	reset();
	CHECK(rd(u, 0, 100) == 0);
	ts_stage_get(out, 1);
	CHECK(out[M].status == TS_ST_ACTIVE);
	CHECK(out[M].time_ms == 2 && out[M].max_ms == 2);     /* 2500 us */
	ts_stage_get(out, 0);
	CHECK(out[M].status == TS_ST_UNAVAIL);
	free(u);
}

/* io and copy time are counted apart, per chunk that succeeded, and the
 * verify-mode re-read is in neither. */
static void test_timing_split(void)
{
	TsTimingDev t[TS_DEV_COUNT];
	uint8_t *u = user_buf(1100);

	reset();
	copy_cost_us = 300;
	CHECK(rd(u, 0, 1100) == 0);                    /* 512 + 512 + 76: 3 chunks */
	CHECK(wr(u, 0, 600, REF_RD) == 0);             /* 512 + 88: 2 chunks */
	ts_stage_timing(t);
	CHECK(t[M].rd_sectors == 1100 && t[M].rd_io_us == 3 * 2500 && t[M].rd_copy_us == 3 * 300);
	CHECK(t[M].wr_sectors == 600 && t[M].wr_io_us == 2 * 2500 && t[M].wr_copy_us == 2 * 300);
	CHECK(t[TS_DEV_SD2VITA].rd_sectors == 0 && t[TS_DEV_INTERNAL].wr_io_us == 0);

	/* small transfers stay stock and are not counted */
	CHECK(rd(u, 0, 8) == 0);
	ts_stage_timing(t);
	CHECK(t[M].rd_sectors == 1100);

	/* a failing chunk is not counted (the rest is finished stock) */
	reset();
	copy_cost_us = 300;
	fail_at_call = 2;
	CHECK(rd(u, 0, 1100) == 0);
	ts_stage_timing(t);
	CHECK(t[M].rd_sectors == 512 && t[M].rd_io_us == 2500 && t[M].rd_copy_us == 300);

	/* verify mode: the stock re-read is not in io time */
	reset();
	g_verify = 1;
	g_shadow = calloc(1, STAGE_BYTES);
	CHECK(rd(u, 0, 100) == 0);
	CHECK(io_calls == 2);
	ts_stage_timing(t);
	CHECK(t[M].rd_sectors == 100 && t[M].rd_io_us == 2500 && t[M].rd_copy_us == 0);
	free(g_shadow);
	g_shadow = NULL;

	/* wrap at 2^32: a difference of two readings stays right */
	reset();
	g_dev[M].tm.rd_io_us = 0xFFFFFF00u;
	CHECK(rd(u, 0, 100) == 0);
	ts_stage_timing(t);
	CHECK((uint32_t)(t[M].rd_io_us - 0xFFFFFF00u) == 2500);
	free(u);
}

/* ---- the experimental cached read path ---------------------------------------- */

/* The memory card device with a real (fake) memory block, as ts_stage_init
 * would leave it, so a mirror of it can be made. */
static void reset_with_block(void)
{
	Dev *d = &g_dev[M];

	reset();
	d->uid = ksceKernelAllocMemBlock("t", MEMBLOCK_KERNEL_NC_RW, STAGE_BYTES, NULL);
	CHECK(d->uid > 0 && ksceKernelGetMemBlockBase(d->uid, &d->buf) == 0);
}

static void cached_on(int on)
{
	TsSettings t;

	memset(&t, 0, sizeof(t));
	t.memcard = 1;                 /* the device under test stays enabled */
	t.cached_read = on;
	ts_stage_apply(&t);
}

/* Free the device block (and its mirror) the way ts_stage_fini does. */
static void release_block(void)
{
	ts_stage_fini();
	CHECK(g_dev[M].alias == NULL && g_dev[M].alias_uid == -1 && g_dev[M].uid == -1);
}

static void test_cached_read_path(void)
{
	Dev *d = &g_dev[M];
	uint8_t *u = user_buf(1100);

	reset_with_block();
	CHECK(ts_stage_cached_state(M) == TS_CR_OFF);
	cached_on(1);
	CHECK(ts_stage_cached_state(M) == TS_CR_PENDING);     /* until the worker runs */
	ts_stage_worker_tick();
	CHECK(ts_stage_cached_state(M) == TS_CR_ACTIVE);
	CHECK(fake_mirror_calls == 1 && fake_mirror_type == MEMBLOCK_KERNEL_RW);
	CHECK(d->alias != NULL && d->alias != d->buf && d->busy == 0);
	CHECK(fake_dc_clean_inv_calls == 1 && fake_dc_bad_range == 0);

	/* a read: the driver still gets the NC address; each chunk is invalidated
	 * after the DMA and copied out through the cached mapping */
	fake_dc_inv_calls = 0;
	fake_dc_inv_bytes = 0;
	CHECK(rd(u, 7, 1100) == 0);                           /* 512 + 512 + 76 */
	CHECK(memcmp(u, disk + 7 * 512, 1100 * 512) == 0);
	CHECK(last_buf == d->buf);
	CHECK(last_copy_src == d->alias);
	CHECK(fake_dc_inv_calls == 3 && fake_dc_inv_bytes == 1100 * 512 && fake_dc_bad_range == 0);

	/* writes are unchanged: no cache calls, copied into the NC address */
	fake_dc_inv_calls = 0;
	CHECK(wr(u, 40, 600, REF_RD) == 0);
	CHECK(memcmp(disk + 40 * 512, u, 600 * 512) == 0);
	CHECK(fake_dc_inv_calls == 0 && last_buf == d->buf);

	/* switched off: reads go back to the NC copy at once, the mapping stays */
	cached_on(0);
	CHECK(ts_stage_cached_state(M) == TS_CR_OFF);
	CHECK(rd(u, 3, 100) == 0 && last_copy_src == d->buf && fake_dc_inv_calls == 0);
	cached_on(1);
	ts_stage_worker_tick();
	CHECK(fake_mirror_calls == 1);                         /* not made twice */
	CHECK(rd(u, 3, 100) == 0 && last_copy_src == d->alias);

	release_block();                                       /* mirror freed first */
	free(u);
}

static void test_cached_waits_for_a_busy_buffer(void)
{
	Dev *d = &g_dev[M];

	reset_with_block();
	cached_on(1);
	d->busy = 1;                                           /* a transfer in flight */
	ts_stage_worker_tick();
	CHECK(fake_mirror_calls == 0 && ts_stage_cached_state(M) == TS_CR_PENDING);
	d->busy = 0;
	ts_stage_worker_tick();
	CHECK(fake_mirror_calls == 1 && ts_stage_cached_state(M) == TS_CR_ACTIVE);
	release_block();
}

static void test_cached_unavailable_falls_back(void)
{
	Dev *d = &g_dev[M];
	uint8_t *u = user_buf(100);

	/* the mirror cannot be made */
	reset_with_block();
	fake_mirror_fail = 1;
	cached_on(1);
	ts_stage_worker_tick();
	CHECK(ts_stage_cached_state(M) == TS_CR_UNAVAILABLE && d->alias == NULL && d->busy == 0);
	ts_stage_worker_tick();
	CHECK(fake_mirror_calls == 1);                         /* not retried every second */
	CHECK(rd(u, 10, 100) == 0 && memcmp(u, disk + 10 * 512, 100 * 512) == 0);
	CHECK(last_copy_src == d->buf);
	fake_mirror_fail = 0;                                  /* off and on again retries */
	cached_on(0);
	cached_on(1);
	ts_stage_worker_tick();
	CHECK(fake_mirror_calls == 2 && ts_stage_cached_state(M) == TS_CR_ACTIVE);
	release_block();

	/* a mapping that does not show the buffer fails the live check */
	reset_with_block();
	fake_mirror_wrong = 1;
	cached_on(1);
	ts_stage_worker_tick();
	CHECK(ts_stage_cached_state(M) == TS_CR_UNAVAILABLE && d->alias == NULL && d->alias_uid == -1);
	CHECK(rd(u, 10, 100) == 0 && last_copy_src == d->buf);
	release_block();

	/* no cache functions: never tries to map */
	reset_with_block();
	fake_dcache_missing = 1;
	cached_on(1);
	ts_stage_worker_tick();
	CHECK(fake_mirror_calls == 0 && ts_stage_cached_state(M) == TS_CR_UNAVAILABLE);
	release_block();

	/* a device without a buffer just reports off */
	reset();
	g_dev[M].buf = NULL;
	cached_on(1);
	ts_stage_worker_tick();
	CHECK(fake_mirror_calls == 0 && ts_stage_cached_state(M) == TS_CR_OFF);
	CHECK(ts_stage_cached_state(-1) == TS_ERR_ARG && ts_stage_cached_state(TS_DEV_COUNT) == TS_ERR_ARG);
	free(u);
}

static void count_free_bufs(int *n)
{
	*n = 0;
	for (int i = 0; i < TS_DEV_COUNT; i++)
		if (g_dev[i].buf)
			(*n)++;
}

static void test_init_all_hooks(void)
{
	int n;

	reset();
	for (int i = 0; i < TS_DEV_COUNT; i++) { g_dev[i].buf = NULL; g_dev[i].uid = -1; }
	CHECK(ts_stage_init() == TS_OK);
	count_free_bufs(&n);
	CHECK(n == 3 && fake_hook_calls == 6);
	ts_stage_fini();
	CHECK(fake_hook_releases == 6 && fake_free_calls == 3);
	count_free_bufs(&n);
	CHECK(n == 0);
}

static void test_init_sdif_hook_fails_releases_everything(void)
{
	int n;

	reset();
	for (int i = 0; i < TS_DEV_COUNT; i++) { g_dev[i].buf = NULL; g_dev[i].uid = -1; }
	fake_hook_fail_at = 2;                    /* the second SDIF hook fails */
	CHECK(ts_stage_init() == TS_ERR_RESOLVE);
	count_free_bufs(&n);
	CHECK(n == 0 && fake_free_calls == 3);    /* all buffers given back */
	CHECK(fake_hook_releases == 1);           /* and the one hook that was live */
}

static void test_init_memcard_hook_fails_only_memcard_stock(void)
{
	reset();
	for (int i = 0; i < TS_DEV_COUNT; i++) { g_dev[i].buf = NULL; g_dev[i].uid = -1; }
	fake_hook_fail_at = 6;                    /* the memory card's write hook fails */
	CHECK(ts_stage_init() == TS_OK);
	CHECK(g_dev[TS_DEV_MEMCARD].buf == NULL);
	CHECK(g_dev[TS_DEV_SD2VITA].buf != NULL && g_dev[TS_DEV_INTERNAL].buf != NULL);
	CHECK(fake_hook_releases == 1);           /* the memory card's read hook */
	ts_stage_fini();
	CHECK(fake_hook_releases == 5);
}

int main(void)
{
	staging = calloc(1, STAGE_BYTES);

	RUN(test_small_goes_stock);
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
	RUN(test_get_reports_time);
	RUN(test_timing_split);
	RUN(test_cached_read_path);
	RUN(test_cached_waits_for_a_busy_buffer);
	RUN(test_cached_unavailable_falls_back);
	RUN(test_init_all_hooks);
	RUN(test_init_sdif_hook_fails_releases_everything);
	RUN(test_init_memcard_hook_fails_only_memcard_stock);

	free(staging);
	printf(ts_failures ? "test_stage: %d FAILED\n" : "test_stage: ok\n", ts_failures);
	return ts_failures ? 1 : 0;
}
