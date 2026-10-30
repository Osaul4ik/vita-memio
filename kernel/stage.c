/*
 * stage.c - the faster storage path ("staging").
 *
 * Why it is faster: before and after every transfer, the system's storage driver
 * cleans and invalidates the CPU data cache over the whole caller buffer, and
 * that work is not overlapped with the transfer. On the SD2Vita it costs ~13 us
 * of every ~34 us 512-byte block. The driver skips it for non-cacheable memory.
 * So a caller buffer that is itself non-cacheable (games load data for the
 * graphics chip into such memory) stays on the standard path: there the
 * driver has no cache work and no copy is needed (SD2Vita reads, 1 MiB:
 * 21.0 MiB/s standard vs 16.5 MiB/s through the staging buffer).
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
 * Never touched: small transfers (< 32 KiB: file-system metadata; unless
 * "small transfers" is switched on, then every transfer qualifies), other ports
 * (the Wi-Fi chip is on its own port and never calls these), USB storage.
 *
 * Safety rules:
 *   - one staging buffer per device, allocated when the device is first
 *     switched on (at boot for those already on) and then kept; if it is busy
 *     or could not be allocated, the transfer goes stock;
 *   - nothing is cached or deferred: a write has reached the card before the
 *     call returns, exactly as stock;
 *   - any error on the faster path switches that device back to stock for the
 *     session and finishes the same request the stock way;
 *   - "GPU-safe reads": the stock driver's DMA leaves read data in memory,
 *     while the CPU copy can leave it in the data cache for a while; with the
 *     setting on, each read's copy is cleaned out of the cache to memory
 *     before the call returns, so the graphics chip (which reads memory, not
 *     the CPU cache) sees exactly what the stock path gives it;
 *   - overlapped reads (experimental, off by default): a faster read of at
 *     least PIPE_MIN_SECTORS is split in two. The caller's thread reads the
 *     head into the staging buffer, hands the tail to the device's helper
 *     thread (read into a second buffer) and copies the head while the card
 *     reads the tail; then it copies the tail. The split is sized so the head's
 *     copy takes about as long as the tail's read. Nothing is read ahead or
 *     kept: both parts belong to the request, which is complete when it
 *     returns. Any error finishes the whole part stock, as above;
 *   - verify mode re-reads every faster transfer the stock way and compares;
 *     on a mismatch the caller gets stock data (reads) or the chunk is
 *     rewritten the stock way (writes), and the device falls back.
 *
 * HOT PATH: no logging here, only counters (the 1 Hz worker logs).
 */
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
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
/* Experimental: the copy variant that keeps a load in flight, chosen while the
 * file ur0:data/vita-memio/copy2.on exists (looked for by the worker), so the
 * two can be compared with the tester on the same build. */
static volatile int g_copy2;
#ifndef vm_copy_from_stage
#define vm_copy_from_stage(d, s, n) (g_copy2 ? vm_fast_copy_pipe(d, s, n) : vm_fast_copy(d, s, n))
#endif
#ifndef vm_copy_to_stage
#define vm_copy_to_stage memcpy
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

void *ksceSdifGetSdContextGlobal(int port);
int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func);   /* taiHEN */

/* Calling the original: SceSdif takes the card context first, SceMsif has a
 * single slot and no context. `part` is NULL for the memory card. */
typedef int (*io_fn)(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n);

typedef struct Dev {
	int           port;          /* SDIF port, or -1: the memory card          */
	SceUID        uid;
	void         *buf;           /* NC staging buffer, NULL until allocated    */
	volatile int  busy;
	volatile int  enabled;       /* from settings                              */
	volatile int  fallback;      /* error/mismatch seen: stock for the session */
	int           unusable;      /* its hooks are not installed: no buffer     */
	int           alloc_err;     /* last failed allocation (0: none); not
	                              * retried until the device is switched again */
	int           alloc_logged;
	uint64_t      rd_bytes, wr_bytes;   /* moved on the faster path            */
	uint64_t      all_rd, all_wr;       /* every transfer on this device (HUD) */
	VmTimingDev   tm;                   /* io vs copy time (wraps; see the API)  */
	VmDevState    st;            /* counters (status filled in by stage_get)   */
	/* Overlapped reads: the helper thread, its buffer and the one job it runs.
	 * All valid while pipe_made; pipe_ready publishes them to the hooks. */
	int           pipe_made, pipe_failed;
	volatile int  pipe_ready, pipe_quit;
	SceUID        uid2, th, sem_go, sem_done;
	void         *buf2;
	io_fn         job_io;
	tai_hook_ref_t job_ref;
	void         *job_part;
	uint32_t      job_sector, job_n, job_us;
	volatile int  job_ret;
	uint32_t      est_io_ns, est_cp_ns;   /* per sector, for the split        */
	uint32_t      est_cmd_ns;             /* the fixed cost of one card read   */
} Dev;

static Dev g_dev[VM_DEV_COUNT] = {
	[VM_DEV_SD2VITA]  = { .port = 1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1 },
	[VM_DEV_INTERNAL] = { .port = 0, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1 },
	[VM_DEV_MEMCARD]  = { .port = -1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1 },
};

/* Set by vm_stage_init: from then on a device switched on gets its buffer.
 * Before it (settings applied at boot, safe mode, unknown firmware) nothing is
 * allocated. */
static int g_live;

/* Smallest transfer the faster path takes (sectors): MIN_SECTORS, or 1 with
 * small transfers switched on. */
static volatile uint32_t g_min_sectors = MIN_SECTORS;

/* What is counted on every transfer beyond what safety needs (requests,
 * errors, verify results, buffer busy - always):
 *   g_stats      the io/copy timers and the faster-path bytes (the tester);
 *   g_count_all  every transfer's bytes (the HUD's speeds), also with stats. */
static volatile int g_stats;
static volatile int g_count_all;

/* GPU-safe reads: ksceKernelDcacheCleanRange (SceCpuForDriver: L1 and L2),
 * looked up at run time by vm_stage_init. */
#define LIB_CPU_DRIVER     0x40ECDB0E
#define NID_DCACHE_CLEAN   0x103872A5
static void (*g_dcache_clean)(void *start, unsigned int size);
static volatile int g_gpu_safe;
static int g_gpu_safe_logged;
static volatile uint32_t g_gpu_cleaned_kib, g_nc_stock_kib;   /* for the log */

/* SceSysmemForDriver: the memory type at an address of the current address
 * space, masked with 0xFF00 (0xD000 cached, 0x4000 L1 write-back only,
 * 0x8000 non-cacheable - also CDRAM -, 0x0800 device), or < 0. */
int ksceKernelGetMemBlockMemtypeByAddr(const void *addr);
#define MEMTYPE_NC      0x8000
#define MEMTYPE_DEVICE  0x0800

static int is_uncached(const void *a)
{
	int m = ksceKernelGetMemBlockMemtypeByAddr(a);

	return m == MEMTYPE_NC || m == MEMTYPE_DEVICE;
}

/* Whether the caller's [p, p + n) is non-cacheable memory, looked at both
 * ends; anything unknown counts as cached (and is staged as before). */
static int caller_uncached(const uint8_t *p, uint32_t n)
{
	return is_uncached(p) && is_uncached(p + n - 1);
}

/* Write [p, p + n) out of the data cache to memory (whole 32-byte lines). */
static inline void clean_out(void *p, uint32_t n)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)31, e = ((uintptr_t)p + n + 31) & ~(uintptr_t)31;

	g_dcache_clean((void *)a, (unsigned int)(e - a));
}

/* Verify mode: one shared cached buffer for the stock re-read. */
static volatile int g_verify;
static void        *g_shadow;
static SceUID       g_shadow_uid = -1;
static volatile int g_shadow_busy;
static volatile int g_shadow_failed;   /* allocation failed: do not retry every tick */

/* Returned to the caller when a hook is entered before taiHEN has stored its
 * continuation (SCE_ERROR_ERRNO_EIO): a failed transfer, never a jump to NULL. */
#define VM_IO_ERROR  ((int)0x80010005)

enum { H_RD_SD, H_WR_SD, H_RD_MMC, H_WR_MMC, H_RD_MS, H_WR_MS, H_COUNT };
#define H_SDIF_COUNT H_RD_MS          /* the SceSdif hooks come first          */
static volatile tai_hook_ref_t g_ref[H_COUNT];
static SceUID g_hook[H_COUNT] = { -1, -1, -1, -1, -1, -1 };

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

static int alloc_buf(unsigned type, int phycont, SceSize size, SceUID *uid, void **base)
{
	SceKernelAllocMemBlockKernelOpt opt;

	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	if (phycont)
		opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT;
	*uid = ksceKernelAllocMemBlock("SceVitaMemio", type, size, &opt);
	if (*uid < 0)
		return *uid;
	if (ksceKernelGetMemBlockBase(*uid, base) < 0) {
		ksceKernelFreeMemBlock(*uid);
		*uid = -1;
		return VM_ERR_NOMEM;
	}
	return VM_OK;
}

/* Give device `d` its staging buffer if it has none. The pointer is published
 * last, so a transfer sees either no buffer or a complete one. */
static void ensure_buf(Dev *d)
{
	SceUID uid;
	void *base = NULL;
	int r;

	if (!g_live || d->buf || d->unusable || d->alloc_err)
		return;
	r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, STAGE_BYTES, &uid, &base);
	if (r < 0) {
		d->alloc_err = r;
		d->alloc_logged = 0;            /* the worker logs it */
		return;
	}
	d->uid = uid;
	__sync_synchronize();
	d->buf = base;
}

/* Take the device buffer for one request, or 0 if the request stays stock. */
static int take(Dev *d, int mine, uint32_t nsec)
{
	if (!mine || !d->enabled || d->fallback || !d->buf || nsec < g_min_sectors || !nsec)
		return 0;
	if (__sync_lock_test_and_set(&d->busy, 1)) {
		d->st.passthru++;
		return 0;
	}
	/* Re-check after taking the buffer: vm_stage_fini clears `enabled` first and
	 * then waits for `busy` to drop, so a request that passed the check above
	 * just before that must not go on to use the buffer. */
	__sync_synchronize();
	if (!d->enabled || d->fallback) {
		__sync_lock_release(&d->busy);
		return 0;
	}
	return 1;
}

/* ---- overlapped reads ------------------------------------------------------ */

#define PIPE_MIN_SECTORS   128        /* 64 KiB: smaller reads are not split    */
#define PIPE_MIN_TAIL      8
#define PIPE_TAIL_SECTORS  (STAGE_SECTORS / 2)   /* the second buffer's size    */
/* Per-sector costs before anything is measured (SD2Vita): the card read, and
 * the copy without and with the GPU-safe clean. */
#define PIPE_DEF_IO_NS     25000u
#define PIPE_DEF_CP_NS     4200u
#define PIPE_DEF_CPCL_NS   11100u

static volatile int g_pipeline;              /* the setting                    */
static volatile uint32_t g_pipe_reads;       /* split reads, for the log       */

/* The tail of an n-sector read: its card read (a fixed cost per command plus a
 * cost per sector) should take about as long as the head's copy, so neither
 * side waits:  (n - t) * cp = cmd + t * io,  t = (n * cp - cmd) / (io + cp).
 * A shorter tail leaves less to copy at the end. */
static uint32_t pipe_tail(const Dev *d, uint32_t n, int clean)
{
	uint32_t io = d->est_io_ns ? d->est_io_ns : PIPE_DEF_IO_NS;
	uint32_t cp = d->est_cp_ns ? d->est_cp_ns : clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS;
	uint64_t work = (uint64_t)n * cp;
	uint32_t t = work > d->est_cmd_ns
	             ? (uint32_t)((work - d->est_cmd_ns) / ((uint64_t)io + cp)) : 0;

	if (t < PIPE_MIN_TAIL)
		t = PIPE_MIN_TAIL;
	if (t > n / 2)
		t = n / 2;
	if (t > PIPE_TAIL_SECTORS)
		t = PIPE_TAIL_SECTORS;
	return t;
}

/* From one split read, the card's cost per sector and per command: the head
 * (a sectors) took head_us, the tail (t sectors) tail_us, both one command:
 *   per sector = (head - tail) / (a - t),  per command = tail - t * per sector.
 * Running averages; a pair that does not fit (noise) is skipped. */
static void pipe_learn_io(Dev *d, uint32_t head_us, uint32_t a, uint32_t tail_us, uint32_t t)
{
	uint64_t r, cmd;

	if (a <= t + 16 || head_us <= tail_us)
		return;
	r = (uint64_t)(head_us - tail_us) * 1000u / (a - t);
	if ((uint64_t)tail_us * 1000u <= r * t)
		cmd = 0;
	else
		cmd = (uint64_t)tail_us * 1000u - r * t;
	if (r > 1000000u || cmd > 20000000u)
		return;                             /* not a plausible pair */
	if (!d->est_io_ns)
		d->est_io_ns = PIPE_DEF_IO_NS;
	d->est_io_ns = d->est_io_ns - (d->est_io_ns >> 3) + (uint32_t)(r >> 3);
	d->est_cmd_ns = d->est_cmd_ns - (d->est_cmd_ns >> 3) + (uint32_t)(cmd >> 3);
}

/* A running average (1/8 weight) of a per-sector cost in ns. */
static void pipe_learn(uint32_t *est, uint32_t us, uint32_t sectors, uint32_t def)
{
	uint32_t ns;

	if (!sectors)
		return;
	ns = (uint32_t)((uint64_t)us * 1000u / sectors);
	if (!*est)
		*est = def;
	*est = *est - (*est >> 3) + (ns >> 3);
}

/* The helper: runs one card read at a time for its device, into buf2. */
static int pipe_helper(SceSize args, void *argp)
{
	Dev *d;

	(void)args;
	memcpy(&d, argp, sizeof(d));          /* the start argument, copied */
	for (;;) {
		uint32_t t0 = 0;

		ksceKernelWaitSema(d->sem_go, 1, NULL);
		if (d->pipe_quit)
			break;
		__sync_synchronize();               /* the job before its fields */
		if (g_stats)
			t0 = ksceKernelGetSystemTimeLow();
		d->job_ret = d->job_io(d->job_ref, d->job_part, d->job_sector, d->buf2, d->job_n);
		d->job_us = g_stats ? ksceKernelGetSystemTimeLow() - t0 : 0;
		__sync_synchronize();               /* the result before the signal */
		ksceKernelSignalSema(d->sem_done, 1);
	}
	return 0;
}

static void pipe_release(Dev *d)
{
	if (d->th >= 0) {
		d->pipe_quit = 1;
		__sync_synchronize();
		ksceKernelSignalSema(d->sem_go, 1);
		ksceKernelWaitThreadEnd(d->th, NULL, NULL);
		ksceKernelDeleteThread(d->th);
	}
	if (d->sem_go >= 0)
		ksceKernelDeleteSema(d->sem_go);
	if (d->sem_done >= 0)
		ksceKernelDeleteSema(d->sem_done);
	if (d->uid2 >= 0)
		ksceKernelFreeMemBlock(d->uid2);
	d->th = d->sem_go = d->sem_done = d->uid2 = -1;
	d->buf2 = NULL;
	d->pipe_made = 0;
}

/* Worker thread: everything the overlapped reads of device `d` need. On any
 * failure nothing is kept and the device reads as before. Returns 0 or the
 * error. */
static int pipe_setup(Dev *d)
{
	Dev *arg = d;
	int r;

	d->th = d->sem_go = d->sem_done = d->uid2 = -1;
	d->buf2 = NULL;
	d->pipe_quit = 0;
	r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, PIPE_TAIL_SECTORS * 512, &d->uid2, &d->buf2);
	if (r < 0) {
		d->uid2 = -1;
		d->buf2 = NULL;
		return r;
	}
	d->sem_go = ksceKernelCreateSema("SceVitaMemioPipeGo", 0, 0, 1, NULL);
	d->sem_done = ksceKernelCreateSema("SceVitaMemioPipeDone", 0, 0, 1, NULL);
	if (d->sem_go < 0 || d->sem_done < 0) {
		r = d->sem_go < 0 ? d->sem_go : d->sem_done;
		pipe_release(d);
		return r;
	}
	d->th = ksceKernelCreateThread("SceVitaMemioPipe", pipe_helper, 0x40, 0x2000, 0, 0, NULL);
	if (d->th < 0) {
		r = d->th;
		d->th = -1;
		pipe_release(d);
		return r;
	}
	r = ksceKernelStartThread(d->th, sizeof(arg), &arg);
	if (r < 0) {
		ksceKernelDeleteThread(d->th);
		d->th = -1;
		pipe_release(d);
		return r;
	}
	d->pipe_made = 1;
	__sync_synchronize();
	d->pipe_ready = 1;                    /* the hooks may use it from now on */
	return 0;
}

static int take_shadow(void)
{
	return g_verify && g_shadow && !__sync_lock_test_and_set(&g_shadow_busy, 1);
}

/* One request, either way. `rd_ref` is the read function, used by a write's
 * verify re-read (a read verifies with `ref` itself). */
static int stage_xfer(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                      void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	uint8_t *p = buf;
	uint32_t left = nsec;
	int verify, ret = 0, mine = on_port(part, d->port);

	if (mine && g_count_all) {
		if (write)
			d->all_wr += (uint64_t)nsec * 512;
		else
			d->all_rd += (uint64_t)nsec * 512;
	}
	/* A non-cacheable caller buffer: the standard path is the faster one
	 * (see the top). Looked at only for transfers that would be staged. */
	if (mine && d->enabled && !d->fallback && d->buf && nsec && nsec >= g_min_sectors &&
	    caller_uncached(p, nsec * 512)) {
		g_nc_stock_kib += nsec / 2;
		return io(ref, part, sector, buf, nsec);
	}
	if (!take(d, mine, nsec))
		return io(ref, part, sector, buf, nsec);
	if (!write)
		rd_ref = ref;
	verify = take_shadow();
	if (verify && !rd_ref) {            /* no read hook yet: cannot verify this one */
		__sync_lock_release(&g_shadow_busy);
		verify = 0;
	}
	const int stats = g_stats;
	const int clean = !write && g_gpu_safe && g_dcache_clean;
	const int pipe = !write && !verify && g_pipeline && d->pipe_ready;

	if (clean)
		g_gpu_cleaned_kib += left / 2;
	while (left && !d->fallback) {
		uint32_t n = left > STAGE_SECTORS ? STAGE_SECTORS : left, bytes = n * 512;
		uint32_t t1 = 0, t2 = 0, t3 = 0;

		/* Timing (statistics on): system call vs CPU copy, per chunk that
		 * succeeded. Off, the clock is not read at all. */
		if (stats)
			t1 = ksceKernelGetSystemTimeLow();
		if (write) {
			vm_copy_to_stage(d->buf, p, bytes);
			if (stats)
				t2 = ksceKernelGetSystemTimeLow();
			ret = io(ref, part, sector, d->buf, n);
			if (ret < 0)
				break;
			if (stats) {
				t3 = ksceKernelGetSystemTimeLow();
				d->tm.wr_sectors += n;
				d->tm.wr_copy_us += t2 - t1;
				d->tm.wr_io_us += t3 - t2;
			}
		} else if (pipe && n >= PIPE_MIN_SECTORS) {
			/* Overlapped: the head here, the tail on the helper meanwhile. */
			const uint32_t t = pipe_tail(d, n, clean), a = n - t;
			uint32_t t4 = 0, t5 = 0;

			ret = io(ref, part, sector, d->buf, a);
			if (ret < 0)
				break;
			if (stats)
				t2 = ksceKernelGetSystemTimeLow();
			d->job_io = io;
			d->job_ref = ref;
			d->job_part = part;
			d->job_sector = sector + a;
			d->job_n = t;
			__sync_synchronize();
			ksceKernelSignalSema(d->sem_go, 1);
			vm_copy_from_stage(p, d->buf, a * 512);
			if (clean)
				clean_out(p, a * 512);
			if (stats)
				t3 = ksceKernelGetSystemTimeLow();
			ksceKernelWaitSema(d->sem_done, 1, NULL);
			__sync_synchronize();
			ret = d->job_ret;
			if (ret < 0)
				break;                  /* the whole part is read again stock */
			if (stats)
				t4 = ksceKernelGetSystemTimeLow();
			vm_copy_from_stage(p + a * 512, d->buf2, t * 512);
			if (clean)
				clean_out(p + a * 512, t * 512);
			g_pipe_reads++;
			if (stats) {
				t5 = ksceKernelGetSystemTimeLow();
				d->tm.rd_sectors += n;
				d->tm.rd_io_us += (t2 - t1) + d->job_us;
				d->tm.rd_copy_us += (t3 - t2) + (t5 - t4);
				pipe_learn_io(d, t2 - t1, a, d->job_us, t);
				pipe_learn(&d->est_cp_ns, t3 - t2, a,
				           clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS);
			}
		} else {
			ret = io(ref, part, sector, d->buf, n);
			if (ret < 0)
				break;
			if (stats)
				t2 = ksceKernelGetSystemTimeLow();
			vm_copy_from_stage(p, d->buf, bytes);
			if (clean)
				clean_out(p, bytes);   /* counted as copy time */
			if (stats) {
				t3 = ksceKernelGetSystemTimeLow();
				d->tm.rd_sectors += n;
				d->tm.rd_io_us += t2 - t1;
				d->tm.rd_copy_us += t3 - t2;
			}
		}
		if (verify && io(rd_ref, part, sector, g_shadow, n) >= 0) {
			d->st.verified++;
			if (memcmp(p, g_shadow, bytes) != 0) {
				d->st.mismatches++;
				d->fallback = 1;
				if (write) {
					/* rewrite this chunk the stock way, from the caller's data */
					ret = io(ref, part, sector, p, n);
					if (ret < 0)
						break;
				} else {
					memcpy(p, g_shadow, bytes);   /* the caller gets stock data */
					if (clean)
						clean_out(p, bytes);
				}
			}
		}
		if (stats) {
			if (write)
				d->wr_bytes += bytes;
			else
				d->rd_bytes += bytes;
		}
		p += bytes;
		sector += n;
		left -= n;
	}
	if (verify)
		__sync_lock_release(&g_shadow_busy);
	__sync_lock_release(&d->busy);
	if (ret < 0) {
		d->st.errors++;
		d->fallback = 1;
	} else if (write) {
		d->st.writes++;
	} else {
		d->st.reads++;
	}
	/* Anything not done on the faster path (error, mismatch) is done stock. */
	return left ? io(ref, part, sector, p, left) : ret;
}

/* The six hooks: a read and a write per device, SceSdif with a card context,
 * SceMsif without. */
#define SDIF_HOOKS(name, dev, h_rd, h_wr)                                                  \
	static int h_rd_##name(void *part, uint32_t sector, void *buf, uint32_t nsec)      \
	{                                                                                  \
		tai_hook_ref_t r = ref_of(h_rd);                                           \
		return r ? stage_xfer(&g_dev[dev], 0, io_sdif, r, 0, part, sector, buf, nsec) \
		         : VM_IO_ERROR;                                                    \
	}                                                                                  \
	static int h_wr_##name(void *part, uint32_t sector, void *buf, uint32_t nsec)      \
	{                                                                                  \
		tai_hook_ref_t r = ref_of(h_wr);                                           \
		return r ? stage_xfer(&g_dev[dev], 1, io_sdif, r, g_ref[h_rd], part, sector,  \
		                      buf, nsec)                                           \
		         : VM_IO_ERROR;                                                    \
	}

SDIF_HOOKS(sd,  VM_DEV_SD2VITA,  H_RD_SD,  H_WR_SD)
SDIF_HOOKS(mmc, VM_DEV_INTERNAL, H_RD_MMC, H_WR_MMC)

static int h_rd_ms(uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_RD_MS);

	return r ? stage_xfer(&g_dev[VM_DEV_MEMCARD], 0, io_msif, r, 0, NULL, sector, buf, nsec)
	         : VM_IO_ERROR;
}

static int h_wr_ms(uint32_t sector, void *buf, uint32_t nsec)
{
	tai_hook_ref_t r = ref_of(H_WR_MS);

	return r ? stage_xfer(&g_dev[VM_DEV_MEMCARD], 1, io_msif, r, g_ref[H_RD_MS], NULL,
	                      sector, buf, nsec)
	         : VM_IO_ERROR;
}

int vm_stage_init(void)
{
	static const struct { int h; uint32_t nid; void *fn; } k_hooks[H_COUNT] = {
		{ H_RD_SD,  NID_READ_SD,   h_rd_sd  },
		{ H_WR_SD,  NID_WRITE_SD,  h_wr_sd  },
		{ H_RD_MMC, NID_READ_MMC,  h_rd_mmc },
		{ H_WR_MMC, NID_WRITE_MMC, h_wr_mmc },
		{ H_RD_MS,  NID_READ_MS,   h_rd_ms  },
		{ H_WR_MS,  NID_WRITE_MS,  h_wr_ms  },
	};

	{
		uintptr_t f = 0;

		if (module_get_export_func(KERNEL_PID, "SceSysmem", LIB_CPU_DRIVER, NID_DCACHE_CLEAN,
		                           &f) >= 0)
			g_dcache_clean = (void (*)(void *, unsigned int))f;
	}
	/* Buffers for the devices already switched on, now at boot while
	 * contiguous memory is easy to find; the others get theirs when they are
	 * switched on (vm_stage_apply). */
	g_live = 1;
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (g_dev[i].enabled)
			ensure_buf(&g_dev[i]);
	for (int i = 0; i < H_SDIF_COUNT; i++) {
		g_hook[k_hooks[i].h] = taiHookFunctionExportForKernel(KERNEL_PID,
			(tai_hook_ref_t *)&g_ref[k_hooks[i].h], "SceSdif", LIB_SDIF, k_hooks[i].nid,
			k_hooks[i].fn);
		if (g_hook[k_hooks[i].h] < 0) {
			VM_KLOGE("stage: hook %d failed (0x%08x) - releasing all", i,
			         (unsigned)g_hook[k_hooks[i].h]);
			vm_stage_fini();
			return VM_ERR_RESOLVE;
		}
	}
	/* The memory card is optional: if its hooks can't be installed, only the
	 * memory card stays stock (its buffer is released and never allocated). */
	for (int i = H_SDIF_COUNT; i < H_COUNT; i++) {
		g_hook[k_hooks[i].h] = taiHookFunctionExportForKernel(KERNEL_PID,
			(tai_hook_ref_t *)&g_ref[k_hooks[i].h], "SceMsif", LIB_MSIF, k_hooks[i].nid,
			k_hooks[i].fn);
		if (g_hook[k_hooks[i].h] < 0) {
			VM_KLOGW("stage: memory card hook %d failed (0x%08x) - memory card stays stock",
			         i, (unsigned)g_hook[k_hooks[i].h]);
			for (int j = H_SDIF_COUNT; j < H_COUNT; j++)
				if (g_hook[j] >= 0) {
					taiHookReleaseForKernel(g_hook[j], g_ref[j]);
					g_hook[j] = -1;
				}
			Dev *m = &g_dev[VM_DEV_MEMCARD];
			m->unusable = 1;
			m->buf = NULL;
			if (m->uid >= 0) {
				ksceKernelFreeMemBlock(m->uid);
				m->uid = -1;
			}
			break;
		}
	}
	return VM_OK;
}

void vm_stage_fini(void)
{
	/* Stop taking the faster path, then wait for any transfer in flight. */
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		g_dev[i].enabled = 0;
		g_dev[i].unusable = 1;          /* and never allocate again */
	}
	for (int i = 0; i < VM_DEV_COUNT; i++)
		while (g_dev[i].busy)
			ksceKernelDelayThread(1000);
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		g_dev[i].pipe_ready = 0;          /* no transfer is in flight now */
		if (g_dev[i].pipe_made)
			pipe_release(&g_dev[i]);
	}
	for (int i = H_COUNT - 1; i >= 0; i--)
		if (g_hook[i] >= 0) {
			taiHookReleaseForKernel(g_hook[i], g_ref[i]);
			g_hook[i] = -1;
		}
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (g_dev[i].uid >= 0) {
			g_dev[i].buf = NULL;
			ksceKernelFreeMemBlock(g_dev[i].uid);
			g_dev[i].uid = -1;
		}
	if (g_shadow_uid >= 0) {
		ksceKernelFreeMemBlock(g_shadow_uid);
		g_shadow_uid = -1;
		g_shadow = NULL;
	}
}

void vm_stage_apply(const VmSettings *s)
{
	const int32_t want[VM_DEV_COUNT] = { s->sd2vita, s->internal, s->memcard };

	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];

		if (want[i] && !d->enabled) {
			d->fallback = 0;      /* switching it back on is a fresh start */
			d->alloc_err = 0;     /* ... and a fresh try for its buffer    */
		}
		if (want[i])
			ensure_buf(d);
		d->enabled = want[i] ? 1 : 0;
	}
	g_min_sectors = s->small ? 1 : MIN_SECTORS;
	g_stats = s->stats ? 1 : 0;
	g_gpu_safe = s->gpusafe ? 1 : 0;
	if (s->pipeline != g_pipeline) {
		for (int i = 0; i < VM_DEV_COUNT; i++) {
			g_dev[i].est_cp_ns = 0;   /* measured afresh */
			if (s->pipeline)
				g_dev[i].pipe_failed = 0;   /* switching it on again is a fresh try */
		}
	}
	g_pipeline = s->pipeline ? 1 : 0;
	g_count_all = s->stats || s->hud != VM_HUD_OFF;
	g_verify = s->verify ? 1 : 0;
	if (!g_verify)
		g_shadow_failed = 0;      /* turning verify off and on again retries the buffer */
}

void vm_stage_worker_tick(void)
{
	static uint32_t ticks, last_cleaned, last_nc;

	if (g_gpu_safe && !g_dcache_clean && g_live && !g_gpu_safe_logged) {
		g_gpu_safe_logged = 1;
		VM_KLOGW("stage: GPU-safe reads are on but the cache clean function was not found");
	}
	/* Once a minute, if anything changed: how much went to non-cacheable
	 * buffers the standard way, and how much GPU-safe reads cleaned. */
	if (++ticks % 60 == 0 && (g_gpu_cleaned_kib != last_cleaned || g_nc_stock_kib != last_nc)) {
		last_cleaned = g_gpu_cleaned_kib;
		last_nc = g_nc_stock_kib;
		VM_KLOGI("stage: non-cacheable buffers (standard path) %u KiB, GPU-safe cleaned %u KiB, "
		         "overlapped reads %u", (unsigned)last_nc, (unsigned)last_cleaned,
		         (unsigned)g_pipe_reads);
	}
	/* Logged here, not where it happens (that can be a syscall). */
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (g_dev[i].alloc_err && !g_dev[i].alloc_logged) {
			g_dev[i].alloc_logged = 1;
			VM_KLOGW("stage: dev %d staging buffer not allocated (0x%08x) - stock", i,
			         (unsigned)g_dev[i].alloc_err);
		}
	/* The experimental copy variant, by its flag file (every 5 s). */
	if (ticks % 5 == 0) {
		SceUID fd = ksceIoOpen(VM_DATA_DIR "/copy2.on", SCE_O_RDONLY, 0);
		int on = fd >= 0;

		if (fd >= 0)
			ksceIoClose(fd);
		if (on != g_copy2) {
			g_copy2 = on;
			VM_KLOGI("stage: copy variant %s", on ? "2 (load in flight) on" : "1 (default)");
		}
	}
	/* Overlapped reads: the helper and its buffer for every device switched on,
	 * made here (never in a transfer) and then kept. */
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];

		if (g_pipeline && g_live && d->enabled && d->buf && !d->unusable &&
		    !d->pipe_made && !d->pipe_failed) {
			int r = pipe_setup(d);

			if (r < 0) {
				d->pipe_failed = 1;
				VM_KLOGW("stage: dev %d overlapped reads not set up (0x%08x) - reads as before",
				         i, (unsigned)r);
			}
		}
	}
	/* The verify buffer is only allocated once verify mode is first turned on. */
	if (g_verify && !g_shadow && g_shadow_uid < 0 && !g_shadow_failed) {
		int r = alloc_buf(SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, 0, STAGE_BYTES, &g_shadow_uid,
		                  &g_shadow);

		if (r < 0) {
			g_shadow = NULL;
			g_shadow_uid = -1;
			g_shadow_failed = 1;    /* do not retry every second */
			VM_KLOGW("stage: verify buffer not allocated (0x%08x)", (unsigned)r);
		}
	}
}

void vm_stage_bytes(int dev, uint64_t *rd, uint64_t *wr)
{
	*rd = g_dev[dev].all_rd;
	*wr = g_dev[dev].all_wr;
}

int vm_stage_verify_ready(void)
{
	return g_shadow != NULL;
}

void vm_stage_get(VmDevState out[VM_DEV_COUNT], int plugin_active)
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const Dev *d = &g_dev[i];

		out[i] = d->st;
		out[i].read_mib = (uint32_t)(d->rd_bytes >> 20);
		out[i].write_mib = (uint32_t)(d->wr_bytes >> 20);
		out[i].time_ms = 0;           /* no longer measured (VmDevState is frozen) */
		out[i].max_ms = 0;
		out[i].status = !plugin_active ? VM_ST_UNAVAIL :
		                !d->enabled    ? VM_ST_OFF :
		                !d->buf        ? VM_ST_NO_BUFFER :
		                d->fallback    ? VM_ST_FALLBACK : VM_ST_ACTIVE;
	}
}

void vm_stage_timing(VmTimingDev out[VM_DEV_COUNT])
{
	for (int i = 0; i < VM_DEV_COUNT; i++)
		out[i] = g_dev[i].tm;
}

/* Memory card slow mode (Sony's own throttle: it sleeps after every memory
 * card transfer). Read only; vita-memio never changes it. 1 on, 0 off,
 * -1 if the driver's getter isn't available. */

int vm_stage_memcard_slow(void)
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
