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
 *   - overlapped reads (Auto by default, see VM_PIPE_*): a faster read is
 *     split into two or three parts (pipe_plan; Fixed from PIPE_MIN_SECTORS,
 *     Auto from PIPE_MIN_AUTO when that beats one piece). The caller's thread
 *     reads the first into the staging buffer; the device's helper thread (on
 *     the system core) reads the next ones back to back while the caller
 *     copies the one before (pipe_run). The parts are sized from the
 *     measured costs so each copy is hidden behind the next read. On Auto a
 *     request larger than the staging buffer is read as one stream of
 *     parts through both buffers instead (stream_plan). Nothing is
 *     read ahead or kept: every part belongs to the request, which is
 *     complete when it returns. Any error finishes the whole part stock, as
 *     above;
 *   - verify mode re-reads every faster transfer the stock way and compares;
 *     on a mismatch the caller gets stock data (reads) or the chunk is
 *     rewritten the stock way (writes), and the device falls back.
 *
 * HOT PATH: no logging here, only counters (the worker logs).
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
#ifndef vm_copy_from_stage
#define vm_copy_from_stage vm_fast_copy
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
	SceUID        uid2, th, sem_go, sem_done, sem_free;
	void         *buf2;
	/* the job: parts 1 .. k-1 of job_plan, from job_sector on (see pipe_run) */
	io_fn         job_io;
	tai_hook_ref_t job_ref;
	void         *job_part;
	const struct Stream *job_plan;
	uint32_t      job_sector;
	int           job_ret;                /* the failed part's error           */
	uint32_t      job_fail;               /* 0, or the part that failed        */
	uint32_t      job_io_us, job_last_us, job_big_us, job_big_n;   /* timings */
	uint32_t      est_io_ns;              /* per sector, for the split        */
	uint32_t      est_cp_ns[2];           /* the copy: [0] plain, [1] + clean */
	uint32_t      est_cmd_ns;             /* the fixed cost of one card read   */
} Dev;

static Dev g_dev[VM_DEV_COUNT] = {
	[VM_DEV_SD2VITA]  = { .port = 1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1 },
	[VM_DEV_INTERNAL] = { .port = 0, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1 },
	[VM_DEV_MEMCARD]  = { .port = -1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1 },
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

#define PIPE_MIN_SECTORS   128        /* Fixed: 64 KiB, smaller reads are not split */
#define PIPE_MIN_AUTO      32         /* Auto: from 16 KiB, when the costs say so   */
/* Auto splits a read only when the plan beats one piece by more than this:
 * what the costs leave out, chiefly waking the helper. */
#define PIPE_HANDOFF_NS    50000u
#define PIPE_MIN_TAIL      8
#define PIPE_TAIL_SECTORS  (STAGE_SECTORS / 2)   /* the second buffer's size    */
/* Per-sector costs before anything is measured (SD2Vita): the card read, and
 * the copy without and with the GPU-safe clean. */
#define PIPE_DEF_IO_NS     25000u
#define PIPE_DEF_CP_NS     4200u
#define PIPE_DEF_CPCL_NS   11100u
#define PIPE_DEF_CMD_NS    600000u    /* a card read's fixed cost            */
#define PIPE_MAX_PARTS     3

#define PIPE_CPU_SYSTEM    0x00080000 /* SCE_KERNEL_CPU_MASK_SYSTEM: core 3      */

static volatile int g_pipeline;              /* the setting                    */
static void (*volatile g_on_problem)(void);  /* wakes the worker to log       */
static volatile uint32_t g_pipe_reads;       /* split reads, for the log       */
static volatile uint32_t g_pipe_reads3;      /* ... of them in three parts      */
static volatile uint32_t g_stream_reads;     /* streamed requests (Auto, > 256 KiB) */
static volatile uint32_t g_rd_max;           /* the largest read request, sectors */

/* How to split an n-sector read: into 2 or 3 parts, read one after another,
 * part i+1 (by the helper) while part i is copied (by the caller). A part's
 * copy is hidden when it takes no longer than the next part's read:
 *     s[i] * cp <= cmd + s[i+1] * io
 * Then the whole read takes  k * cmd + n * io + s[k-1] * cp : every command,
 * every sector, and the last part's copy. For each k the smallest last part
 * that still lets the earlier parts (each as large as it may be) cover n is
 * found, and the k with the lower cost wins. Without GPU-safe the copy is
 * cheap and 2 parts win; with it, a third command buys a much shorter last
 * part. Odd parts go to the second buffer (PIPE_TAIL_SECTORS), even ones to
 * the staging buffer. Returns k (0: no plan, read in one piece). */
static uint32_t pipe_cap(int i)
{
	return (i & 1) ? PIPE_TAIL_SECTORS : STAGE_SECTORS;
}

/* The parts for k and a last part of x sectors, each earlier one as large as
 * it may be; s[0] takes the rest. 1 if that covers n exactly with every part
 * within its bounds. */
static int pipe_fit(uint32_t n, int k, uint32_t x, uint64_t io, uint64_t cp, uint64_t cmd,
                    uint32_t s[PIPE_MAX_PARTS])
{
	uint32_t sum = x;

	if (x > pipe_cap(k - 1))
		return 0;
	s[k - 1] = x;
	for (int i = k - 2; i >= 0; i--) {
		uint64_t m = (cmd + (uint64_t)s[i + 1] * io) / cp;

		if (m > pipe_cap(i))
			m = pipe_cap(i);
		if (i == 0) {
			if (sum >= n || n - sum > m || n - sum < PIPE_MIN_TAIL)
				return 0;
			s[0] = n - sum;
			return 1;
		}
		if (m < PIPE_MIN_TAIL)
			return 0;
		s[i] = (uint32_t)m;
		sum += s[i];
	}
	return 0;
}

/* The plan for k in [kmin, kmax] and the given costs. */
static int pipe_plan_costs(uint32_t n, uint64_t io, uint64_t cp, uint64_t cmd, int kmin, int kmax,
                           uint32_t out[PIPE_MAX_PARTS])
{
	uint64_t best = ~(uint64_t)0;
	int best_k = 0;

	for (int k = kmin; k <= kmax; k++) {
		uint32_t s[PIPE_MAX_PARTS], lo = PIPE_MIN_TAIL, hi = n / k, x = 0;

		/* The earlier parts only grow with x, so the x that fit form one
		 * range: below it the parts do not reach n, above it they leave
		 * part 0 too little. Find its start. */
		while (lo <= hi) {
			uint32_t mid = lo + (hi - lo) / 2, t[PIPE_MAX_PARTS], sum = mid;

			if (pipe_fit(n, k, mid, io, cp, cmd, s)) {
				x = mid;
				hi = mid - 1;
				continue;
			}
			if (mid > pipe_cap(k - 1)) {
				hi = mid - 1;
				continue;
			}
			t[k - 1] = mid;
			for (int i = k - 2; i >= 1; i--) {
				uint64_t m = (cmd + (uint64_t)t[i + 1] * io) / cp;

				t[i] = (uint32_t)(m > pipe_cap(i) ? pipe_cap(i) : m);
				sum += t[i];
			}
			if (sum + PIPE_MIN_TAIL > n)
				hi = mid - 1;               /* part 0 left too small */
			else
				lo = mid + 1;               /* the parts do not reach n */
		}
		if (x && pipe_fit(n, k, x, io, cp, cmd, s)) {
			uint64_t cost = (uint64_t)k * cmd + (uint64_t)x * cp;

			if (cost < best) {
				best = cost;
				best_k = k;
				for (int i = 0; i < k; i++)
					out[i] = s[i];
			}
		}
	}
	if (!best_k && kmin <= 2 && n >= 2 * PIPE_MIN_TAIL) {
		/* no part can be hidden (a copy slower than the card): halves */
		out[1] = n / 2 > PIPE_TAIL_SECTORS ? PIPE_TAIL_SECTORS : n / 2;
		out[0] = n - out[1];
		best_k = out[0] <= STAGE_SECTORS ? 2 : 0;
	}
	return best_k;
}

/* Auto: the learnt costs (built-in ones until measured), 2 or 3 parts, or
 * none when one piece is as fast (small reads, a cheap copy).
 * Fixed: the built-in costs, 2 parts, or 3 with GPU-safe reads (2 if 3 do
 * not fit this size). */
static int pipe_plan(const Dev *d, uint32_t n, int clean, uint32_t out[PIPE_MAX_PARTS])
{
	const uint64_t def_cp = clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS;
	int k;

	if (g_pipeline == VM_PIPE_FIXED) {
		k = pipe_plan_costs(n, PIPE_DEF_IO_NS, def_cp, PIPE_DEF_CMD_NS, clean ? 3 : 2,
		                    clean ? 3 : 2, out);
		if (!k && clean)
			k = pipe_plan_costs(n, PIPE_DEF_IO_NS, def_cp, PIPE_DEF_CMD_NS, 2, 2, out);
		return k;
	}
	{
		const uint64_t io = d->est_io_ns ? d->est_io_ns : PIPE_DEF_IO_NS;
		const uint64_t cp = d->est_cp_ns[clean ? 1 : 0] ? d->est_cp_ns[clean ? 1 : 0] : def_cp;
		const uint64_t cmd = d->est_cmd_ns ? d->est_cmd_ns : PIPE_DEF_CMD_NS;

		k = pipe_plan_costs(n, io, cp, cmd, 2, PIPE_MAX_PARTS, out);
		/* in one piece: one command and the whole copy, nothing to hand off */
		if (k >= 2 && (uint64_t)k * cmd + (uint64_t)out[k - 1] * cp + PIPE_HANDOFF_NS >=
		              cmd + (uint64_t)n * cp)
			k = 0;
		return k;
	}
}

/* ---- streamed overlapped reads (Auto, requests over STAGE_SECTORS) ----------
 *
 * A request larger than the staging buffer is not cut into separate 256 KiB
 * chunks, each with its own short tail; it is read as one stream of parts,
 * alternately into the staging buffer (up to STAGE_SECTORS) and the second
 * buffer (up to PIPE_TAIL_SECTORS), each read by the helper while the caller
 * copies the one before. Only the very last part's copy is not hidden, and
 * the whole request takes about  k * cmd + n * io + last * cp.
 *
 * The plan is built from the end: the last part is x sectors; each part
 * before it as large as it may be while its copy still hides behind the
 * next part's read (the rule above), up to its buffer's size. A few parts
 * grow from x (the ramp); then two sizes repeat (usually both buffers' full
 * sizes), and part 0 takes what is left. Which buffer the last part goes to
 * and x are chosen for the lowest cost. */
#define STREAM_RAMP_MAX  16   /* the plan lives on the caller's stack */

typedef struct Stream {
	uint32_t k;                       /* parts                                */
	uint32_t first;                   /* part 0's size                        */
	uint32_t ramp_len;                /* the last ramp_len parts: ramp[j] is  */
	uint32_t ramp[STREAM_RAMP_MAX];   /* the j-th from the end                */
	uint32_t rep[2];                  /* before the ramp: rep[0], rep[1], ... */
	int      last_buf;                /* the last part's buffer: 0 staging, 1 the second */
} Stream;

static inline uint32_t stream_cap(int b)
{
	return b ? PIPE_TAIL_SECTORS : STAGE_SECTORS;
}

/* Part i's buffer (0 staging, 1 second) and size. */
static inline int stream_buf(const Stream *s, uint32_t i)
{
	return s->last_buf ^ (int)((s->k - 1 - i) & 1);
}

static inline uint32_t stream_part(const Stream *s, uint32_t i)
{
	const uint32_t j = s->k - 1 - i;

	if (i == 0)
		return s->first;
	return j < s->ramp_len ? s->ramp[j] : s->rep[(j - s->ramp_len) & 1];
}

/* The plan for a last part of x sectors in buffer lb. 1 if it covers n with
 * at least two parts, every part within its buffer. Each part depends only on
 * the next one and on its buffer, and the buffers alternate: once a size
 * repeats two parts back, the two sizes repeat from there on (often both
 * buffers' full sizes), and the rest is counted, not walked. */
static int stream_fit(uint32_t n, uint32_t x, int lb, uint64_t io, uint64_t cp, uint64_t cmd,
                      Stream *o)
{
	uint64_t sum = x;
	uint32_t j = 0;

	if (x < PIPE_MIN_TAIL || x > stream_cap(lb) || !cp)
		return 0;
	o->last_buf = lb;
	o->rep[0] = o->rep[1] = 0;
	o->ramp[0] = x;
	for (;;) {
		uint64_t m;
		uint32_t c;

		if (sum >= n) {                     /* ends within the ramp */
			if (!j)
				return 0;                   /* one part: nothing to overlap */
			o->k = j + 1;
			o->ramp_len = j + 1;
			o->first = (uint32_t)(n - (sum - o->ramp[j]));
			return 1;
		}
		if (j >= 2 && o->ramp[j] == o->ramp[j - 2])
			break;                          /* repeats from here on */
		m = (cmd + (uint64_t)o->ramp[j] * io) / cp;
		c = stream_cap(lb ^ (int)((j + 1) & 1));
		if (m > c)
			m = c;
		if (m < PIPE_MIN_TAIL || j + 1 >= STREAM_RAMP_MAX)
			return 0;
		o->ramp[++j] = (uint32_t)m;
		sum += m;
	}
	/* from j + 1 on: ramp[j - 1], ramp[j], ramp[j - 1], ...; part 0 takes the rest */
	{
		const uint32_t a = o->ramp[j - 1], b = o->ramp[j];
		const uint64_t pair = (uint64_t)a + b;
		uint64_t r = n - sum, pairs = (r - 1) / pair;
		uint32_t jn = j + 1;

		r -= pairs * pair;
		jn += (uint32_t)(2 * pairs);
		while (r > ((jn - j) & 1 ? a : b)) {
			r -= (jn - j) & 1 ? a : b;
			jn++;
		}
		o->k = jn + 1;
		o->ramp_len = j + 1;
		o->first = (uint32_t)r;
		o->rep[0] = a;
		o->rep[1] = b;
	}
	return 1;
}

/* The lowest-cost plan for an n-sector read. The earlier parts only grow
 * with x, so the part count only falls as x grows: for each count the
 * smallest x reaching it is found by bisection. 1 if there is a plan. */
static int stream_plan_costs(uint32_t n, uint64_t io, uint64_t cp, uint64_t cmd, Stream *out)
{
	uint64_t best = ~(uint64_t)0;
	int found = 0;

	for (int lb = 0; lb <= 1; lb++) {
		uint32_t hi = stream_cap(lb);
		Stream t;

		while (hi >= PIPE_MIN_TAIL && stream_fit(n, hi, lb, io, cp, cmd, &t)) {
			const uint32_t k = t.k;
			uint32_t lo = PIPE_MIN_TAIL, x = hi;
			uint64_t cost;

			/* the smallest x in [lo, hi] with at most k parts */
			while (lo < x) {
				uint32_t mid = lo + (x - lo) / 2;
				Stream u;

				if (stream_fit(n, mid, lb, io, cp, cmd, &u) && u.k <= k)
					x = mid;
				else
					lo = mid + 1;
			}
			if (!stream_fit(n, x, lb, io, cp, cmd, &t))
				break;                      /* cannot happen: hi fits */
			cost = (uint64_t)t.k * cmd + (uint64_t)x * cp;
			if (cost < best) {
				best = cost;
				*out = t;
				found = 1;
			}
			/* more parts cost at least one more command */
			if (x <= PIPE_MIN_TAIL || (uint64_t)(t.k + 1) * cmd >= best)
				break;
			hi = x - 1;
		}
	}
	return found;
}

/* Every part fits its buffer and the parts add up to n: checked once more
 * before anything is read (a part larger than its buffer would be a DMA past
 * its end). Only the ramp, the two repeated sizes and part 0 are distinct. */
static int stream_ok(const Stream *s, uint32_t n)
{
	uint64_t sum = s->first;

	if (s->k < 2 || s->ramp_len > STREAM_RAMP_MAX || s->ramp_len > s->k || !s->first ||
	    s->first > stream_cap(stream_buf(s, 0)))
		return 0;
	for (uint32_t j = 0; j < s->ramp_len && j < s->k - 1; j++) {
		if (!s->ramp[j] || s->ramp[j] > stream_cap(s->last_buf ^ (int)(j & 1)))
			return 0;
		sum += s->ramp[j];
	}
	if (s->k - 1 > s->ramp_len) {           /* parts ramp_len .. k - 2 from the end */
		const uint64_t reps = s->k - 1 - s->ramp_len;
		const int b0 = s->last_buf ^ (int)(s->ramp_len & 1);

		if (!s->rep[0] || s->rep[0] > stream_cap(b0) ||
		    (reps > 1 && (!s->rep[1] || s->rep[1] > stream_cap(b0 ^ 1))))
			return 0;
		sum += (reps + 1) / 2 * s->rep[0] + reps / 2 * s->rep[1];
	}
	return sum == n;
}

/* Auto only: the learnt costs (built-in ones until measured). */
static int stream_plan(const Dev *d, uint32_t n, int clean, Stream *out)
{
	const uint64_t def_cp = clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS;

	return stream_plan_costs(n,
	                         d->est_io_ns ? d->est_io_ns : PIPE_DEF_IO_NS,
	                         d->est_cp_ns[clean ? 1 : 0] ? d->est_cp_ns[clean ? 1 : 0] : def_cp,
	                         d->est_cmd_ns ? d->est_cmd_ns : PIPE_DEF_CMD_NS, out) &&
	       stream_ok(out, n);
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
	if (!d->est_cmd_ns)
		d->est_cmd_ns = PIPE_DEF_CMD_NS;
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

/* The helper: reads parts 1 .. k-1 of a job one after another, each as soon
 * as its buffer is free again (sem_free: the caller has copied the part that
 * used it two parts back), and signals sem_done for each. So the card goes
 * from one part to the next without waiting for the caller's thread to wake
 * up. After a failed part it stops; its sem_free still owed is taken first,
 * so nothing is left over for the next job. */
static int pipe_helper(SceSize args, void *argp)
{
	Dev *d;

	(void)args;
	memcpy(&d, argp, sizeof(d));          /* the start argument, copied */
	for (;;) {
		const struct Stream *s;
		uint32_t sector, k, io_us = 0, big_us = 0, big_n = 0;
		int timed;

		ksceKernelWaitSema(d->sem_go, 1, NULL);
		if (d->pipe_quit)
			break;
		__sync_synchronize();               /* the job before its fields */
		s = d->job_plan;
		k = s->k;
		sector = d->job_sector;
		timed = g_stats || g_pipeline == VM_PIPE_AUTO;   /* see stage_xfer */
		/* The plan is on the caller's stack: not touched after the last
		 * signal, when the caller may already have returned. */
		for (uint32_t i = 1; i < k; i++) {
			const uint32_t n = stream_part(s, i);
			void *buf = stream_buf(s, i) ? d->buf2 : d->buf;
			uint32_t t0, us;
			int r;

			if (i >= 2)
				ksceKernelWaitSema(d->sem_free, 1, NULL);
			t0 = timed ? ksceKernelGetSystemTimeLow() : 0;
			r = d->job_io(d->job_ref, d->job_part, sector, buf, n);
			us = timed ? ksceKernelGetSystemTimeLow() - t0 : 0;
			if (r < 0) {
				d->job_ret = r;
				__atomic_store_n(&d->job_fail, i, __ATOMIC_RELEASE);
				if (i + 1 < k)              /* the caller frees this one first */
					ksceKernelWaitSema(d->sem_free, 1, NULL);
				ksceKernelSignalSema(d->sem_done, 1);
				break;
			}
			io_us += us;
			if (n > big_n) {
				big_n = n;
				big_us = us;
			}
			sector += n;
			if (i + 1 == k) {               /* read by the caller after this signal */
				d->job_io_us = io_us;
				d->job_last_us = us;
				d->job_big_us = big_us;
				d->job_big_n = big_n;
			}
			__sync_synchronize();           /* the part before the signal */
			ksceKernelSignalSema(d->sem_done, 1);
		}
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
	if (d->sem_free >= 0)
		ksceKernelDeleteSema(d->sem_free);
	if (d->uid2 >= 0)
		ksceKernelFreeMemBlock(d->uid2);
	d->th = d->sem_go = d->sem_done = d->sem_free = d->uid2 = -1;
	d->buf2 = NULL;
	d->pipe_made = 0;
}

/* Worker thread: stop the hooks from starting overlapped reads on `d`, wait
 * for the request in flight (if any) to finish, then free the helper and its
 * buffer. A request decides on overlapping only after taking `busy`, so once
 * `busy` drops after pipe_ready went to 0, nothing can use them. */
static void pipe_retire(Dev *d)
{
	__atomic_store_n(&d->pipe_ready, 0, __ATOMIC_SEQ_CST);
	__sync_synchronize();                 /* pairs with the fence in take() */
	/* an acquire load: the request's last use of the buffers comes first */
	while (__atomic_load_n(&d->busy, __ATOMIC_ACQUIRE))
		ksceKernelDelayThread(1000);
	pipe_release(d);
}

/* Worker thread: everything the overlapped reads of device `d` need. On any
 * failure nothing is kept and the device reads as before. Returns 0 or the
 * error. */
static int pipe_setup(Dev *d)
{
	Dev *arg = d;
	int r;

	d->th = d->sem_go = d->sem_done = d->sem_free = d->uid2 = -1;
	d->buf2 = NULL;
	d->pipe_quit = 0;
	r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, PIPE_TAIL_SECTORS * 512, &d->uid2, &d->buf2);
	if (r < 0) {
		d->uid2 = -1;
		d->buf2 = NULL;
		return r;
	}
	/* done and free run at most two ahead of their waiter (see pipe_run) */
	d->sem_go = ksceKernelCreateSema("SceVitaMemioPipeGo", 0, 0, 1, NULL);
	d->sem_done = ksceKernelCreateSema("SceVitaMemioPipeDone", 0, 0, 4, NULL);
	d->sem_free = ksceKernelCreateSema("SceVitaMemioPipeFree", 0, 0, 4, NULL);
	if (d->sem_go < 0 || d->sem_done < 0 || d->sem_free < 0) {
		r = d->sem_go < 0 ? d->sem_go : d->sem_done < 0 ? d->sem_done : d->sem_free;
		pipe_release(d);
		return r;
	}
	/* On the system core (core 3), which games do not run on: the helper then
	 * starts the tail's read at once even when the game's cores are busy. If
	 * that is refused, any core. */
	d->th = ksceKernelCreateThread("SceVitaMemioPipe", pipe_helper, 0x40, 0x2000, 0,
	                               PIPE_CPU_SYSTEM, NULL);
	if (d->th < 0)
		d->th = ksceKernelCreateThread("SceVitaMemioPipe", pipe_helper, 0x40, 0x2000, 0, 0,
		                               NULL);
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
	/* the hooks may use it from now on; everything above is seen first */
	__atomic_store_n(&d->pipe_ready, 1, __ATOMIC_RELEASE);
	return 0;
}

static int take_shadow(void)
{
	return g_verify && g_shadow && !__sync_lock_test_and_set(&g_shadow_busy, 1);
}

/* An overlapped read of plan `s` (a chunk's 2-3 parts, or a whole streamed
 * request) into the caller's p. Part 0 is read here; the helper then reads
 * parts 1 .. k-1 back to back while this thread copies (and, GPU-safe,
 * cleans) each part in turn and hands its buffer back (sem_free) for the part
 * two further on. *done: the sectors already in the caller's buffer when it
 * returns, all of them on success; after an error the rest is the caller's
 * to finish stock. */
static int pipe_run(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                    uint8_t *p, const Stream *s, int clean, int stats, uint32_t *done)
{
	/* Auto times itself, whatever Statistics says: the plan learns its
	 * costs from these few clock reads. Fixed reads the clock only for
	 * Statistics. */
	const int timed = stats || g_pipeline == VM_PIPE_AUTO;
	uint32_t off = 0, n = stream_part(s, 0), cp_us = 0, h0, h1;
	void *cur = stream_buf(s, 0) ? d->buf2 : d->buf;
	int ret;

	*done = 0;
	h0 = timed ? ksceKernelGetSystemTimeLow() : 0;
	ret = io(ref, part, sector, cur, n);
	if (ret < 0)
		return ret;
	h1 = timed ? ksceKernelGetSystemTimeLow() : 0;
	d->job_io = io;
	d->job_ref = ref;
	d->job_part = part;
	d->job_plan = s;
	d->job_sector = sector + n;
	d->job_ret = 0;
	__atomic_store_n(&d->job_fail, 0, __ATOMIC_RELAXED);
	__sync_synchronize();
	ksceKernelSignalSema(d->sem_go, 1);
	for (uint32_t i = 1; i <= s->k; i++) {
		uint32_t c0 = timed ? ksceKernelGetSystemTimeLow() : 0;

		vm_copy_from_stage(p + off * 512, cur, n * 512);
		if (clean)
			clean_out(p + off * 512, n * 512);
		if (timed)
			cp_us += ksceKernelGetSystemTimeLow() - c0;
		off += n;
		*done = off;
		if (i + 1 < s->k)                   /* part i+1 reads into this buffer */
			ksceKernelSignalSema(d->sem_free, 1);
		if (i < s->k) {
			uint32_t f;

			ksceKernelWaitSema(d->sem_done, 1, NULL);
			__sync_synchronize();
			f = __atomic_load_n(&d->job_fail, __ATOMIC_ACQUIRE);
			if (f && f <= i)
				return d->job_ret;          /* this part on is read again stock */
			n = stream_part(s, i);
			cur = stream_buf(s, i) ? d->buf2 : d->buf;
		}
	}
	if (g_pipeline == VM_PIPE_AUTO) {
		/* the largest part against the last one: per sector and per command */
		uint32_t big_us = d->job_big_us, big_n = d->job_big_n;

		if (stream_part(s, 0) > big_n) {
			big_n = stream_part(s, 0);
			big_us = h1 - h0;
		}
		pipe_learn_io(d, big_us, big_n, d->job_last_us, n);
		pipe_learn(&d->est_cp_ns[clean ? 1 : 0], cp_us, off,
		           clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS);
	}
	if (stats) {                            /* the tester's figures */
		d->tm.rd_sectors += off;
		d->tm.rd_io_us += (h1 - h0) + d->job_io_us;
		d->tm.rd_copy_us += cp_us;
	}
	return 0;
}

/* A chunk's 2-3 parts (pipe_plan) as a plan for pipe_run: part 0 into the
 * staging buffer, then alternating. */
static void stream_of_parts(Stream *s, const uint32_t *sz, int k)
{
	memset(s, 0, sizeof(*s));
	s->k = (uint32_t)k;
	s->first = sz[0];
	s->ramp_len = (uint32_t)k;
	for (int j = 0; j < k; j++)
		s->ramp[j] = sz[k - 1 - j];
	s->last_buf = (k - 1) & 1;
}

/* One request, either way. `rd_ref` is the read function, used by a write's
 * verify re-read (a read verifies with `ref` itself). */
static int stage_xfer(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                      void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	uint8_t *p = buf;
	uint32_t left = nsec;
	int verify, ret = 0, mine = on_port(part, d->port);

	/* the largest read request seen, for the log (a lost update is fine) */
	if (mine && !write && nsec > __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED))
		__atomic_store_n(&g_rd_max, nsec, __ATOMIC_RELAXED);

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
	/* GPU-safe reads asked for, but this firmware has no cache clean to
	 * keep that promise: such reads take the standard path, which leaves
	 * the data in memory as promised. */
	if (!write && g_gpu_safe && !g_dcache_clean)
		return io(ref, part, sector, buf, nsec);
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
	int problem = 0;                    /* an error or a mismatch: wake the worker */
	const int clean = !write && g_gpu_safe && g_dcache_clean;
	const int pipe = !write && !verify && g_pipeline &&
	                 __atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE);
	uint32_t sz[PIPE_MAX_PARTS];
	int k = 0;

	if (clean)
		g_gpu_cleaned_kib += left / 2;
	/* Auto, a request larger than the staging buffer: one stream of parts
	 * instead of separate chunks (see stream_plan). */
	if (pipe && g_pipeline == VM_PIPE_AUTO && left > STAGE_SECTORS) {
		Stream st;
		uint32_t done = 0;

		if (stream_plan(d, left, clean, &st)) {
			ret = pipe_run(d, io, ref, part, sector, p, &st, clean, stats, &done);
			if (ret >= 0) {
				g_stream_reads++;
				if (stats)
					d->rd_bytes += (uint64_t)done * 512;
			}
			p += (size_t)done * 512;
			sector += done;
			left -= done;
		}
	}
	while (ret >= 0 && left && !d->fallback) {
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
		} else if (pipe &&
		           n >= (g_pipeline == VM_PIPE_AUTO ? PIPE_MIN_AUTO : PIPE_MIN_SECTORS) &&
		           (k = pipe_plan(d, n, clean, sz)) >= 2) {
			/* Overlapped, this chunk in 2-3 parts (see pipe_run). */
			Stream st;
			uint32_t done = 0;

			stream_of_parts(&st, sz, k);
			ret = pipe_run(d, io, ref, part, sector, p, &st, clean, stats, &done);
			if (ret < 0) {
				p += (size_t)done * 512;    /* the parts already copied stay */
				sector += done;
				left -= done;
				break;
			}
			g_pipe_reads++;
			if (k > 2)
				g_pipe_reads3++;
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
				problem = 1;
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
		problem = 1;
	} else if (write) {
		d->st.writes++;
	} else {
		d->st.reads++;
	}
	if (problem && g_on_problem)
		g_on_problem();
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
		while (__atomic_load_n(&g_dev[i].busy, __ATOMIC_ACQUIRE))
			ksceKernelDelayThread(1000);
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		__atomic_store_n(&g_dev[i].pipe_ready, 0, __ATOMIC_SEQ_CST);   /* nothing in flight now */
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
			g_dev[i].est_cp_ns[0] = g_dev[i].est_cp_ns[1] = 0;   /* measured afresh */
			if (!g_pipeline)
				g_dev[i].pipe_failed = 0;   /* switching it on again is a fresh try */
		}
	}
	g_pipeline = s->pipeline >= VM_PIPE_OFF && s->pipeline <= VM_PIPE_FIXED ? s->pipeline
	                                                                         : VM_PIPE_OFF;
	g_count_all = s->stats || s->hud != VM_HUD_OFF;
	g_verify = s->verify ? 1 : 0;
	if (!g_verify)
		g_shadow_failed = 0;      /* turning verify off and on again retries the buffer */
}

void vm_stage_on_problem(void (*fn)(void))
{
	g_on_problem = fn;
}

void vm_stage_worker_tick(void)
{
	static uint32_t last_log_us, last_cleaned, last_nc, last_max;
	const uint32_t now_us = ksceKernelGetSystemTimeLow();

	if (g_gpu_safe && !g_dcache_clean && g_live && !g_gpu_safe_logged) {
		g_gpu_safe_logged = 1;
		VM_KLOGW("stage: GPU-safe reads are on but the cache clean function was not found - "
		         "reads take the standard path");
	}
	/* Once a minute, if anything changed: how much went to non-cacheable
	 * buffers the standard way, and how much GPU-safe reads cleaned. */
	if (now_us - last_log_us >= 60u * 1000u * 1000u &&
	    (g_gpu_cleaned_kib != last_cleaned || g_nc_stock_kib != last_nc ||
	     __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED) != last_max)) {
		last_log_us = now_us;
		last_cleaned = g_gpu_cleaned_kib;
		last_nc = g_nc_stock_kib;
		last_max = __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED);
		VM_KLOGI("stage: non-cacheable buffers (standard path) %u KiB, GPU-safe cleaned %u KiB, "
		         "overlapped reads %u (%u in 3 parts), streamed %u, largest read %u KiB",
		         (unsigned)last_nc, (unsigned)last_cleaned, (unsigned)g_pipe_reads,
		         (unsigned)g_pipe_reads3, (unsigned)g_stream_reads, (unsigned)(last_max / 2));
	}
	/* Logged here, not where it happens (that can be a syscall). */
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (g_dev[i].alloc_err && !g_dev[i].alloc_logged) {
			g_dev[i].alloc_logged = 1;
			VM_KLOGW("stage: dev %d staging buffer not allocated (0x%08x) - stock", i,
			         (unsigned)g_dev[i].alloc_err);
		}
	/* Overlapped reads: the helper and its buffer exist only while they can
	 * be used - made here (never in a transfer) for every device switched on
	 * while the setting is on, and given back once either is switched off. */
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];
		const int wanted = g_pipeline && g_live && d->enabled && d->buf && !d->unusable;

		if (wanted && !d->pipe_made && !d->pipe_failed) {
			int r = pipe_setup(d);

			if (r < 0) {
				d->pipe_failed = 1;
				VM_KLOGW("stage: dev %d overlapped reads not set up (0x%08x) - reads as before",
				         i, (unsigned)r);
			} else {
				VM_KLOGI("stage: dev %d overlapped reads ready (helper thread, %u KiB buffer)",
				         i, (unsigned)(PIPE_TAIL_SECTORS / 2));
			}
		} else if (!wanted && d->pipe_made) {
			pipe_retire(d);
			VM_KLOGI("stage: dev %d overlapped reads off - helper and buffer released", i);
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
