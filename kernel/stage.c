/*
 * stage.c - the faster storage path ("staging").
 *
 * Why it is faster: before and after every transfer, the system's storage driver
 * cleans and invalidates the CPU data cache over the whole caller buffer, and
 * that work is not overlapped with the transfer. On the SD2Vita it costs ~13 us
 * of every ~34 us 512-byte block. The driver skips it for non-cacheable memory.
 *
 * What this does: for a large transfer on an enabled device, call the system's
 * own, unmodified sector read/write with a physically contiguous non-cacheable
 * staging buffer in place of the caller's buffer, and copy between the two with
 * the CPU. The driver still issues every command, handles every error and runs
 * the DMA; only the buffer it is handed changes.
 *
 * Hooked exports (SceSdifForDriver), all pass-through unless a transfer
 * qualifies:
 *   ksceSdifReadSectorSd / ksceSdifWriteSectorSd    -> SD2Vita (port 1, SD card)
 *   ksceSdifReadSectorMmc / ksceSdifWriteSectorMmc  -> internal (port 0 only;
 *                                                      game cards stay stock)
 * and (SceMsifForDriver):
 *   ksceMsifReadSector / ksceMsifWriteSector        -> the memory card (one slot)
 * Never touched: small transfers (< 32 KiB: file-system metadata), other ports
 * (the Wi-Fi chip is on its own port and never calls these), USB storage.
 *
 * Safety rules:
 *   - one staging buffer per device; if it is busy the transfer goes stock;
 *   - nothing is cached or deferred: a write has reached the card before the
 *     call returns, exactly as stock;
 *   - any error on the faster path switches that device back to stock for the
 *     session and finishes the same request the stock way;
 *   - verify mode re-reads every faster transfer the stock way and compares;
 *     on a mismatch the caller gets stock data (reads) or the chunk is
 *     rewritten the stock way (writes), and the device falls back.
 *
 * Experimental cached read path (setting "cached_read", off by default): the
 * copy OUT of the non-cacheable buffer is the slow half of a staged read (the
 * Cortex-A9 reads non-cacheable memory at ~120 MB/s). When it is on, the worker
 * gives each device buffer a second, cacheable mapping of the same physical
 * pages (a SceSysmem mirror block). The driver still gets the non-cacheable
 * address, so nothing changes for it; after its read returns, the chunk is
 * invalidated in the L1 and L2 data caches through the cached mapping and
 * copied out through it. Rules that keep this safe:
 *   - the mapping is created only while the device buffer is held (busy), then
 *     cleaned and invalidated in full, so no dirty line of it can ever be
 *     written back over data the card delivered;
 *   - nothing ever writes through the cached mapping;
 *   - every chunk is invalidated after the DMA and before it is read;
 *   - the mapping must pass a live check (a marker written through the
 *     non-cacheable address reads back through the cached one) or the device
 *     keeps the normal path; same if the cache functions can't be resolved.
 * Writes are unchanged (they go into the buffer through the non-cacheable
 * address).
 *
 * HOT PATH: no logging here, only counters (the 1 Hz worker logs).
 */
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "stage.h"
#include "klog.h"

/* The CPU copies between the caller's buffer and the staging buffer (the host
 * tests replace both to give the copy a measurable time).
 *   out of staging (reads): 8-word LDM/STM bursts (fastcopy.h). Measured on
 *     SD2Vita: 4.5 -> 4.2 us per 512-byte block.
 *   into staging (writes): the system memcpy. The burst copy made it slower
 *     (1.7 -> 2.3 us per block), so writes keep memcpy. */
#include "fastcopy.h"
#ifndef ts_copy_from_stage
#define ts_copy_from_stage ts_fast_copy
#endif
#ifndef ts_copy_to_stage
#define ts_copy_to_stage memcpy
#endif

#define LIB_SDIF       0x96D306FA    /* SceSdifForDriver                       */
#define NID_READ_SD    0xB9593652    /* ksceSdifReadSectorSd                   */
#define NID_WRITE_SD   0xE0781171    /* ksceSdifWriteSectorSd                  */
#define NID_READ_MMC   0x6F8D529B    /* ksceSdifReadSectorMmc                  */
#define NID_WRITE_MMC  0x175543D2    /* ksceSdifWriteSectorMmc                 */
#define LIB_MSIF       0xB706084A    /* SceMsifForDriver                       */
#define NID_READ_MS    0x58654AA3    /* ksceMsifReadSector(sector, buf, count) */
#define NID_WRITE_MS   0x329035EF    /* ksceMsifWriteSector                    */

/* The largest request the storage stack issues is 512 sectors (256 KiB);
 * anything larger is moved in pieces. */
#define STAGE_SECTORS  512
#define STAGE_BYTES    (STAGE_SECTORS * 512)
#define MIN_SECTORS    64            /* 32 KiB: smaller transfers stay stock   */

/* Kernel non-cacheable RW memory, physically contiguous (attr below): the same
 * kind the system uses for its own USB storage I/O buffer. */
#define MEMBLOCK_KERNEL_NC_RW  0x10208006
/* Kernel cacheable RW: the type of the second (mirror) mapping. */
#define MEMBLOCK_KERNEL_RW     0x1020D006

/* SceCpuForDriver (in SceSysmem), resolved at run time so a missing export only
 * makes the cached read path unavailable instead of stopping the module. */
#define LIB_CPU_DRV            0x40ECDB0E
#define NID_DCACHE_INV         0x02796361    /* ksceKernelDcacheInvalidateRange (L1+L2)      */
#define NID_DCACHE_CLEAN_INV   0x364E68A4    /* ksceKernelDcacheCleanInvalidateRange (L1+L2) */
#define CACHE_LINE             64            /* the functions want 0x40-aligned ranges */
#define LIB_SYSMEM_DRV         0x6F25E18A
#define NID_VA_TO_PA           0x8D160E65    /* ksceKernelVAtoPA (diagnostics only) */

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func);

/* Wait until earlier memory accesses have completed (a CPU store into the
 * non-cacheable mapping must reach memory before it is read back through the
 * other mapping). */
static inline void ts_dsb(void)
{
#if defined(__arm__)
	__asm__ volatile("dsb" ::: "memory");
#else
	__sync_synchronize();
#endif
}

void *ksceSdifGetSdContextGlobal(int port);

typedef struct Dev {
	int           port;          /* SDIF port, or -1: the memory card          */
	SceUID        uid;
	void         *buf;           /* NC staging buffer                          */
	volatile int  busy;
	volatile int  enabled;       /* from settings                              */
	volatile int  fallback;      /* error/mismatch seen: stock for the session */
	uint64_t      rd_bytes, wr_bytes;   /* moved on the faster path            */
	uint64_t      all_rd, all_wr;       /* every transfer on this device (HUD) */
	uint64_t      us_total;             /* time inside faster-path transfers, us */
	uint32_t      us_max;               /* the slowest one, us                   */
	TsTimingDev   tm;                   /* io vs copy time (wraps; see the API)  */
	void * volatile alias;              /* cached mapping of buf, or NULL        */
	SceUID        alias_uid;
	volatile int  cr_state;             /* TS_CR_UNAVAILABLE once setup failed   */
	TsDevState    st;            /* counters (status filled in by stage_get)   */
} Dev;

static Dev g_dev[TS_DEV_COUNT] = {
	[TS_DEV_SD2VITA]  = { .port = 1, .uid = -1, .alias_uid = -1 },
	[TS_DEV_INTERNAL] = { .port = 0, .uid = -1, .alias_uid = -1 },
	[TS_DEV_MEMCARD]  = { .port = -1, .uid = -1, .alias_uid = -1 },
};

/* Cached read path: the setting, and the two cache functions. */
static volatile int g_cached;
static void (*g_dc_inv)(void *start, unsigned int size);
static void (*g_dc_clean_inv)(void *start, unsigned int size);
static int (*g_va2pa)(const void *va, uintptr_t *pa);
static int g_dc_tried;

/* Physical address of va, or 0 when it cannot be read (log only). */
static uint32_t pa_of(const void *va)
{
	uintptr_t pa = 0;

	if (!g_va2pa || g_va2pa(va, &pa) < 0)
		return 0;
	return (uint32_t)pa;
}

/* Verify mode: one shared cached buffer for the stock re-read. */
static volatile int g_verify;
static void        *g_shadow;
static SceUID       g_shadow_uid = -1;
static volatile int g_shadow_busy;
static volatile int g_shadow_failed;   /* allocation failed: do not retry every tick */

/* Returned to the caller when a hook is entered before taiHEN has stored its
 * continuation (SCE_ERROR_ERRNO_EIO): a failed transfer, never a jump to NULL. */
#define TS_IO_ERROR  ((int)0x80010005)

enum { H_RD_SD, H_WR_SD, H_RD_MMC, H_WR_MMC, H_RD_MS, H_WR_MS, H_COUNT };
#define H_SDIF_COUNT H_RD_MS          /* the SceSdif hooks come first          */
static volatile tai_hook_ref_t g_ref[H_COUNT];
static SceUID g_hook[H_COUNT] = { -1, -1, -1, -1, -1, -1 };

/* Calling the original: SceSdif takes the card context first, SceMsif has a
 * single slot and no context. `part` is NULL for the memory card. */
typedef int (*io_fn)(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n);

static int io_sdif(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	return TAI_CONTINUE(int, ref, part, sector, buf, n);
}

static int io_msif(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	(void)part;
	return TAI_CONTINUE(int, ref, sector, buf, n);
}

/* taiHEN can make a hook live a moment before it stores the ref. A call that
 * lands in that window waits for it (bounded) instead of sleeping blindly. */
static tai_hook_ref_t ref_of(int h)
{
	for (int i = 0; i < 2000 && g_ref[h] == 0; i++)
		ksceKernelDelayThread(10);
	return g_ref[h];
}

static int on_port(void *part, int port)
{
	if (port < 0)
		return 1;              /* the memory card hooks only see the card      */
	/* A card context's first word is its controller context. */
	return part && *(void **)part == ksceSdifGetSdContextGlobal(port);
}

static int alloc_buf(unsigned type, int phycont, SceUID *uid, void **base)
{
	SceKernelAllocMemBlockKernelOpt opt;

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	if (phycont)
		opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT;
	*uid = ksceKernelAllocMemBlock("SceStorageTurbo", type, STAGE_BYTES, &opt);
	if (*uid < 0)
		return *uid;
	if (ksceKernelGetMemBlockBase(*uid, base) < 0) {
		ksceKernelFreeMemBlock(*uid);
		*uid = -1;
		return TS_ERR_NOMEM;
	}
	return TS_OK;
}

/* Take the device buffer for one request, or 0 if the request stays stock. */
static int take(Dev *d, int mine, uint32_t nsec)
{
	if (!mine || !d->enabled || d->fallback || !d->buf || nsec < MIN_SECTORS)
		return 0;
	if (__sync_lock_test_and_set(&d->busy, 1)) {
		d->st.passthru++;
		return 0;
	}
	/* Re-check after taking the buffer: ts_stage_fini clears `enabled` first and
	 * then waits for `busy` to drop, so a request that passed the check above
	 * just before that must not go on to use the buffer. */
	__sync_synchronize();
	if (!d->enabled || d->fallback) {
		__sync_lock_release(&d->busy);
		return 0;
	}
	return 1;
}

static int take_shadow(void)
{
	return g_verify && g_shadow && !__sync_lock_test_and_set(&g_shadow_busy, 1);
}

/* Time one faster-path request (called while the device buffer is held, so only
 * one thread updates a device's figures). Two clock reads and a few adds: cheap
 * enough for the hot path, which must not log. */
static void note_time(Dev *d, uint32_t t0)
{
	uint32_t dt = ksceKernelGetSystemTimeLow() - t0;

	d->us_total += dt;
	if (dt > d->us_max)
		d->us_max = dt;
}

static int stage_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                      void *buf, uint32_t nsec)
{
	uint8_t *dst = buf, *alias;
	uint32_t left = nsec, t0;
	int verify, ret = 0, mine = on_port(part, d->port);

	if (mine)
		d->all_rd += (uint64_t)nsec * 512;
	if (!take(d, mine, nsec))
		return io(ref, part, sector, buf, nsec);
	t0 = ksceKernelGetSystemTimeLow();
	verify = take_shadow();
	/* Read once per request: the worker publishes alias only while it holds
	 * this buffer, and nothing frees it while the buffer is held. */
	alias = g_cached ? (uint8_t *)d->alias : NULL;
	while (left && !d->fallback) {
		uint32_t n = left > STAGE_SECTORS ? STAGE_SECTORS : left, bytes = n * 512;

		uint32_t t1 = ksceKernelGetSystemTimeLow(), t2, t3;

		ret = io(ref, part, sector, d->buf, n);
		if (ret < 0)
			break;
		t2 = ksceKernelGetSystemTimeLow();
		if (alias) {
			/* Drop whatever the cache holds for these lines (stale data from
			 * before the DMA, or lines fetched speculatively during it), then
			 * read the card's data through the cache. bytes is a multiple of
			 * 512 and alias is page-aligned: whole 64-byte lines. */
			g_dc_inv(alias, bytes);
			ts_copy_from_stage(dst, alias, bytes);
		} else {
			ts_copy_from_stage(dst, d->buf, bytes);
		}
		t3 = ksceKernelGetSystemTimeLow();
		d->tm.rd_sectors += n;
		d->tm.rd_io_us += t2 - t1;
		d->tm.rd_copy_us += t3 - t2;
		if (verify && io(ref, part, sector, g_shadow, n) >= 0) {
			d->st.verified++;
			if (memcmp(dst, g_shadow, bytes) != 0) {
				d->st.mismatches++;
				d->fallback = 1;
				memcpy(dst, g_shadow, bytes);   /* the caller gets stock data */
			}
		}
		d->rd_bytes += bytes;
		dst += bytes;
		sector += n;
		left -= n;
	}
	if (verify)
		__sync_lock_release(&g_shadow_busy);
	note_time(d, t0);
	__sync_lock_release(&d->busy);
	if (ret < 0) {
		d->st.errors++;
		d->fallback = 1;
	} else {
		d->st.reads++;
	}
	/* Anything not done on the faster path (error, mismatch) is done stock. */
	return left ? io(ref, part, sector, dst, left) : ret;
}

static int stage_write(Dev *d, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                       void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	const uint8_t *src = buf;
	uint32_t left = nsec, t0;
	int verify, ret = 0, mine = on_port(part, d->port);

	if (mine)
		d->all_wr += (uint64_t)nsec * 512;
	if (!take(d, mine, nsec))
		return io(ref, part, sector, buf, nsec);
	t0 = ksceKernelGetSystemTimeLow();
	verify = take_shadow();
	if (verify && !rd_ref) {            /* no read hook yet: cannot verify this one */
		__sync_lock_release(&g_shadow_busy);
		verify = 0;
	}
	while (left && !d->fallback) {
		uint32_t n = left > STAGE_SECTORS ? STAGE_SECTORS : left, bytes = n * 512;

		uint32_t t1 = ksceKernelGetSystemTimeLow(), t2, t3;

		ts_copy_to_stage(d->buf, src, bytes);
		t2 = ksceKernelGetSystemTimeLow();
		ret = io(ref, part, sector, d->buf, n);
		if (ret < 0)
			break;
		t3 = ksceKernelGetSystemTimeLow();
		d->tm.wr_sectors += n;
		d->tm.wr_copy_us += t2 - t1;
		d->tm.wr_io_us += t3 - t2;
		if (verify && io(rd_ref, part, sector, g_shadow, n) >= 0) {
			d->st.verified++;
			if (memcmp(g_shadow, src, bytes) != 0) {
				d->st.mismatches++;
				d->fallback = 1;
				/* rewrite this chunk the stock way, from the caller's data */
				ret = io(ref, part, sector, (void *)src, n);
				if (ret < 0)
					break;
			}
		}
		d->wr_bytes += bytes;
		src += bytes;
		sector += n;
		left -= n;
	}
	if (verify)
		__sync_lock_release(&g_shadow_busy);
	note_time(d, t0);
	__sync_lock_release(&d->busy);
	if (ret < 0) {
		d->st.errors++;
		d->fallback = 1;
	} else {
		d->st.writes++;
	}
	return left ? io(ref, part, sector, (void *)src, left) : ret;
}

static int h_rd_sd(void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_RD_SD);

	if (!r)
		return TS_IO_ERROR;
	return stage_read(&g_dev[TS_DEV_SD2VITA], io_sdif, r, part, sector, buf, nsec);
}

static int h_wr_sd(void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_WR_SD);

	if (!r)
		return TS_IO_ERROR;
	return stage_write(&g_dev[TS_DEV_SD2VITA], io_sdif, r, g_ref[H_RD_SD], part,
	                   sector, buf, nsec);
}

static int h_rd_mmc(void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_RD_MMC);

	if (!r)
		return TS_IO_ERROR;
	return stage_read(&g_dev[TS_DEV_INTERNAL], io_sdif, r, part, sector, buf, nsec);
}

static int h_wr_mmc(void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_WR_MMC);

	if (!r)
		return TS_IO_ERROR;
	return stage_write(&g_dev[TS_DEV_INTERNAL], io_sdif, r, g_ref[H_RD_MMC], part,
	                   sector, buf, nsec);
}

static int h_rd_ms(uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_RD_MS);

	if (!r)
		return TS_IO_ERROR;
	return stage_read(&g_dev[TS_DEV_MEMCARD], io_msif, r, NULL, sector, buf, nsec);
}

static int h_wr_ms(uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_WR_MS);

	if (!r)
		return TS_IO_ERROR;
	return stage_write(&g_dev[TS_DEV_MEMCARD], io_msif, r, g_ref[H_RD_MS], NULL,
	                   sector, buf, nsec);
}

int ts_stage_init(void)
{
	static const struct { int h; uint32_t nid; void *fn; } k_hooks[H_COUNT] = {
		{ H_RD_SD,  NID_READ_SD,   h_rd_sd  },
		{ H_WR_SD,  NID_WRITE_SD,  h_wr_sd  },
		{ H_RD_MMC, NID_READ_MMC,  h_rd_mmc },
		{ H_WR_MMC, NID_WRITE_MMC, h_wr_mmc },
		{ H_RD_MS,  NID_READ_MS,   h_rd_ms  },
		{ H_WR_MS,  NID_WRITE_MS,  h_wr_ms  },
	};

	/* Buffers first, at boot while contiguous memory is easy to find. A device
	 * without one simply stays stock. */
	for (int i = 0; i < TS_DEV_COUNT; i++) {
		int r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, &g_dev[i].uid, &g_dev[i].buf);

		if (r < 0) {
			g_dev[i].buf = NULL;
			TS_KLOGW("stage: dev %d staging buffer not allocated (0x%08x) - stock", i,
			         (unsigned)r);
		}
	}
	for (int i = 0; i < H_SDIF_COUNT; i++) {
		g_hook[k_hooks[i].h] = taiHookFunctionExportForKernel(KERNEL_PID,
			(tai_hook_ref_t *)&g_ref[k_hooks[i].h], "SceSdif", LIB_SDIF, k_hooks[i].nid,
			k_hooks[i].fn);
		if (g_hook[k_hooks[i].h] < 0) {
			TS_KLOGE("stage: hook %d failed (0x%08x) - releasing all", i,
			         (unsigned)g_hook[k_hooks[i].h]);
			ts_stage_fini();
			return TS_ERR_RESOLVE;
		}
	}
	/* The memory card is optional: if its hooks can't be installed, only the
	 * memory card stays stock (its buffer is released). */
	for (int i = H_SDIF_COUNT; i < H_COUNT; i++) {
		g_hook[k_hooks[i].h] = taiHookFunctionExportForKernel(KERNEL_PID,
			(tai_hook_ref_t *)&g_ref[k_hooks[i].h], "SceMsif", LIB_MSIF, k_hooks[i].nid,
			k_hooks[i].fn);
		if (g_hook[k_hooks[i].h] < 0) {
			TS_KLOGW("stage: memory card hook %d failed (0x%08x) - memory card stays stock",
			         i, (unsigned)g_hook[k_hooks[i].h]);
			for (int j = H_SDIF_COUNT; j < H_COUNT; j++)
				if (g_hook[j] >= 0) {
					taiHookReleaseForKernel(g_hook[j], g_ref[j]);
					g_hook[j] = -1;
				}
			Dev *m = &g_dev[TS_DEV_MEMCARD];
			if (m->uid >= 0) {
				ksceKernelFreeMemBlock(m->uid);
				m->uid = -1;
			}
			m->buf = NULL;
			break;
		}
	}
	TS_KLOGI("stage: hooks installed; buffers sd2vita=%s internal=%s memcard=%s",
	         g_dev[TS_DEV_SD2VITA].buf ? "ok" : "none",
	         g_dev[TS_DEV_INTERNAL].buf ? "ok" : "none",
	         g_dev[TS_DEV_MEMCARD].buf ? "ok" : "none");
	return TS_OK;
}

void ts_stage_fini(void)
{
	/* Stop taking the faster path, then wait for any transfer in flight. */
	for (int i = 0; i < TS_DEV_COUNT; i++)
		g_dev[i].enabled = 0;
	for (int i = 0; i < TS_DEV_COUNT; i++)
		while (g_dev[i].busy)
			ksceKernelDelayThread(1000);
	for (int i = H_COUNT - 1; i >= 0; i--)
		if (g_hook[i] >= 0) {
			taiHookReleaseForKernel(g_hook[i], g_ref[i]);
			g_hook[i] = -1;
		}
	for (int i = 0; i < TS_DEV_COUNT; i++)
		if (g_dev[i].alias_uid >= 0) {    /* the mirror goes before what it maps */
			g_dev[i].alias = NULL;
			ksceKernelFreeMemBlock(g_dev[i].alias_uid);
			g_dev[i].alias_uid = -1;
		}
	for (int i = 0; i < TS_DEV_COUNT; i++)
		if (g_dev[i].uid >= 0) {
			ksceKernelFreeMemBlock(g_dev[i].uid);
			g_dev[i].uid = -1;
			g_dev[i].buf = NULL;
		}
	if (g_shadow_uid >= 0) {
		ksceKernelFreeMemBlock(g_shadow_uid);
		g_shadow_uid = -1;
		g_shadow = NULL;
	}
}

void ts_stage_apply(const TsSettings *s)
{
	const int32_t want[TS_DEV_COUNT] = { s->sd2vita, s->internal, s->memcard };

	for (int i = 0; i < TS_DEV_COUNT; i++) {
		if (want[i] && !g_dev[i].enabled)
			g_dev[i].fallback = 0;   /* switching it back on is a fresh start */
		g_dev[i].enabled = want[i] ? 1 : 0;
	}
	g_verify = s->verify ? 1 : 0;
	if (!g_verify)
		g_shadow_failed = 0;      /* turning verify off and on again retries the buffer */
	g_cached = s->cached_read ? 1 : 0;
	if (!g_cached)                /* turning it off and on again retries the setup */
		for (int i = 0; i < TS_DEV_COUNT; i++)
			if (g_dev[i].cr_state == TS_CR_UNAVAILABLE)
				g_dev[i].cr_state = TS_CR_OFF;
}

/* Give device i a cached mapping of its staging buffer. Runs in the worker,
 * holding the buffer, so no transfer uses it meanwhile. 1 = done (or for good
 * unavailable), 0 = the buffer was busy: try again on the next tick. */
static int setup_alias(int i)
{
	static const uint32_t marker[4] = { 0x54534341u, 0x5348454Fu, 0x0BADF00Du, 0xC0FFEE11u };
	Dev *d = &g_dev[i];
	SceKernelAllocMemBlockKernelOpt opt;
	SceUID uid;
	void *base = NULL;
	int ok;

	if (__sync_lock_test_and_set(&d->busy, 1))
		return 0;
	__sync_synchronize();

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_MIRROR_BLOCKID;
	opt.mirror_blockid = d->uid;
	uid = ksceKernelAllocMemBlock("SceStorageTurboCached", MEMBLOCK_KERNEL_RW, STAGE_BYTES, &opt);
	if (uid < 0) {
		TS_KLOGW("stage: dev %d cached mapping not created (0x%08x) - normal reads",
		         i, (unsigned)uid);
		goto unavailable;
	}
	if (ksceKernelGetMemBlockBase(uid, &base) < 0 || !base || base == d->buf) {
		TS_KLOGW("stage: dev %d cached mapping has no usable address - normal reads", i);
		ksceKernelFreeMemBlock(uid);
		goto unavailable;
	}
	/* Write back and drop anything the new mapping holds (it may have been
	 * zero-filled through the cache), while nothing else uses the buffer. */
	g_dc_clean_inv(base, STAGE_BYTES);
	ts_dsb();
	/* Live check: the cached mapping must show the same physical memory. The
	 * stores go through the NC address and must have reached memory (dsb)
	 * before the line is dropped from the cache and read back. */
	{
		uint32_t seen[4], seen2, pa_buf = pa_of(d->buf), pa_alias = pa_of(base);

		memcpy(d->buf, marker, sizeof(marker));
		ts_dsb();
		g_dc_inv(base, CACHE_LINE);
		ts_dsb();
		memcpy(seen, base, sizeof(seen));
		ok = memcmp(seen, marker, sizeof(marker)) == 0;
		((uint32_t *)d->buf)[0] = ~marker[0];
		ts_dsb();
		g_dc_inv(base, CACHE_LINE);
		ts_dsb();
		seen2 = ((const uint32_t *)base)[0];
		ok = ok && seen2 == ~marker[0];
		g_dc_inv(base, CACHE_LINE);                   /* leave no line of it behind */
		ts_dsb();
		TS_KLOGI("stage: dev %d mapping nc=0x%08x (pa 0x%08x) cached=0x%08x (pa 0x%08x) check %s",
		         i, (unsigned)(uintptr_t)d->buf, (unsigned)pa_buf, (unsigned)(uintptr_t)base,
		         (unsigned)pa_alias, ok ? "ok" : "FAILED");
		if (!ok) {
			TS_KLOGW("stage: dev %d wrote %08x %08x %08x %08x then %08x, cached saw %08x %08x %08x %08x then %08x",
			         i, (unsigned)marker[0], (unsigned)marker[1], (unsigned)marker[2],
			         (unsigned)marker[3], (unsigned)~marker[0], (unsigned)seen[0],
			         (unsigned)seen[1], (unsigned)seen[2], (unsigned)seen[3], (unsigned)seen2);
			TS_KLOGW("stage: dev %d cached mapping does not show the buffer - normal reads", i);
			ksceKernelFreeMemBlock(uid);
			goto unavailable;
		}
	}
	d->alias_uid = uid;
	__sync_synchronize();
	d->alias = base;
	d->cr_state = TS_CR_ACTIVE;
	__sync_lock_release(&d->busy);
	TS_KLOGI("stage: dev %d cached read path ready", i);
	return 1;

unavailable:
	d->cr_state = TS_CR_UNAVAILABLE;
	__sync_lock_release(&d->busy);
	return 1;
}

static void cached_tick(void)
{
	if (!g_dc_tried) {
		uintptr_t inv = 0, cinv = 0;

		g_dc_tried = 1;
		if (module_get_export_func(KERNEL_PID, "SceSysmem", LIB_CPU_DRV, NID_DCACHE_INV, &inv) >= 0 &&
		    module_get_export_func(KERNEL_PID, "SceSysmem", LIB_CPU_DRV, NID_DCACHE_CLEAN_INV, &cinv) >= 0 &&
		    inv && cinv) {
			g_dc_inv = (void (*)(void *, unsigned int))inv;
			g_dc_clean_inv = (void (*)(void *, unsigned int))cinv;
		} else {
			TS_KLOGW("stage: cache functions not found - cached read path unavailable");
		}
		{
			uintptr_t f = 0;

			if (module_get_export_func(KERNEL_PID, "SceSysmem", LIB_SYSMEM_DRV, NID_VA_TO_PA, &f) >= 0)
				g_va2pa = (int (*)(const void *, uintptr_t *))f;
		}
	}
	for (int i = 0; i < TS_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];

		if (!d->buf || d->alias || d->cr_state == TS_CR_UNAVAILABLE)
			continue;
		if (!g_dc_inv || !g_dc_clean_inv) {
			d->cr_state = TS_CR_UNAVAILABLE;
			continue;
		}
		setup_alias(i);
	}
}

void ts_stage_worker_tick(void)
{
	if (g_cached)
		cached_tick();
	/* The verify buffer is only allocated once verify mode is first turned on. */
	if (g_verify && !g_shadow && g_shadow_uid < 0 && !g_shadow_failed) {
		int r = alloc_buf(SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, 0, &g_shadow_uid, &g_shadow);

		if (r < 0) {
			g_shadow = NULL;
			g_shadow_uid = -1;
			g_shadow_failed = 1;    /* do not retry every second */
			TS_KLOGW("stage: verify buffer not allocated (0x%08x)", (unsigned)r);
		} else {
			TS_KLOGI("stage: verify buffer allocated");
		}
	}
}

void ts_stage_bytes(int dev, uint64_t *rd, uint64_t *wr)
{
	*rd = g_dev[dev].all_rd;
	*wr = g_dev[dev].all_wr;
}

int ts_stage_verify_ready(void)
{
	return g_shadow != NULL;
}

void ts_stage_get(TsDevState out[TS_DEV_COUNT], int plugin_active)
{
	for (int i = 0; i < TS_DEV_COUNT; i++) {
		const Dev *d = &g_dev[i];

		out[i] = d->st;
		out[i].read_mib = (uint32_t)(d->rd_bytes >> 20);
		out[i].write_mib = (uint32_t)(d->wr_bytes >> 20);
		out[i].time_ms = (uint32_t)(d->us_total / 1000u);
		out[i].max_ms = d->us_max / 1000u;
		out[i].status = !plugin_active ? TS_ST_UNAVAIL :
		                !d->enabled    ? TS_ST_OFF :
		                !d->buf        ? TS_ST_NO_BUFFER :
		                d->fallback    ? TS_ST_FALLBACK : TS_ST_ACTIVE;
	}
}

int ts_stage_cached_state(int dev)
{
	const Dev *d;

	if (dev < 0 || dev >= TS_DEV_COUNT)
		return TS_ERR_ARG;
	d = &g_dev[dev];
	if (!g_cached || !d->buf)
		return TS_CR_OFF;
	if (d->cr_state == TS_CR_UNAVAILABLE)
		return TS_CR_UNAVAILABLE;
	return d->alias ? TS_CR_ACTIVE : TS_CR_PENDING;
}

void ts_stage_timing(TsTimingDev out[TS_DEV_COUNT])
{
	for (int i = 0; i < TS_DEV_COUNT; i++)
		out[i] = g_dev[i].tm;
}

/* Memory card slow mode (Sony's own throttle: it sleeps after every memory
 * card transfer). Read only; Turbo Storage never changes it. 1 on, 0 off,
 * -1 if the driver's getter isn't available. */
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func);

int ts_stage_memcard_slow(void)
{
	static int (*get)(void);
	static int tried;

	if (!tried) {
		uintptr_t f = 0;
		tried = 1;
		if (module_get_export_func(KERNEL_PID, "SceMsif", LIB_MSIF, 0x491E25B5, &f) >= 0)
			get = (int (*)(void))f;   /* ksceMsifGetSlowCardModeState */
	}
	return get ? (get() ? 1 : 0) : -1;
}
