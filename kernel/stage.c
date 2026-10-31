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
 *     measured costs so each copy is hidden behind the next read. Nothing is
 *     read ahead or kept here (that is "read ahead", off by default): every
 *     part belongs to the request, which is complete when it returns. Any
 *     error finishes the whole part stock, as above;
 *   - copy on the system core (VM_OFFLOAD_*, off by default): a game's read
 *     of 64 KiB or more is copied by a thread on the system core through a
 *     kernel mapping of the game's buffer, used only if it is writable and of
 *     the buffer's own memory type; otherwise the game's thread copies;
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
#include "vm_memblock.h"
#ifndef vm_copy_from_stage
#define vm_copy_from_stage vm_fast_copy
#endif
#ifndef vm_copy_to_stage
#define vm_copy_to_stage memcpy
#endif

/* Words shared between the hooks (any thread, any core), the helper and
 * copier, the worker and the settings (a syscall): each is read and written
 * whole with these, and counters are added to atomically, so no update is
 * lost and no 64-bit counter is read half-written. Relaxed: each value stands
 * on its own; where memory changes hands (a buffer, a job, a result) the
 * order is set separately (semaphores, acquire/release). */
#define LD(x)      __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define ST(x, v)   __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define ADD(x, v)  ((void)__atomic_fetch_add(&(x), (v), __ATOMIC_RELAXED))

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

/* Read ahead: three pieces of 128 KiB, in the staging buffer (two) and the
 * second buffer (one). */
#define RA_PIECE_SECTORS  256
#define RA_PIECES         3
#define RA_FREE           0
#define RA_QUEUED         1           /* for the thread to read             */
#define RA_READING        2
#define RA_READY          3
#define RA_FAILED         4

typedef struct RaPiece {
	int            state;             /* RA_*, atomic                         */
	uint32_t       seq;               /* the order the thread reads them in    */
	uint32_t       sector, n;         /* atomic: a write looks at them         */
	uint32_t       off;               /* sectors already handed out            */
	io_fn          io;
	tai_hook_ref_t ref;
	void          *part;
	uint32_t       epoch, t_done;     /* see ra_valid                          */
	int            ok;                /* read, and no write was running then    */
	int            dirty;             /* a write touched it since it was queued */
} RaPiece;

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
	/* Overlapped reads and copy on the system core: the helper thread (card
	 * reads), the copier thread (copies into a game's mapped buffer), the
	 * second buffer and the one job they run. All valid while pipe_made;
	 * pipe_ready publishes them to the hooks. */
	int           pipe_made, pipe_failed;
	volatile int  pipe_ready, pipe_quit;
	SceUID        uid2, th, sem_go, sem_done, sem_free;
	SceUID        th2, sem_cgo, sem_fin;  /* the copier (copy on the system core) */
	void         *buf2;
	/* the job: parts job_first .. job_k-1 of job_sz, the first of them at
	 * job_sector (pipe_run: from part 1; offload_run: all of them) */
	uint32_t      job_first;
	io_fn         job_io;
	tai_hook_ref_t job_ref;
	void         *job_part;
	uint32_t      job_k, job_sz[3];       /* 3: PIPE_MAX_PARTS                 */
	uint32_t      job_sector;
	int           job_ret;                /* the failed part's error           */
	uint32_t      job_fail;               /* 0, or 1 + the part that failed    */
	uint32_t      job_io_us, job_last_us, job_big_us, job_big_n;   /* timings */
	/* the copier's job (the plan above) and its result */
	struct OffMap *cjob_map;              /* the caller's buffer, mapped by the
	                                       * copier on the request's first chunk */
	uint32_t      cjob_off;               /* this chunk's offset in it, bytes  */
	int           cjob_clean, cjob_last, cjob_ret, cjob_nomap;
	uint32_t      cjob_done, cjob_cp_us;
	uint32_t      est_io_ns;              /* per sector, for the split        */
	uint32_t      est_cp_ns[2];           /* the copy: [0] plain, [1] + clean */
	uint32_t      est_cmd_ns;             /* the fixed cost of one card read   */
	/* a copier job whose parts are already read (read ahead): job_src[i]
	 * instead of the staging buffers, no sem_free */
	int           job_ra;
	const void   *job_src[3];
	/* Read ahead (see "read ahead"): the thread and its pieces, valid while
	 * ra_made; ra_ready publishes them to the hooks. The pieces and the
	 * stream are the busy holder's; the thread only reads queued pieces. */
	int           ra_made, ra_failed;
	volatile int  ra_ready, ra_quit;
	SceUID        ra_th, ra_go, ra_done;
	int           ra_waiting;             /* a request sleeps on ra_done      */
	RaPiece       ra[RA_PIECES];
	uint32_t      ra_seq;                 /* the last piece queued            */
	uint32_t      ra_last_end, ra_streak; /* the read stream so far           */
	int           ra_nc;                  /* ... into non-cacheable memory     */
	void         *ra_last_part;
	uint32_t      ra_last_us;             /* when the stream was last read (atomic) */
	uint32_t      ra_wbusy;               /* writes on the device right now    */
	uint32_t      ra_epoch;               /* + 1 when the device is switched   */
} Dev;

static Dev g_dev[VM_DEV_COUNT] = {
	[VM_DEV_SD2VITA]  = { .port = 1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1,
	                      .th2 = -1, .sem_cgo = -1, .sem_fin = -1,
	                      .ra_th = -1, .ra_go = -1, .ra_done = -1 },
	[VM_DEV_INTERNAL] = { .port = 0, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1,
	                      .th2 = -1, .sem_cgo = -1, .sem_fin = -1,
	                      .ra_th = -1, .ra_go = -1, .ra_done = -1 },
	[VM_DEV_MEMCARD]  = { .port = -1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1,
	                      .th2 = -1, .sem_cgo = -1, .sem_fin = -1,
	                      .ra_th = -1, .ra_go = -1, .ra_done = -1 },
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

	if (!LD(g_live) || __atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) || LD(d->unusable) ||
	    LD(d->alloc_err))
		return;
	r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, STAGE_BYTES, &uid, &base);
	if (r < 0) {
		ST(d->alloc_logged, 0);         /* the worker logs it */
		ST(d->alloc_err, r);
		return;
	}
	d->uid = uid;
	__atomic_store_n(&d->buf, base, __ATOMIC_RELEASE);   /* published last */
}

/* Take the device buffer for one request, or 0 if the request stays stock. */
static int take(Dev *d, int mine, uint32_t nsec)
{
	if (!mine || !LD(d->enabled) || LD(d->fallback) ||
	    !__atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) || nsec < LD(g_min_sectors) || !nsec)
		return 0;
	if (__sync_lock_test_and_set(&d->busy, 1)) {
		ADD(d->st.passthru, 1);
		return 0;
	}
	/* Re-check after taking the buffer: vm_stage_fini clears `enabled` first and
	 * then waits for `busy` to drop, so a request that passed the check above
	 * just before that must not go on to use the buffer. */
	__sync_synchronize();
	if (!LD(d->enabled) || LD(d->fallback)) {
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
static volatile uint32_t g_rd_max;           /* the largest read request, sectors */
/* How long the helpers waited for a caller to copy a part out before they
 * could read the next one into its buffer: the card idle because the
 * caller's thread was late (a busy core). Waits under PIPE_WAIT_US are not
 * counted (the handoff itself). For the log. */
#define PIPE_WAIT_US       200u
static uint32_t g_pipe_waits, g_pipe_wait_us;   /* atomics; us wraps after 71 min */

/* ---- where the copy runs (log) ---------------------------------------------
 *
 * Which core ran each staged read's copy (the caller's thread, or the copier
 * on the system core), and whether the caller was a system thread (kernel
 * process) or a game (a user process). */
static uint32_t g_cp_core_kib[4], g_cp_sys_kib, g_cp_game_kib;    /* atomics */
static volatile int g_log_diag;            /* the log is on: count            */

static inline uint32_t cpu_id(void)
{
#if defined(__arm__)
	uint32_t m;

	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(m));   /* MPIDR */
	return m & 3;
#else
	return 0;
#endif
}

/* A staged read finished: note who the caller was and, for the part its own
 * thread copied, on which core (the copier notes its own). */
static void diag_note(uint32_t nsec, uint32_t own)
{
	if (own)
		__atomic_fetch_add(&g_cp_core_kib[cpu_id()], own / 2, __ATOMIC_RELAXED);
	if (ksceKernelGetProcessId() == KERNEL_PID)
		__atomic_fetch_add(&g_cp_sys_kib, nsec / 2, __ATOMIC_RELAXED);
	else
		__atomic_fetch_add(&g_cp_game_kib, nsec / 2, __ATOMIC_RELAXED);
}

/* ---- copy on the system core (VM_KEY_OFFLOAD) -------------------------------
 *
 * A game's read is copied out of the staging buffers by the copier thread on
 * the system core instead of the game's own thread, which only maps its
 * buffer, hands the work over and sleeps. The buffer is in the game's own
 * address space; the copier reaches it through a kernel mapping of it
 * (ksceKernelProcUserMap, made by the game's thread for the request, removed
 * at its end). The mapping is used only if it is writable (checked on the
 * first one) and of the same memory type as the game's buffer (cached: one
 * coherent copy of the data for every core; checked on every one). Anything
 * else, and the caller copies as before. */
#define VM_USERMAP_RW      3          /* read | write (1: read, 2: write)  */
/* Reads of 16 KiB and up move. The mapping is made by the copier while the
 * card reads, so a small read pays only the two hand-offs; the copy and the
 * GPU-safe clean it frees the game's core from are larger (measured: a 32 KiB
 * clean ~0.26 ms on the game's thread, ~0.16 ms on the copier). The file
 * system reads in 32 KiB pieces, so this covers most of a game's small reads.
 * Under 16 KiB the copy is too short to be worth a hand-off. */
#define OFFLOAD_MIN_SECTORS 32
static volatile int g_offload;             /* the setting, VM_OFFLOAD_*       */
static volatile int g_offload_state;       /* 0 unchecked, 1 usable, < 0 refused */
static int          g_offload_logged;      /* the state the worker last logged */
static uint32_t     g_offload_type;        /* the first mapping's memblock type */
static int          g_offload_err;         /* why refused / the last map error */
static uint32_t     g_off_reads, g_off_kib, g_off_maperr;      /* atomics      */
/* Read ahead into non-cacheable buffers: their mapping did not fit (memory
 * type or access) - the caller copies those from then on; cached ones are not
 * affected. */
static volatile int g_nc_off_refused;
static uint32_t     g_nc_off_type;

/* The caller's buffer for one request, mapped for the copier (see the top of
 * "copy on the system core"). */
typedef struct OffMap {
	SceUID   pid;
	SceUID   uid;              /* the mapping, or < 0                        */
	uint8_t *user;             /* the request's buffer in the caller's space */
	uint8_t *kern;             /* the same bytes through the mapping         */
	uint32_t len;
	int      tried;
	int      mt;               /* the buffer's memory type at both ends (read by
	                            * the caller: an address of its own space), or -1 */
	int      nc;               /* a non-cacheable buffer (read ahead): a mapping
	                            * that does not fit refuses only those */
} OffMap;
static void off_map(OffMap *m);

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
/* x / d without a division on the hot path: the Cortex-A9 has no divide
 * instruction, so a 64-bit division is a library call of a few hundred cycles.
 * inv = recip(d) is worked out once per plan; then q = x * inv >> 32 is at
 * most 2 below x / d (x < 2^32) and is corrected up to the exact quotient.
 * Anything out of that range divides as before. */
static inline uint32_t recip(uint64_t d)
{
	return d && d <= 0xFFFFFFFFu ? 0xFFFFFFFFu / (uint32_t)d : 0;
}

static inline uint64_t div_by(uint64_t x, uint64_t d, uint32_t inv)
{
	uint32_t q;

	if (!inv || x > 0xFFFFFFFFu)
		return d ? x / d : ~(uint64_t)0;
	q = (uint32_t)(((uint64_t)(uint32_t)x * inv) >> 32);
	while (((uint64_t)q + 1) * d <= x)
		q++;
	return q;
}

static uint32_t pipe_cap(int i)
{
	return (i & 1) ? PIPE_TAIL_SECTORS : STAGE_SECTORS;
}

/* The parts for k and a last part of x sectors, each earlier one as large as
 * it may be; s[0] takes the rest. 1 if that covers n exactly with every part
 * within its bounds. */
static int pipe_fit(uint32_t n, int k, uint32_t x, uint64_t io, uint64_t cp, uint32_t inv,
                    uint64_t cmd, uint32_t s[PIPE_MAX_PARTS])
{
	uint32_t sum = x;

	if (x > pipe_cap(k - 1))
		return 0;
	s[k - 1] = x;
	for (int i = k - 2; i >= 0; i--) {
		uint64_t m = div_by(cmd + (uint64_t)s[i + 1] * io, cp, inv);

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
	const uint32_t inv = recip(cp);
	int best_k = 0;

	for (int k = kmin; k <= kmax; k++) {
		/* k is 2 or 3: constant divisors, which the compiler multiplies */
		uint32_t s[PIPE_MAX_PARTS], lo = PIPE_MIN_TAIL, hi = k == 2 ? n / 2 : k == 3 ? n / 3 : n / k;
		uint32_t x = 0;

		/* The earlier parts only grow with x, so the x that fit form one
		 * range: below it the parts do not reach n, above it they leave
		 * part 0 too little. Find its start. */
		while (lo <= hi) {
			uint32_t mid = lo + (hi - lo) / 2, t[PIPE_MAX_PARTS], sum = mid;

			if (pipe_fit(n, k, mid, io, cp, inv, cmd, s)) {
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
				uint64_t m = div_by(cmd + (uint64_t)t[i + 1] * io, cp, inv);

				t[i] = (uint32_t)(m > pipe_cap(i) ? pipe_cap(i) : m);
				sum += t[i];
			}
			if (sum + PIPE_MIN_TAIL > n)
				hi = mid - 1;               /* part 0 left too small */
			else
				lo = mid + 1;               /* the parts do not reach n */
		}
		if (x && pipe_fit(n, k, x, io, cp, inv, cmd, s)) {
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

	if (LD(g_pipeline) == VM_PIPE_FIXED) {
		k = pipe_plan_costs(n, PIPE_DEF_IO_NS, def_cp, PIPE_DEF_CMD_NS, clean ? 3 : 2,
		                    clean ? 3 : 2, out);
		if (!k && clean)
			k = pipe_plan_costs(n, PIPE_DEF_IO_NS, def_cp, PIPE_DEF_CMD_NS, 2, 2, out);
		return k;
	}
	{
		const uint32_t eio = LD(d->est_io_ns), ecp = LD(d->est_cp_ns[clean ? 1 : 0]);
		const uint32_t ecmd = LD(d->est_cmd_ns);
		const uint64_t io = eio ? eio : PIPE_DEF_IO_NS;
		const uint64_t cp = ecp ? ecp : def_cp;
		const uint64_t cmd = ecmd ? ecmd : PIPE_DEF_CMD_NS;

		/* any split costs at least one more command and a shortest last
		 * part: when that already loses to one piece, no plan is worked
		 * out (it takes 64-bit divisions, on every small read) */
		if ((uint64_t)n * cp <= cmd + (uint64_t)PIPE_MIN_TAIL * cp + PIPE_HANDOFF_NS)
			return 0;
		k = pipe_plan_costs(n, io, cp, cmd, 2, PIPE_MAX_PARTS, out);
		/* in one piece: one command and the whole copy, nothing to hand off */
		if (k >= 2 && (uint64_t)k * cmd + (uint64_t)out[k - 1] * cp + PIPE_HANDOFF_NS >=
		              cmd + (uint64_t)n * cp)
			k = 0;
		return k;
	}
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
	/* a 32-bit division (much cheaper than a 64-bit one) whenever it fits */
	r = head_us - tail_us < 4294967u ? (uint64_t)((head_us - tail_us) * 1000u / (a - t))
	                                 : (uint64_t)(head_us - tail_us) * 1000u / (a - t);
	if ((uint64_t)tail_us * 1000u <= r * t)
		cmd = 0;
	else
		cmd = (uint64_t)tail_us * 1000u - r * t;
	if (r > 1000000u || cmd > 20000000u)
		return;                             /* not a plausible pair */
	{
		uint32_t eio = LD(d->est_io_ns), ecmd = LD(d->est_cmd_ns);

		if (!eio)
			eio = PIPE_DEF_IO_NS;
		if (!ecmd)
			ecmd = PIPE_DEF_CMD_NS;
		ST(d->est_io_ns, eio - (eio >> 3) + (uint32_t)(r >> 3));
		ST(d->est_cmd_ns, ecmd - (ecmd >> 3) + (uint32_t)(cmd >> 3));
	}
}

/* A running average (1/8 weight) of a per-sector cost in ns. */
static void pipe_learn(uint32_t *est, uint32_t us, uint32_t sectors, uint32_t def)
{
	uint32_t ns;

	if (!sectors)
		return;
	ns = us < 4294967u ? us * 1000u / sectors : (uint32_t)((uint64_t)us * 1000u / sectors);
	{
		uint32_t e = __atomic_load_n(est, __ATOMIC_RELAXED);

		if (!e)
			e = def;
		__atomic_store_n(est, e - (e >> 3) + (ns >> 3), __ATOMIC_RELAXED);
	}
}

/* The helper: reads parts first .. k-1 of a job one after another, each as soon
 * as its buffer is free again (sem_free: the caller has copied the part that
 * used it two parts back), and signals sem_done for each. So the card goes
 * from one part to the next without waiting for the caller's thread to wake
 * up. Odd parts go to the second buffer, even ones to the staging buffer.
 * After a failed part it stops; its sem_free still owed is taken first, so
 * nothing is left over for the next job. */
static int pipe_helper(SceSize args, void *argp)
{
	Dev *d;

	(void)args;
	memcpy(&d, argp, sizeof(d));          /* the start argument, copied */
	for (;;) {
		uint32_t sector, k, sz[PIPE_MAX_PARTS], io_us = 0, big_us = 0, big_n = 0;
		int timed;

		ksceKernelWaitSema(d->sem_go, 1, NULL);
		if (d->pipe_quit)
			break;
		__sync_synchronize();               /* the job before its fields */
		k = d->job_k;
		memcpy(sz, d->job_sz, sizeof(sz));
		sector = d->job_sector;
		timed = LD(g_stats) || LD(g_pipeline) == VM_PIPE_AUTO;   /* see stage_xfer */
		for (uint32_t i = d->job_first; i < k; i++) {
			void *buf = (i & 1) ? d->buf2 : d->buf;
			uint32_t t0, us;
			int r;

			if (i >= 2) {
				/* free at once unless the copy is late (a busy caller) */
				uint32_t w0 = ksceKernelGetSystemTimeLow(), w;

				ksceKernelWaitSema(d->sem_free, 1, NULL);
				w = ksceKernelGetSystemTimeLow() - w0;
				if (w >= PIPE_WAIT_US) {
					__atomic_fetch_add(&g_pipe_waits, 1, __ATOMIC_RELAXED);
					__atomic_fetch_add(&g_pipe_wait_us, w, __ATOMIC_RELAXED);
				}
			}
			t0 = timed ? ksceKernelGetSystemTimeLow() : 0;
			r = d->job_io(d->job_ref, d->job_part, sector, buf, sz[i]);
			us = timed ? ksceKernelGetSystemTimeLow() - t0 : 0;
			if (r < 0) {
				d->job_ret = r;
				__atomic_store_n(&d->job_fail, i + 1, __ATOMIC_RELEASE);
				if (i >= 1 && i + 1 < k)    /* the copier frees this one first */
					ksceKernelWaitSema(d->sem_free, 1, NULL);
				ksceKernelSignalSema(d->sem_done, 1);
				break;
			}
			io_us += us;
			if (sz[i] > big_n) {
				big_n = sz[i];
				big_us = us;
			}
			sector += sz[i];
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

/* Wait until this core's writes have reached memory: after a copy into a
 * non-cacheable buffer, which the graphics chip may read at once (a DMB only
 * orders them). */
static inline void vm_dsb(void)
{
#if defined(__arm__)
	__asm__ volatile("dsb" ::: "memory");
#else
	__sync_synchronize();
#endif
}

/* Copy the parts of the current job into dst as each one arrives (sem_done),
 * handing each buffer back (sem_free) for the part two further on. Run by the
 * copier, or by the caller when its buffer could not be mapped. *done: the
 * sectors copied; the failed part's error, or 0. */
static int copy_parts(Dev *d, uint8_t *dst, int clean, uint32_t *done, uint32_t *cp_us)
{
	const uint32_t k = d->job_k;
	const int timed = LD(g_stats) || LD(g_pipeline) == VM_PIPE_AUTO;
	uint32_t sz[PIPE_MAX_PARTS], off = 0;

	memcpy(sz, d->job_sz, sizeof(sz));
	*done = *cp_us = 0;
	for (uint32_t i = 0; i < k; i++) {
		const void *cur = d->job_ra ? d->job_src[i] : (i & 1) ? d->buf2 : d->buf;
		uint32_t f, c0;

		ksceKernelWaitSema(d->sem_done, 1, NULL);
		__sync_synchronize();
		f = __atomic_load_n(&d->job_fail, __ATOMIC_ACQUIRE);
		if (f && f - 1 <= i)
			return d->job_ret;              /* this part on is read again stock */
		c0 = timed ? ksceKernelGetSystemTimeLow() : 0;
		vm_copy_from_stage(dst + off * 512, cur, sz[i] * 512);
		if (clean)
			clean_out(dst + off * 512, sz[i] * 512);
		if (timed)
			*cp_us += ksceKernelGetSystemTimeLow() - c0;
		off += sz[i];
		*done = off;
		if (!d->job_ra && i + 2 < k)        /* part i+2 reads into this buffer */
			ksceKernelSignalSema(d->sem_free, 1);
	}
	if (d->job_ra && !clean)
		vm_dsb();                           /* non-cacheable: in memory now */
	return 0;
}

/* The copier: one job at a time (sem_cgo), copy_parts into the caller's
 * mapped buffer, then the result (sem_fin). */
static int pipe_copier(SceSize args, void *argp)
{
	Dev *d;

	(void)args;
	memcpy(&d, argp, sizeof(d));          /* the start argument, copied */
	for (;;) {
		uint32_t done = 0, cp = 0;
		int r;

		ksceKernelWaitSema(d->sem_cgo, 1, NULL);
		if (d->pipe_quit)
			break;
		__sync_synchronize();               /* the job before its fields */
		{
			struct OffMap *m = d->cjob_map;

			/* the mapping on the request's first chunk, here on the system
			 * core rather than on the caller's; none usable: the caller
			 * copies (nothing of the job has been taken yet) */
			if (!m->tried)
				off_map(m);
			if (m->uid < 0) {
				d->cjob_nomap = 1;
				__sync_synchronize();
				ksceKernelSignalSema(d->sem_fin, 1);
				continue;
			}
			d->cjob_nomap = 0;
			r = copy_parts(d, m->kern + d->cjob_off, d->cjob_clean, &done, &cp);
			if (d->cjob_last) {
				ksceKernelUserUnmap(m->uid);      /* the request's last chunk */
				m->uid = -1;
			}
		}
		if (done && LD(g_log_diag))
			__atomic_fetch_add(&g_cp_core_kib[cpu_id()], done / 2, __ATOMIC_RELAXED);
		d->cjob_ret = r;
		d->cjob_done = done;
		d->cjob_cp_us = cp;
		__sync_synchronize();               /* the result before the signal */
		ksceKernelSignalSema(d->sem_fin, 1);
	}
	return 0;
}

static void ra_release(Dev *d);
static int ra_retire(Dev *d);

static void pipe_release(Dev *d)
{
	if (d->ra_made)
		ra_release(d);                    /* its pieces are in these buffers */
	if (d->th >= 0) {
		d->pipe_quit = 1;
		__sync_synchronize();
		ksceKernelSignalSema(d->sem_go, 1);
		ksceKernelWaitThreadEnd(d->th, NULL, NULL);
		ksceKernelDeleteThread(d->th);
	}
	if (d->th2 >= 0) {
		d->pipe_quit = 1;
		__sync_synchronize();
		ksceKernelSignalSema(d->sem_cgo, 1);
		ksceKernelWaitThreadEnd(d->th2, NULL, NULL);
		ksceKernelDeleteThread(d->th2);
	}
	if (d->sem_cgo >= 0)
		ksceKernelDeleteSema(d->sem_cgo);
	if (d->sem_fin >= 0)
		ksceKernelDeleteSema(d->sem_fin);
	if (d->sem_go >= 0)
		ksceKernelDeleteSema(d->sem_go);
	if (d->sem_done >= 0)
		ksceKernelDeleteSema(d->sem_done);
	if (d->sem_free >= 0)
		ksceKernelDeleteSema(d->sem_free);
	if (d->uid2 >= 0)
		ksceKernelFreeMemBlock(d->uid2);
	d->th = d->sem_go = d->sem_done = d->sem_free = d->uid2 = -1;
	d->th2 = d->sem_cgo = d->sem_fin = -1;
	d->buf2 = NULL;
	ST(d->pipe_made, 0);
}

/* Worker thread: stop the hooks from starting overlapped reads on `d`, wait
 * for the request in flight (if any) to finish, then free the helper and its
 * buffer. A request decides on overlapping only after taking `busy`, so once
 * `busy` drops after pipe_ready went to 0, nothing can use them.
 * The wait is bounded (PIPE_RETIRE_MS): a request stuck in the driver (a card
 * pulled out mid-read) must not stop the worker - the threads and buffer are
 * then kept untouched, and the next tick tries again (vm_stage_pending). 1 if
 * released. */
#define PIPE_RETIRE_MS  50
static int pipe_retire(Dev *d)
{
	if (d->ra_made && !ra_retire(d))
		return 0;
	__atomic_store_n(&d->pipe_ready, 0, __ATOMIC_SEQ_CST);
	__sync_synchronize();                 /* pairs with the fence in take() */
	/* an acquire load: the request's last use of the buffers comes first */
	for (int i = 0; __atomic_load_n(&d->busy, __ATOMIC_ACQUIRE); i++) {
		if (i >= PIPE_RETIRE_MS)
			return 0;
		ksceKernelDelayThread(1000);
	}
	pipe_release(d);
	return 1;
}

/* Worker thread: everything the overlapped reads of device `d` need. On any
 * failure nothing is kept and the device reads as before. Returns 0 or the
 * error. */
static int pipe_setup(Dev *d)
{
	Dev *arg = d;
	int r;

	d->th = d->sem_go = d->sem_done = d->sem_free = d->uid2 = -1;
	d->th2 = d->sem_cgo = d->sem_fin = -1;
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
	d->sem_cgo = ksceKernelCreateSema("SceVitaMemioCopyGo", 0, 0, 1, NULL);
	d->sem_fin = ksceKernelCreateSema("SceVitaMemioCopyFin", 0, 0, 1, NULL);
	if (d->sem_go < 0 || d->sem_done < 0 || d->sem_free < 0 || d->sem_cgo < 0 ||
	    d->sem_fin < 0) {
		r = d->sem_go < 0 ? d->sem_go : d->sem_done < 0 ? d->sem_done
		  : d->sem_free < 0 ? d->sem_free : d->sem_cgo < 0 ? d->sem_cgo : d->sem_fin;
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
	/* The copier: also on the system core, one step below the helper, so
	 * the helper starts the next read at once even while a copy runs. */
	d->th2 = ksceKernelCreateThread("SceVitaMemioCopy", pipe_copier, 0x48, 0x2000, 0,
	                                PIPE_CPU_SYSTEM, NULL);
	if (d->th2 < 0)
		d->th2 = ksceKernelCreateThread("SceVitaMemioCopy", pipe_copier, 0x48, 0x2000, 0, 0,
		                                NULL);
	if (d->th2 < 0) {
		r = d->th2;
		d->th2 = -1;
		pipe_release(d);
		return r;
	}
	r = ksceKernelStartThread(d->th2, sizeof(arg), &arg);
	if (r < 0) {
		ksceKernelDeleteThread(d->th2);
		d->th2 = -1;
		pipe_release(d);
		return r;
	}
	ST(d->pipe_made, 1);
	/* the hooks may use it from now on; everything above is seen first */
	__atomic_store_n(&d->pipe_ready, 1, __ATOMIC_RELEASE);
	return 0;
}

/* The verify buffer for one request (NULL: no verify for it). Published by
 * the worker with release, taken here with acquire. */
static void *take_shadow(void)
{
	void *b;

	if (!LD(g_verify) || !(b = __atomic_load_n(&g_shadow, __ATOMIC_ACQUIRE)))
		return NULL;
	return __sync_lock_test_and_set(&g_shadow_busy, 1) ? NULL : b;
}

/* An overlapped read of one chunk in k parts (sz, from pipe_plan) into the
 * caller's p. Part 0 is read here into the staging buffer; the helper then
 * reads parts 1 .. k-1 back to back (odd ones into the second buffer) while
 * this thread copies (and, GPU-safe, cleans) each part in turn and hands its
 * buffer back (sem_free) for the part two further on. *done: the sectors
 * already in the caller's buffer when it returns, all of them on success;
 * after an error the rest is the caller's to finish stock. */
static int pipe_run(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                    uint8_t *p, const uint32_t *sz, uint32_t k, int clean, int stats,
                    uint32_t *done)
{
	/* Auto times itself, whatever Statistics says: the plan learns its
	 * costs from these few clock reads. Fixed reads the clock only for
	 * Statistics. */
	const int timed = stats || LD(g_pipeline) == VM_PIPE_AUTO;
	uint32_t off = 0, cp_us = 0, h0, h1;
	int ret;

	*done = 0;
	h0 = timed ? ksceKernelGetSystemTimeLow() : 0;
	ret = io(ref, part, sector, d->buf, sz[0]);
	if (ret < 0)
		return ret;
	h1 = timed ? ksceKernelGetSystemTimeLow() : 0;
	d->job_io = io;
	d->job_ref = ref;
	d->job_part = part;
	d->job_k = k;
	for (uint32_t i = 0; i < PIPE_MAX_PARTS; i++)
		d->job_sz[i] = i < k ? sz[i] : 0;
	d->job_first = 1;
	d->job_sector = sector + sz[0];
	d->job_ret = 0;
	__atomic_store_n(&d->job_fail, 0, __ATOMIC_RELAXED);
	__sync_synchronize();
	ksceKernelSignalSema(d->sem_go, 1);
	for (uint32_t i = 1; i <= k; i++) {
		const uint32_t n = sz[i - 1];
		const void *cur = ((i - 1) & 1) ? d->buf2 : d->buf;
		uint32_t c0 = timed ? ksceKernelGetSystemTimeLow() : 0;

		vm_copy_from_stage(p + off * 512, cur, n * 512);
		if (clean)
			clean_out(p + off * 512, n * 512);
		if (timed)
			cp_us += ksceKernelGetSystemTimeLow() - c0;
		off += n;
		*done = off;
		if (i + 1 < k)                      /* part i+1 reads into this buffer */
			ksceKernelSignalSema(d->sem_free, 1);
		if (i < k) {
			uint32_t f;

			ksceKernelWaitSema(d->sem_done, 1, NULL);
			__sync_synchronize();
			f = __atomic_load_n(&d->job_fail, __ATOMIC_ACQUIRE);
			if (f && f - 1 <= i)
				return d->job_ret;          /* this part on is read again stock */
		}
	}
	if (LD(g_pipeline) == VM_PIPE_AUTO) {
		/* the largest part against the last one: per sector and per command */
		uint32_t big_us = d->job_big_us, big_n = d->job_big_n;

		if (sz[0] > big_n) {
			big_n = sz[0];
			big_us = h1 - h0;
		}
		pipe_learn_io(d, big_us, big_n, d->job_last_us, sz[k - 1]);
		pipe_learn(&d->est_cp_ns[clean ? 1 : 0], cp_us, off,
		           clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS);
	}
	if (stats) {                            /* the tester's figures */
		ADD(d->tm.rd_sectors, off);
		ADD(d->tm.rd_io_us, (h1 - h0) + d->job_io_us);
		ADD(d->tm.rd_copy_us, cp_us);
	}
	return 0;
}


/* Refuse copy on the system core until the setting is switched again (a
 * refusal always wins over a concurrent "usable"). */
static void off_refuse(int state, int err, uint32_t type)
{
	ST(g_offload_type, type);
	ST(g_offload_err, err);
	__atomic_store_n(&g_offload_state, state, __ATOMIC_RELEASE);
}

/* Map m->user for the copier. Leaves m->uid < 0 (the caller copies) unless
 * the mapping is usable. Every mapping is checked, so no request relies on
 * another one's check:
 *   - writable: its memblock type has the kernel's read and write (as
 *     SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, 0x1020D006), and the type word was
 *     read right (its memory type is the one at the address);
 *   - the same memory type as the caller's buffer at both ends of both
 *     (cached: one coherent copy of the data for every core). */
static void off_map(OffMap *m)
{
	void *page = NULL;
	SceSize size = 0;
	SceUInt32 off = 0;
	SceUID uid;
	VmMemBlockInfoEx info;
	uint8_t *kern;
	int r, mt;

	m->tried = 1;
	uid = ksceKernelProcUserMap(m->pid, "SceVitaMemioCopy", VM_USERMAP_RW, m->user, m->len, &page,
	                            &size, &off);
	if (uid < 0) {
		ADD(g_off_maperr, 1);
		ST(g_offload_err, uid);
		return;
	}
	if (!page || size < off || size - off < m->len) {
		ksceKernelUserUnmap(uid);
		ADD(g_off_maperr, 1);
		ST(g_offload_err, VM_ERR_NOMEM);
		return;
	}
	kern = (uint8_t *)page + off;
	mt = m->mt;
	if (mt < 0 || ksceKernelGetMemBlockMemtypeByAddr(kern) != mt ||
	    ksceKernelGetMemBlockMemtypeByAddr(kern + m->len - 1) != mt) {
		ksceKernelUserUnmap(uid);
		if (m->nc)
			ST(g_nc_off_refused, 1);
		else
			off_refuse(-2, -2, 0);
		return;
	}
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	r = ksceKernelMemBlockGetInfoEx(uid, &info);
	if (r < 0 || (int)(info.type & 0xFF00) != mt || (info.type & 0x6) != 0x6) {
		ksceKernelUserUnmap(uid);
		if (m->nc) {
			ST(g_nc_off_type, info.type);
			ST(g_nc_off_refused, 1);
		} else {
			off_refuse(-1, r < 0 ? r : -1, info.type);
		}
		return;
	}
	if (!m->nc) {
		int zero = 0;                       /* first usable one: for the log */

		if (__atomic_compare_exchange_n(&g_offload_state, &zero, 1, 0, __ATOMIC_ACQ_REL,
		                                __ATOMIC_RELAXED))
			ST(g_offload_type, info.type);
	}
	if (__atomic_load_n(&g_offload_state, __ATOMIC_ACQUIRE) < 0) {
		ksceKernelUserUnmap(uid);           /* refused meanwhile (another request) */
		return;
	}
	m->uid = uid;
	m->kern = kern;
}

/* One chunk (plan sz/k) of a game's read, copied on the system core: the
 * helper reads every part, the copier copies each into the mapped buffer,
 * and this thread only maps the buffer (while part 0 is being read) and
 * waits. Without a usable mapping this thread copies, as pipe_run does.
 * *done: as pipe_run. */
static int offload_run(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                       uint8_t *p, const uint32_t *sz, uint32_t k, int clean, int stats,
                       OffMap *m, int last, uint32_t *done, uint32_t *own)
{
	uint32_t cp_us = 0, n = 0;
	int ret;

	d->job_io = io;
	d->job_ref = ref;
	d->job_part = part;
	d->job_k = k;
	for (uint32_t i = 0; i < PIPE_MAX_PARTS; i++) {
		d->job_sz[i] = i < k ? sz[i] : 0;
		n += i < k ? sz[i] : 0;
	}
	d->job_first = 0;
	d->job_sector = sector;
	d->job_ret = 0;
	__atomic_store_n(&d->job_fail, 0, __ATOMIC_RELAXED);
	__sync_synchronize();
	ksceKernelSignalSema(d->sem_go, 1);       /* the card starts at once */
	int nomap = 1;

	if (!m->tried || m->uid >= 0) {
		/* the copier maps the buffer (first chunk), copies, and on the
		 * last chunk unmaps it: this thread only waits */
		d->cjob_map = m;
		d->cjob_off = (uint32_t)(p - m->user);
		d->cjob_clean = clean;
		d->cjob_last = last;
		__sync_synchronize();
		ksceKernelSignalSema(d->sem_cgo, 1);
		ksceKernelWaitSema(d->sem_fin, 1, NULL);
		__sync_synchronize();
		nomap = d->cjob_nomap;
	}
	if (nomap) {
		ret = copy_parts(d, p, clean, done, &cp_us);   /* no usable mapping */
		*own = *done;
	} else {
		ret = d->cjob_ret;
		*done = d->cjob_done;
		cp_us = d->cjob_cp_us;
		*own = 0;
		if (ret >= 0) {
			__atomic_fetch_add(&g_off_reads, 1, __ATOMIC_RELAXED);
			__atomic_fetch_add(&g_off_kib, n / 2, __ATOMIC_RELAXED);
		}
	}
	d->cjob_map = NULL;
	if (ret < 0)
		return ret;
	if (LD(g_pipeline) == VM_PIPE_AUTO) {
		pipe_learn_io(d, d->job_big_us, d->job_big_n, d->job_last_us, sz[k - 1]);
		pipe_learn(&d->est_cp_ns[clean ? 1 : 0], cp_us, n,
		           clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS);
	}
	if (stats) {
		ADD(d->tm.rd_sectors, n);
		ADD(d->tm.rd_io_us, d->job_io_us);
		ADD(d->tm.rd_copy_us, cp_us);
	}
	return 0;
}

/* ---- read ahead (VM_KEY_READAHEAD) -------------------------------------------
 *
 * EXPERIMENT, the SD2Vita and the memory card only (never the internal
 * storage, which holds the system's own data). While a program reads one
 * stretch of a storage in order (five
 * staged reads in a row, each starting where the last one ended - so the four
 * pieces of one 1 MiB read never start it), the device's read-ahead thread on
 * the system core reads the next 384 KiB in three pieces of 128 KiB, one
 * card read each, while the program copies what it has. The next read of the
 * stream then takes its data from those pieces (waiting only for a piece still
 * being read) and the card goes on to the next one: it never waits for the
 * program, and a stream needs one card command per 128 KiB.
 *
 * What makes this safe:
 *   - a piece is handed out only to the read that continues the stream exactly
 *     where it is, with the same card context; anything else (another sector,
 *     a write through the staging buffer, a non-staged read is left alone)
 *     first drops every piece, waiting for the one being read (up to one
 *     piece, ~6 ms: the card cannot stop a read);
 *   - every write on the device, on every path (staged or not), is counted
 *     in ra_wbusy while it runs and, as it starts, marks the pieces whose
 *     sectors it touches dirty. A piece read while any write was running, or
 *     touched by one since it was queued, is not handed out: a write that
 *     ends before a piece's read starts is on the card, any other one is seen
 *     (ra_wbusy is read after the piece is marked reading, the pieces after
 *     ra_wbusy is counted up, both in one order), so nothing written since is
 *     ever read from a piece; writes elsewhere leave the pieces alone;
 *   - switching the device (off, on, after an error) counts ra_epoch up, and
 *     a piece older than RA_MAX_AGE_US is not used: a swapped card never
 *     answers a read with the old card's data;
 *   - verify mode covers it: every read from the pieces is read again the
 *     standard way and compared (ra_verify); a difference falls back;
 *   - a piece the card failed to read is not used, and does not switch the
 *     device to stock (it may simply lie past the card's end): the read is
 *     done as without read ahead;
 *   - the pieces live in the staging buffers: every request holding `busy`
 *     drops them before it uses those buffers itself, and the worker releases
 *     the thread only once no piece is being read and no request is in flight.
 * Nothing is written ahead or kept back: writes are as before. */
#define RA_STREAK       5               /* staged reads in a row before it starts */
/* Into non-cacheable memory (games' graphics data): the same five reads (a
 * random 1 MiB read, four requests, never starts it), then one piece ahead at
 * first, two after the next read, then three, so little is read past the end
 * of a file. */
#define RA_STREAK_NC    5
/* Only reads of 64 KiB and up make or continue a stream; smaller transfers
 * (file-system bookkeeping, small files, with "small transfers too") neither
 * start nor break one, and while pieces are kept they go the standard way, so
 * they do not drop them (see stage_xfer_one). */
#define RA_MIN_SECTORS  128
/* ... but only while the stream is live (read within this long): pieces left
 * behind by a stream that has ended are dropped by the next staged transfer,
 * which then goes the faster way as usual. */
#define RA_LIVE_US      30000u
#define RA_MAX_AGE_US   500000u
#define RA_RETIRE_MS    50

static volatile int g_readahead;          /* the setting                      */
static uint32_t g_ra_hits, g_ra_kib, g_ra_drops, g_ra_waste_kib, g_ra_bad;   /* atomics */

static uint8_t *ra_ptr(const Dev *d, int i)
{
	return i < 2 ? (uint8_t *)d->buf + (size_t)i * RA_PIECE_SECTORS * 512 : (uint8_t *)d->buf2;
}

static int ra_pending(int st)
{
	return st == RA_QUEUED || st == RA_READING;
}

/* The read-ahead thread: reads the queued pieces, oldest first, one at a time.
 * A request waiting for one (ra_waiting) is woken after each. */
static int ra_thread(SceSize args, void *argp)
{
	Dev *d;

	(void)args;
	memcpy(&d, argp, sizeof(d));          /* the start argument, copied */
	for (;;) {
		int best = -1, exp = RA_QUEUED, r;
		int wr;
		RaPiece *pc;

		ksceKernelWaitSema(d->ra_go, 1, NULL);
		if (__atomic_load_n(&d->ra_quit, __ATOMIC_ACQUIRE))
			break;
		for (int i = 0; i < RA_PIECES; i++)
			if (__atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) == RA_QUEUED &&
			    (best < 0 || (int32_t)(d->ra[i].seq - d->ra[best].seq) < 0))
				best = i;
		if (best < 0)
			continue;                       /* dropped meanwhile */
		pc = &d->ra[best];
		/* seq_cst, as the read of ra_wbusy below and a write's own steps */
		if (!__atomic_compare_exchange_n(&pc->state, &exp, RA_READING, 0, __ATOMIC_SEQ_CST,
		                                 __ATOMIC_RELAXED))
			continue;
		wr = __atomic_load_n(&d->ra_wbusy, __ATOMIC_SEQ_CST);   /* after READING */
		r = pc->io(pc->ref, pc->part, LD(pc->sector), ra_ptr(d, best), LD(pc->n));
		pc->ok = r >= 0 && !wr;
		pc->t_done = ksceKernelGetSystemTimeLow();
		__atomic_store_n(&pc->state, r >= 0 ? RA_READY : RA_FAILED, __ATOMIC_SEQ_CST);
		if (__atomic_exchange_n(&d->ra_waiting, 0, __ATOMIC_SEQ_CST))
			ksceKernelSignalSema(d->ra_done, 1);
	}
	return 0;
}

/* Wait until piece i is no longer queued or being read; its state. */
static int ra_wait(Dev *d, int i)
{
	for (;;) {
		int st = __atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE);

		if (!ra_pending(st))
			return st;
		__atomic_store_n(&d->ra_waiting, 1, __ATOMIC_SEQ_CST);
		st = __atomic_load_n(&d->ra[i].state, __ATOMIC_SEQ_CST);
		if (!ra_pending(st)) {
			/* done meanwhile: if the thread took the flag, its signal is ours */
			if (!__atomic_exchange_n(&d->ra_waiting, 0, __ATOMIC_SEQ_CST))
				ksceKernelWaitSema(d->ra_done, 1, NULL);
			return st;
		}
		ksceKernelWaitSema(d->ra_done, 1, NULL);   /* the thread cleared the flag */
	}
}

static int ra_any(const Dev *d)
{
	for (int i = 0; i < RA_PIECES; i++)
		if (__atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) != RA_FREE)
			return 1;
	return 0;
}

/* Drop every piece: the queued ones at once, the one being read once it is
 * done. The staging buffers are then this request's again. */
static void ra_drop(Dev *d)
{
	uint32_t waste = 0;
	int any = 0;

	for (int i = 0; i < RA_PIECES; i++) {
		int exp = RA_QUEUED;

		if (__atomic_compare_exchange_n(&d->ra[i].state, &exp, RA_FREE, 0, __ATOMIC_ACQ_REL,
		                                __ATOMIC_RELAXED))
			any = 1;                        /* not read yet: nothing wasted */
	}
	for (int i = 0; i < RA_PIECES; i++) {
		const int st = ra_wait(d, i);

		if (st != RA_FREE) {
			any = 1;
			if (st == RA_READY)
				waste += d->ra[i].n - d->ra[i].off;
			__atomic_store_n(&d->ra[i].state, RA_FREE, __ATOMIC_RELEASE);
		}
	}
	if (any) {
		ADD(g_ra_drops, 1);
		ADD(g_ra_waste_kib, waste / 2);
	}
}

/* Whether piece i may be handed out (see the top). */
static int ra_valid(const Dev *d, int i)
{
	const RaPiece *pc = &d->ra[i];

	return __atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) == RA_READY && pc->ok &&
	       !__atomic_load_n(&pc->dirty, __ATOMIC_SEQ_CST) && pc->epoch == LD(d->ra_epoch) &&
	       ksceKernelGetSystemTimeLow() - pc->t_done < RA_MAX_AGE_US;
}

/* The pieces that continue the stream at `sector`, oldest first (idx), and
 * how many sectors of the n asked for they hold. 0: they do not continue it. */
static uint32_t ra_cover(const Dev *d, void *part, io_fn io, uint32_t sector, uint32_t n,
                         int idx[RA_PIECES], int *cnt)
{
	uint32_t at = sector, cov = 0;

	*cnt = 0;
	while (cov < n && *cnt < RA_PIECES) {
		int found = -1;

		for (int i = 0; i < RA_PIECES; i++) {
			const RaPiece *pc = &d->ra[i];

			if (__atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) != RA_FREE &&
			    pc->part == part && pc->io == io && pc->sector + pc->off == at &&
			    pc->off < pc->n)
				found = i;
		}
		if (found < 0)
			break;
		idx[(*cnt)++] = found;
		{
			const uint32_t have = d->ra[found].n - d->ra[found].off;
			const uint32_t take_n = have < n - cov ? have : n - cov;

			cov += take_n;
			at += take_n;
		}
	}
	return cov;
}

/* The buffer's memory type for the copier, read here (see stage_xfer). */
static void off_memtype(OffMap *om)
{
	if (!om->tried && om->mt == -2) {
		const int mt = ksceKernelGetMemBlockMemtypeByAddr(om->user);

		om->mt = mt >= 0 && ksceKernelGetMemBlockMemtypeByAddr(om->user + om->len - 1) == mt
		       ? mt : -1;
	}
}

/* Copy k ready parts (src, sz) into p on the copier, as offload_run does with
 * parts it reads; without a usable mapping this thread copies. *own: the
 * sectors this thread copied. */
static void ra_copy_off(Dev *d, uint8_t *p, const void *const *src, const uint32_t *sz, uint32_t k,
                        int clean, OffMap *m, int last, uint32_t *own, uint32_t *cp_us)
{
	uint32_t done = 0, n = 0;
	int nomap = 1;

	d->job_k = k;
	for (uint32_t i = 0; i < PIPE_MAX_PARTS; i++) {
		d->job_sz[i] = i < k ? sz[i] : 0;
		d->job_src[i] = i < k ? src[i] : NULL;
		n += i < k ? sz[i] : 0;
	}
	d->job_ra = 1;
	d->job_ret = 0;
	__atomic_store_n(&d->job_fail, 0, __ATOMIC_RELAXED);
	__sync_synchronize();
	ksceKernelSignalSema(d->sem_done, (int)k);   /* every part is there already */
	off_memtype(m);
	if (!m->tried || m->uid >= 0) {
		d->cjob_map = m;
		d->cjob_off = (uint32_t)(p - m->user);
		d->cjob_clean = clean;
		d->cjob_last = last;
		__sync_synchronize();
		ksceKernelSignalSema(d->sem_cgo, 1);
		ksceKernelWaitSema(d->sem_fin, 1, NULL);
		__sync_synchronize();
		nomap = d->cjob_nomap;
	}
	if (nomap) {
		copy_parts(d, p, clean, &done, cp_us);
		*own += done;
	} else {
		*cp_us = d->cjob_cp_us;
		__atomic_fetch_add(&g_off_reads, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&g_off_kib, n / 2, __ATOMIC_RELAXED);
	}
	d->cjob_map = NULL;
	d->job_ra = 0;
}

/* The start of a read from the pieces that continue the stream: the sectors
 * done (0: none, the pieces are to be dropped). `off`: the copy goes to the
 * copier (m, last as in offload_run). */
static uint32_t ra_serve(Dev *d, void *part, io_fn io, uint32_t sector, uint8_t *p, uint32_t n,
                         int clean, int stats, int off, OffMap *m, uint32_t left_all,
                         uint32_t *own)
{
	int idx[RA_PIECES], cnt;
	const void *src[RA_PIECES];
	uint32_t sz[RA_PIECES], cov = ra_cover(d, part, io, sector, n, idx, &cnt), w0, wait_us,
	         cp_us = 0, at = 0;

	if (!cov)
		return 0;
	w0 = stats ? ksceKernelGetSystemTimeLow() : 0;
	for (int j = 0; j < cnt; j++)
		if (ra_wait(d, idx[j]) != RA_READY || !ra_valid(d, idx[j])) {
			ADD(g_ra_bad, 1);
			return 0;
		}
	wait_us = stats ? ksceKernelGetSystemTimeLow() - w0 : 0;
	for (int j = 0; j < cnt; j++) {
		const RaPiece *pc = &d->ra[idx[j]];
		const uint32_t have = pc->n - pc->off;

		sz[j] = have < cov - at ? have : cov - at;
		src[j] = ra_ptr(d, idx[j]) + (size_t)pc->off * 512;
		at += sz[j];
	}
	if (off) {
		ra_copy_off(d, p, src, sz, (uint32_t)cnt, clean, m, cov == left_all, own, &cp_us);
	} else {
		const uint32_t c0 = stats ? ksceKernelGetSystemTimeLow() : 0;

		at = 0;
		for (int j = 0; j < cnt; j++) {
			vm_copy_from_stage(p + (size_t)at * 512, src[j], sz[j] * 512);
			if (clean)
				clean_out(p + (size_t)at * 512, sz[j] * 512);
			at += sz[j];
		}
		if (!clean)
			vm_dsb();
		*own += cov;
		if (stats)
			cp_us = ksceKernelGetSystemTimeLow() - c0;
	}
	for (int j = 0; j < cnt; j++) {
		RaPiece *pc = &d->ra[idx[j]];

		pc->off += sz[j];
		if (pc->off >= pc->n)
			__atomic_store_n(&pc->state, RA_FREE, __ATOMIC_RELEASE);
	}
	ADD(g_ra_hits, 1);
	ADD(g_ra_kib, cov / 2);
	if (stats) {
		ADD(d->tm.rd_sectors, cov);
		ADD(d->tm.rd_io_us, wait_us);
		ADD(d->tm.rd_copy_us, cp_us);
		ADD(d->rd_bytes, (uint64_t)cov * 512);
	}
	return cov;
}

/* A read [sector, sector + n) has completed: the stream so far. 1 if it
 * counts (then the pieces may be refilled after it). */
static int ra_note(Dev *d, void *part, uint32_t sector, uint32_t n, int nc)
{
	if (n < RA_MIN_SECTORS)
		return 0;                           /* neither part of a stream nor a break */
	if (part == d->ra_last_part && sector == d->ra_last_end && d->ra_streak && nc == d->ra_nc)
		d->ra_streak += d->ra_streak < 1000;
	else
		d->ra_streak = 1;
	d->ra_nc = nc;
	d->ra_last_part = part;
	d->ra_last_end = sector + n;
	if (__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE))     /* no clock read otherwise */
		__atomic_store_n(&d->ra_last_us, ksceKernelGetSystemTimeLow(), __ATOMIC_RELAXED);
	return 1;
}

/* Queue the free pieces after the stream (after the last piece queued, or
 * where the stream is). */
static void ra_refill(Dev *d, io_fn io, tai_hook_ref_t ref, void *part)
{
	const uint32_t thr = d->ra_nc ? RA_STREAK_NC : RA_STREAK;
	uint32_t next = d->ra_last_end;
	int want, live = 0;

	if (d->ra_streak < thr)
		return;
	want = d->ra_nc && d->ra_streak - thr + 1 < RA_PIECES ? (int)(d->ra_streak - thr + 1)
	                                                      : RA_PIECES;
	for (int i = 0; i < RA_PIECES; i++) {
		const RaPiece *pc = &d->ra[i];

		if (__atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) != RA_FREE) {
			live++;
			if (pc->sector + pc->n > next)
				next = pc->sector + pc->n;
		}
	}
	for (int i = 0; i < RA_PIECES; i++) {
		RaPiece *pc = &d->ra[i];

		if (__atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) != RA_FREE)
			continue;
		if (live >= want || next > 0xFFFFFFFFu - RA_PIECE_SECTORS)
			return;
		live++;
		pc->seq = ++d->ra_seq;
		ST(pc->sector, next);
		ST(pc->n, (uint32_t)RA_PIECE_SECTORS);
		pc->off = 0;
		__atomic_store_n(&pc->dirty, 0, __ATOMIC_RELAXED);
		pc->io = io;
		pc->ref = ref;
		pc->part = part;
		pc->epoch = LD(d->ra_epoch);
		pc->ok = 0;
		next += RA_PIECE_SECTORS;
		__atomic_store_n(&pc->state, RA_QUEUED, __ATOMIC_RELEASE);
		ksceKernelSignalSema(d->ra_go, 1);
	}
}

static void ra_release(Dev *d)
{
	if (d->ra_th >= 0) {
		__atomic_store_n(&d->ra_quit, 1, __ATOMIC_RELEASE);
		ksceKernelSignalSema(d->ra_go, 1);
		ksceKernelWaitThreadEnd(d->ra_th, NULL, NULL);
		ksceKernelDeleteThread(d->ra_th);
	}
	if (d->ra_go >= 0)
		ksceKernelDeleteSema(d->ra_go);
	if (d->ra_done >= 0)
		ksceKernelDeleteSema(d->ra_done);
	d->ra_th = d->ra_go = d->ra_done = -1;
	for (int i = 0; i < RA_PIECES; i++)
		__atomic_store_n(&d->ra[i].state, RA_FREE, __ATOMIC_RELEASE);
	__atomic_store_n(&d->ra_waiting, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&d->ra_ready, 0, __ATOMIC_RELEASE);
	ST(d->ra_made, 0);
}

/* Worker: the read-ahead thread of a device whose helper exists. 0 or the
 * error (nothing kept). */
static int ra_setup(Dev *d)
{
	Dev *arg = d;
	int r;

	/* the stream (ra_streak ...) is the requests' own, kept as it is */
	d->ra_th = -1;
	__atomic_store_n(&d->ra_quit, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&d->ra_waiting, 0, __ATOMIC_RELAXED);
	for (int i = 0; i < RA_PIECES; i++)
		__atomic_store_n(&d->ra[i].state, RA_FREE, __ATOMIC_RELEASE);
	d->ra_go = ksceKernelCreateSema("SceVitaMemioAheadGo", 0, 0, 16, NULL);
	d->ra_done = ksceKernelCreateSema("SceVitaMemioAheadDone", 0, 0, 1, NULL);
	if (d->ra_go < 0 || d->ra_done < 0) {
		r = d->ra_go < 0 ? d->ra_go : d->ra_done;
		ra_release(d);
		return r;
	}
	/* on the system core, as the helper: the card goes on at once */
	d->ra_th = ksceKernelCreateThread("SceVitaMemioAhead", ra_thread, 0x40, 0x2000, 0,
	                                  PIPE_CPU_SYSTEM, NULL);
	if (d->ra_th < 0)
		d->ra_th = ksceKernelCreateThread("SceVitaMemioAhead", ra_thread, 0x40, 0x2000, 0, 0,
		                                  NULL);
	if (d->ra_th < 0) {
		r = d->ra_th;
		d->ra_th = -1;
		ra_release(d);
		return r;
	}
	r = ksceKernelStartThread(d->ra_th, sizeof(arg), &arg);
	if (r < 0) {
		ksceKernelDeleteThread(d->ra_th);
		d->ra_th = -1;
		ra_release(d);
		return r;
	}
	ST(d->ra_made, 1);
	__atomic_store_n(&d->ra_ready, 1, __ATOMIC_RELEASE);
	return 0;
}

/* Worker: stop read ahead on `d` - no new pieces, the queued ones dropped -
 * and release the thread once no request is in flight and no piece is being
 * read. Bounded as pipe_retire (a card stuck mid-read): 1 if released. */
static int ra_retire(Dev *d)
{
	__atomic_store_n(&d->ra_ready, 0, __ATOMIC_SEQ_CST);
	__sync_synchronize();
	for (int i = 0; __atomic_load_n(&d->busy, __ATOMIC_ACQUIRE); i++) {
		if (i >= RA_RETIRE_MS)
			return 0;
		ksceKernelDelayThread(1000);
	}
	for (int i = 0; i < RA_PIECES; i++) {
		int exp = RA_QUEUED;

		__atomic_compare_exchange_n(&d->ra[i].state, &exp, RA_FREE, 0, __ATOMIC_ACQ_REL,
		                            __ATOMIC_RELAXED);
	}
	for (int t = 0;; t++) {
		int reading = 0;

		for (int i = 0; i < RA_PIECES; i++)
			reading |= __atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) == RA_READING;
		if (!reading && !__atomic_load_n(&d->busy, __ATOMIC_ACQUIRE))
			break;
		if (t >= RA_RETIRE_MS)
			return 0;
		ksceKernelDelayThread(1000);
	}
	ra_release(d);
	return 1;
}

/* Verify mode: what read ahead gave [sector, sector + n) at p is read again
 * the standard way and compared, in pieces the size of the check buffer. On a
 * mismatch the caller gets the standard data and the device falls back, as
 * any faster transfer that differs. 1 on a mismatch. */
static int ra_verify(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                     uint8_t *p, uint32_t n, int clean, void *shadow)
{
	int bad = 0;

	while (n) {
		const uint32_t k = n > STAGE_SECTORS ? STAGE_SECTORS : n;

		if (io(ref, part, sector, shadow, k) >= 0) {
			ADD(d->st.verified, 1);
			if (memcmp(p, shadow, (size_t)k * 512) != 0) {
				ADD(d->st.mismatches, 1);
				ST(d->fallback, 1);
				bad = 1;
				memcpy(p, shadow, (size_t)k * 512);
				if (clean)
					clean_out(p, k * 512);
				else
					vm_dsb();
			}
		}
		p += (size_t)k * 512;
		sector += k;
		n -= k;
	}
	return bad;
}

/* A read into non-cacheable memory (64 KiB and up) while read ahead is on:
 * from the pieces where they continue the stream (copied, by the copier as
 * the setting says; no cache to clean), the rest the standard way straight
 * into the buffer as before; then the stream noted and the pieces refilled.
 * The standard read's result is the caller's, as without read ahead. */
static uint32_t g_ra_nc_kib;              /* atomic: for the log */
static int ra_nc_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                      uint8_t *p, uint32_t nsec)
{
	OffMap om = { .uid = -1, .user = p, .len = nsec * 512, .mt = -2, .nc = 1 };
	uint32_t got = 0, own = 0;
	int ret = 0, stats, off, offload, bad = 0;
	void *shadow;

	if (!take(d, 1, nsec)) {
		ADD(g_nc_stock_kib, nsec / 2);
		return io(ref, part, sector, p, nsec);
	}
	if (!__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE)) {
		__sync_lock_release(&d->busy);          /* switched off meanwhile */
		ADD(g_nc_stock_kib, nsec / 2);
		return io(ref, part, sector, p, nsec);
	}
	shadow = take_shadow();                     /* verify mode: compared after */
	stats = LD(g_stats);
	offload = LD(g_offload);
	off = __atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE) &&
	      __atomic_load_n(&g_offload_state, __ATOMIC_ACQUIRE) >= 0 && !LD(g_nc_off_refused) &&
	      offload != VM_OFFLOAD_OFF && (om.pid = ksceKernelGetProcessId()) != KERNEL_PID;
	if (ra_any(d)) {
		got = ra_serve(d, part, io, sector, p, nsec, 0, stats, off, &om, nsec, &own);
		if (got < nsec)
			ra_drop(d);                         /* elsewhere, or used up */
	}
	if (om.uid >= 0)
		ksceKernelUserUnmap(om.uid);              /* the copier is done with it */
	if (got && shadow)
		bad = ra_verify(d, io, ref, part, sector, p, got, 0, shadow);
	if (got < nsec) {
		ret = io(ref, part, sector + got, p + (size_t)got * 512, nsec - got);
		ADD(g_nc_stock_kib, (nsec - got) / 2);
	}
	if (shadow)
		__sync_lock_release(&g_shadow_busy);
	if (got)
		ADD(g_ra_nc_kib, got / 2);
	if (ret >= 0 && !bad && ra_note(d, part, sector, nsec, 1))
		ra_refill(d, io, ref, part);
	if (LD(g_log_diag) && own)
		__atomic_fetch_add(&g_cp_core_kib[cpu_id()], own / 2, __ATOMIC_RELAXED);
	__sync_lock_release(&d->busy);
	if (bad) {
		void (*const fn)(void) = __atomic_load_n(&g_on_problem, __ATOMIC_ACQUIRE);

		if (fn)
			fn();
	}
	return ret;
}

/* One request, either way. `rd_ref` is the read function, used by a write's
 * verify re-read (a read verifies with `ref` itself). */
static int stage_xfer_one(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                          void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	uint8_t *p = buf;
	uint32_t left = nsec;
	int verify, ret = 0, mine = on_port(part, d->port);

	/* the largest read request seen, for the log (a lost update is fine) */
	if (mine && !write && nsec > __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED))
		__atomic_store_n(&g_rd_max, nsec, __ATOMIC_RELAXED);

	if (mine && LD(g_count_all)) {
		if (write)
			ADD(d->all_wr, (uint64_t)nsec * 512);
		else
			ADD(d->all_rd, (uint64_t)nsec * 512);
	}
	/* A non-cacheable caller buffer: the standard path is the faster one
	 * (see the top). Looked at only for transfers that would be staged. */
	if (mine && LD(d->enabled) && !LD(d->fallback) && __atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) &&
	    nsec && nsec >= LD(g_min_sectors) && caller_uncached(p, nsec * 512)) {
		/* ... unless read ahead holds it: a stream into non-cacheable
		 * memory, as games load their graphics data */
		if (!write && nsec >= RA_MIN_SECTORS && __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) &&
		    ksceKernelGetMemBlockMemtypeByAddr(p) == MEMTYPE_NC &&
		    ksceKernelGetMemBlockMemtypeByAddr(p + nsec * 512 - 1) == MEMTYPE_NC)
			return ra_nc_read(d, io, ref, part, sector, p, nsec);
		ADD(g_nc_stock_kib, nsec / 2);
		return io(ref, part, sector, buf, nsec);
	}
	/* Read ahead keeps pieces in the staging buffers: a small transfer goes
	 * the standard way meanwhile, instead of dropping them (a read elsewhere
	 * waits for the card either way; a write still makes them unusable). */
	if (mine && nsec < RA_MIN_SECTORS && ra_any(d) &&
	    ksceKernelGetSystemTimeLow() - __atomic_load_n(&d->ra_last_us, __ATOMIC_RELAXED) <
	    RA_LIVE_US)
		return io(ref, part, sector, buf, nsec);
	/* GPU-safe reads asked for, but this firmware has no cache clean to
	 * keep that promise: such reads take the standard path, which leaves
	 * the data in memory as promised. */
	if (!write && LD(g_gpu_safe) && !g_dcache_clean)
		return io(ref, part, sector, buf, nsec);
	if (!take(d, mine, nsec))
		return io(ref, part, sector, buf, nsec);
	if (!write)
		rd_ref = ref;
	void *const shadow = take_shadow();
	verify = shadow != NULL;
	if (verify && !rd_ref) {            /* no read hook yet: cannot verify this one */
		__sync_lock_release(&g_shadow_busy);
		verify = 0;
	}
	const int stats = LD(g_stats);
	int problem = 0;                    /* an error or a mismatch: wake the worker */
	const int clean = !write && LD(g_gpu_safe) && g_dcache_clean;
	const int ready = !write && __atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE);
	const int mode = LD(g_pipeline);    /* one value for the whole request */
	const int pipe = ready && !verify && mode;
	const int offload = LD(g_offload);
	/* copy on the system core: a game's read (a system thread's buffers
	 * need no mapping, and copy where they are), as the setting says */
	OffMap om = { .uid = -1, .user = p, .len = nsec * 512, .mt = -2 };
	const int off = ready && __atomic_load_n(&g_offload_state, __ATOMIC_ACQUIRE) >= 0 &&
	                nsec >= OFFLOAD_MIN_SECTORS &&
	                (offload == VM_OFFLOAD_ALWAYS || (offload == VM_OFFLOAD_GPU && clean)) &&
	                (om.pid = ksceKernelGetProcessId()) != KERNEL_PID;
	uint32_t sz[PIPE_MAX_PARTS], own = 0;
	int k = 0;
	const uint32_t sector0 = sector;
	/* read ahead: this read continues the stream from the pieces, if they
	 * hold it; anything else drops them first (they are in the buffers) */
	const int ra = !write && __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE);

	if (clean)
		ADD(g_gpu_cleaned_kib, left / 2);
	if (write && nsec >= RA_MIN_SECTORS)
		d->ra_streak = 0;
	if (ra_any(d)) {
		const uint32_t got = ra ? ra_serve(d, part, io, sector, p, left, clean, stats, off, &om,
		                                   left, &own)
		                        : 0;

		if (got && verify && ra_verify(d, io, ref, part, sector, p, got, clean, shadow))
			problem = 1;                      /* fallen back: the rest stock */
		p += (size_t)got * 512;
		sector += got;
		left -= got;
		if (!got || left)
			ra_drop(d);
	}
	while (ret >= 0 && left && !LD(d->fallback)) {
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
				ADD(d->tm.wr_sectors, n);
				ADD(d->tm.wr_copy_us, t2 - t1);
				ADD(d->tm.wr_io_us, t3 - t2);
			}
		} else if (off) {
			/* Copied on the system core, in parts as pipe_plan says (one
			 * without overlapped reads). */
			uint32_t done = 0, mine_n = 0;

			if (!(pipe &&
			      n >= (mode == VM_PIPE_AUTO ? PIPE_MIN_AUTO : PIPE_MIN_SECTORS) &&
			      (k = pipe_plan(d, n, clean, sz)) >= 2)) {
				k = 1;
				sz[0] = n;
			}
			/* the buffer's memory type, read here: the copier, which maps
			 * it, is not in this process */
			off_memtype(&om);
			ret = offload_run(d, io, ref, part, sector, p, sz, (uint32_t)k, clean, stats, &om,
			                  left == n, &done, &mine_n);
			own += mine_n;
			if (ret < 0) {
				p += (size_t)done * 512;    /* the parts already copied stay */
				sector += done;
				left -= done;
				break;
			}
			if (k >= 2)
				ADD(g_pipe_reads, 1);
			if (k > 2)
				ADD(g_pipe_reads3, 1);
		} else if (pipe &&
		           n >= (mode == VM_PIPE_AUTO ? PIPE_MIN_AUTO : PIPE_MIN_SECTORS) &&
		           (k = pipe_plan(d, n, clean, sz)) >= 2) {
			/* Overlapped, this chunk in 2-3 parts (see pipe_run). */
			uint32_t done = 0;

			ret = pipe_run(d, io, ref, part, sector, p, sz, (uint32_t)k, clean, stats, &done);
			own += done;
			if (ret < 0) {
				p += (size_t)done * 512;    /* the parts already copied stay */
				sector += done;
				left -= done;
				break;
			}
			ADD(g_pipe_reads, 1);
			if (k > 2)
				ADD(g_pipe_reads3, 1);
		} else {
			ret = io(ref, part, sector, d->buf, n);
			if (ret < 0)
				break;
			if (stats)
				t2 = ksceKernelGetSystemTimeLow();
			vm_copy_from_stage(p, d->buf, bytes);
			if (clean)
				clean_out(p, bytes);   /* counted as copy time */
			own += n;
			if (stats) {
				t3 = ksceKernelGetSystemTimeLow();
				ADD(d->tm.rd_sectors, n);
				ADD(d->tm.rd_io_us, t2 - t1);
				ADD(d->tm.rd_copy_us, t3 - t2);
			}
		}
		if (verify && io(rd_ref, part, sector, shadow, n) >= 0) {
			ADD(d->st.verified, 1);
			if (memcmp(p, shadow, bytes) != 0) {
				ADD(d->st.mismatches, 1);
				ST(d->fallback, 1);
				problem = 1;
				if (write) {
					/* rewrite this chunk the stock way, from the caller's data */
					ret = io(ref, part, sector, p, n);
					if (ret < 0)
						break;
				} else {
					memcpy(p, shadow, bytes);     /* the caller gets stock data */
					if (clean)
						clean_out(p, bytes);
				}
			}
		}
		if (stats) {
			if (write)
				ADD(d->wr_bytes, (uint64_t)bytes);
			else
				ADD(d->rd_bytes, (uint64_t)bytes);
		}
		p += bytes;
		sector += n;
		left -= n;
	}
	if (om.uid >= 0)
		ksceKernelUserUnmap(om.uid);          /* the copier is done with it */
	if (!write && ret >= 0 && !left && !LD(d->fallback) && !problem) {
		if (ra_note(d, part, sector0, nsec, 0) && ra)
			ra_refill(d, io, ref, part);
	}
	if (verify)
		__sync_lock_release(&g_shadow_busy);
	__sync_lock_release(&d->busy);
	if (ret < 0) {
		ADD(d->st.errors, 1);
		ST(d->fallback, 1);
		problem = 1;
	} else if (write) {
		ADD(d->st.writes, 1);
	} else {
		ADD(d->st.reads, 1);
		if (LD(g_log_diag) && !left)
			diag_note(nsec, own);
	}
	if (problem) {
		void (*const fn)(void) = __atomic_load_n(&g_on_problem, __ATOMIC_ACQUIRE);

		if (fn)
			fn();
	}
	/* Anything not done on the faster path (error, mismatch) is done stock. */
	return left ? io(ref, part, sector, p, left) : ret;
}

/* Every write, whichever way it goes, is counted while it runs and marks the
 * pieces it touches dirty first (see "read ahead"). */
static int stage_xfer(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                      void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	int r;

	if (!write)
		return stage_xfer_one(d, 0, io, ref, rd_ref, part, sector, buf, nsec);
	__atomic_fetch_add(&d->ra_wbusy, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < RA_PIECES; i++) {
		RaPiece *pc = &d->ra[i];

		if (__atomic_load_n(&pc->state, __ATOMIC_SEQ_CST) != RA_FREE) {
			const uint64_t a = LD(pc->sector), e = a + LD(pc->n);

			if ((uint64_t)sector < e && (uint64_t)sector + nsec > a)
				__atomic_store_n(&pc->dirty, 1, __ATOMIC_SEQ_CST);
		}
	}
	r = stage_xfer_one(d, 1, io, ref, rd_ref, part, sector, buf, nsec);
	__atomic_fetch_sub(&d->ra_wbusy, 1, __ATOMIC_SEQ_CST);
	return r;
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
	ST(g_live, 1);
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (LD(g_dev[i].enabled))
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
			ST(m->unusable, 1);
			__atomic_store_n(&m->buf, NULL, __ATOMIC_RELEASE);
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
		ST(g_dev[i].enabled, 0);
		ST(g_dev[i].unusable, 1);       /* and never allocate again */
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
			__atomic_store_n(&g_dev[i].buf, NULL, __ATOMIC_RELEASE);
			ksceKernelFreeMemBlock(g_dev[i].uid);
			g_dev[i].uid = -1;
		}
	if (g_shadow_uid >= 0) {
		__atomic_store_n(&g_shadow, NULL, __ATOMIC_RELEASE);
		ksceKernelFreeMemBlock(g_shadow_uid);
		g_shadow_uid = -1;
	}
}

void vm_stage_apply(const VmSettings *s)
{
	const int32_t want[VM_DEV_COUNT] = { s->sd2vita, s->internal, s->memcard };
	const int needed = LD(g_pipeline) || LD(g_offload) || LD(g_readahead);   /* the helper */
	const int pipeline = s->pipeline >= VM_PIPE_OFF && s->pipeline <= VM_PIPE_FIXED
	                   ? s->pipeline : VM_PIPE_OFF;
	const int offload = s->offload >= VM_OFFLOAD_OFF && s->offload <= VM_OFFLOAD_ALWAYS
	                  ? s->offload : VM_OFFLOAD_OFF;

	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];

		if ((want[i] ? 1 : 0) != LD(d->enabled))
			ADD(d->ra_epoch, 1);  /* nothing read ahead before is used after */
		if (want[i] && !LD(d->enabled)) {
			ST(d->fallback, 0);   /* switching it back on is a fresh start */
			ST(d->alloc_err, 0);  /* ... and a fresh try for its buffer    */
		}
		if (want[i])
			ensure_buf(d);
		ST(d->enabled, want[i] ? 1 : 0);
	}
	ST(g_min_sectors, s->small ? 1u : (uint32_t)MIN_SECTORS);
	ST(g_stats, s->stats ? 1 : 0);
	ST(g_gpu_safe, s->gpusafe ? 1 : 0);
	if (pipeline != LD(g_pipeline))
		for (int i = 0; i < VM_DEV_COUNT; i++) {
			ST(g_dev[i].est_cp_ns[0], 0u);    /* measured afresh */
			ST(g_dev[i].est_cp_ns[1], 0u);
		}
	ST(g_pipeline, pipeline);
	ST(g_count_all, (s->stats || s->hud != VM_HUD_OFF) ? 1 : 0);
	ST(g_verify, s->verify ? 1 : 0);
	ST(g_log_diag, s->log ? 1 : 0);
	if (offload != LD(g_offload) || !s->readahead != !LD(g_readahead))
		ST(g_nc_off_refused, 0);                  /* checked afresh */
	if (offload != LD(g_offload)) {
		__atomic_store_n(&g_offload_state, 0, __ATOMIC_RELEASE);   /* checked afresh */
		ST(g_offload_logged, 0);
	}
	ST(g_offload, offload);
	if (!s->readahead != !LD(g_readahead))
		for (int i = 0; i < VM_DEV_COUNT; i++)
			ST(g_dev[i].ra_failed, 0);    /* a fresh try at its thread */
	ST(g_readahead, s->readahead == 1 ? 1 : 0);
	/* The helper (and copier) wanted again after no setting wanted it: a
	 * fresh try at making them, whichever setting was switched on. */
	if (!needed && (pipeline || offload || LD(g_readahead)))
		for (int i = 0; i < VM_DEV_COUNT; i++)
			ST(g_dev[i].pipe_failed, 0);
	if (!s->verify)
		ST(g_shadow_failed, 0);   /* turning verify off and on again retries the buffer */
}

void vm_stage_on_problem(void (*fn)(void))
{
	__atomic_store_n(&g_on_problem, fn, __ATOMIC_RELEASE);
}

void vm_stage_worker_tick(void)
{
	static uint32_t last_log_us, last_cleaned, last_nc, last_max;
	const uint32_t now_us = ksceKernelGetSystemTimeLow();

	if (LD(g_gpu_safe) && !g_dcache_clean && LD(g_live) && !g_gpu_safe_logged) {
		g_gpu_safe_logged = 1;
		VM_KLOGW("stage: GPU-safe reads are on but the cache clean function was not found - "
		         "reads take the standard path");
	}
	/* Once a minute, if anything changed: how much went to non-cacheable
	 * buffers the standard way, and how much GPU-safe reads cleaned. */
	if (now_us - last_log_us >= 60u * 1000u * 1000u &&
	    (LD(g_gpu_cleaned_kib) != last_cleaned || LD(g_nc_stock_kib) != last_nc ||
	     __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED) != last_max)) {
		last_log_us = now_us;
		last_cleaned = LD(g_gpu_cleaned_kib);
		last_nc = LD(g_nc_stock_kib);
		last_max = __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED);
		/* each line under the log's 192 characters */
		VM_KLOGI("stage: non-cacheable (standard path) %u KiB, GPU-safe cleaned %u KiB, "
		         "largest read %u KiB", (unsigned)last_nc, (unsigned)last_cleaned,
		         (unsigned)(last_max / 2));
		VM_KLOGI("stage: overlapped reads %u (%u in 3 parts), helper waited %u times %u ms",
		         (unsigned)LD(g_pipe_reads), (unsigned)LD(g_pipe_reads3),
		         (unsigned)__atomic_load_n(&g_pipe_waits, __ATOMIC_RELAXED),
		         (unsigned)(__atomic_load_n(&g_pipe_wait_us, __ATOMIC_RELAXED) / 1000));
		VM_KLOGI("stage: copied on core 0/1/2/3: %u/%u/%u/%u MiB; caller system %u MiB, "
		         "game %u MiB",
		         (unsigned)(__atomic_load_n(&g_cp_core_kib[0], __ATOMIC_RELAXED) / 1024),
		         (unsigned)(__atomic_load_n(&g_cp_core_kib[1], __ATOMIC_RELAXED) / 1024),
		         (unsigned)(__atomic_load_n(&g_cp_core_kib[2], __ATOMIC_RELAXED) / 1024),
		         (unsigned)(__atomic_load_n(&g_cp_core_kib[3], __ATOMIC_RELAXED) / 1024),
		         (unsigned)(__atomic_load_n(&g_cp_sys_kib, __ATOMIC_RELAXED) / 1024),
		         (unsigned)(__atomic_load_n(&g_cp_game_kib, __ATOMIC_RELAXED) / 1024));
		if (LD(g_readahead) || LD(g_ra_hits) || LD(g_ra_drops))
			VM_KLOGI("stage: read ahead %u reads %u MiB (%u MiB non-cacheable), dropped %u "
			         "(%u MiB unused), unusable %u, NC copier refused %d (0x%08x)",
			         (unsigned)LD(g_ra_hits), (unsigned)(LD(g_ra_kib) / 1024),
			         (unsigned)(LD(g_ra_nc_kib) / 1024), (unsigned)LD(g_ra_drops),
			         (unsigned)(LD(g_ra_waste_kib) / 1024), (unsigned)LD(g_ra_bad),
			         (int)LD(g_nc_off_refused), (unsigned)LD(g_nc_off_type));
		if (LD(g_offload) || __atomic_load_n(&g_off_reads, __ATOMIC_RELAXED))
			VM_KLOGI("stage: copied on the system core %u reads %u MiB, mapping failed %u (0x%08x)",
			         (unsigned)__atomic_load_n(&g_off_reads, __ATOMIC_RELAXED),
			         (unsigned)(__atomic_load_n(&g_off_kib, __ATOMIC_RELAXED) / 1024),
			         (unsigned)__atomic_load_n(&g_off_maperr, __ATOMIC_RELAXED),
			         (unsigned)LD(g_offload_err));
	}
	/* Copy on the system core: whether the mapping was found usable, once. */
	{
		const int st = __atomic_load_n(&g_offload_state, __ATOMIC_ACQUIRE);

		if (LD(g_offload) && st != LD(g_offload_logged)) {
			ST(g_offload_logged, st);
			if (st > 0)
				VM_KLOGI("stage: copy on the system core ready (mapping type 0x%08x)",
				         (unsigned)LD(g_offload_type));
			else if (st == -1)
				VM_KLOGW("stage: copy on the system core refused: the mapping is not "
				         "writable (type 0x%08x) - games copy as before",
				         (unsigned)LD(g_offload_type));
			else if (st == -2)
				VM_KLOGW("stage: copy on the system core refused: the mapping's memory type "
				         "differs - games copy as before");
		}
	}
	/* Logged here, not where it happens (that can be a syscall). */
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (LD(g_dev[i].alloc_err) && !LD(g_dev[i].alloc_logged)) {
			ST(g_dev[i].alloc_logged, 1);
			VM_KLOGW("stage: dev %d staging buffer not allocated (0x%08x) - stock", i,
			         (unsigned)LD(g_dev[i].alloc_err));
		}
	/* Overlapped reads: the helper and its buffer exist only while they can
	 * be used - made here (never in a transfer) for every device switched on
	 * while the setting is on, and given back once either is switched off. */
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];
		const int wanted = (LD(g_pipeline) || LD(g_offload) || LD(g_readahead)) && LD(g_live) &&
		                   LD(d->enabled) && __atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) &&
		                   !LD(d->unusable);
		/* never on the internal storage: the system's own data lives there,
		 * read and written by the system at any time */
		const int ra_wanted = wanted && LD(g_readahead) && !LD(d->fallback) &&
		                      i != VM_DEV_INTERNAL;

		/* read ahead: its thread, once the helper and buffers are there */
		if (!ra_wanted && d->ra_made) {
			if (ra_retire(d))
				VM_KLOGI("stage: dev %d read ahead off", i);
		} else if (ra_wanted && d->ra_made && !__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE)) {
			__atomic_store_n(&d->ra_ready, 1, __ATOMIC_RELEASE);   /* wanted again */
		} else if (ra_wanted && !d->ra_made && !LD(d->ra_failed) && d->pipe_made &&
		           __atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE)) {
			const int r = ra_setup(d);

			if (r < 0) {
				ST(d->ra_failed, 1);
				VM_KLOGW("stage: dev %d read ahead not set up (0x%08x)", i, (unsigned)r);
			} else {
				VM_KLOGI("stage: dev %d read ahead ready (EXPERIMENT)", i);
			}
		}

		if (wanted && !d->pipe_made && !LD(d->pipe_failed)) {
			int r = pipe_setup(d);

			if (r < 0) {
				ST(d->pipe_failed, 1);
				VM_KLOGW("stage: dev %d overlapped reads not set up (0x%08x) - reads as before",
				         i, (unsigned)r);
			} else {
				VM_KLOGI("stage: dev %d overlapped reads ready (helper and copier, %u KiB buffer)",
				         i, (unsigned)(PIPE_TAIL_SECTORS / 2));
			}
		} else if (wanted && d->pipe_made && !__atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE)) {
			/* wanted again while a release waited for a request: still intact */
			__atomic_store_n(&d->pipe_ready, 1, __ATOMIC_RELEASE);
		} else if (!wanted && d->pipe_made) {
			if (pipe_retire(d))
				VM_KLOGI("stage: dev %d overlapped reads off - helper, copier and buffer "
				         "released", i);
		}
	}
	/* The verify buffer is only allocated once verify mode is first turned on. */
	if (LD(g_verify) && !__atomic_load_n(&g_shadow, __ATOMIC_ACQUIRE) && g_shadow_uid < 0 &&
	    !LD(g_shadow_failed)) {
		void *b = NULL;
		int r = alloc_buf(SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, 0, STAGE_BYTES, &g_shadow_uid, &b);

		if (r < 0) {
			g_shadow_uid = -1;
			ST(g_shadow_failed, 1);   /* do not retry every second */
			VM_KLOGW("stage: verify buffer not allocated (0x%08x)", (unsigned)r);
		} else {
			__atomic_store_n(&g_shadow, b, __ATOMIC_RELEASE);   /* published last */
		}
	}
}

void vm_stage_bytes(int dev, uint64_t *rd, uint64_t *wr)
{
	*rd = LD(g_dev[dev].all_rd);
	*wr = LD(g_dev[dev].all_wr);
}

int vm_stage_verify_ready(void)
{
	return __atomic_load_n(&g_shadow, __ATOMIC_ACQUIRE) != NULL;
}

/* A release of the helper waits for a request still in flight (see
 * pipe_retire): the worker should come back soon. */
int vm_stage_pending(void)
{
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if ((LD(g_dev[i].pipe_made) && !__atomic_load_n(&g_dev[i].pipe_ready, __ATOMIC_ACQUIRE)) ||
		    (LD(g_dev[i].ra_made) && !__atomic_load_n(&g_dev[i].ra_ready, __ATOMIC_ACQUIRE)))
			return 1;
	return 0;
}

void vm_stage_get(VmDevState out[VM_DEV_COUNT], int plugin_active)
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const Dev *d = &g_dev[i];

		out[i].reads = LD(d->st.reads);
		out[i].writes = LD(d->st.writes);
		out[i].passthru = LD(d->st.passthru);
		out[i].errors = LD(d->st.errors);
		out[i].verified = LD(d->st.verified);
		out[i].mismatches = LD(d->st.mismatches);
		out[i].read_mib = (uint32_t)(LD(d->rd_bytes) >> 20);
		out[i].write_mib = (uint32_t)(LD(d->wr_bytes) >> 20);
		out[i].time_ms = 0;           /* no longer measured (VmDevState is frozen) */
		out[i].max_ms = 0;
		out[i].status = !plugin_active ? VM_ST_UNAVAIL :
		                !LD(d->enabled) ? VM_ST_OFF :
		                !__atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) ? VM_ST_NO_BUFFER :
		                LD(d->fallback) ? VM_ST_FALLBACK : VM_ST_ACTIVE;
	}
}

void vm_stage_timing(VmTimingDev out[VM_DEV_COUNT])
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const VmTimingDev *t = &g_dev[i].tm;

		out[i].rd_sectors = LD(t->rd_sectors);
		out[i].rd_io_us = LD(t->rd_io_us);
		out[i].rd_copy_us = LD(t->rd_copy_us);
		out[i].wr_sectors = LD(t->wr_sectors);
		out[i].wr_io_us = LD(t->wr_io_us);
		out[i].wr_copy_us = LD(t->wr_copy_us);
	}
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
