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
 * and (SceMsifForDriver):
 *   ksceMsifReadSector / ksceMsifWriteSector        -> the memory card (one slot)
 * Never touched: the internal storage (the MMC functions, which the system's
 * own data and game cards go through, are not hooked at all), small transfers (< 32 KiB: file-system metadata; unless
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
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/io/dirent.h>
#include <taihen.h>

#include "stage.h"
#include "klog.h"
#include "vm_hook.h"

/* The CPU copies between the caller's buffer and the staging buffer (the host
 * tests replace both to give the copy a measurable time).
 *   out of staging (reads): 8-word LDM/STM bursts (fastcopy.h). Measured on
 *     SD2Vita: 4.5 -> 4.2 us per 512-byte block.
 *   into staging (writes): the system memcpy. The burst copy made it slower
 *     (1.7 -> 2.3 us per block), so writes keep memcpy. */
#include "fastcopy.h"
#include "vm_memblock.h"
#ifndef vm_copy_from_stage
#define vm_copy_from_stage stage_copy_out   /* the DMA controller or the CPU (below) */
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
	int            nc;                /* read for a non-cacheable stream (adaptive) */
	uint32_t       wait_from;         /* calibration: atomic, when a request first
	                                   * came while it was read (0: none)    */
	uint32_t       held_us;           /* ... how long it held that one up      */
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
	uint8_t      *job_dst;                /* NULL: the staging buffers; else the
	                                       * caller's own buffer, mapped (one part) */
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
	/* the stream of 32 KiB reads, kept apart (see RA_SMALL_SECTORS) */
	uint32_t      rs_end, rs_streak;
	int           rs_nc;
	void         *rs_part;
	int           ra_noted_small;         /* the last read noted was one of those */
	int           ra_small;               /* atomic: the pieces are for that stream */
	volatile int  rs_lock;                /* its fields: any thread, a moment only */
	/* the tiny reads' stream and their block (see RA_TINY_SECTORS), the
	 * request holder's; a write looks at t_start, t_end, t_dirty */
	void         *t_buf;
	SceUID        t_uid;
	uint32_t      t_next, t_streak;
	uint32_t      t_start, t_end;         /* atomic; t_end == t_start: empty   */
	int           t_dirty;                /* atomic: a write touched it        */
	void         *t_part;
	io_fn         t_io;
	uint32_t      t_epoch, t_us;
	void         *ra_last_part;
	uint32_t      ra_last_us;             /* when the stream was last read (atomic) */
	uint32_t      ra_us, rs_us;           /* when each stream was last read    */
	uint32_t      ra_wbusy;               /* writes on the device right now    */
	uint32_t      ra_epoch;               /* + 1 when the device is switched   */
} Dev;

static Dev g_dev[VM_DEV_COUNT] = {
	[VM_DEV_SD2VITA]  = { .port = 1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1,
	                      .th2 = -1, .sem_cgo = -1, .sem_fin = -1,
	                      .ra_th = -1, .ra_go = -1, .ra_done = -1, .t_uid = -1 },
	/* the internal storage: never sped up, nothing reaches it (no hooks) */
	[VM_DEV_INTERNAL] = { .port = 0, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1,
	                      .th2 = -1, .sem_cgo = -1, .sem_fin = -1,
	                      .ra_th = -1, .ra_go = -1, .ra_done = -1, .t_uid = -1 },
	[VM_DEV_MEMCARD]  = { .port = -1, .uid = -1, .uid2 = -1, .th = -1, .sem_go = -1, .sem_done = -1, .sem_free = -1,
	                      .th2 = -1, .sem_cgo = -1, .sem_fin = -1,
	                      .ra_th = -1, .ra_go = -1, .ra_done = -1, .t_uid = -1 },
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
static volatile int g_count_cfg, g_count_hud;   /* it is: as set, or shown by keys */

/* GPU-safe reads: ksceKernelDcacheCleanRange (SceCpuForDriver: L1 and L2),
 * looked up at run time by vm_stage_init. */
#define LIB_CPU_DRIVER     0x40ECDB0E
#define NID_DCACHE_CLEAN   0x103872A5
static void (*g_dcache_clean)(void *start, unsigned int size);
static volatile int g_gpu_safe;
static int g_gpu_safe_logged;
static volatile uint32_t g_gpu_cleaned_kib, g_nc_stock_kib;   /* for the log */
static volatile uint32_t g_gpu_dma_kib;   /* ... of it not cleaned: DMA copies, "Non-DMA" */

/* SceSysmemForDriver: the memory type at an address of the current address
 * space, masked with 0xFF00 (0xD000 cached, 0x4000 L1 write-back only,
 * 0x8000 non-cacheable - also CDRAM -, 0x0800 device), or < 0. */
int ksceKernelGetMemBlockMemtypeByAddr(const void *addr);
#define MEMTYPE_NC      0x8000
#define MEMTYPE_DEVICE  0x0800

/* Whether a memory type is non-cacheable; anything unknown counts as cached
 * (and is staged as before). */
static int mt_uncached(int m)
{
	return m == MEMTYPE_NC || m == MEMTYPE_DEVICE;
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

enum { H_RD_SD, H_WR_SD, H_RD_MS, H_WR_MS, H_COUNT };
#define H_SDIF_COUNT H_RD_MS          /* the SceSdif hooks come first          */
static volatile tai_hook_ref_t g_ref[H_COUNT];
static SceUID g_hook[H_COUNT] = { -1, -1, -1, -1 };

static int io_sdif(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	return TAI_CONTINUE(int, ref, part, sector, buf, n);
}

static int io_msif(tai_hook_ref_t ref, void *part, uint32_t sector, void *buf, uint32_t n)
{
	(void)part;
	return TAI_CONTINUE(int, ref, sector, buf, n);
}

static tai_hook_ref_t ref_of(int h)
{
	return vm_hook_ref(&g_ref[h]);
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
/* The least stack each thread has had free (bytes), for the log: how much
 * of VM_STACK_* is left. NULL: not found. */
static int (*g_stack_free_fn)(SceUID thid);
/* Graphics data (non-cacheable) from read ahead, copied by the copier on the
 * system core / by the game's thread: they count as NC, not as copies (C). */
static uint32_t g_ra_nc_sys_kib, g_ra_nc_own_kib;                  /* atomics */
static volatile int g_log_diag;            /* the log is on: count            */
static volatile int g_count_cores;         /* the overlay is open: count too  */
#define COUNT_CORES() (LD(g_log_diag) || LD(g_count_cores))

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
#define OFF_PAGE            4096u      /* the system's page (OffMap.head)   */
/* "C + NC": the game's thread maps its own buffer for the helper,
 * so only reads of 64 KiB and up. */
#define NCD_MIN_SECTORS     128
static volatile int g_offload;             /* the setting, VM_OFFLOAD_*       */
static volatile int g_offload_state;       /* 0 unchecked, 1 usable, < 0 refused */
static int          g_offload_logged;      /* the state the worker last logged */
static uint32_t     g_offload_type;        /* the first mapping's memblock type */
static int          g_offload_err;         /* why refused / the last map error */
static uint32_t     g_off_reads, g_off_kib, g_off_maperr;      /* atomics      */
static uint32_t     g_off_headed;          /* requests mapped from their 2nd page */
/* The counters as they were when the app now in front was started (for
 * VmRaNow.bufmap: its own buffers only). */
static uint32_t     g_bm_reads, g_bm_headed, g_bm_fail;
static SceUID       g_bm_pid;
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
	int      first;            /* its memory type at the start, if already read, or -2 */
	int      mt;               /* the buffer's memory type at both ends (read by
	                            * the caller: an address of its own space), or -1 */
	int      nc;               /* a non-cacheable buffer (read ahead): a mapping
	                            * that does not fit refuses only those */
	uint32_t head;             /* bytes at its start the mapping leaves out: a
	                            * first page the system has in no memory block
	                            * (PCSB00724's buffer at the very start of its
	                            * block); a chunk that takes any of them is the
	                            * caller's to copy, the rest the copier's      */
} OffMap;
static void off_map(OffMap *m);

/* Mappings of a caller's buffer that the system refused (copy on the system
 * core, "C + NC All"): the first MAPFAIL_LOG described for the log, each
 * written once by the worker - the error, the buffer and what the system
 * says about the memory block(s) it lies in - so a refusal seen on a console
 * can be looked into. Only on a refusal (rare): nothing on the hot path. */
#define MAPFAIL_LOG 8
/* What covers a refused buffer: walked in MAPFAIL_STEP steps, up to
 * MAPFAIL_SEGS stretches of the same block (or of none), and for each block
 * met (up to MAPFAIL_BLKS) what GetInfoEx says and whether its part of the
 * buffer maps on its own; a stretch with no block: whether its first step
 * maps. */
#define MAPFAIL_STEP 4096u
#define MAPFAIL_SEGS 6
#define MAPFAIL_BLKS 4
typedef struct MapFailSeg {
	uint32_t off, len;         /* in the buffer                             */
	SceUID   uid;              /* its block, or the lookup's error          */
	int      map;              /* mapping just this stretch: 0 it maps, or the error */
} MapFailSeg;
typedef struct MapFailBlk {
	SceUID   uid;
	int      info;             /* GetInfoEx's result                        */
	uint32_t type, base, size;
} MapFailBlk;
typedef struct MapFail {
	int      ready;            /* atomic: the fields below are written      */
	int      which;            /* 0 copy on the system core, 1 C + NC All   */
	int      err;              /* what ksceKernelProcUserMap returned       */
	SceUID   pid;
	uintptr_t user;            /* the buffer in the app's space             */
	uint32_t len;
	int      mt, nc;           /* its memory type as the caller saw it, NC  */
	int      nseg, nblk, more; /* more: stretches past MAPFAIL_SEGS          */
	MapFailSeg seg[MAPFAIL_SEGS];
	MapFailBlk blk[MAPFAIL_BLKS];
	char     title[16];        /* the app in front                          */
} MapFail;
static MapFail           g_mapfail[MAPFAIL_LOG];
static volatile uint32_t g_mapfail_n;          /* refusals described (claimed) */
static uint32_t          g_mapfail_logged;     /* the worker's                 */

/* VM_OFFLOAD_FULL ("C + NC"): a game's read into
 * NON-cacheable memory, which otherwise takes the standard path on the game's
 * own thread, is read by the helper on the system core straight into the
 * game's buffer through a kernel mapping of it - no staging, no copy - while
 * the game's thread sleeps. The mapping must be non-cacheable as well (the
 * driver then has no cache to clean); anything else, and the read stays
 * standard. */
static volatile int g_ncd_state;           /* 0 unchecked, 1 usable, < 0 refused */
static int          g_ncd_logged;
static uint32_t     g_ncd_type;
static int          g_ncd_err;
static uint32_t     g_ncd_reads, g_ncd_kib, g_ncd_maperr, g_ncd_busy, g_ncd_fail;  /* atomics */

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
			void *buf = d->job_dst ? (void *)d->job_dst : (i & 1) ? d->buf2 : d->buf;
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

/* ---- DMA copy (VM_KEY_DMACOPY) ---------------------------------------------
 *
 * A read's data copied out of the staging buffer by the console's DMA
 * controller instead of the CPU: the system's own ksceDmacMemcpy (SceDmacmgr),
 * the call the system offers for this. It is expected to deal with the CPU
 * caches itself, since games use it on their own cached memory through
 * sceDmacMemcpy; that is not documented, so verify mode is how it is checked
 * on a console (a difference stops it). Looked up at start: a system without
 * it reads as before.
 *
 * Only for copies that cannot share a cache line with anything else: whole
 * lines (VM_DMA_ALIGN) at both ends, from VM_DMA_MIN_BYTES up: the smallest
 * part of an overlapped read, which in games carries most of the data (below
 * it are the tiny reads, a few MiB). The 16-32 KiB copies are timed apart, to
 * see whether starting the controller pays for them. Everything else, and
 * any copy it refuses, is the CPU's (vm_fast_copy), so the caller's data is
 * the same either way. It stops itself after VM_DMA_MAX_ERRORS refusals, or
 * at the first verify-mode difference while it is on (dma_stop), until the
 * setting is switched off and on (vm_stage_apply). Hot path: counters only;
 * the worker logs. */
#define LIB_DMAC_DRIVER  0xEB4E3738    /* SceDmacmgrForDriver                    */
#define NID_DMAC_MEMCPY  0x00896B11    /* ksceDmacMemcpy(dst, src, size)        */

static int (*g_dmac_memcpy)(void *dst, const void *src, SceSize size);
static volatile int      g_dma_want;           /* the setting                   */
static volatile int      g_dma_stop;           /* VM_DMA_WHY_*: stopped itself  */
static volatile int      g_dma_logged;         /* the worker logged the stop    */
static volatile uint32_t g_dma_copies, g_dma_kib, g_dma_us, g_dma_errors;
/* ... the 16-32 KiB ones (overlapped reads' parts), to judge the threshold */
static volatile uint32_t g_dma_mid, g_dma_mid_us;
/* left to the CPU while on: too small (and of them half the threshold up), not
 * whole lines */
static volatile uint32_t g_dma_small, g_dma_small_kib, g_dma_small_half;
static volatile uint32_t g_dma_unal, g_dma_unal_kib;
static volatile int32_t  g_dma_last_err;

static int dma_state(void)
{
	if (!LD(g_dma_want))
		return VM_DMA_OFF;
	if (!g_dmac_memcpy)
		return VM_DMA_UNAVAIL;
	return LD(g_dma_stop) ? VM_DMA_STOPPED : VM_DMA_ON;
}

/* First reason wins; the worker logs it once. */
static void dma_stop(int why)
{
	int none = VM_DMA_WHY_NONE;

	__atomic_compare_exchange_n(&g_dma_stop, &none, why, 0, __ATOMIC_RELAXED,
	                            __ATOMIC_RELAXED);
}

/* 1 if the DMA controller made the copy, 0 if the CPU has to. */
static int dma_try(void *dst, const void *src, size_t n)
{
	uint32_t t0;
	int r;

	if (dma_state() != VM_DMA_ON)
		return 0;
	if (n < VM_DMA_MIN_BYTES) {
		ADD(g_dma_small, 1);
		ADD(g_dma_small_kib, (uint32_t)(n >> 10));
		if (n >= VM_DMA_MIN_BYTES / 2)
			ADD(g_dma_small_half, 1);
		return 0;
	}
	if (((uintptr_t)dst | (uintptr_t)src | (uintptr_t)n) & (VM_DMA_ALIGN - 1u)) {
		ADD(g_dma_unal, 1);
		ADD(g_dma_unal_kib, (uint32_t)(n >> 10));
		return 0;
	}
	t0 = ksceKernelGetSystemTimeLow();
	r = g_dmac_memcpy(dst, src, (SceSize)n);
	if (r < 0) {
		ST(g_dma_last_err, r);
		if (__atomic_add_fetch(&g_dma_errors, 1, __ATOMIC_RELAXED) >= VM_DMA_MAX_ERRORS)
			dma_stop(VM_DMA_WHY_ERRORS);
		return 0;
	}
	{
		const uint32_t us = ksceKernelGetSystemTimeLow() - t0;

		ADD(g_dma_us, us);
		if (n < 2u * VM_DMA_MIN_BYTES) {
			ADD(g_dma_mid, 1);
			ADD(g_dma_mid_us, us);
		}
	}
	ADD(g_dma_copies, 1);
	ADD(g_dma_kib, (uint32_t)(n >> 10));
	return 1;
}

/* Out of the staging buffer into the caller's: the DMA controller when it
 * can, else the CPU. A refused DMA copy may have written part of `dst`: the
 * CPU copy then writes all of it. 1 if the DMA controller made it. */
static int __attribute__((unused)) stage_copy_out(void *dst, const void *src, size_t n)
{
	if (dma_try(dst, src, n)) {
		__sync_synchronize();   /* the controller's data before what follows */
		return 1;
	}
	vm_fast_copy(dst, src, n);
	return 0;
}

/* A read's data out of the staging buffer, then, with GPU-safe reads
 * (`clean`), out of the CPU cache to memory - except, with GPU-safe
 * "Non-DMA", after a copy the DMA controller made: its data was written by
 * the controller, not through the CPU's cache, which is what that setting
 * relies on (the graphics chip shows whether it holds). */
static void copy_out(void *dst, const void *src, uint32_t n, int clean)
{
	const int dma = vm_copy_from_stage(dst, src, n);

	if (!clean)
		return;
	if (dma && LD(g_gpu_safe) == VM_GPUSAFE_NODMA) {
		ADD(g_gpu_dma_kib, n >> 10);
		vm_dsb();
		return;
	}
	clean_out(dst, n);
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
		copy_out(dst + off * 512, cur, sz[i] * 512, clean);
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
			/* no mapping, or a chunk that takes bytes it leaves out: the
			 * caller copies this one (the mapping stays for the rest) */
			if (m->uid < 0 || d->cjob_off < m->head) {
				if (d->cjob_last && m->uid >= 0) {
					ksceKernelUserUnmap(m->uid);
					m->uid = -1;
				}
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
		if (done && COUNT_CORES()) {
			if (d->cjob_map && d->cjob_map->nc)   /* graphics data: NC */
				__atomic_fetch_add(&g_ra_nc_sys_kib, done / 2, __ATOMIC_RELAXED);
			else
				__atomic_fetch_add(&g_cp_core_kib[cpu_id()], done / 2, __ATOMIC_RELAXED);
		}
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
	d->th = ksceKernelCreateThread("SceVitaMemioPipe", pipe_helper, 0x40, VM_STACK_DEV, 0,
	                               PIPE_CPU_SYSTEM, NULL);
	if (d->th < 0)
		d->th = ksceKernelCreateThread("SceVitaMemioPipe", pipe_helper, 0x40, VM_STACK_DEV, 0, 0,
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
	d->th2 = ksceKernelCreateThread("SceVitaMemioCopy", pipe_copier, 0x48, VM_STACK_DEV, 0,
	                                PIPE_CPU_SYSTEM, NULL);
	if (d->th2 < 0)
		d->th2 = ksceKernelCreateThread("SceVitaMemioCopy", pipe_copier, 0x48, VM_STACK_DEV, 0, 0,
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
	if (__sync_lock_test_and_set(&g_shadow_busy, 1))
		return NULL;
	/* taken: the worker frees it only while it holds g_shadow_busy itself */
	if (__atomic_load_n(&g_shadow, __ATOMIC_ACQUIRE) != b) {
		__sync_lock_release(&g_shadow_busy);
		return NULL;
	}
	return b;
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
	d->job_dst = NULL;
	d->job_sector = sector + sz[0];
	d->job_ret = 0;
	__atomic_store_n(&d->job_fail, 0, __ATOMIC_RELAXED);
	__sync_synchronize();
	ksceKernelSignalSema(d->sem_go, 1);
	for (uint32_t i = 1; i <= k; i++) {
		const uint32_t n = sz[i - 1];
		const void *cur = ((i - 1) & 1) ? d->buf2 : d->buf;
		uint32_t c0 = timed ? ksceKernelGetSystemTimeLow() : 0;

		copy_out(p + off * 512, cur, n * 512, clean);
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
static char g_fg_title[16];               /* (with the app events, below) */

/* A stretch of the buffer mapped on its own, and unmapped: 0 or the error. */
static int mapfail_probe(SceUID pid, uintptr_t user, uint32_t len)
{
	void *page = NULL;
	SceSize size = 0;
	SceUInt32 off = 0;
	const SceUID uid = ksceKernelProcUserMap(pid, "SceVitaMemioProbe", VM_USERMAP_RW,
	                                         (const void *)(uintptr_t)user, len, &page, &size, &off);

	if (uid < 0)
		return uid;
	ksceKernelUserUnmap(uid);
	return 0;
}

static void mapfail_note(int which, const OffMap *m, int err)
{
	const uint32_t i = __atomic_fetch_add(&g_mapfail_n, 1, __ATOMIC_RELAXED);
	const uintptr_t base = (uintptr_t)m->user;
	MapFail *f;
	VmMemBlockInfoEx info;

	if (i >= MAPFAIL_LOG)
		return;
	f = &g_mapfail[i];
	f->which = which;
	f->err = err;
	f->pid = m->pid;
	f->user = base;
	f->len = m->len;
	f->mt = m->mt;
	f->nc = m->nc;
	f->nseg = f->nblk = f->more = 0;
	/* the stretches: a step at a time, from the buffer's start (the first
	 * step from its own address, the rest from page boundaries) */
	for (uint32_t at = 0; at < m->len;) {
		const uintptr_t a = base + at;
		const uint32_t step = MAPFAIL_STEP - (uint32_t)(a & (MAPFAIL_STEP - 1));
		const uint32_t n = step < m->len - at ? step : m->len - at;
		const SceUID uid = ksceKernelFindProcMemBlockByAddr(m->pid, (const void *)(uintptr_t)a, 0);

		if (f->nseg && f->seg[f->nseg - 1].uid == uid) {
			f->seg[f->nseg - 1].len += n;
		} else if (f->nseg < MAPFAIL_SEGS) {
			f->seg[f->nseg].off = at;
			f->seg[f->nseg].len = n;
			f->seg[f->nseg].uid = uid;
			f->nseg++;
		} else {
			f->more = 1;
			break;
		}
		at += n;
	}
	/* each stretch on its own (with no block: its first step), and the blocks */
	for (int k = 0; k < f->nseg; k++) {
		MapFailSeg *g = &f->seg[k];
		int known = 0;

		g->map = mapfail_probe(m->pid, base + g->off,
		                       g->uid >= 0 || g->len < MAPFAIL_STEP ? g->len : MAPFAIL_STEP);
		if (g->uid < 0)
			continue;
		for (int b = 0; b < f->nblk; b++)
			known |= f->blk[b].uid == g->uid;
		if (known || f->nblk >= MAPFAIL_BLKS)
			continue;
		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		f->blk[f->nblk].uid = g->uid;
		f->blk[f->nblk].info = ksceKernelMemBlockGetInfoEx(g->uid, &info);
		f->blk[f->nblk].type = info.type;
		f->blk[f->nblk].base = (uint32_t)(uintptr_t)info.mapped_base;
		f->blk[f->nblk].size = info.mapped_size;
		f->nblk++;
	}
	memcpy(f->title, g_fg_title, sizeof(f->title) - 1);
	f->title[sizeof(f->title) - 1] = 0;
	__atomic_store_n(&f->ready, 1, __ATOMIC_RELEASE);
	{
		void (*const fn)(void) = __atomic_load_n(&g_on_problem, __ATOMIC_ACQUIRE);

		if (fn)
			fn();                         /* the worker writes it now */
	}
}

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
	/* from m->head on (0 but for a first page in no block, off_memtype) */
	uid = ksceKernelProcUserMap(m->pid, "SceVitaMemioCopy", VM_USERMAP_RW, m->user + m->head,
	                            m->len - m->head, &page, &size, &off);
	if (uid < 0) {
		ADD(g_off_maperr, 1);
		ST(g_offload_err, uid);
		mapfail_note(0, m, uid);
		return;
	}
	if (!page || size < off || size - off < m->len - m->head) {
		ksceKernelUserUnmap(uid);
		ADD(g_off_maperr, 1);
		ST(g_offload_err, VM_ERR_NOMEM);
		return;
	}
	/* kern + o is the buffer's byte o, for o >= head (the only ones used) */
	kern = (uint8_t *)page + off - m->head;
	mt = m->mt;
	if (mt < 0 || ksceKernelGetMemBlockMemtypeByAddr(kern + m->head) != mt ||
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
	if (m->head)
		ADD(g_off_headed, 1);
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
	d->job_dst = NULL;
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
 * Off by default, the SD2Vita and the memory card only (never the internal
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
/* Many apps read a file in the file system's 32 KiB pieces, and some read
 * nearly everything so. From this size to RA_MIN_SECTORS the reads make a
 * stream of their own: it neither breaks the large reads' stream nor is broken
 * by it, and takes the pieces only while that one has none. */
#define RA_SMALL_SECTORS 64
#define RA_SMALL_FACTOR  4
#define RA_SMALL_STREAK  16              /* reads in a row before it starts   */
/* Under that, a stream of tiny reads (some apps read whole files a sector at a
 * time, one card command each): from RA_TINY_STREAK reads in a row, the read
 * that finds nothing takes RA_TINY_SECTORS at once, on the caller's own thread,
 * into a buffer of their own; the next ones are served from it. As the pieces:
 * only for the read that lies in it, never after a write touched it or ran while
 * it was read, nor after the storage was switched or RA_MAX_AGE_US. */
#define RA_TINY_SECTORS  64
#define RA_TINY_STREAK   8
/* ... but only while the stream is live (read within this long): pieces left
 * behind by a stream that has ended are dropped by the next staged transfer,
 * which then goes the faster way as usual. */
#define RA_LIVE_US      30000u
/* A slow stream still gets what was read for it; a pause longer than this ends
 * the stream, so nothing is read over and over for a program that has stopped. */
#define RA_MAX_AGE_US   5000000u
#define RA_RETIRE_MS    50

static volatile int g_readahead;          /* the setting                      */
static uint32_t g_ra_hits, g_ra_kib, g_ra_drops, g_ra_waste_kib, g_ra_bad;   /* atomics */
static uint32_t g_ra_nc_kib;              /* atomic (defined again with ra_nc_read) */
static uint32_t g_ra_tiny_reads;          /* atomic: tiny reads from their block */
static uint32_t g_ra_bad_why[5];          /* atomics: failed, read during a write, dirty, switched, old */
/* The app in front and how much it reads where (the per-app settings; the HUD). */
#define RA_FG_DEV_MIN     2048            /* sectors (1 MiB): the HUD tells its storage */
#define RA_THR_NONE       0xFFFFu         /* an app's threshold: never start   */
#define RA_PROFILE_DIR    VM_DATA_DIR "/ra"
#define RA_PROFILE_MAGIC  0x41524D56u     /* "VMRA" */

static volatile int g_ra_mode;            /* VM_RA_*                          */
static int (*g_title_fn)(SceUID pid, char *titleid, SceSize len);
/* The app in front, from the process events (ra_proc_event): reads while
 * it is in front are its, whichever thread does them (a game's own, or a
 * system thread reading for it). 0: the home screen / none. */
#define LIB_PROCMGR_KERNEL     0x887F19D0
#define NID_PROC_EVENT         0x414CC813   /* ksceKernelInvokeProcEventHandler */
#define LIB_SYSROOT_KERNEL     0x3691DA45   /* SceSysrootForKernel (SceSysmem) */
#define NID_PROCESS_TITLE_ID   0xEC3124A3   /* ksceKernelSysrootGetProcessTitleId */
#define LIB_APPMGR_DRIVER      0xDCE180F8   /* SceAppMgrForDriver */
#define NID_EXCLUSIVE_RUNNING  0xA888C2DC   /* ksceAppMgrIsExclusiveProcessRunning */
static int (*g_exclusive_fn)(const char *title_id);
static SceUID         g_proc_hook = -1;
static volatile tai_hook_ref_t g_proc_ref;
static SceUID         g_ra_fg;              /* atomic */
static char           g_fg_title[16];       /* its title id ("" none / not known) */
static uint32_t       g_fg_sec[VM_DEV_COUNT];   /* its reads per storage (atomics) */
static uint32_t       g_fg_noid;            /* atomic: processes with no title id (log) */
static void eff_update(void);
static SceUID g_app_pid;

/* The overlay's read ahead test (ra_test.h): while on, reads of the app it was
 * started for are followed, nothing else changes. */
#include "ra_test.h"
static RaTest       g_rt;
static volatile int g_rt_on;
static volatile int g_rt_lock;
static SceUID       g_rt_pid;
static VmApp  g_app_cur;

/* A stream of `pid` has ended: L reads. */
/* The worker, now (from a request: only a semaphore signal, and rarely). */
static void ra_wake(void)
{
	void (*const fn)(void) = __atomic_load_n(&g_on_problem, __ATOMIC_ACQUIRE);

	if (fn)
		fn();
}

/* An app's profile: its own settings (VmApp): read ahead, copy on core 3,
 * overlapped reads, GPU-safe reads (-1: the global one, from Settings), and its
 * thresholds with read ahead On (0: the fixed). Written aside and swapped in, so
 * a cut-off write leaves the old file or the finished copy. Anything not exactly
 * right - another size, a value out of range, a wrong check - is not used: the
 * app then has the global settings. */
#define RA_PROFILE_VERSION 6u

typedef struct RaProf {
	VmApp app;
} RaProf;

typedef struct RaProfileFile {
	uint32_t magic, version;
	int32_t  ra, offload, pipe, gpusafe;
	uint32_t thr[2];
	uint32_t check;
} RaProfileFile;
#define RA_PROFILE_WORDS ((int)(sizeof(RaProfileFile) / 4) - 1)

static uint32_t ra_words_check(const uint32_t *w, int n)
{
	uint32_t c = 0x811C9DC5u;

	for (int i = 0; i < n; i++)
		c = (c ^ w[i]) * 16777619u;
	return c ^ 0xA5C3A5C3u;
}

static uint32_t ra_profile_check(const RaProfileFile *p)
{
	return ra_words_check((const uint32_t *)p, RA_PROFILE_WORDS);
}

static void ra_profile_path(char *out, size_t n, const char *title)
{
	snprintf(out, n, "%s/%s.bin", RA_PROFILE_DIR, title);
}

static void ra_profile_tmp(char *out, size_t n, const char *title)
{
	snprintf(out, n, "%s/%s.tmp", RA_PROFILE_DIR, title);
}

static int ra_thr_ok(uint32_t t)
{
	return t == RA_THR_NONE || (t >= 2 && t <= 8);
}

/* An app's own settings, every one -1 (global) or a value of its setting;
 * thresholds 0 (the fixed) or 2..8 / never. */
static int ra_app_ok(const VmApp *a)
{
	return a->ra >= -1 && a->ra <= VM_RA_ADAPTIVE && (a->offload == -1 || vm_setting_valid(VM_KEY_OFFLOAD, a->offload)) &&
	       a->pipe >= -1 && a->pipe <= VM_PIPE_FIXED &&
	       a->gpusafe >= -1 && a->gpusafe <= VM_GPUSAFE_NODMA &&
	       (!a->thr[0] || ra_thr_ok(a->thr[0])) && (!a->thr[1] || ra_thr_ok(a->thr[1]));
}

static void ra_app_global(VmApp *a)
{
	a->ra = a->offload = a->pipe = a->gpusafe = -1;
	a->thr[0] = a->thr[1] = 0;
}

static int ra_app_set_any(const VmApp *a)
{
	return a->ra >= 0 || a->offload >= 0 || a->pipe >= 0 || a->gpusafe >= 0 || a->thr[0] ||
	       a->thr[1];
}

/* One profile file, if usable. */
static int ra_prof_read(const char *path, RaProf *out)
{
	RaProfileFile f;
	RaProf p;
	SceUID fd;
	int n;

	fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return 0;
	memset(&f, 0, sizeof(f));
	n = ksceIoRead(fd, &f, sizeof(f));
	ksceIoClose(fd);
	if (n != (int)sizeof(f) || f.magic != RA_PROFILE_MAGIC || f.version != RA_PROFILE_VERSION ||
	    f.check != ra_profile_check(&f))
		return 0;
	p.app.ra = f.ra;
	p.app.offload = f.offload;
	p.app.pipe = f.pipe;
	p.app.gpusafe = f.gpusafe;
	p.app.thr[0] = f.thr[0];
	p.app.thr[1] = f.thr[1];
	if (!ra_app_ok(&p.app))
		return 0;
	*out = p;
	return 1;
}

/* 1 and the profile if the app has a usable one: its file, or the finished
 * copy a save cut short between removing it and the rename left. */
static int ra_prof_load(const char *title, RaProf *out)
{
	char path[96];
	int ok;

	ra_profile_path(path, sizeof(path), title);
	ok = ra_prof_read(path, out);
	if (!ok) {
		ra_profile_tmp(path, sizeof(path), title);
		ok = ra_prof_read(path, out);
	}
	return ok;
}

/* The app's profile gone, its copy with it. The result: its file's removal. */
static int ra_prof_remove(const char *title)
{
	char path[96];

	ra_profile_tmp(path, sizeof(path), title);
	ksceIoRemove(path);
	ra_profile_path(path, sizeof(path), title);
	return ksceIoRemove(path);
}

static void ra_prof_save(const char *title, const RaProf *in)
{
	char path[96], tmp[96];
	RaProfileFile p;
	SceUID fd;
	int w;

	/* nothing out of range is ever written */
	if (!ra_app_ok(&in->app))
		return;
	ra_profile_path(path, sizeof(path), title);
	ra_profile_tmp(tmp, sizeof(tmp), title);
	memset(&p, 0, sizeof(p));
	p.magic = RA_PROFILE_MAGIC;
	p.version = RA_PROFILE_VERSION;
	p.ra = in->app.ra;
	p.offload = in->app.offload;
	p.pipe = in->app.pipe;
	p.gpusafe = in->app.gpusafe;
	p.thr[0] = in->app.thr[0];
	p.thr[1] = in->app.thr[1];
	p.check = ra_profile_check(&p);
	ksceIoMkdir("ur0:data", 0777);
	ksceIoMkdir(VM_DATA_DIR, 0777);
	ksceIoMkdir(RA_PROFILE_DIR, 0777);
	/* written aside, then swapped in: the old file stays until the new one is
	 * whole (as the settings) */
	fd = ksceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0) {
		VM_KLOGW("stage: profile for %s not saved (0x%08x)", title, (unsigned)fd);
		return;
	}
	w = ksceIoWrite(fd, &p, sizeof(p));
	ksceIoClose(fd);
	if (w != (int)sizeof(p)) {
		VM_KLOGW("stage: profile for %s not saved (short write)", title);
		ksceIoRemove(tmp);
		return;
	}
	ksceIoRemove(path);
	if ((w = ksceIoRename(tmp, path)) < 0)
		VM_KLOGW("stage: profile for %s: rename failed (0x%08x), the copy stays", title,
		         (unsigned)w);
}

/* System apps (the home screen, Settings, ...: NPXS*, and "main") are never
 * in front: no settings of their own. */
static int ra_title_system(const char *t)
{
	return (t[0] == 'N' && t[1] == 'P' && t[2] == 'X' && t[3] == 'S') ||
	       (t[0] == 'm' && t[1] == 'a' && t[2] == 'i' && t[3] == 'n' && !t[4]);
}

/* Title ids are letters and digits; anything else is not used as a file name. */
static int ra_title_ok(const char *t)
{
	int n = 0;

	for (; t[n]; n++)
		if (!((t[n] >= 'A' && t[n] <= 'Z') || (t[n] >= 'a' && t[n] <= 'z') ||
		      (t[n] >= '0' && t[n] <= '9') || t[n] == '_') || n >= 12)
			return 0;
	return n >= 4;
}

/* A process event: 1 started, 5 resumed (now in front), 3 exited, 4
 * suspended (back to the home screen). As PSVshell follows them: the
 * system's own apps (NPXS) never take the front, and while an exclusive
 * app (a game) runs, another process starting does not either. Any thread;
 * the worker is woken to follow the change. */
static void ra_proc_event(SceUID pid, int ev)
{
	/* a process id is a UID: any value but 0 (the high bit may be set) */
	if (!pid)
		return;
	if (ev == 1 || ev == 5) {
		char t[32];
		const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);
		int r = -1;

		memset(t, 0, sizeof(t));
		/* no title id: not an app we can tell (never one in front) */
		if (!g_title_fn || (r = g_title_fn(pid, t, sizeof(t) - 1)) < 0 || !ra_title_ok(t)) {
			__atomic_fetch_add(&g_fg_noid, 1, __ATOMIC_RELAXED);
			return;
		}
		if (ra_title_system(t)) {
			return;
		}
		if (fg && fg != pid && g_exclusive_fn && g_exclusive_fn(NULL) > 0) {
			return;
		}
		if (pid != fg) {                      /* another app: its storage afresh */
			for (int i = 0; i < VM_DEV_COUNT; i++)
				__atomic_store_n(&g_fg_sec[i], 0, __ATOMIC_RELAXED);
			memcpy(g_fg_title, t, sizeof(g_fg_title) - 1);
			g_fg_title[sizeof(g_fg_title) - 1] = 0;
		}
		__atomic_store_n(&g_ra_fg, pid, __ATOMIC_RELEASE);
		if (pid != fg)
			ra_wake();                        /* the worker puts its settings in effect */
	} else if (ev == 3 || ev == 4) {
		SceUID exp = pid;

		if (__atomic_compare_exchange_n(&g_ra_fg, &exp, 0, 0, __ATOMIC_ACQ_REL,
		                                __ATOMIC_RELAXED)) {
			g_fg_title[0] = 0;
			ra_wake();                        /* the worker: the global settings again */
		}
	}
}

static int ra_proc_event_hook(int pid, int ev, int a3, int a4, int *a5, int a6)
{
	const tai_hook_ref_t ref = vm_hook_ref(&g_proc_ref);

	ra_proc_event(pid, ev);
	return ref ? TAI_CONTINUE(int, ref, pid, ev, a3, a4, a5, a6) : 0;
}

/* Adaptive (VM_RA_ADAPTIVE): the threshold of each kind (0 cached, 1 NC),
 * within 2..8. Read ahead's own streams are counted as the R/A Test counts
 * reads (ra_test.h, a copy of its own): runs of reads in a row, by length. Every RA_ADAPT_RUNS runs
 * of a kind it works out, as the test does, what each threshold would have
 * brought over them (a read from the pieces saves RT_GAIN_*_US a sector, a
 * piece read for nothing costs RT_WASTE_US) and takes the best one at once -
 * if it is better than the one in effect by more than RA_ADAPT_MARGIN_US, so
 * a run or two either way moves nothing. Then the counts are halved: what
 * was read long ago weighs less. Seeing every threshold, not only the one in
 * effect, it goes down as readily as up. Never off: "never" (the test's)
 * stays never. Back to where On has it when what is in effect changes
 * (eff_update: another app in front, a setting). */
#define RA_ADAPT_RUNS       32u
#define RA_ADAPT_ONE        16u    /* a run or a sector counts 16: halving keeps a rare one */
#define RA_ADAPT_MARGIN_US  ((int64_t)RA_ADAPT_ONE * 3 * RT_WASTE_US)   /* three pieces for nothing */
#define RA_THR_LO         2u
#define RA_THR_HI         8u
static volatile int      g_ra_adj[2];
static volatile uint32_t g_ra_moves[2];                  /* threshold changes (log) */
static RaTest            g_ad;                           /* the runs, as the test has them */
static uint32_t          g_ad_runs[2][RT_RUN];           /* the same, not halved (log) */
static volatile int      g_ad_lock;

/* The threshold On has for a stream of kind nc: the app in front's own, or
 * the fixed. */
static uint32_t ra_base_threshold(int nc)
{
	const SceUID app = __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE);
	uint32_t t;

	if (app && app == __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE) &&
	    (t = __atomic_load_n(&g_app_cur.thr[nc], __ATOMIC_RELAXED)) != 0)
		return t;
	return nc ? RA_STREAK_NC : RA_STREAK;
}

/* The threshold in effect for a stream of kind nc. While the R/A Test runs,
 * the standard one (5), whatever the app has or Adaptive made of it: the
 * test then sees read ahead as it is out of the box. */
static uint32_t ra_threshold(int nc)
{
	uint32_t t;
	int v;

	if (__atomic_load_n(&g_rt_on, __ATOMIC_ACQUIRE))
		return nc ? RA_STREAK_NC : RA_STREAK;
	t = ra_base_threshold(nc);
	if (t == RA_THR_NONE || LD(g_ra_mode) != VM_RA_ADAPTIVE)
		return t;
	v = (int)t + LD(g_ra_adj[nc]);
	return v < (int)RA_THR_LO ? RA_THR_LO : v > (int)RA_THR_HI ? RA_THR_HI : (uint32_t)v;
}

/* ---- calibration (VmCal) ------------------------------------------------
 * Per storage: how long a large read takes from the card in a stream before
 * read ahead began (plain) and from the pieces (served), per 128 KiB, by kind
 * (C, NC), in buckets of CAL_STEP_US - the medians' difference is the gain;
 * and for each piece read for nothing, how long it held up a request that
 * came while it was read (none came: nothing) - their mean is the cost.
 * Counted by the request holder (busy), so plainly; read by the worker for
 * the log and the overlay (relaxed). A series keeps its last CAL_KEEP or so. */
#define CAL_BUCKETS      64
#define CAL_STEP_US      250u
#define CAL_KEEP         4096u
#define CAL_HELD_MAX_US  16000u
#define CAL_FILE         VM_DATA_DIR "/calib.bin"
#define CAL_MAGIC        0x4C41434Du      /* "MCAL" */
#define CAL_VERSION      1u

typedef struct CalDev {
	uint16_t plain[2][CAL_BUCKETS], served[2][CAL_BUCKETS];
	uint32_t n_plain[2], n_served[2];
	uint32_t waste_us, waste_n;
} CalDev;

static CalDev        g_cal[VM_DEV_COUNT];
static volatile int  g_cal_on;
static volatile int  g_cal_dirty;            /* measured since the last save */
static int           g_cal_loaded;           /* worker */

/* A request of `nsec` starting (calibration on): the pieces being read now
 * are holding it up - each notes when the first such request came. Its start
 * time, if it is timed (a large read) - else 0 - and *held whether a piece was
 * being read. The clock is read only if one of the two needs it: most small
 * requests cost a look at three pieces. */
static uint32_t cal_start(Dev *d, uint32_t nsec, int *held)
{
	int reading = 0;
	uint32_t now;

	if (__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE))
		for (int i = 0; i < RA_PIECES; i++)
			reading |= (__atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) == RA_READING) << i;
	*held = reading != 0;
	if (!reading && nsec < RA_MIN_SECTORS)
		return 0;
	now = ksceKernelGetSystemTimeLow() | 1u;
	for (int i = 0; i < RA_PIECES; i++)
		if (reading & (1 << i)) {
			uint32_t exp = 0;

			__atomic_compare_exchange_n(&d->ra[i].wait_from, &exp, now, 0,
			                            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
		}
	return nsec >= RA_MIN_SECTORS ? now : 0;
}

static void cal_add(uint16_t *h, uint32_t *n, uint32_t us)
{
	const uint32_t b = us / CAL_STEP_US < CAL_BUCKETS ? us / CAL_STEP_US : CAL_BUCKETS - 1;
	uint32_t total;

	ST(h[b], (uint16_t)(LD(h[b]) + 1));
	total = LD(*n) + 1;
	if (total >= CAL_KEEP) {              /* the older half goes */
		total = 0;
		for (int i = 0; i < CAL_BUCKETS; i++) {
			ST(h[i], (uint16_t)(LD(h[i]) >> 1));
			total += LD(h[i]);
		}
	}
	ST(*n, total);
	ST(g_cal_dirty, 1);
}

/* A large read done (its stream noted: ra_note), `got` sectors of it from the
 * pieces, started at t0 (0: calibration off). Only whole ones count: all from
 * the pieces, or none and in a stream, with no piece being read meanwhile. */
static inline void cal_read(Dev *d, int nc, uint32_t nsec, uint32_t got, uint32_t t0, int held)
{
	CalDev *c;
	uint32_t us;

	if (!t0 || nsec < RA_MIN_SECTORS)
		return;
	nc = !!nc;
	if (got != nsec && (got || held || d->ra_streak < 2))
		return;
	us = (uint32_t)((uint64_t)(ksceKernelGetSystemTimeLow() - t0) * 256u / nsec);
	c = &g_cal[d - g_dev];
	if (got)
		cal_add(c->served[nc], &c->n_served[nc], us);
	else
		cal_add(c->plain[nc], &c->n_plain[nc], us);
}

/* A piece read for nothing: what it held up. */
static void cal_waste(Dev *d, const RaPiece *pc)
{
	CalDev *c = &g_cal[d - g_dev];
	uint32_t n;

	if (!LD(g_cal_on))
		return;
	ST(c->waste_us, LD(c->waste_us) + pc->held_us);
	n = LD(c->waste_n) + 1;
	if (n >= CAL_KEEP) {
		ST(c->waste_us, LD(c->waste_us) >> 1);
		n >>= 1;
	}
	ST(c->waste_n, n);
	ST(g_cal_dirty, 1);
}

/* A series' median, us per 128 KiB (the middle of its bucket). */
static uint32_t cal_median(const uint16_t *h, uint32_t n)
{
	uint32_t cum = 0;

	for (int i = 0; i < CAL_BUCKETS; i++) {
		cum += LD(h[i]);
		if (cum * 2 >= n)
			return (uint32_t)i * CAL_STEP_US + CAL_STEP_US / 2;
	}
	return CAL_BUCKETS * CAL_STEP_US;
}

/* A series' median for the log, "-" if it has none. */
static void cal_text(char *o, size_t n, const uint16_t *h, uint32_t count)
{
	if (count)
		snprintf(o, n, "%u", (unsigned)(cal_median(h, count) % 100000u));
	else
		snprintf(o, n, "-");
}

/* What is in use for storage dev (-1: the fixed figures), and the counts. */
static void cal_cost(int dev, RtCost *cost, VmCal *o)
{
	VmCal v;

	memset(&v, 0, sizeof(v));
	*cost = rt_cost_fixed;
	v.on = LD(g_cal_on);
	if (dev >= 0 && dev < VM_DEV_COUNT) {
		const CalDev *c = &g_cal[dev];

		for (int k = 0; k < 2; k++) {
			v.n_plain[k] = LD(c->n_plain[k]);
			v.n_served[k] = LD(c->n_served[k]);
			if (v.n_plain[k] >= VM_CAL_MIN_PLAIN && v.n_served[k] >= VM_CAL_MIN_SERVED) {
				const int32_t diff = (int32_t)cal_median(c->plain[k], v.n_plain[k]) -
				                     (int32_t)cal_median(c->served[k], v.n_served[k]);

				/* per 128 KiB -> per sector, sixteenths: x 16 / 256 */
				cost->gain16[k] = diff > 0 ? diff / 16 : 0;
				v.have[k] = 1;
			}
		}
		v.n_waste = LD(c->waste_n);
		if (v.n_waste >= VM_CAL_MIN_WASTE) {
			cost->waste_us = (int32_t)(LD(c->waste_us) / v.n_waste);
			v.have_waste = 1;
		}
	}
	v.gain16[0] = cost->gain16[0];
	v.gain16[1] = cost->gain16[1];
	v.waste_us = cost->waste_us;
	if (o)
		*o = v;
}

void vm_stage_cal(int dev, VmCal *out)
{
	RtCost c;

	cal_cost(dev, &c, out);
}

/* The file: a header, each storage's counts, a check over all of it. Written
 * aside, then swapped in, as the profiles are; read straight into g_cal and
 * cleared again if anything is wrong. Worker thread. */
typedef struct CalHead {
	uint32_t magic, version, on, devs;
} CalHead;

static uint32_t cal_fnv(uint32_t c, const void *p, size_t bytes)
{
	const uint32_t *w = p;

	for (size_t i = 0; i < bytes / 4; i++)
		c = (c ^ w[i]) * 16777619u;
	return c;
}

/* What the file holds, per storage (as last saved or loaded): the counts
 * and the figures in use then. A save is worth its write only if what is in
 * use would change (internal storage: written as little as can be). */
typedef struct CalSaved {
	uint32_t n_plain[2], n_served[2], n_waste;
	RtCost   cost;
	int      have[2], have_waste;
} CalSaved;
static CalSaved g_cal_saved[VM_DEV_COUNT];   /* worker */

static void cal_snapshot(void)
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		CalSaved *o = &g_cal_saved[i];
		VmCal v;

		cal_cost(i, &o->cost, &v);
		for (int k = 0; k < 2; k++) {
			o->n_plain[k] = v.n_plain[k];
			o->n_served[k] = v.n_served[k];
			o->have[k] = v.have[k];
		}
		o->n_waste = v.n_waste;
		o->have_waste = v.have_waste;
	}
}

/* `now` more than 5 % off `then` (and more than a sixteenth of a us apart). */
static int cal_moved(int32_t now, int32_t then)
{
	const int32_t d = now > then ? now - then : then - now;

	return d > 1 && d * 20 > (then > 0 ? then : 1);
}

/* A count still short of what is used, grown by half since the save (and by
 * 8 or more): the way there kept, in few writes. */
static int cal_grown(uint32_t now, uint32_t then, uint32_t min)
{
	return now < min + min / 2 && now >= then + 8 && now >= then + then / 2;
}

/* Whether what was measured since the last save is worth writing: a figure
 * in use now (or no longer) that was not then, one in use moved by more than
 * 5 %, or, while too little is measured, a count grown by half. Once the
 * figures settle, nothing more is written. */
static int cal_worth_saving(void)
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const CalSaved *o = &g_cal_saved[i];
		RtCost c;
		VmCal v;

		cal_cost(i, &c, &v);
		for (int k = 0; k < 2; k++) {
			if (v.have[k] != o->have[k] ||
			    (v.have[k] && cal_moved(c.gain16[k], o->cost.gain16[k])) ||
			    (!v.have[k] && (cal_grown(v.n_plain[k], o->n_plain[k], VM_CAL_MIN_PLAIN) ||
			                    cal_grown(v.n_served[k], o->n_served[k], VM_CAL_MIN_SERVED))))
				return 1;
		}
		if (v.have_waste != o->have_waste ||
		    (v.have_waste && cal_moved(c.waste_us, o->cost.waste_us)) ||
		    (!v.have_waste && cal_grown(v.n_waste, o->n_waste, VM_CAL_MIN_WASTE)))
			return 1;
	}
	return 0;
}

static void cal_save(void)
{
	CalHead h = { CAL_MAGIC, CAL_VERSION, 0, VM_DEV_COUNT };
	uint32_t c = 0x811C9DC5u;
	SceUID fd;
	int ok;

	if (!g_cal_loaded)
		return;
	ST(g_cal_dirty, 0);
	h.on = (uint32_t)LD(g_cal_on);
	ksceIoMkdir("ur0:data", 0777);
	ksceIoMkdir(VM_DATA_DIR, 0777);
	fd = ksceIoOpen(CAL_FILE ".tmp", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return;
	ok = ksceIoWrite(fd, &h, sizeof(h)) == (int)sizeof(h);
	c = cal_fnv(c, &h, sizeof(h));
	for (int i = 0; i < VM_DEV_COUNT && ok; i++) {
		CalDev one;                       /* as it is now, the check over the same */

		for (int k = 0; k < 2; k++) {
			for (int b = 0; b < CAL_BUCKETS; b++) {
				one.plain[k][b] = LD(g_cal[i].plain[k][b]);
				one.served[k][b] = LD(g_cal[i].served[k][b]);
			}
			one.n_plain[k] = LD(g_cal[i].n_plain[k]);
			one.n_served[k] = LD(g_cal[i].n_served[k]);
		}
		one.waste_us = LD(g_cal[i].waste_us);
		one.waste_n = LD(g_cal[i].waste_n);
		ok = ksceIoWrite(fd, &one, sizeof(one)) == (int)sizeof(one);
		c = cal_fnv(c, &one, sizeof(one));
	}
	c ^= 0xA5C3A5C3u;
	ok = ok && ksceIoWrite(fd, &c, sizeof(c)) == (int)sizeof(c);
	ksceIoClose(fd);
	if (ok) {
		ksceIoRemove(CAL_FILE);
		ksceIoRename(CAL_FILE ".tmp", CAL_FILE);
		cal_snapshot();                   /* what the file holds now */
	} else {
		ksceIoRemove(CAL_FILE ".tmp");
	}
}

/* One file into g_cal; 1 if it was whole and right (else g_cal cleared). */
static int cal_read_file(const char *path)
{
	const SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	CalHead h;
	uint32_t c = 0x811C9DC5u, check = 0;
	int ok;

	if (fd < 0)
		return 0;
	ok = ksceIoRead(fd, &h, sizeof(h)) == (int)sizeof(h) && h.magic == CAL_MAGIC &&
	     h.version == CAL_VERSION && h.devs == VM_DEV_COUNT &&
	     ksceIoRead(fd, g_cal, sizeof(g_cal)) == (int)sizeof(g_cal) &&
	     ksceIoRead(fd, &check, sizeof(check)) == (int)sizeof(check);
	ksceIoClose(fd);
	if (ok) {
		c = cal_fnv(cal_fnv(c, &h, sizeof(h)), g_cal, sizeof(g_cal)) ^ 0xA5C3A5C3u;
		ok = c == check;
		for (int i = 0; i < VM_DEV_COUNT && ok; i++)
			for (int k = 0; k < 2; k++)
				if (g_cal[i].n_plain[k] >= CAL_KEEP || g_cal[i].n_served[k] >= CAL_KEEP)
					ok = 0;
	}
	if (!ok) {
		memset(g_cal, 0, sizeof(g_cal));
		return 0;
	}
	ST(g_cal_on, h.on ? 1 : 0);
	return 1;
}

/* Once, by the worker: what was measured before (the copy a save cut short,
 * if the file is not whole). */
static void cal_load(void)
{
	if (g_cal_loaded)
		return;
	g_cal_loaded = 1;
	if (!cal_read_file(CAL_FILE))
		cal_read_file(CAL_FILE ".tmp");
	cal_snapshot();                       /* what the file holds (or nothing) */
}

void vm_stage_cal_set(int on)
{
	cal_load();
	ST(g_cal_on, on ? 1 : 0);
	cal_save();                           /* the switch kept, and what was measured */
	VM_KLOGI("stage: calibration %s", on ? "on" : "off");
}

static void ra_adapt_reset(void)
{
	while (__sync_lock_test_and_set(&g_ad_lock, 1))
		ksceKernelDelayThread(10);
	memset(&g_ad, 0, sizeof(g_ad));
	memset(g_ad_runs, 0, sizeof(g_ad_runs));
	for (int k = 0; k < 2; k++)
		ST(g_ra_adj[k], 0);
	__sync_lock_release(&g_ad_lock);
}

/* What read ahead's own streams did, for Adaptive: counted where read ahead
 * counts them (ra_note), so both see the same runs. Only while Adaptive is in
 * effect and the R/A Test is not running (another thread at it: lost). */
static int ra_adapt_on(void)
{
	return LD(g_ra_mode) == VM_RA_ADAPTIVE && !__atomic_load_n(&g_rt_on, __ATOMIC_ACQUIRE);
}

/* A read of `n` sectors, the `at`-th in its stream (`small`: 32 KiB reads). */
static void ra_adapt_read(int small, int nc, uint32_t at, uint32_t n)
{
	if (!ra_adapt_on() || __sync_lock_test_and_set(&g_ad_lock, 1))
		return;
	nc = !!nc;
	if (small) {
		const uint32_t b = (at - 1) / 4;

		g_ad.spos[nc][b < RT_POS ? b : RT_POS - 1] += n * RA_ADAPT_ONE;
	} else {
		const uint32_t have = RT_PIECES * RT_PIECE_SECTORS;

		g_ad.pos[nc][at < RT_POS ? at : RT_POS - 1] += (n < have ? n : have) * RA_ADAPT_ONE;
	}
	g_ad.total_sectors += n;
	__sync_lock_release(&g_ad_lock);
}

/* A stream of `len` reads has ended; every RA_ADAPT_RUNS of a kind, its
 * threshold weighed again. */
static void ra_adapt_end(const Dev *d, int small, int nc, uint32_t len)
{
	if (!len || !ra_adapt_on() || __sync_lock_test_and_set(&g_ad_lock, 1))
		return;
	nc = !!nc;
	if (small) {
		const uint32_t b = len / 4;

		g_ad.srun[nc][b < RT_RUN ? b : RT_RUN - 1] += RA_ADAPT_ONE;
	} else {
		g_ad.run[nc][len < RT_RUN ? len : RT_RUN - 1] += RA_ADAPT_ONE;
		g_ad_runs[nc][len < RT_RUN ? len : RT_RUN - 1]++;
	}
	if (++g_ad.ended[nc] >= RA_ADAPT_RUNS) {
		const uint32_t base = ra_base_threshold(nc), t = ra_threshold(nc);
		int pick;

		RtCost cost;

		cal_cost((int)(d - g_dev), &cost, NULL);   /* this storage's, if measured */
		if (t != RA_THR_NONE &&
		    (pick = ra_adapt_pick(&g_ad, nc, (int)t, RA_ADAPT_MARGIN_US, &cost)) != (int)t) {
			ST(g_ra_adj[nc], pick - (int)base);
			ADD(g_ra_moves[nc], 1);
		}
		ra_adapt_age(&g_ad, nc);
	}
	__sync_lock_release(&g_ad_lock);
}

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
		__atomic_store_n(&pc->wait_from, 0, __ATOMIC_RELAXED);   /* calibration */
		/* seq_cst, as the read of ra_wbusy below and a write's own steps */
		if (!__atomic_compare_exchange_n(&pc->state, &exp, RA_READING, 0, __ATOMIC_SEQ_CST,
		                                 __ATOMIC_RELAXED))
			continue;
		wr = __atomic_load_n(&d->ra_wbusy, __ATOMIC_SEQ_CST);   /* after READING */
		r = pc->io(pc->ref, pc->part, LD(pc->sector), ra_ptr(d, best), LD(pc->n));
		pc->ok = r >= 0 && !wr;
		pc->t_done = ksceKernelGetSystemTimeLow();
		{                                   /* calibration: what it held up */
			const uint32_t from = __atomic_exchange_n(&pc->wait_from, 0, __ATOMIC_RELAXED);
			const uint32_t held = from ? pc->t_done - from : 0;

			pc->held_us = held < CAL_HELD_MAX_US ? held : CAL_HELD_MAX_US;
		}
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
			if (st == RA_READY) {
				waste += d->ra[i].n - d->ra[i].off;
				if (!d->ra[i].off)
					cal_waste(d, &d->ra[i]);   /* none of it used */
			}
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
		const int mt = om->first != -2 ? om->first : ksceKernelGetMemBlockMemtypeByAddr(om->user);
		const int end = ksceKernelGetMemBlockMemtypeByAddr(om->user + om->len - 1);

		om->mt = mt >= 0 && end == mt ? mt : -1;
		/* its first page in no block (the system has none there), the rest
		 * in one: mapped from the next page, that page the caller's */
		if (mt < 0 && end >= 0) {
			const uint32_t head = OFF_PAGE - ((uint32_t)(uintptr_t)om->user & (OFF_PAGE - 1));

			if (head < om->len && ksceKernelGetMemBlockMemtypeByAddr(om->user + head) == end) {
				om->head = head;
				om->mt = end;
			}
		}
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
			const RaPiece *const pc = &d->ra[idx[j]];
			const int why = LD(pc->state) != RA_READY ? 0 : !pc->ok ? 1 : LD(pc->dirty) ? 2 :
			                pc->epoch != LD(d->ra_epoch) ? 3 : 4;

			ADD(g_ra_bad_why[why], 1);
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
			copy_out(p + (size_t)at * 512, src[j], sz[j] * 512, clean);
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

/* A read of 32 KiB: its stream (see RA_SMALL_SECTORS). */
static void ra_note_small(Dev *d, void *part, uint32_t sector, uint32_t n, int nc)
{
	if (__sync_lock_test_and_set(&d->rs_lock, 1))
		return;                               /* another thread at it: a lost update */
	{
		const int ready = __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE);
		const uint32_t now = ready ? ksceKernelGetSystemTimeLow() : 0;

		if (part == d->rs_part && sector == d->rs_end && d->rs_streak && nc == d->rs_nc &&
		    (!ready || now - d->rs_us < RA_MAX_AGE_US)) {
			d->rs_streak += d->rs_streak < 1000;
		} else {
			ra_adapt_end(d, 1, d->rs_nc, d->rs_streak);
			d->rs_streak = 1;
		}
		d->rs_us = now;
		ra_adapt_read(1, nc, d->rs_streak, n);
	}
	d->rs_nc = nc;
	d->rs_part = part;
	d->rs_end = sector + n;
	__sync_lock_release(&d->rs_lock);
}

/* The same for one that went the standard way: a stream that lost such a read
 * would fall apart into pieces. */
static inline void ra_note_small_stock(Dev *d, void *part, uint32_t sector, uint32_t n, int nc, int ret)
{
	if (ret >= 0 && n >= RA_SMALL_SECTORS && n < RA_MIN_SECTORS &&
	    __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE))
		ra_note_small(d, part, sector, n, nc);
}

/* A read [sector, sector + n) has completed: the stream so far. 1 if it
 * counts (then the pieces may be refilled after it). */
static int ra_note(Dev *d, void *part, uint32_t sector, uint32_t n, int nc)
{
	d->ra_noted_small = 0;
	if (n < RA_SMALL_SECTORS)
		return 0;                           /* neither part of a stream nor a break */
	if (n < RA_MIN_SECTORS) {
		ra_note_small(d, part, sector, n, nc);
		d->ra_noted_small = 1;
	} else {
		const int ready = __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE);
		const uint32_t now = ready ? ksceKernelGetSystemTimeLow() : 0;

		if (part == d->ra_last_part && sector == d->ra_last_end && d->ra_streak && nc == d->ra_nc &&
		    (!ready || now - d->ra_us < RA_MAX_AGE_US)) {
			d->ra_streak += d->ra_streak < 1000;
		} else {
			ra_adapt_end(d, 0, d->ra_nc, d->ra_streak);
			d->ra_streak = 1;
		}
		ra_adapt_read(0, nc, d->ra_streak, n);
		d->ra_us = now;
		d->ra_nc = nc;
		d->ra_last_part = part;
		d->ra_last_end = sector + n;
	}
	if (__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE))     /* no clock read otherwise */
		__atomic_store_n(&d->ra_last_us, ksceKernelGetSystemTimeLow(), __ATOMIC_RELAXED);
	return 1;
}

/* Queue the free pieces after the stream (after the last piece queued, or
 * where the stream is). */
static void ra_refill(Dev *d, io_fn io, tai_hook_ref_t ref, void *part)
{
	const int small = d->ra_noted_small;
	const int nc = small ? d->rs_nc : d->ra_nc;
	const uint32_t streak = small ? d->rs_streak : d->ra_streak;
	uint32_t thr = ra_threshold(nc), next = small ? d->rs_end : d->ra_last_end;
	int want, live = 0;

	if (small && thr != RA_THR_NONE) {
		const uint32_t own = thr == (nc ? RA_STREAK_NC : RA_STREAK) ? RA_SMALL_STREAK
		                                                           : thr * RA_SMALL_FACTOR;

		thr = own < RA_SMALL_STREAK ? RA_SMALL_STREAK : own;
	}
	if (streak < thr)
		return;
	/* the pieces are the large reads' stream's while it has any */
	if (small && !LD(d->ra_small) && ra_any(d))
		return;
	/* step by step (a stream may end at any time) */
	{
		const uint32_t ahead = (streak - thr) / (small ? RA_SMALL_FACTOR : 1) + 1;

		want = ahead < RA_PIECES ? (int)ahead : RA_PIECES;
	}
	ST(d->ra_small, small);
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
		pc->nc = nc;
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
	if (d->t_uid >= 0)
		ksceKernelFreeMemBlock(d->t_uid);
	d->t_uid = -1;
	d->t_buf = NULL;
	ST(d->t_end, LD(d->t_start));
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
	/* the tiny reads' block: without it they read as before */
	if (alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, RA_TINY_SECTORS * 512, &d->t_uid, &d->t_buf) < 0) {
		d->t_uid = -1;
		d->t_buf = NULL;
	}
	/* on the system core, as the helper: the card goes on at once */
	d->ra_th = ksceKernelCreateThread("SceVitaMemioAhead", ra_thread, 0x40, VM_STACK_DEV, 0,
	                                  PIPE_CPU_SYSTEM, NULL);
	if (d->ra_th < 0)
		d->ra_th = ksceKernelCreateThread("SceVitaMemioAhead", ra_thread, 0x40, VM_STACK_DEV, 0, 0,
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
				if (dma_state() == VM_DMA_ON)   /* it may be the DMA copy's */
					dma_stop(VM_DMA_WHY_MISMATCH);
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

/* A tiny read while read ahead is on. 1 if it is done here (*ret: its result),
 * else the caller goes on as usual. */
static int ra_tiny_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                        uint8_t *p, uint32_t n, int *ret)
{
	const int clean = LD(g_gpu_safe) && g_dcache_clean;
	int wr;
	void *shadow;

	if ((LD(g_gpu_safe) && !g_dcache_clean) || __sync_lock_test_and_set(&d->busy, 1))
		return 0;                             /* (no cache clean: the standard path) */
	__sync_synchronize();                     /* pairs with ra_retire's fence */
	if (!__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) || !d->t_buf || !LD(d->enabled) ||
	    LD(d->fallback)) {
		__sync_lock_release(&d->busy);
		return 0;
	}
	d->t_streak = sector == d->t_next && part == d->t_part ? d->t_streak + (d->t_streak < 1000) : 1;
	d->t_next = sector + n;
	if (!(LD(d->t_end) != LD(d->t_start) && sector >= LD(d->t_start) && sector + n <= LD(d->t_end) &&
	      part == d->t_part && io == d->t_io && d->t_epoch == LD(d->ra_epoch) &&
	      !__atomic_load_n(&d->t_dirty, __ATOMIC_SEQ_CST) &&
	      ksceKernelGetSystemTimeLow() - d->t_us < RA_MAX_AGE_US)) {   /* last: only with a block */
		/* not in it: the block is of no use now */
		ST(d->t_end, LD(d->t_start));
		d->t_part = part;
		if (d->t_streak < RA_TINY_STREAK || sector > 0xFFFFFFFFu - RA_TINY_SECTORS) {
			__sync_lock_release(&d->busy);
			return 0;
		}
		/* as a piece: the range before the look at ra_wbusy, both in one order
		 * with a write's own steps */
		__atomic_store_n(&d->t_dirty, 0, __ATOMIC_SEQ_CST);
		ST(d->t_start, sector);
		__atomic_store_n(&d->t_end, sector + RA_TINY_SECTORS, __ATOMIC_SEQ_CST);
		wr = __atomic_load_n(&d->ra_wbusy, __ATOMIC_SEQ_CST);
		if (wr || io(ref, part, sector, d->t_buf, RA_TINY_SECTORS) < 0) {
			/* a write now, or the card's end: this read the standard way */
			ST(d->t_end, sector);
			__sync_lock_release(&d->busy);
			return 0;
		}
		d->t_io = io;
		d->t_epoch = LD(d->ra_epoch);
		d->t_us = ksceKernelGetSystemTimeLow();
	}
	copy_out(p, (uint8_t *)d->t_buf + (size_t)(sector - LD(d->t_start)) * 512, n * 512, clean);
	if (!clean)
		vm_dsb();
	ADD(g_ra_tiny_reads, 1);
	*ret = 0;
	shadow = take_shadow();                   /* verify mode: read again, compared */
	if (shadow) {
		if (ra_verify(d, io, ref, part, sector, p, n, clean, shadow))
			ST(d->t_end, LD(d->t_start));
		__sync_lock_release(&g_shadow_busy);
	}
	__sync_lock_release(&d->busy);
	if (LD(d->fallback)) {
		void (*const fn)(void) = __atomic_load_n(&g_on_problem, __ATOMIC_ACQUIRE);

		if (fn)
			fn();
	}
	return 1;
}

/* A read into non-cacheable memory (64 KiB and up) while read ahead is on:
 * from the pieces where they continue the stream (copied, by the copier as
 * the setting says; no cache to clean), the rest the standard way straight
 * into the buffer as before; then the stream noted and the pieces refilled.
 * The standard read's result is the caller's, as without read ahead. */
static uint32_t g_ra_nc_kib;              /* atomic: for the log */
static int ncd_job(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                   uint8_t *p, uint32_t nsec, SceUID pid, int *ret);
static SceUID ncd_wanted(uint32_t nsec);
static int ra_nc_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                      uint8_t *p, uint32_t nsec, uint32_t cal_t0, int cal_held)
{
	/* the caller found it non-cacheable at both ends */
	OffMap om = { .uid = -1, .user = p, .len = nsec * 512, .first = -2, .mt = MEMTYPE_NC, .nc = 1 };
	uint32_t got = 0, own = 0;
	int ret = 0, stats, off, offload, bad = 0;
	void *shadow;

	if (!take(d, 1, nsec)) {
		ADD(g_nc_stock_kib, nsec / 2);
		ret = io(ref, part, sector, p, nsec);
		ra_note_small_stock(d, part, sector, nsec, 1, ret);
		return ret;
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
	      vm_offload_nc(offload) && (om.pid = ksceKernelGetProcessId()) != KERNEL_PID;
	/* a 32 KiB read leaves the large reads' pieces alone: it goes straight
	 * into its own buffer, they are not in its way */
	if (ra_any(d) && (nsec >= RA_MIN_SECTORS || LD(d->ra_small))) {
		got = ra_serve(d, part, io, sector, p, nsec, 0, stats, off, &om, nsec, &own);
		if (got < nsec)
			ra_drop(d);                         /* elsewhere, or used up */
	}
	if (om.uid >= 0)
		ksceKernelUserUnmap(om.uid);              /* the copier is done with it */
	if (got && shadow)
		bad = ra_verify(d, io, ref, part, sector, p, got, 0, shadow);
	if (got < nsec) {
		/* the rest: "C + NC" on the system core, else the standard way */
		const SceUID pid = shadow ? KERNEL_PID : ncd_wanted(nsec - got);

		if (pid == KERNEL_PID ||
		    !ncd_job(d, io, ref, part, sector + got, p + (size_t)got * 512, nsec - got, pid,
		             &ret)) {
			ret = io(ref, part, sector + got, p + (size_t)got * 512, nsec - got);
			ADD(g_nc_stock_kib, (nsec - got) / 2);
		}
	}
	if (shadow)
		__sync_lock_release(&g_shadow_busy);
	if (got)
		ADD(g_ra_nc_kib, got / 2);
	if (ret >= 0 && !bad && ra_note(d, part, sector, nsec, 1)) {
		cal_read(d, 1, nsec, got, cal_t0, cal_held);   /* before the refill: it may start a piece */
		ra_refill(d, io, ref, part);
	}
	if (COUNT_CORES() && own)                  /* graphics data the game's thread copied */
		__atomic_fetch_add(&g_ra_nc_own_kib, own / 2, __ATOMIC_RELAXED);
	__sync_lock_release(&d->busy);
	if (bad) {
		void (*const fn)(void) = __atomic_load_n(&g_on_problem, __ATOMIC_ACQUIRE);

		if (fn)
			fn();
	}
	return ret;
}

/* Map the game's non-cacheable buffer for the helper (see
 * g_ncd_state). The mapping is used only if it is non-cacheable at both ends,
 * as the buffer is, and writable. */
static void ncd_map(OffMap *m)
{
	void *page = NULL;
	SceSize size = 0;
	SceUInt32 off = 0;
	SceUID uid;
	VmMemBlockInfoEx info;
	uint8_t *kern;
	int r;

	if (ksceKernelGetMemBlockMemtypeByAddr(m->user) != MEMTYPE_NC ||
	    ksceKernelGetMemBlockMemtypeByAddr(m->user + m->len - 1) != MEMTYPE_NC)
		return;                           /* device memory: this one stays standard */
	uid = ksceKernelProcUserMap(m->pid, "SceVitaMemioDirect", VM_USERMAP_RW, m->user, m->len,
	                            &page, &size, &off);
	if (uid < 0) {
		ADD(g_ncd_maperr, 1);
		ST(g_ncd_err, uid);
		mapfail_note(1, m, uid);
		return;
	}
	if (!page || size < off || size - off < m->len) {
		ksceKernelUserUnmap(uid);
		ADD(g_ncd_maperr, 1);
		ST(g_ncd_err, VM_ERR_NOMEM);
		return;
	}
	kern = (uint8_t *)page + off;
	if (ksceKernelGetMemBlockMemtypeByAddr(kern) != MEMTYPE_NC ||
	    ksceKernelGetMemBlockMemtypeByAddr(kern + m->len - 1) != MEMTYPE_NC) {
		/* the mapping is not non-cacheable: the driver would clean a cache
		 * for it - refused until the setting is switched again */
		ST(g_ncd_type, (uint32_t)ksceKernelGetMemBlockMemtypeByAddr(kern));
		ST(g_ncd_err, -2);
		__atomic_store_n(&g_ncd_state, -2, __ATOMIC_RELEASE);
		ksceKernelUserUnmap(uid);
		return;
	}
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	r = ksceKernelMemBlockGetInfoEx(uid, &info);
	if (r < 0 || (int)(info.type & 0xFF00) != MEMTYPE_NC || (info.type & 0x6) != 0x6) {
		ST(g_ncd_type, info.type);
		ST(g_ncd_err, r < 0 ? r : -1);
		__atomic_store_n(&g_ncd_state, -1, __ATOMIC_RELEASE);
		ksceKernelUserUnmap(uid);
		return;
	}
	{
		int zero = 0;                       /* first usable one: for the log */

		if (__atomic_compare_exchange_n(&g_ncd_state, &zero, 1, 0, __ATOMIC_ACQ_REL,
		                                __ATOMIC_RELAXED))
			ST(g_ncd_type, info.type);
	}
	if (__atomic_load_n(&g_ncd_state, __ATOMIC_ACQUIRE) < 0) {
		ksceKernelUserUnmap(uid);           /* refused meanwhile (another request) */
		return;
	}
	m->uid = uid;
	m->kern = kern;
}

/* A game's read into non-cacheable memory, by the helper straight
 * into the game's buffer, with d->busy already held. 1 if done here (*ret:
 * its result), 0 if it stays standard (no helper, no usable mapping). On a
 * failed read the whole request is read again the standard way, so the
 * caller gets what the system gives. */
static int ncd_job(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                   uint8_t *p, uint32_t nsec, SceUID pid, int *ret)
{
	OffMap m = { .pid = pid, .uid = -1, .user = p, .len = nsec * 512 };
	uint32_t f;

	if (!__atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE))
		return 0;
	ncd_map(&m);
	if (m.uid < 0)
		return 0;
	d->job_io = io;
	d->job_ref = ref;
	d->job_part = part;
	d->job_k = 1;
	d->job_sz[0] = nsec;
	d->job_sz[1] = d->job_sz[2] = 0;
	d->job_first = 0;
	d->job_dst = m.kern;
	d->job_sector = sector;
	d->job_ret = 0;
	__atomic_store_n(&d->job_fail, 0, __ATOMIC_RELAXED);
	__sync_synchronize();
	ksceKernelSignalSema(d->sem_go, 1);
	ksceKernelWaitSema(d->sem_done, 1, NULL);
	__sync_synchronize();
	f = __atomic_load_n(&d->job_fail, __ATOMIC_ACQUIRE);
	d->job_dst = NULL;
	ksceKernelUserUnmap(m.uid);
	if (f) {
		ADD(g_ncd_fail, 1);
		*ret = io(ref, part, sector, p, nsec);    /* the standard way, all of it */
		return 1;
	}
	ADD(g_ncd_reads, 1);
	ADD(g_ncd_kib, nsec / 2);
	*ret = 0;
	return 1;
}

/* Whether "C + NC" takes an n-sector read: the process to map for, or
 * KERNEL_PID if not. */
static SceUID ncd_wanted(uint32_t nsec)
{
	if (!vm_offload_direct(LD(g_offload)) || LD(g_verify) || nsec < NCD_MIN_SECTORS ||
	    __atomic_load_n(&g_ncd_state, __ATOMIC_ACQUIRE) < 0)
		return KERNEL_PID;
	return ksceKernelGetProcessId();
}

/* ncd_job, taking the device first (0 if busy: the read stays standard). */
static int ncd_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                    uint8_t *p, uint32_t nsec, SceUID pid, int *ret)
{
	int done;

	if (__sync_lock_test_and_set(&d->busy, 1)) {
		ADD(g_ncd_busy, 1);
		return 0;
	}
	__sync_synchronize();                 /* pairs with pipe_retire's fence */
	if (!LD(d->enabled) || LD(d->fallback)) {
		__sync_lock_release(&d->busy);
		return 0;
	}
	done = ncd_job(d, io, ref, part, sector, p, nsec, pid, ret);
	__sync_lock_release(&d->busy);
	return done;
}

/* The test's look at a transfer (a moment; another thread at it: this one lost). */
static void rt_note(const Dev *d, void *part, uint32_t sector, int nc, uint32_t nsec, int write)
{
	if (d == &g_dev[VM_DEV_INTERNAL] ||
	    __atomic_load_n(&g_ra_fg, __ATOMIC_RELAXED) != __atomic_load_n(&g_rt_pid, __ATOMIC_RELAXED))
		return;
	if (__sync_lock_test_and_set(&g_rt_lock, 1))
		return;
	ra_test_note(&g_rt, part ? part : (const void *)d, sector, nsec, nc, write,
	             ksceKernelGetSystemTimeLow());
	__sync_lock_release(&g_rt_lock);
}

/* One request, either way. `rd_ref` is the read function, used by a write's
 * verify re-read (a read verifies with `ref` itself). */
static int stage_xfer_one(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                          void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	uint8_t *p = buf;
	uint32_t left = nsec;
	int verify, ret = 0, mine = on_port(part, d->port);
	int mt0 = -2, mt1 = -2;               /* the buffer's memory type at its ends */
	int cal_held = 0;
	/* calibration on: when it started, and whether a piece held it up */
	const uint32_t cal_t0 = mine && nsec && LD(g_cal_on) ? cal_start(d, nsec, &cal_held) : 0;

	/* the largest read request seen, for the log (a lost update is fine) */
	if (mine && !write && nsec > __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED))
		__atomic_store_n(&g_rd_max, nsec, __ATOMIC_RELAXED);

	if (mine && LD(g_count_all)) {
		if (write) {
			ADD(d->all_wr, (uint64_t)nsec * 512);
		} else {
			ADD(d->all_rd, (uint64_t)nsec * 512);
			/* the app in front's reads, per storage (the HUD shows its own) */
			if (nsec >= RA_MIN_SECTORS && __atomic_load_n(&g_ra_fg, __ATOMIC_RELAXED))
				__atomic_fetch_add(&g_fg_sec[d - g_dev], nsec, __ATOMIC_RELAXED);
		}
	}
	/* the R/A Test's look at it: the buffer's memory type looked up once,
	 * kept for the non-cacheable check below */
	if (mine && nsec && __atomic_load_n(&g_rt_on, __ATOMIC_RELAXED)) {
		const int nc = !write && nsec >= RA_SMALL_SECTORS &&
		               (mt0 = ksceKernelGetMemBlockMemtypeByAddr(p)) == MEMTYPE_NC;

		rt_note(d, part, sector, nc, nsec, write);
	}
	/* Tiny reads: from their own block, if it holds them */
	if (mine && !write && nsec && nsec < RA_SMALL_SECTORS &&
	    __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) && ra_tiny_read(d, io, ref, part, sector, p, nsec, &ret))
		return ret;
	/* A non-cacheable caller buffer: the standard path is the faster one
	 * (see the top). Looked at only for transfers that would be staged. */
	if (mine && LD(d->enabled) && !LD(d->fallback) && __atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) &&
	    nsec && nsec >= LD(g_min_sectors) &&
	    mt_uncached(mt0 = mt0 != -2 ? mt0 : ksceKernelGetMemBlockMemtypeByAddr(p)) &&
	    mt_uncached(mt1 = ksceKernelGetMemBlockMemtypeByAddr(p + nsec * 512 - 1))) {
		SceUID pid;
		int r;

		/* ... unless read ahead holds it: a stream into non-cacheable
		 * memory, as games load their graphics data (what the pieces do not
		 * cover goes to the system core there too, with "C + NC") */
		if (!write && nsec >= RA_SMALL_SECTORS && __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) &&
		    mt0 == MEMTYPE_NC && mt1 == MEMTYPE_NC)
			return ra_nc_read(d, io, ref, part, sector, p, nsec, cal_t0, cal_held);
		/* "C + NC": read on the system core instead */
		if (!write && (pid = ncd_wanted(nsec)) != KERNEL_PID &&
		    ncd_read(d, io, ref, part, sector, p, nsec, pid, &r))
			return r;
		ADD(g_nc_stock_kib, nsec / 2);
		return io(ref, part, sector, buf, nsec);
	}
	/* Read ahead keeps pieces in the staging buffers: a small transfer goes
	 * the standard way meanwhile, instead of dropping them (a read elsewhere
	 * waits for the card either way; a write still makes them unusable). */
	if (mine && nsec < RA_MIN_SECTORS && (nsec < RA_SMALL_SECTORS || !LD(d->ra_small)) && ra_any(d) &&
	    ksceKernelGetSystemTimeLow() - __atomic_load_n(&d->ra_last_us, __ATOMIC_RELAXED) <
	    RA_LIVE_US) {
		ret = io(ref, part, sector, buf, nsec);
		if (!write)
			ra_note_small_stock(d, part, sector, nsec, 0, ret);
		return ret;
	}
	/* GPU-safe reads asked for, but this firmware has no cache clean to
	 * keep that promise: such reads take the standard path, which leaves
	 * the data in memory as promised. */
	if (!write && LD(g_gpu_safe) && !g_dcache_clean)
		return io(ref, part, sector, buf, nsec);
	if (!take(d, mine, nsec)) {
		ret = io(ref, part, sector, buf, nsec);
		if (mine && !write)
			ra_note_small_stock(d, part, sector, nsec, 0, ret);
		return ret;
	}
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
	/* what the split plans for: no clean after the DMA copies with "Non-DMA" */
	const int plan_clean = clean && !(LD(g_gpu_safe) == VM_GPUSAFE_NODMA &&
	                                  dma_state() == VM_DMA_ON);
	const int ready = !write && __atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE);
	const int mode = LD(g_pipeline);    /* one value for the whole request */
	const int pipe = ready && !verify && mode;
	const int offload = LD(g_offload);
	/* copy on the system core: a game's read (a system thread's buffers
	 * need no mapping, and copy where they are), as the setting says */
	OffMap om = { .uid = -1, .user = p, .len = nsec * 512, .first = mt0, .mt = -2 };
	const int off = ready && __atomic_load_n(&g_offload_state, __ATOMIC_ACQUIRE) >= 0 &&
	                nsec >= OFFLOAD_MIN_SECTORS &&
	                vm_offload_cached(offload) &&
	                (om.pid = ksceKernelGetProcessId()) != KERNEL_PID;
	uint32_t sz[PIPE_MAX_PARTS], own = 0;
	int k = 0;
	const uint32_t sector0 = sector;
	/* read ahead: this read continues the stream from the pieces, if they
	 * hold it; anything else drops them first (they are in the buffers) */
	const int ra = !write && __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE);
	uint32_t ra_got = 0;                  /* of it from the pieces (calibration) */

	if (clean)
		ADD(g_gpu_cleaned_kib, left / 2);
	if (write && nsec >= RA_MIN_SECTORS) {
		ra_adapt_end(d, 0, d->ra_nc, d->ra_streak);
		d->ra_streak = 0;                     /* a write ends the streams */
		if (!__sync_lock_test_and_set(&d->rs_lock, 1)) {
			ra_adapt_end(d, 1, d->rs_nc, d->rs_streak);
			d->rs_streak = 0;
			__sync_lock_release(&d->rs_lock);
		}
	}
	if (ra_any(d)) {
		const uint32_t got = ra ? ra_serve(d, part, io, sector, p, left, clean, stats, off, &om,
		                                   left, &own)
		                        : 0;

		ra_got = got;
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
			      (k = pipe_plan(d, n, plan_clean, sz)) >= 2)) {
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
		           (k = pipe_plan(d, n, plan_clean, sz)) >= 2) {
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
			copy_out(p, d->buf, bytes, clean);   /* the clean counted as copy time */
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
				if (!write && dma_state() == VM_DMA_ON)   /* it may be the DMA copy's */
					dma_stop(VM_DMA_WHY_MISMATCH);
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
	if (!write && ret >= 0 && !left && !LD(d->fallback) && !problem &&
	    ra_note(d, part, sector0, nsec, 0)) {
		cal_read(d, 0, nsec, ra_got, cal_t0, cal_held);
		if (ra)
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
		if (COUNT_CORES() && !left)
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
	{
		const uint64_t a = LD(d->t_start), e = __atomic_load_n(&d->t_end, __ATOMIC_SEQ_CST);

		if (e != a && (uint64_t)sector < e && (uint64_t)sector + nsec > a)
			__atomic_store_n(&d->t_dirty, 1, __ATOMIC_SEQ_CST);
	}
	r = stage_xfer_one(d, 1, io, ref, rd_ref, part, sector, buf, nsec);
	__atomic_fetch_sub(&d->ra_wbusy, 1, __ATOMIC_SEQ_CST);
	return r;
}

/* The four hooks: a read and a write per device, SceSdif with a card context,
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
		{ H_RD_MS,  NID_READ_MS,   h_rd_ms  },
		{ H_WR_MS,  NID_WRITE_MS,  h_wr_ms  },
	};

	{
		uintptr_t f = 0;

		if (module_get_export_func(KERNEL_PID, "SceSysmem", LIB_CPU_DRIVER, NID_DCACHE_CLEAN,
		                           &f) >= 0)
			g_dcache_clean = (void (*)(void *, unsigned int))f;
		/* title ids (ksceKernelSysrootGetProcessTitleId) and whether an
		 * exclusive app runs (ksceAppMgrIsExclusiveProcessRunning: 1 if one
		 * does; title_id NULL: any), looked up by their NIDs - the way that
		 * found them on the console */
		f = 0;
		if (module_get_export_func(KERNEL_PID, "SceSysmem", LIB_SYSROOT_KERNEL,
		                           NID_PROCESS_TITLE_ID, &f) >= 0 && f)
			g_title_fn = (int (*)(SceUID, char *, SceSize))f;
		f = 0;
		if (module_get_export_func(KERNEL_PID, "SceAppMgr", LIB_APPMGR_DRIVER,
		                           NID_EXCLUSIVE_RUNNING, &f) >= 0 && f)
			g_exclusive_fn = (int (*)(const char *))f;
		f = 0;
		if (module_get_export_func(KERNEL_PID, "SceKernelDmacMgr", LIB_DMAC_DRIVER,
		                           NID_DMAC_MEMCPY, &f) >= 0 && f)
			g_dmac_memcpy = (int (*)(void *, const void *, SceSize))f;
		/* ksceKernelGetThreadStackFreeSize (SceThreadmgrForDriver
		 * 0xE2C40624, 0x7B278A0B in vita-headers db/360): the log's stack
		 * line only */
		f = 0;
		if (module_get_export_func(KERNEL_PID, "SceKernelThreadMgr", 0xE2C40624, 0x7B278A0B,
		                           &f) >= 0 && f)
			g_stack_free_fn = (int (*)(SceUID))f;

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
	/* the app in front: the process events, hooked as PSVshell hooks them -
	 * SceProcessmgr's import of ksceKernelInvokeProcEventHandler (an import,
	 * not an export: there is none to hook) */
	g_proc_ref = 0;
	g_proc_hook = taiHookFunctionImportForKernel(KERNEL_PID, (tai_hook_ref_t *)&g_proc_ref, "SceProcessmgr",
	                                             LIB_PROCMGR_KERNEL, NID_PROC_EVENT,
	                                             ra_proc_event_hook);
	if (g_proc_hook >= 0)
		VM_KLOGI("stage: app events followed");
	else
		VM_KLOGW("stage: app events not hooked (0x%08x) - the per-app settings and read "
		         "ahead Auto follow no app", (unsigned)g_proc_hook);
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
	if (g_proc_hook >= 0) {
		taiHookReleaseForKernel(g_proc_hook, g_proc_ref);
		g_proc_hook = -1;
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

/* ---- the settings in effect: the global ones, or the app in front's -------
 *
 * Settings sets the global ones; the app in front may have its own (the
 * overlay; its profile): read ahead, copy on core 3, overlapped reads,
 * GPU-safe reads, and its thresholds with read ahead On. What every request
 * uses (g_ra_mode, g_offload, g_pipeline, g_gpu_safe, ...) is put in effect
 * here from both, whenever either changes or another app comes to the front
 * (the worker loads its profile then; changes not saved are dropped). */
static volatile int g_glob_ra, g_glob_offload, g_glob_pipe, g_glob_gpu = 1;
/* g_app_cur: declared above; set to the global ones at init (app_load) */
static VmApp        g_app_saved = { -1, -1, -1, -1, { 0, 0 } };
/* g_app_pid: whose they are (atomic), declared above */
static char         g_app_title[16];
static volatile int g_app_reload;           /* its profile changed (Settings)    */
static volatile int g_eff_lock;

static void eff_lock(void)
{
	while (__sync_lock_test_and_set(&g_eff_lock, 1))
		ksceKernelDelayThread(100);
}

/* Put the settings in effect. Any thread. */
static void eff_update(void)
{
	VmApp a;
	int needed, pipeline, offload, gpusafe, ra;

	eff_lock();
	if (__atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE) == __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE))
		a = g_app_cur;
	else
		ra_app_global(&a);
	needed = LD(g_pipeline) || LD(g_offload) || LD(g_readahead);   /* the helper */
	pipeline = a.pipe >= 0 ? a.pipe : LD(g_glob_pipe);
	offload = a.offload >= 0 ? a.offload : LD(g_glob_offload);
	gpusafe = a.gpusafe >= 0 ? a.gpusafe : LD(g_glob_gpu);
	ra = a.ra >= 0 ? a.ra : LD(g_glob_ra);
	ST(g_gpu_safe, gpusafe >= VM_GPUSAFE_OFF && gpusafe <= VM_GPUSAFE_NODMA ? gpusafe
	                                                                        : VM_GPUSAFE_ALL);
	if (pipeline != LD(g_pipeline))
		for (int i = 0; i < VM_DEV_COUNT; i++) {
			ST(g_dev[i].est_cp_ns[0], 0u);    /* measured afresh */
			ST(g_dev[i].est_cp_ns[1], 0u);
		}
	ST(g_pipeline, pipeline);
	if (offload != LD(g_offload) || !ra != !LD(g_readahead))
		ST(g_nc_off_refused, 0);                  /* checked afresh */
	if (offload != LD(g_offload)) {
		__atomic_store_n(&g_offload_state, 0, __ATOMIC_RELEASE);   /* checked afresh */
		ST(g_offload_logged, 0);
		__atomic_store_n(&g_ncd_state, 0, __ATOMIC_RELEASE);
		ST(g_ncd_logged, 0);
	}
	ST(g_offload, offload);
	if (!ra != !LD(g_readahead))
		for (int i = 0; i < VM_DEV_COUNT; i++)
			ST(g_dev[i].ra_failed, 0);    /* a fresh try at its thread */
	ST(g_ra_mode, ra);
	ST(g_readahead, ra != VM_RA_OFF ? 1 : 0);
	ra_adapt_reset();                     /* adaptive: afresh from where On is */
	/* The helper (and copier) wanted again after no setting wanted it: a
	 * fresh try at making them, whichever setting was switched on. */
	if (!needed && (pipeline || offload || ra))
		for (int i = 0; i < VM_DEV_COUNT; i++)
			ST(g_dev[i].pipe_failed, 0);
	__sync_lock_release(&g_eff_lock);
}

void vm_stage_apply(const VmSettings *s)
{
	/* the internal storage never: its setting is reserved (VM_KEY_INTERNAL) */
	const int32_t want[VM_DEV_COUNT] = { s->sd2vita, 0, s->memcard };

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
	ST(g_count_cfg, s->stats ? 1 : 0);   /* the HUD asks while shown (count_hud) */
	/* DMA copy: switched on (again) is a fresh start */
	if (s->dmacopy && !LD(g_dma_want)) {
		ST(g_dma_errors, 0);
		ST(g_dma_last_err, 0);
		ST(g_dma_logged, 0);
		ST(g_dma_stop, VM_DMA_WHY_NONE);
	}
	ST(g_dma_want, s->dmacopy ? 1 : 0);
	ST(g_count_all, LD(g_count_cfg) || LD(g_count_hud) ? 1 : 0);
	ST(g_verify, s->verify ? 1 : 0);
	ST(g_log_diag, s->log ? 1 : 0);
	/* the global ones, then what is in effect */
	ST(g_glob_gpu, s->gpusafe >= VM_GPUSAFE_OFF && s->gpusafe <= VM_GPUSAFE_NODMA ? s->gpusafe
	                                                                         : VM_GPUSAFE_ALL);
	ST(g_glob_pipe, s->pipeline >= VM_PIPE_OFF && s->pipeline <= VM_PIPE_FIXED ? s->pipeline
	                                                                         : VM_PIPE_OFF);
	ST(g_glob_offload, vm_setting_valid(VM_KEY_OFFLOAD, s->offload) ? s->offload : VM_OFFLOAD_OFF);
	ST(g_glob_ra, vm_setting_valid(VM_KEY_READAHEAD, s->readahead) ? s->readahead : VM_RA_OFF);
	eff_update();
	if (!s->verify)
		ST(g_shadow_failed, 0);   /* turning verify off and on again retries the buffer */
}

/* The app in front's own settings, from its profile (worker thread; fg 0:
 * none). Changes not saved for the one before are dropped. */
static void app_load(SceUID fg)
{
	RaProf pf;

	eff_lock();
	ra_app_global(&g_app_cur);
	if (fg && fg == __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE)) {
		memcpy(g_app_title, g_fg_title, sizeof(g_app_title) - 1);
		g_app_title[sizeof(g_app_title) - 1] = 0;
	} else if (!fg || fg != __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE)) {
		g_app_title[0] = 0;
	}                                     /* else suspended, reloaded: its title kept */
	__sync_lock_release(&g_eff_lock);
	if (fg && g_app_title[0] && ra_prof_load(g_app_title, &pf)) {
		eff_lock();
		g_app_cur = pf.app;
		__sync_lock_release(&g_eff_lock);
		if (ra_app_set_any(&pf.app))
			VM_KLOGI("stage: %s - its own settings (read ahead %d, copy %d, overlapped %d, "
			         "GPU-safe %d, start after %u / %u)", g_app_title, (int)pf.app.ra,
			         (int)pf.app.offload, (int)pf.app.pipe, (int)pf.app.gpusafe,
			         (unsigned)pf.app.thr[0], (unsigned)pf.app.thr[1]);
	}
	g_app_saved = g_app_cur;
	__atomic_store_n(&g_app_pid, fg, __ATOMIC_RELEASE);
	eff_update();
}

void vm_stage_app_now(VmAppNow *o)
{
	memset(o, 0, sizeof(*o));
	eff_lock();
	if (__atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE) &&
	    __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE) == __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE)) {
		memcpy(o->title, g_app_title, sizeof(o->title) - 1);
		o->cur = g_app_cur;
		o->saved = g_app_saved;
	} else {
		ra_app_global(&o->cur);
		ra_app_global(&o->saved);
	}
	__sync_lock_release(&g_eff_lock);
	o->global.thr[0] = RA_STREAK;                 /* the fixed thresholds (On) */
	o->global.thr[1] = RA_STREAK_NC;
	o->global.ra = LD(g_glob_ra);
	o->global.offload = LD(g_glob_offload);
	o->global.pipe = LD(g_glob_pipe);
	o->global.gpusafe = LD(g_glob_gpu);
}

static int app_in_front(void)
{
	const SceUID pid = __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE);

	return pid && pid == __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE) && g_app_title[0];
}

int vm_stage_app_set(const VmApp *a)
{
	VmApp v;

	if (!a || !ra_app_ok(a))
		return VM_ERR_ARG;
	if (!app_in_front())
		return VM_ERR_NOT_READY;
	v = *a;
	/* its thresholds only mean something with read ahead on for it */
	if ((v.ra == VM_APP_GLOBAL ? LD(g_glob_ra) : v.ra) == VM_RA_OFF)
		v.thr[0] = v.thr[1] = 0;
	eff_lock();
	g_app_cur = v;
	__sync_lock_release(&g_eff_lock);
	eff_update();
	return VM_OK;
}

int vm_stage_ratest_start(void)
{
	if (!app_in_front())
		return VM_ERR_NOT_READY;
	__atomic_store_n(&g_rt_on, 0, __ATOMIC_RELEASE);
	while (__sync_lock_test_and_set(&g_rt_lock, 1))
		ksceKernelDelayThread(100);
	memset(&g_rt, 0, sizeof(g_rt));
	__atomic_store_n(&g_rt_pid, __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE), __ATOMIC_RELAXED);
	__sync_lock_release(&g_rt_lock);
	__atomic_store_n(&g_rt_on, 1, __ATOMIC_RELEASE);
	return VM_OK;
}

int vm_stage_ratest_running(uint32_t *mib)
{
	const int on = __atomic_load_n(&g_rt_on, __ATOMIC_ACQUIRE);

	if (on && mib)
		*mib = __atomic_load_n(&g_rt.total_sectors, __ATOMIC_RELAXED) / 2048u;
	return on;
}

int vm_stage_ratest_stale(void)
{
	const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);

	return __atomic_load_n(&g_rt_on, __ATOMIC_ACQUIRE) && fg &&
	       fg != __atomic_load_n(&g_rt_pid, __ATOMIC_RELAXED);
}

void vm_stage_ratest_cancel(void)
{
	__atomic_store_n(&g_rt_on, 0, __ATOMIC_RELEASE);
}

void vm_stage_ratest_stop(VmRaTestPick *o)
{
	RaPick p;

	__atomic_store_n(&g_rt_on, 0, __ATOMIC_RELEASE);
	while (__sync_lock_test_and_set(&g_rt_lock, 1))
		ksceKernelDelayThread(100);
	{
		VmFg fg;
		RtCost cost;

		vm_stage_fg(&fg);
		cal_cost(fg.dev, &cost, NULL);    /* the app's storage's, if measured */
		ra_test_pick_cost(&g_rt, &p, &cost);
	}
	o->mib = g_rt.total_sectors / 2048u;
	__sync_lock_release(&g_rt_lock);
	o->little = p.little;
	o->ra = p.ra ? VM_RA_ON : VM_RA_OFF;
	o->thr[0] = p.thr[0];
	o->thr[1] = p.thr[1];
	o->offload = p.copy_nc ? VM_OFFLOAD_ALL : VM_OFFLOAD_ALWAYS;
}

int vm_stage_app_save(void)
{
	RaProf pf;

	if (!app_in_front())
		return VM_ERR_NOT_READY;
	pf.app = g_app_cur;
	if (!ra_app_set_any(&pf.app)) {
		/* all global: no profile at all */
		ra_prof_remove(g_app_title);
	} else {
		ra_prof_save(g_app_title, &pf);
		if (!ra_prof_load(g_app_title, &pf))
			return VM_ERR_IO;
	}
	g_app_saved = g_app_cur;
	VM_KLOGI("stage: %s - its settings saved", g_app_title);
	return VM_OK;
}

void vm_stage_on_problem(void (*fn)(void))
{
	__atomic_store_n(&g_on_problem, fn, __ATOMIC_RELEASE);
}

void vm_stage_worker_tick(void)
{
	static uint32_t last_log_us, last_cleaned, last_nc, last_max;
	const uint32_t now_us = ksceKernelGetSystemTimeLow();

	if (!g_cal_loaded && LD(g_live))
		cal_load();                       /* what was measured before, once */
	if (LD(g_gpu_safe) && !g_dcache_clean && LD(g_live) && !g_gpu_safe_logged) {
		g_gpu_safe_logged = 1;
		VM_KLOGW("stage: GPU-safe reads are on but the cache clean function was not found - "
		         "reads take the standard path");
	}
	/* DMA copy: why it is not in use, once (again after it is switched off and on) */
	if (LD(g_dma_want) && LD(g_live) && !LD(g_dma_logged)) {
		const int st = dma_state();

		if (st == VM_DMA_UNAVAIL) {
			ST(g_dma_logged, 1);
			VM_KLOGW("stage: DMA copy is on but ksceDmacMemcpy was not found - the CPU copies");
		} else if (st == VM_DMA_STOPPED) {
			ST(g_dma_logged, 1);
			VM_KLOGW("stage: DMA copy stopped (%s) after %u copies, %u refused (last 0x%08x) - "
			         "the CPU copies", LD(g_dma_stop) == VM_DMA_WHY_MISMATCH ?
			         "a verify difference" : "refused too often", (unsigned)LD(g_dma_copies),
			         (unsigned)LD(g_dma_errors), (unsigned)LD(g_dma_last_err));
		}
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
		VM_KLOGI("stage: non-cacheable (standard path) %u KiB, GPU-safe cleaned %u KiB "
		         "(not after DMA: %u KiB), largest read %u KiB", (unsigned)last_nc,
		         (unsigned)last_cleaned, (unsigned)LD(g_gpu_dma_kib), (unsigned)(last_max / 2));
		if (LD(g_dma_want)) {
			VM_KLOGI("stage: DMA copies %u (%u MiB in %u ms; %u of 16-32 KiB in %u ms), "
			         "refused %u",
			         (unsigned)LD(g_dma_copies), (unsigned)(LD(g_dma_kib) / 1024),
			         (unsigned)(LD(g_dma_us) / 1000), (unsigned)LD(g_dma_mid),
			         (unsigned)(LD(g_dma_mid_us) / 1000), (unsigned)LD(g_dma_errors));
			VM_KLOGI("stage: DMA left to the CPU: small %u (%u MiB, %u of 8 KiB or more), "
			         "not aligned %u (%u MiB)",
			         (unsigned)LD(g_dma_small), (unsigned)(LD(g_dma_small_kib) / 1024),
			         (unsigned)LD(g_dma_small_half), (unsigned)LD(g_dma_unal),
			         (unsigned)(LD(g_dma_unal_kib) / 1024));
		}
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
		if (LD(g_ra_mode) == VM_RA_ADAPTIVE) {
			VM_KLOGI("stage: read ahead adaptive: streak C %u NC %u now, moved %u / %u times",
			         (unsigned)ra_threshold(0), (unsigned)ra_threshold(1),
			         (unsigned)LD(g_ra_moves[0]), (unsigned)LD(g_ra_moves[1]));
			/* read ahead's runs by length 1..10+ since what is in effect
			 * changed: what the gain and cost figures can be checked against */
			uint32_t runs[2][RT_RUN];

			while (__sync_lock_test_and_set(&g_ad_lock, 1))
				ksceKernelDelayThread(10);
			memcpy(runs, g_ad_runs, sizeof(runs));
			__sync_lock_release(&g_ad_lock);
			for (int k = 0; k < 2; k++) {
				const uint32_t *r = runs[k];

				VM_KLOGI("stage: read ahead runs %s 1-10+: %u %u %u %u %u %u %u %u %u %u",
				         k ? "NC" : "C", (unsigned)r[1], (unsigned)r[2], (unsigned)r[3],
				         (unsigned)r[4], (unsigned)r[5], (unsigned)r[6], (unsigned)r[7],
				         (unsigned)r[8], (unsigned)r[9], (unsigned)r[10]);
			}
		}
		/* calibration: the medians it has, per storage, and what is in use */
		for (int i = 0; i < VM_DEV_COUNT; i++) {
			const CalDev *c = &g_cal[i];
			VmCal v;
			RtCost cost;

			if (!LD(c->n_plain[0]) && !LD(c->n_plain[1]) && !LD(c->waste_n))
				continue;
			cal_cost(i, &cost, &v);
			char m[5][8];                 /* the medians and the mean, "-" without any */

			cal_text(m[0], sizeof(m[0]), c->plain[0], v.n_plain[0]);
			cal_text(m[1], sizeof(m[1]), c->served[0], v.n_served[0]);
			cal_text(m[2], sizeof(m[2]), c->plain[1], v.n_plain[1]);
			cal_text(m[3], sizeof(m[3]), c->served[1], v.n_served[1]);
			if (v.n_waste)
				snprintf(m[4], sizeof(m[4]), "%u", (unsigned)(LD(c->waste_us) / v.n_waste % 100000u));
			else
				snprintf(m[4], sizeof(m[4]), "-");
			/* per 128 KiB: from the card / from the pieces (how many each) */
			VM_KLOGI("stage: calibration dev %d%s: C %s/%s us (%u/%u), NC %s/%s (%u/%u), "
			         "waste %s us (%u); in use C %d NC %d /16 us, waste %d",
			         i, v.on ? "" : " (off)", m[0], m[1],
			         (unsigned)v.n_plain[0], (unsigned)v.n_served[0], m[2], m[3],
			         (unsigned)v.n_plain[1], (unsigned)v.n_served[1], m[4], (unsigned)v.n_waste,
			         (int)v.gain16[0], (int)v.gain16[1], (int)v.waste_us);
		}
		if (LD(g_ra_bad))
			VM_KLOGI("stage: read ahead unusable: failed %u, during a write %u, dirty %u, "
			         "switched %u, old %u", (unsigned)LD(g_ra_bad_why[0]),
			         (unsigned)LD(g_ra_bad_why[1]), (unsigned)LD(g_ra_bad_why[2]),
			         (unsigned)LD(g_ra_bad_why[3]), (unsigned)LD(g_ra_bad_why[4]));
		VM_KLOGI("stage: tiny reads from their block %u", (unsigned)LD(g_ra_tiny_reads));
		if (g_stack_free_fn) {
			/* the least each has had free, of its stack (-1: no such thread) */
			int fr[VM_DEV_COUNT][3];

			for (int i = 0; i < VM_DEV_COUNT; i++) {
				const SceUID th[3] = { g_dev[i].th, g_dev[i].th2, g_dev[i].ra_th };

				for (int k = 0; k < 3; k++)
					fr[i][k] = th[k] >= 0 ? g_stack_free_fn(th[k]) : -1;
			}
			VM_KLOGI("stage: stack free (bytes of %u / %u): worker %d; SD2Vita helper %d copier %d "
			         "ahead %d; memory card %d %d %d", VM_STACK_WORKER, VM_STACK_DEV,
			         g_stack_free_fn(ksceKernelGetThreadId()),
			         fr[VM_DEV_SD2VITA][0], fr[VM_DEV_SD2VITA][1], fr[VM_DEV_SD2VITA][2],
			         fr[VM_DEV_MEMCARD][0], fr[VM_DEV_MEMCARD][1], fr[VM_DEV_MEMCARD][2]);
		}
		if (LD(g_offload) || __atomic_load_n(&g_off_reads, __ATOMIC_RELAXED))
			VM_KLOGI("stage: copied on the system core %u reads %u MiB (%u from the 2nd page), "
			         "mapping failed %u (0x%08x)",
			         (unsigned)__atomic_load_n(&g_off_reads, __ATOMIC_RELAXED),
			         (unsigned)(__atomic_load_n(&g_off_kib, __ATOMIC_RELAXED) / 1024),
			         (unsigned)LD(g_off_headed),
			         (unsigned)__atomic_load_n(&g_off_maperr, __ATOMIC_RELAXED),
			         (unsigned)LD(g_offload_err));
		if (vm_offload_direct(LD(g_offload)) || LD(g_ncd_reads))
			VM_KLOGI("stage: uncached read on the system core %u reads %u MiB, busy %u, "
			         "failed %u, mapping failed %u (0x%08x)",
			         (unsigned)LD(g_ncd_reads), (unsigned)(LD(g_ncd_kib) / 1024),
			         (unsigned)LD(g_ncd_busy), (unsigned)LD(g_ncd_fail),
			         (unsigned)LD(g_ncd_maperr), (unsigned)LD(g_ncd_err));
	}
	/* Refused mappings, each once (see MapFail). */
	while (g_mapfail_logged < MAPFAIL_LOG && g_mapfail_logged < LD(g_mapfail_n) &&
	       __atomic_load_n(&g_mapfail[g_mapfail_logged].ready, __ATOMIC_ACQUIRE)) {
		const MapFail *f = &g_mapfail[g_mapfail_logged++];

		VM_KLOGW("stage: map refused (%s) 0x%08x: app %s pid 0x%08x, buffer 0x%08x + %u B, "
		         "memory type 0x%04x%s", f->which ? "C + NC All" : "copy on core 3",
		         (unsigned)f->err, f->title[0] ? f->title : "-", (unsigned)f->pid,
		         (unsigned)f->user, (unsigned)f->len, (unsigned)f->mt & 0xFFFFu,
		         f->nc ? " (NC)" : "");
		for (int k = 0; k < f->nseg; k++) {
			const MapFailSeg *g = &f->seg[k];

			if (g->uid >= 0)
				VM_KLOGW("stage: map refused: +0x%05x..+0x%05x (%u KiB) block 0x%08x, maps "
				         "alone: %s (0x%08x)", (unsigned)g->off, (unsigned)(g->off + g->len),
				         (unsigned)(g->len / 1024), (unsigned)g->uid, g->map >= 0 ? "yes" : "no",
				         (unsigned)g->map);
			else
				VM_KLOGW("stage: map refused: +0x%05x..+0x%05x (%u KiB) no block (0x%08x), "
				         "its first page maps: %s (0x%08x)", (unsigned)g->off,
				         (unsigned)(g->off + g->len), (unsigned)(g->len / 1024), (unsigned)g->uid,
				         g->map >= 0 ? "yes" : "no", (unsigned)g->map);
		}
		if (f->more)
			VM_KLOGW("stage: map refused: more stretches past these");
		for (int b = 0; b < f->nblk; b++)
			VM_KLOGW("stage: map refused: block 0x%08x GetInfoEx 0x%08x: type 0x%08x at "
			         "0x%08x + %u KiB", (unsigned)f->blk[b].uid, (unsigned)f->blk[b].info,
			         (unsigned)f->blk[b].type, (unsigned)f->blk[b].base,
			         (unsigned)(f->blk[b].size / 1024));
		if (g_mapfail_logged == MAPFAIL_LOG)
			VM_KLOGW("stage: map refused: only the first %u are described", MAPFAIL_LOG);
	}
	/* Copy on the system core: whether the mapping was found usable, once. */
	{
		const int st = __atomic_load_n(&g_offload_state, __ATOMIC_ACQUIRE);

		if (LD(g_offload) && st != LD(g_offload_logged)) {
			static uint32_t ready_type;  /* the last one logged: once, not per app */

			ST(g_offload_logged, st);
			if (st > 0 && LD(g_offload_type) != ready_type) {
				ready_type = LD(g_offload_type);
				VM_KLOGI("stage: copy on the system core ready (mapping type 0x%08x)",
				         (unsigned)ready_type);
			}
			else if (st == -1)
				VM_KLOGW("stage: copy on the system core refused: the mapping is not "
				         "writable (type 0x%08x) - games copy as before",
				         (unsigned)LD(g_offload_type));
			else if (st == -2)
				VM_KLOGW("stage: copy on the system core refused: the mapping's memory type "
				         "differs - games copy as before");
		}
	}
	/* the app in front, in the log when it changes (and processes whose
	 * title id could not be had) */
	{
		static SceUID last_fg;
		static uint32_t last_noid;
		const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);
		const uint32_t noid = __atomic_load_n(&g_fg_noid, __ATOMIC_RELAXED);

		if (fg != last_fg) {
			last_fg = fg;
			/* calibration: kept at a change of app, only if what is in
			 * use would change (internal storage: as few writes as can be) */
			if (LD(g_cal_dirty) && cal_worth_saving())
				cal_save();
			if (fg)
				VM_KLOGI("stage: in front: %s (pid 0x%08x)", g_fg_title, (unsigned)fg);
			else
				VM_KLOGI("stage: in front: the home screen");
		}
		if (noid != last_noid) {
			last_noid = noid;
			VM_KLOGW("stage: %u processes started without a title id", (unsigned)noid);
		}
	}
	/* With the log on: when it was switched on, how the app in front is followed */
	{
		static int was_on;
		const int on = LD(g_log_diag);

		if (on && !was_on) {
			const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);

			VM_KLOGI("stage: v%s, app events %s, title ids %s, in front: %s", VM_BUILD_STR,
			         g_proc_hook >= 0 ? "followed" : "NOT hooked",
			         g_title_fn ? "there" : "MISSING", fg ? g_fg_title : "the home screen");
		}
		was_on = on;
	}
	/* the app in front's own settings: loaded when another app comes to the
	 * front (or its profile changed); kept while the home screen is in front
	 * (suspended, it comes back with them, saved or not); in effect only
	 * while it is in front */
	{
		static SceUID last_fg;
		const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);

		if (fg && fg != LD(g_bm_pid)) {    /* another app: its buffers from now */
			ST(g_bm_reads, LD(g_off_reads) + LD(g_ncd_reads));
			ST(g_bm_headed, LD(g_off_headed));
			ST(g_bm_fail, LD(g_off_maperr) + LD(g_ncd_maperr));
			ST(g_bm_pid, fg);
		}
		if (__atomic_exchange_n(&g_app_reload, 0, __ATOMIC_ACQ_REL))
			app_load(fg ? fg : __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE));
		else if (fg && fg != __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE))
			app_load(fg);
		else if (fg != last_fg)
			eff_update();
		last_fg = fg;
	}
	{
		const int st = __atomic_load_n(&g_ncd_state, __ATOMIC_ACQUIRE);

		if (vm_offload_direct(LD(g_offload)) && st != LD(g_ncd_logged)) {
			static uint32_t ready_type;  /* the last one logged: once, not per app */

			ST(g_ncd_logged, st);
			if (st > 0 && LD(g_ncd_type) != ready_type) {
				ready_type = LD(g_ncd_type);
				VM_KLOGI("stage: uncached reads on the system core ready (mapping type 0x%08x)",
				         (unsigned)ready_type);
			}
			else if (st < 0)
				VM_KLOGW("stage: uncached reads on the system core refused (%d, type 0x%08x) - "
				         "standard path", st, (unsigned)LD(g_ncd_type));
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

		/* read ahead: its thread, once the helper and buffers are there
		 * (just made above: in this same tick, the worker may sleep after it) */
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
				VM_KLOGI("stage: dev %d read ahead ready", i);
			}
		}
	}
	/* The verify buffer: allocated once verify mode is turned on, given back
	 * once it is off (not while a request has it: the next tick then). */
	if (!LD(g_verify) && __atomic_load_n(&g_shadow, __ATOMIC_ACQUIRE) &&
	    !__sync_lock_test_and_set(&g_shadow_busy, 1)) {
		__atomic_store_n(&g_shadow, NULL, __ATOMIC_RELEASE);
		ksceKernelFreeMemBlock(g_shadow_uid);
		g_shadow_uid = -1;
		__sync_lock_release(&g_shadow_busy);
	}
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

/* A release of the helper or of the verify buffer waits for a request still
 * in flight (see pipe_retire): the worker should come back soon. */
int vm_stage_pending(void)
{
	if (!LD(g_verify) && __atomic_load_n(&g_shadow, __ATOMIC_ACQUIRE))
		return 1;
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if ((LD(g_dev[i].pipe_made) && !__atomic_load_n(&g_dev[i].pipe_ready, __ATOMIC_ACQUIRE)) ||
		    (LD(g_dev[i].ra_made) && !__atomic_load_n(&g_dev[i].ra_ready, __ATOMIC_ACQUIRE)))
			return 1;
	return 0;
}

void vm_stage_ra_now(VmRaNow *o)
{
	memset(o, 0, sizeof(*o));
	o->mode = LD(g_ra_mode);
	for (int k = 0; k < 2; k++) {
		o->thr[k] = ra_threshold(k);
		/* the R/A Test running: the standard one is set too */
		o->set[k] = __atomic_load_n(&g_rt_on, __ATOMIC_ACQUIRE) ? o->thr[k] : ra_base_threshold(k);
	}
	{
		const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);

		if (!fg || fg != LD(g_bm_pid) || !LD(g_offload))
			o->bufmap = VM_BUFMAP_NONE;
		else if (LD(g_off_maperr) + LD(g_ncd_maperr) != LD(g_bm_fail))
			o->bufmap = VM_BUFMAP_REFUSED;
		else if (LD(g_off_headed) != LD(g_bm_headed))
			o->bufmap = VM_BUFMAP_WORKAROUND;
		else if (LD(g_off_reads) + LD(g_ncd_reads) != LD(g_bm_reads))
			o->bufmap = VM_BUFMAP_OK;
		else
			o->bufmap = VM_BUFMAP_NONE;
	}
}

void vm_stage_fg(VmFg *o)
{
	uint32_t best = 0;

	memset(o, 0, sizeof(*o));
	o->dev = -1;
	if (!__atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE))
		return;
	memcpy(o->title, g_fg_title, sizeof(o->title) - 1);
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const uint32_t v = __atomic_load_n(&g_fg_sec[i], __ATOMIC_RELAXED);

		if (v > best) {
			best = v;
			o->dev = i;
		}
	}
	if (best < RA_FG_DEV_MIN)
		o->dev = -1;                          /* too little read to tell */
}

void vm_stage_core_stats(uint32_t cp_core_kib[4], uint32_t *nc_sys_kib, uint32_t *nc_own_kib)
{
	for (int i = 0; i < 4; i++)
		cp_core_kib[i] = __atomic_load_n(&g_cp_core_kib[i], __ATOMIC_RELAXED);
	/* graphics data: read on the system core ("C + NC All") or copied there
	 * from read ahead, against read the standard way or copied by the game */
	*nc_sys_kib = LD(g_ncd_kib) + LD(g_ra_nc_sys_kib);
	*nc_own_kib = LD(g_nc_stock_kib) + LD(g_ra_nc_own_kib);
}

void vm_stage_count_cores(int on)
{
	ST(g_count_cores, on ? 1 : 0);
}

void vm_stage_count_hud(int on)
{
	ST(g_count_hud, on ? 1 : 0);
	ST(g_count_all, LD(g_count_cfg) || LD(g_count_hud) ? 1 : 0);
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

void vm_stage_dma(VmDma *out)
{
	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->magic = VM_DMA_MAGIC;
	out->state = dma_state();
	out->stop_why = LD(g_dma_stop);
	out->copies = LD(g_dma_copies);
	out->kib = LD(g_dma_kib);
	out->us = LD(g_dma_us);
	out->mid = LD(g_dma_mid);
	out->mid_us = LD(g_dma_mid_us);
	out->small = LD(g_dma_small);
	out->small_kib = LD(g_dma_small_kib);
	out->small_half = LD(g_dma_small_half);
	out->unaligned = LD(g_dma_unal);
	out->unaligned_kib = LD(g_dma_unal_kib);
	out->errors = LD(g_dma_errors);
	out->last_err = LD(g_dma_last_err);
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
