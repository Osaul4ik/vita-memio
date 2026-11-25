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
#include <stddef.h>                     /* offsetof                            */
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
#include "ra_test.h"                     /* the read ahead model (Adaptive) */
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
	int            skipped;           /* RA_FAILED as not read: a write was running
	                                   * (not an error of the card)            */
} RaPiece;

typedef struct Dev {
	int           port;          /* SDIF port, or -1: the memory card          */
	SceUID        uid;
	void         *buf;           /* NC staging buffer, NULL until allocated    */
	volatile int  busy;          /* use dbusy(): shared ones take g_sh.busy    */
	int           shared;        /* Low kernel memory: buffers in g_sh (set at
	                              * the start, before the hooks)               */
	volatile int  enabled;       /* from settings                              */
	volatile int  want;          /* ... as set, also while the console starts  */
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
	int           ra_overlap;             /* its last read overlapped its end  */
	uint32_t      gap_end[2];             /* when the last read of each memory kind ended (us) */
	int           ra_nc;                  /* ... into non-cacheable memory     */
	/* the stream of 32 KiB reads, kept apart (see RA_SMALL_SECTORS) */
	uint32_t      rs_end, rs_streak;
	int           rs_nc, rs_overlap;
	void         *rs_part;
	int           ra_noted_small;         /* the last read noted was one of those */
	/* Adaptive: the reads of each stream (0 large, 1 32 KiB) still going, by
	 * place in it (as RaTest.pos / spos), and their sectors - put with the
	 * rest when it ends (ra_adapt_end), so a round never has the reads of a
	 * run without the run: that would weigh what read ahead saves without
	 * the pieces left unread at its end. The stream's own thread only. */
	uint32_t      ad_pos[2][RT_POS], ad_sec[2];
	uint32_t      ad_gen[2];              /* the Adaptive state (g_ad_gen) they are for */
	uint32_t      t_gen;                  /* ... the tiny reads' run's */
	int           ra_small;               /* atomic: the pieces are for that stream */
	volatile int  rs_lock;                /* its fields: any thread, a moment only */
	/* the tiny reads' stream and their block (see RA_TINY_SECTORS), the
	 * request holder's; a write looks at t_start, t_end, t_dirty */
	void         *t_buf;
	SceUID        t_uid;
	int           t_cached;               /* t_buf in cached memory (tiny_alloc) */
	uint32_t      t_next, t_streak;
	uint32_t      t_run_sec;              /* the run's sectors so far (Adaptive) */
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

/* Low kernel memory (VM_KEY_LOWMEM, read at the start): the SD2Vita and the
 * memory card, both set on, share one staging buffer and one second buffer
 * instead of one each (384 KiB less); each keeps its own tiny reads' block
 * and threads. One request at a time on either takes them (a shared `busy`:
 * the other one's request meanwhile goes the standard way, as on a busy
 * device). Read ahead's pieces live in them: only the storage that used
 * them last (owner) can have pieces, and a request of the other one drops
 * them first (take) - so a piece is never one storage's data in the other's
 * buffer. Freed only at the end (vm_stage_fini).
 *
 * The app in front comes first: while one is in front, another app's
 * request (another process: the system's own, one in the background) does
 * not take them at all and goes the standard way, so it neither holds them
 * when the app wants them nor drops the app's pieces (share_may). The
 * kernel's own requests take them as before: they may be the app's (its
 * asynchronous reads). With the home screen in front, any request. */
static struct {
	volatile int busy;
	Dev         *owner;                /* the busy holder's                    */
	SceUID       uid, uid2;
	void        *buf, *buf2;
	uint32_t     handovers, other_busy; /* atomics: for the log               */
	uint32_t     yielded;              /* atomic: not the app in front's       */
} g_sh = { .uid = -1, .uid2 = -1 };
static volatile int g_lowmem_set;          /* the setting                      */
static int          g_share_on;            /* in effect for this start         */

static inline volatile int *dbusy(Dev *d)
{
	return d->shared ? &g_sh.busy : &d->busy;
}

static SceUID g_ra_fg;                    /* the app in front (see the app events) */

/* The again cache (see "again cache" below): what a game read lately, kept
 * for when it reads it again - in the staging buffers of the storage the
 * game does not use (`host`), or in a block of its own with no such
 * storage. The host's own requests come first: one that takes its buffers
 * bumps `gen`, and what the cache held there is gone. */
#define RC_UNIT     64u                    /* sectors: 32 KiB, the cache's unit  */
#define RC_UNITS    12u                    /* 384 KiB: a staging buffer and a second one */
#define RC_UNITS0   (STAGE_SECTORS / RC_UNIT)   /* in the staging buffer: 8   */
#define RC_HIST     8u
typedef struct {
	struct Dev *dev;                       /* atomic: whose reads (NULL: none)  */
	void       *part;
	io_fn       io;
	uint32_t    sector, n;                 /* atomic: a write looks at them     */
	uint32_t    len;                       /* units                             */
	int         k;                         /* the size it was kept for: 0 32 KiB, 1 large */
	uint32_t    epoch, t;
	int         ok;                        /* read: may be handed out           */
	int         dirty;                     /* atomic: a write touched it        */
} RcEnt;
static struct {
	volatile int lock;                     /* its fields: one request at a time */
	struct Dev  *host;                     /* whose buffers (NULL: its own block) */
	struct Dev  *user;                     /* whose reads it holds              */
	void        *zb[2];                    /* the buffers its entries are in    */
	uint32_t    gen, seen;                 /* gen: atomic, bumped by the host's take */
	uint32_t    next;                      /* the unit the next entry goes to   */
	RcEnt       e[RC_UNITS];               /* by the unit each starts at        */
	SceUID      uid;                       /* its own block (no host)           */
	void       *own;                       /* atomic                            */
	int         want_own;                  /* atomic: the worker makes it       */
	uint32_t    own_freed;                 /* atomic: times it was given back   */
	/* the app it is for, and whether it reads again what it read (by size: 0
	 * 32 KiB, 1 large): the last reads the standard way, and the counts */
	SceUID      pid;
	int         game;                      /* the app is a game (PCS*)          */
	uint32_t    hs[RC_HIST], he[RC_HIST];
	void       *hp[RC_HIST];
	uint8_t     hk[RC_HIST];               /* ... their sizes                   */
	uint32_t    hi;
	uint32_t    reads[2], again[2];
	int         on[2];                     /* atomic                            */
	uint32_t    ykept[2], yhit[2];         /* while on: kept, and read again from it */
	uint32_t    hold[2];                   /* looks a size stays off (its yield) */
	uint32_t    hold_n[2];                 /* ... the next time: doubled each time */
	uint32_t    hits, hit_kib, lost;       /* atomics: for the log              */
	uint32_t    kept_k[2], hits_k[2], moved_k[2], yoff_k[2];   /* ... by size   */
} g_rc = { .uid = -1 };

/* Another app's request than the one in front's (another process: the
 * system's own, one in the background) while an app is in front. The
 * kernel's own requests count as the app's: they may be its asynchronous
 * reads. With the home screen in front: none is. */
/* The request's process, asked once per request: *c starts at REQ_PID_NONE
 * and keeps it (every step of one request runs on its thread). */
#define REQ_PID_NONE ((SceUID)-1)
static SceUID req_pid(SceUID *c)
{
	if (*c == REQ_PID_NONE)
		*c = ksceKernelGetProcessId();
	return *c;
}

static int bg_req(SceUID *c)
{
	const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_RELAXED);
	SceUID pid;

	if (!fg)
		return 0;
	pid = req_pid(c);
	return pid != fg && pid != KERNEL_PID;
}

/* Whether this request may take d's buffers: always, unless they are shared
 * and another app than the one in front asks (see g_sh). */
static int share_may(const Dev *d, SceUID *c)
{
	if (!d->shared || !bg_req(c))
		return 1;
	ADD(g_sh.yielded, 1);
	return 0;
}

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
/* While the console starts (until vm_stage_go: the home screen is up, see
 * guard.h) every device stays off whatever its setting: the hooks pass every
 * transfer on the standard way, and no buffer, helper thread or app event is
 * set up. The settings applied meanwhile take effect at vm_stage_go. */
static volatile int g_hold = 1;

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
/* ksceKernelDcacheCleanInvalidateRange (L1 and L2): the tiny reads' block in
 * cached memory (tiny_alloc); without it the block is non-cacheable. */
#define NID_DCACHE_CINV    0x364E68A4
static void (*g_dcache_cinv)(void *start, unsigned int size);
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
/* Tuning measures only without Verify transfers (it reads everything again,
 * so no time measured then is what it is); what was measured stays in use. */
#define CAL_MEASURING() (LD(g_cal_on) && !LD(g_verify))
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
	if (d->shared && g_sh.buf) {
		__atomic_store_n(&d->buf, g_sh.buf, __ATOMIC_RELEASE);   /* the other one's */
		return;
	}
	r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, STAGE_BYTES, &uid, &base);
	if (r < 0) {
		ST(d->alloc_logged, 0);         /* the worker logs it */
		ST(d->alloc_err, r);
		return;
	}
	if (d->shared) {
		g_sh.uid = uid;                   /* kept to the end, for both */
		g_sh.buf = base;
	} else {
		d->uid = uid;
	}
	__atomic_store_n(&d->buf, base, __ATOMIC_RELEASE);   /* published last */
}


static void ra_drop(Dev *d);

/* Take the device buffer for one request, or 0 if the request stays stock. */
static int take(Dev *d, int mine, uint32_t nsec, SceUID *pc)
{
	if (!mine || !LD(d->enabled) || LD(d->fallback) ||
	    !__atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) || nsec < LD(g_min_sectors) || !nsec ||
	    !share_may(d, pc))
		return 0;
	if (__sync_lock_test_and_set(dbusy(d), 1)) {
		ADD(d->st.passthru, 1);
		if (d->shared)
			ADD(g_sh.other_busy, 1);
		return 0;
	}
	/* Re-check after taking the buffer: vm_stage_fini clears `enabled` first and
	 * then waits for `busy` to drop, so a request that passed the check above
	 * just before that must not go on to use the buffer. */
	__sync_synchronize();
	if (!LD(d->enabled) || LD(d->fallback)) {
		__sync_lock_release(dbusy(d));
		return 0;
	}
	/* the again cache's host: its buffers are its own again (what the cache
	 * kept there is gone) */
	if (__atomic_load_n(&g_rc.host, __ATOMIC_RELAXED) == d)
		__atomic_fetch_add(&g_rc.gen, 1, __ATOMIC_SEQ_CST);
	/* Low kernel memory: the buffers were the other storage's - its pieces
	 * (in them) dropped first, waiting for one being read */
	if (d->shared && __atomic_load_n(&g_sh.owner, __ATOMIC_RELAXED) != d) {
		if (g_sh.owner)
			ra_drop(g_sh.owner);
		__atomic_store_n(&g_sh.owner, d, __ATOMIC_RELAXED);
		ADD(g_sh.handovers, 1);
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

/* Device d's second buffer, if it has none: its own, or with Low kernel
 * memory the shared one (made by the first that needs it). 0 or the error. */
static int ensure_buf2(Dev *d)
{
	int r;

	if (d->buf2)
		return 0;
	if (!d->shared) {
		r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, PIPE_TAIL_SECTORS * 512, &d->uid2, &d->buf2);
		if (r < 0) {
			d->uid2 = -1;
			d->buf2 = NULL;
		}
		return r < 0 ? r : 0;
	}
	if (!g_sh.buf2) {
		void *b = NULL;

		r = alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, PIPE_TAIL_SECTORS * 512, &g_sh.uid2, &b);
		if (r < 0) {
			g_sh.uid2 = -1;
			return r;
		}
		g_sh.buf2 = b;
	}
	d->uid2 = -1;                         /* not this device's to free */
	d->buf2 = g_sh.buf2;
	return 0;
}
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

/* ---- where the copy runs (the HUD's Core 3) --------------------------------
 *
 * Which core ran each staged read's copy (the caller's thread, or the copier
 * on the system core). */
static uint32_t g_cp_core_kib[4];                                  /* atomics */
/* The least stack each thread has had free (bytes), for the log: how much
 * of VM_STACK_* is left. NULL: not found. */
static int (*g_stack_free_fn)(SceUID thid);
#define STACK_LOW_BYTES 1024
/* Graphics data (non-cacheable) from read ahead, copied by the copier on the
 * system core / by the game's thread: they count as NC, not as copies (C). */
static uint32_t g_ra_nc_sys_kib, g_ra_nc_own_kib;                  /* atomics */
static volatile int g_log_diag;            /* the log is on: count            */
/* With the log on: every transfer of each storage timed, whichever way it
 * went, for the session lines (an app's time in front, the worker): reads by
 * size - tiny (< 32 KiB), 32 KiB, large - the reads that came while read
 * ahead was reading a piece (the card busy with it), and the writes. */
#define LG_TINY  0
#define LG_SMALL 1
#define LG_LARGE 2
typedef struct {
	uint64_t us[3], col_us, wr_us;
	uint32_t n[3], sec[3], col_n, wr_n, wr_sec;
	uint32_t max_us;                       /* the longest read (taken per session) */
} LgDev;
/* ... and by kind (the large and 32 KiB reads into ordinary memory, into
 * non-cacheable memory, the tiny ones: LK_*), for Adaptive's work - how the
 * reads went: [0] the standard way, [1] all from read ahead, [2] partly
 * (tiny: [1] from the block, [2] the block read for it); those that came
 * while a piece was being read; the pieces read for nothing, how long they
 * held a read up, and those a write spoilt (tiny: reads in a block a write
 * had spoilt). */
#define LK_C    0
#define LK_NC   1
#define LK_TINY 2
typedef struct {
	uint64_t us[3], col_us, held_us;
	uint32_t n[3], sec[3], col_n, col_max, waste, spoilt;
} LgKind;

/* With the log on: where each read of the app in front lies against the
 * last few reads of its storage - what read ahead could follow of it. Read
 * ahead follows one stream of each size (tiny, 32 KiB, large), each read
 * starting exactly where the last of its size ended; a game that reads two
 * files in turn, mixes sizes, overlaps, skips a little, or reads again what
 * it just read shows up as reads it never gets. By size, how many and how
 * long they took, the first that fits of:
 *   PT_OWN    where the last read of its size ended (what read ahead follows)
 *   PT_SAME   where another of the last PT_HIST of its size ended (two
 *             streams of one size in turn)
 *   PT_MIX    where one of another size ended (one stream, sizes mixed)
 *   PT_OVER   from inside one of them on past its end (a stream that
 *             overlaps: the file system's clusters)
 *   PT_IN     wholly inside one of them (read again)
 *   PT_BACK   up to 256 KiB before one of them
 *   PT_SKIP   up to 256 KiB after one ended
 *   PT_FAR    none of these
 * The last reads are a ring that racing threads may blur (a lost update or a
 * start and end of two reads): it only counts.
 * For the two read ahead does not get, how far they lie (PtAgain): a read
 * again (PT_IN) by how much the storage read after the read it lies in
 * (could pieces kept that long have had it: 384 KiB is what the three hold),
 * one before (PT_BACK) by the gap to the read it lies before (none: a file
 * read backwards, each read ending where the last began). A read again also
 * by the read it lies in: its size, and whether read ahead's pieces gave it
 * (then a piece kept has it: served again) - the rest went the standard
 * way, straight into the game's buffer, with no copy of ours left. */
#define PT_HIST     8
#define PT_OWN      0
#define PT_SAME     1
#define PT_MIX      2
#define PT_OVER     3
#define PT_IN       4
#define PT_BACK     5
#define PT_SKIP     6
#define PT_FAR      7
#define PT_N        8
typedef struct {
	uint32_t start, end, part;
	uint32_t size;                         /* LG_* | 4: from read ahead's pieces (some)
	                                        * | the device's sectors read with it << 3 */
} PtRead;
typedef struct {
	PtRead   h[PT_HIST];                   /* the last reads (a ring)          */
	uint32_t i;                            /* atomic: the next slot            */
	uint32_t own_end[3], own_part[3];      /* the last read's end, by size     */
	uint32_t tot;                          /* atomic: sectors read (wraps)     */
} PtDev;
#define PT_AGAIN_IN    0                   /* + 0 within 384 KiB, 1 within 1 MiB, 2 further */
#define PT_AGAIN_BACK  3                   /* + 0 ending where it began, 1 within 32 KiB, 2 further */
#define PT_AGAIN_N     6
#define PT_OF_RA       3                   /* in_of: + LG_* the read it lies in, PT_OF_RA its pieces' */
typedef struct {
	uint32_t n[3][PT_AGAIN_N];             /* atomics, by size; the session's  */
	uint32_t in_of[3][4];                  /* ... the reads again, by the one they lie in */
} PtAgain;
typedef struct {
	uint32_t n[3][PT_N];
	uint64_t us[3][PT_N];
} PtCount;

/* What a read did, for the log (stage_xfer_one, ra_nc_read, ra_tiny_read). */
typedef struct {
	int      nc;                           /* 0 ordinary, 1 non-cacheable, 2 device memory */
	uint32_t got;                          /* sectors from read ahead's pieces */
	int      tiny;                         /* tiny: 1 from the block, 2 the block read for it */
	SceUID   pid;                          /* the request's process (req_pid), once */
} XInfo;
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

/* A staged read finished: the part its own thread copied, on which core
 * (the copier notes its own). */
static void diag_note(uint32_t own)
{
	if (own)
		__atomic_fetch_add(&g_cp_core_kib[cpu_id()], own / 2, __ATOMIC_RELAXED);
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
/* The session lines' snapshot (see lg_session). */
typedef struct {
	LgDev    dev[VM_DEV_COUNT];
	LgKind   k[3];
	uint32_t hits, kib, drops, waste_kib, bad, nc_kib, tiny, pipe, off, ncd, dma, nc_stock, wskip;
	uint32_t bg_n, bg_kib, bg_kept;
} LgSnap;

/* What only the log uses - the per-transfer counts (lg_note, lk_note), the
 * refused mappings described, the session's starting snapshot - in a block
 * of its own, made the first time the log is switched on (vm_stage_apply)
 * and kept from then on, as a transfer may be using it at any moment: with
 * the log never on, none of it takes memory. */
typedef struct {
	LgDev    lg[3];                    /* VM_DEV_COUNT (atomics)              */
	LgKind   lk[3];                    /* atomics                             */
	MapFail  mapfail[MAPFAIL_LOG];
	LgSnap   at;                       /* the worker's                        */
	PtDev    pt[3];                    /* VM_DEV_COUNT (pt_note)              */
	PtCount  pc;                       /* atomics                             */
	PtCount  pc_at;                    /* the worker's: at the session's start */
	PtAgain  ag;                       /* atomics, cleared by the session line */
} Diag;
static Diag *volatile g_diag;                  /* atomic; published once      */
static SceUID         g_diag_uid = -1;
static volatile int   g_diag_busy;
#define DG() __atomic_load_n(&g_diag, __ATOMIC_ACQUIRE)
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
static int pipe_fit_min(uint32_t n, int k, uint32_t x, uint64_t io, uint64_t cp, uint32_t inv,
                        uint64_t cmd, uint32_t s[PIPE_MAX_PARTS], uint32_t mint)
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
			if (sum >= n || n - sum > m || n - sum < mint)
				return 0;
			s[0] = n - sum;
			return 1;
		}
		if (m < mint)
			return 0;
		s[i] = (uint32_t)m;
		sum += s[i];
	}
	return 0;
}

static int pipe_fit(uint32_t n, int k, uint32_t x, uint64_t io, uint64_t cp, uint32_t inv,
                    uint64_t cmd, uint32_t s[PIPE_MAX_PARTS])
{
	return pipe_fit_min(n, k, x, io, cp, inv, cmd, s, PIPE_MIN_TAIL);
}

/* The plan for k in [kmin, kmax] and the given costs, every part at least
 * mint sectors. */
static int pipe_plan_costs_min(uint32_t n, uint64_t io, uint64_t cp, uint64_t cmd, int kmin,
                               int kmax, uint32_t out[PIPE_MAX_PARTS], uint32_t mint)
{
	uint64_t best = ~(uint64_t)0;
	const uint32_t inv = recip(cp);
	int best_k = 0;

	for (int k = kmin; k <= kmax; k++) {
		/* k is 2 or 3: constant divisors, which the compiler multiplies */
		uint32_t s[PIPE_MAX_PARTS], lo = mint, hi = k == 2 ? n / 2 : k == 3 ? n / 3 : n / k;
		uint32_t x = 0;

		/* The earlier parts only grow with x, so the x that fit form one
		 * range: below it the parts do not reach n, above it they leave
		 * part 0 too little. Find its start. */
		while (lo <= hi) {
			uint32_t mid = lo + (hi - lo) / 2, t[PIPE_MAX_PARTS], sum = mid;

			if (pipe_fit_min(n, k, mid, io, cp, inv, cmd, s, mint)) {
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
			if (sum + mint > n)
				hi = mid - 1;               /* part 0 left too small */
			else
				lo = mid + 1;               /* the parts do not reach n */
		}
		if (x && pipe_fit_min(n, k, x, io, cp, inv, cmd, s, mint)) {
			uint64_t cost = (uint64_t)k * cmd + (uint64_t)x * cp;

			if (cost < best) {
				best = cost;
				best_k = k;
				for (int i = 0; i < k; i++)
					out[i] = s[i];
			}
		}
	}
	if (!best_k && kmin <= 2 && n >= 2 * mint) {
		/* no part can be hidden (a copy slower than the card): halves */
		out[1] = n / 2 > PIPE_TAIL_SECTORS ? PIPE_TAIL_SECTORS : n / 2;
		out[0] = n - out[1];
		best_k = out[0] <= STAGE_SECTORS ? 2 : 0;
	}
	return best_k;
}

static int pipe_plan_costs(uint32_t n, uint64_t io, uint64_t cp, uint64_t cmd, int kmin, int kmax,
                           uint32_t out[PIPE_MAX_PARTS])
{
	return pipe_plan_costs_min(n, io, cp, cmd, kmin, kmax, out, PIPE_MIN_TAIL);
}

/* Auto: the learnt costs (built-in ones until measured), 2 or 3 parts, or
 * none when one piece is as fast (small reads, a cheap copy).
 * Fixed: the built-in costs, 2 parts, or 3 with GPU-safe reads (2 if 3 do
 * not fit this size). */
/* ... every part at least mint sectors (a shortest last part: PIPE_MIN_TAIL) */
static int pipe_plan_min(const Dev *d, uint32_t n, int clean, uint32_t out[PIPE_MAX_PARTS],
                         uint32_t mint)
{
	const uint64_t def_cp = clean ? PIPE_DEF_CPCL_NS : PIPE_DEF_CP_NS;
	int k;

	if (LD(g_pipeline) == VM_PIPE_FIXED) {
		k = pipe_plan_costs_min(n, PIPE_DEF_IO_NS, def_cp, PIPE_DEF_CMD_NS, clean ? 3 : 2,
		                        clean ? 3 : 2, out, mint);
		if (!k && clean)
			k = pipe_plan_costs_min(n, PIPE_DEF_IO_NS, def_cp, PIPE_DEF_CMD_NS, 2, 2, out, mint);
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
		if ((uint64_t)n * cp <= cmd + (uint64_t)mint * cp + PIPE_HANDOFF_NS)
			return 0;
		k = pipe_plan_costs_min(n, io, cp, cmd, 2, PIPE_MAX_PARTS, out, mint);
		/* in one piece: one command and the whole copy, nothing to hand off */
		if (k >= 2 && (uint64_t)k * cmd + (uint64_t)out[k - 1] * cp + PIPE_HANDOFF_NS >=
		              cmd + (uint64_t)n * cp)
			k = 0;
		return k;
	}
}

static int pipe_plan(const Dev *d, uint32_t n, int clean, uint32_t out[PIPE_MAX_PARTS])
{
	return pipe_plan_min(d, n, clean, out, PIPE_MIN_TAIL);
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
	/* Cortex-A9 has no divide instruction (a library call, ~40 cycles): reads come in
	 * power-of-two sizes nearly always (the same result, a shift) */
	if (us < 4294967u && !(sectors & (sectors - 1u)))
		ns = us * 1000u >> __builtin_ctz(sectors);
	else
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
/* read ahead's and the again cache's copies the DMA controller makes whole,
 * done in the request's own thread, not on the copier (ra_copy_off) */
static volatile uint32_t g_dma_here, g_dma_here_kib;
/* ... and staged reads' (stage_xfer_one) */
static volatile uint32_t g_dma_here_st, g_dma_here_st_kib;
/* every read's copy out to the caller (copy_out), in sectors, and of it the
 * DMA controller's: counted like the cores' (COUNT_CORES) */
static volatile uint32_t g_cpo_sec, g_cpo_dma_sec;

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

	if (COUNT_CORES()) {                      /* the HUD: how much the controller made */
		ADD(g_cpo_sec, n >> 9);
		if (dma)
			ADD(g_cpo_dma_sec, n >> 9);
	}

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
	for (int i = 0; __atomic_load_n(dbusy(d), __ATOMIC_ACQUIRE); i++) {
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

	d->th = d->sem_go = d->sem_done = d->sem_free = -1;
	d->th2 = d->sem_cgo = d->sem_fin = -1;
	d->pipe_quit = 0;
	/* its buffer: already there if made at the start (prealloc) */
	if (!d->buf2 && (r = ensure_buf2(d)) < 0) {
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
static SceUID g_ra_fg;                    /* atomic (with the app events, below) */
/* The app in front and its title, changed together: process events come on
 * any thread, two at once too. Held for a copy of a few words at most. */
static volatile int g_fg_lock;

static void fg_lock(void)
{
	while (__sync_lock_test_and_set(&g_fg_lock, 1))
		ksceKernelDelayThread(10);
}

/* The app in front (0: none) and its title, as one. */
static SceUID fg_get(char title[16])
{
	SceUID pid;

	fg_lock();
	pid = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);
	memcpy(title, g_fg_title, 16);
	__sync_lock_release(&g_fg_lock);
	title[15] = 0;
	return pid;
}

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
	Diag *const dg = DG();
	const uintptr_t base = (uintptr_t)m->user;
	uint32_t i;
	MapFail *f;
	VmMemBlockInfoEx info;

	if (!dg)
		return;                                /* the log never on: not described */
	i = __atomic_fetch_add(&g_mapfail_n, 1, __ATOMIC_RELAXED);
	if (i >= MAPFAIL_LOG)
		return;
	f = &dg->mapfail[i];
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
	{
		char t[16];

		fg_get(t);
		memcpy(f->title, t, sizeof(f->title) - 1);
		f->title[sizeof(f->title) - 1] = 0;
	}
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
static volatile int g_ra_own;             /* its value is the app's own (log)  */
/* The app in front first (stage_xfer_one): another app's reads are sped up
 * as before, but read ahead is the app's alone - they neither use nor start
 * nor break its streams, nor drop its pieces (while they are live such a
 * read goes the standard way), nor take the tiny reads' block, nor count
 * for Adaptive or the calibration; a write of theirs no longer ends its
 * streams (the pieces it touches are still marked). For the log: */
static uint32_t g_bg_n, g_bg_kib, g_bg_kept;                    /* atomics */
static uint32_t g_ra_hits, g_ra_kib, g_ra_drops, g_ra_waste_kib, g_ra_bad;   /* atomics */
static uint32_t g_ra_nc_kib;              /* atomic (defined again with ra_nc_read) */
static uint32_t g_ra_tiny_reads;          /* atomic: tiny reads from their block */
static uint32_t g_ra_bad_why[5];          /* atomics: failed, read during a write, dirty, switched, old */
static uint32_t g_ra_wskip;               /* atomic: pieces not read, a write running */
static uint32_t g_ra_over;                /* atomic: reads overlapping the stream's end, their
                                           * head from the card, the rest from the pieces */
/* The app in front and how much it reads where (the per-app settings; the HUD). */
#define RA_FG_DEV_MIN     2048            /* sectors (1 MiB): the HUD tells its storage */
#define RA_THR_NONE       0xFFFFu         /* an app's threshold: never start   */
/* On ux0 (the memory card or SD2Vita), not the internal flash; older
 * versions' on ur0 move there as each app's is first read. */
#define RA_PROFILE_ROOT   "ux0:data/vita-memio"
#define RA_PROFILE_DIR    RA_PROFILE_ROOT "/ra"
#define RA_PROFILE_OLD    VM_DATA_DIR "/ra"
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
static char           g_fg_title[16];       /* its title id ("" none / not known) */
static uint32_t       g_fg_sec[VM_DEV_COUNT];   /* its reads per storage (atomics) */
static uint32_t       g_fg_noid;            /* atomic: processes with no title id (log) */
static void eff_update(void);
static SceUID g_app_pid;

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

static void ra_prof_save(const char *title, const RaProf *in);

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

/* Profiles, one at a time: the worker loads, the overlay saves (its own
 * thread) - a load meanwhile could read one half written. */
static volatile int g_prof_lock;

static void ra_prof_save_locked(const char *title, const RaProf *in);

/* 1 and the profile if the app has a usable one: its file (or the copy an
 * older version's save cut short left), else an older version's on ur0,
 * moved here. g_prof_lock held. */
static int ra_prof_load_locked(const char *title, RaProf *out)
{
	char path[96];
	int ok;

	ra_profile_path(path, sizeof(path), title);
	ok = ra_prof_read(path, out);
	if (!ok) {
		ra_profile_tmp(path, sizeof(path), title);
		ok = ra_prof_read(path, out);
	}
	if (!ok) {                            /* an older version's, on ur0: moved */
		snprintf(path, sizeof(path), "%s/%s.bin", RA_PROFILE_OLD, title);
		ok = ra_prof_read(path, out);
		if (ok) {
			RaProf back;

			ra_prof_save_locked(title, out);
			ra_profile_path(path, sizeof(path), title);
			if (ra_prof_read(path, &back)) {   /* there now: the old one goes */
				snprintf(path, sizeof(path), "%s/%s.bin", RA_PROFILE_OLD, title);
				ksceIoRemove(path);
				snprintf(path, sizeof(path), "%s/%s.tmp", RA_PROFILE_OLD, title);
				ksceIoRemove(path);
			}
		}
	}
	return ok;
}

static int ra_prof_load(const char *title, RaProf *out)
{
	int ok;

	while (__sync_lock_test_and_set(&g_prof_lock, 1))
		ksceKernelDelayThread(100);
	ok = ra_prof_load_locked(title, out);
	__sync_lock_release(&g_prof_lock);
	return ok;
}

/* An app's profile written: in place if it is there (one small write: no
 * copy aside, no rename, no removal - the file stays, all global or not),
 * else a new file. A write cut short leaves a file whose check fails: the
 * app then has the global settings until it is saved again. g_prof_lock
 * held. */
static void ra_prof_save_locked(const char *title, const RaProf *in)
{
	char path[96];
	RaProfileFile p;
	SceUID fd;
	int w;

	/* nothing out of range is ever written */
	if (!ra_app_ok(&in->app))
		return;
	ra_profile_path(path, sizeof(path), title);
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
	fd = ksceIoOpen(path, SCE_O_WRONLY, 0666);   /* there: in place, its size the same */
	if (fd < 0) {
		ksceIoMkdir("ux0:data", 0777);
		ksceIoMkdir(RA_PROFILE_ROOT, 0777);
		ksceIoMkdir(RA_PROFILE_DIR, 0777);
		fd = ksceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	}
	if (fd < 0) {
		VM_KLOGW("stage: profile for %s not saved (0x%08x)", title, (unsigned)fd);
		return;
	}
	w = ksceIoWrite(fd, &p, sizeof(p));
	ksceIoClose(fd);
	if (w != (int)sizeof(p))
		VM_KLOGW("stage: profile for %s not saved (short write)", title);
}

static void ra_prof_save(const char *title, const RaProf *in)
{
	while (__sync_lock_test_and_set(&g_prof_lock, 1))
		ksceKernelDelayThread(100);
	ra_prof_save_locked(title, in);
	__sync_lock_release(&g_prof_lock);
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

/* The home screen's own settings and what Adaptive learns there go by this
 * name (a title id has 9 characters: never this). */
#define HOME_TITLE "HOME"

/* The home screen and the system's own apps have settings of their own
 * (VM_KEY_SYSAPPS), as games do. */
static volatile int g_sysapps = VM_DEF_SYSAPPS;

/* `pid` (title `t`) is the app in front from now. Any thread. */
static void fg_take(SceUID pid, const char *t)
{
	SceUID fg;

	fg_lock();
	fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);
	if (pid != fg) {                      /* another app: its storage afresh */
		for (int i = 0; i < VM_DEV_COUNT; i++)
			__atomic_store_n(&g_fg_sec[i], 0, __ATOMIC_RELAXED);
		memcpy(g_fg_title, t, sizeof(g_fg_title) - 1);
		g_fg_title[sizeof(g_fg_title) - 1] = 0;
	}
	__atomic_store_n(&g_ra_fg, pid, __ATOMIC_RELEASE);
	__sync_lock_release(&g_fg_lock);
	if (pid != fg)
		ra_wake();                        /* the worker puts its settings in effect */
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
		if (fg && fg != pid && g_exclusive_fn && g_exclusive_fn(NULL) > 0) {
			return;
		}
		/* the home screen itself ("main") is in front when no app is; the
		 * other system apps are apps like games (VM_KEY_SYSAPPS) */
		if (ra_title_system(t) &&
		    (!LD(g_sysapps) || (t[0] == 'm' && t[1] == 'a' && t[2] == 'i' && t[3] == 'n')))
			return;
		fg_take(pid, t);
	} else if (ev == 3 || ev == 4) {
		int left;

		fg_lock();
		left = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE) == pid;
		if (left) {
			__atomic_store_n(&g_ra_fg, 0, __ATOMIC_RELEASE);
			g_fg_title[0] = 0;
		}
		__sync_lock_release(&g_fg_lock);
		if (left)
			ra_wake();                        /* the worker: the global settings again */
	}
}

static int ra_proc_event_hook(int pid, int ev, int a3, int a4, int *a5, int a6)
{
	const tai_hook_ref_t ref = vm_hook_ref(&g_proc_ref);

	ra_proc_event(pid, ev);
	return ref ? TAI_CONTINUE(int, ref, pid, ev, a3, a4, a5, a6) : 0;
}

/* Adaptive (VM_RA_ADAPTIVE): the threshold of each kind (0 cached, 1 NC),
 * within 2..8, and the 32 KiB reads' stream's of each kind, its own, within
 * 4..32 reads in fours (the counts of both, every threshold, weighed in one
 * pass: rt_scores). Read ahead's own streams are counted (ra_test.h): runs of
 * reads in a row, by length. Every RA_ADAPT_RUNS runs
 * of a kind, or RA_ADAPT_SECTORS read in them if that comes first (long runs:
 * a loading screen would be over before 32 of them; the first round of a game
 * smaller still, RA_ADAPT_*_FIRST), it works out what each threshold would have
 * brought over them (a read from the pieces saves RT_GAIN_*_US a sector, a
 * piece read for nothing costs RT_WASTE_US) and takes the best one at once -
 * if it is better than the one in effect by more than RA_ADAPT_MARGIN, so
 * a run or two either way moves nothing. Then the counts are halved: what
 * was read long ago weighs less. Seeing every threshold, not only the one in
 * effect, it goes down as readily as up. Never off: "never"
 * stays never. Back to where On has it when what is in effect changes
 * (eff_update: another app in front, a setting). */
#define RA_ADAPT_RUNS       32u
#define RA_ADAPT_SECTORS    6144u  /* 3 MiB                                    */
#define RA_ADAPT_RUNS_FIRST 8u     /* the first round, with nothing learned yet */
#define RA_ADAPT_SECTORS_FIRST 2048u   /* 1 MiB; a first round reads that at least */
#define RA_ADAPT_TINY_FIRST 64u    /* tiny reads a first round of them counts at least */
#define RA_ADAPT_ONE        16u    /* a run or a sector counts 16: halving keeps a rare one */
/* three pieces read for nothing, at what one costs here (measured, or the fixed) */
#define RA_ADAPT_MARGIN(cost) ((int64_t)RA_ADAPT_ONE * 3 * (cost)->waste_us)
#define RA_THR_LO         2u
#define RA_THR_HI         8u
static volatile int      g_ra_adj[2];
/* the 32 KiB reads' threshold, its own: what Adaptive adds to On's (reads) */
static volatile int      g_rs_adj[2];
static volatile uint32_t g_ra_moves[2], g_rs_moves[2];  /* threshold changes (log) */
static RaTest            g_ad;                           /* the runs (ra_test.h)          */
/* Tiny reads (ra_tiny_read): from which read in a row a block is read, and
 * its size, in effect with Adaptive (the fixed ones otherwise); the runs of
 * two reads or more by how many (rt_tiny_*), in sixteenths as above, and
 * weighed every RA_ADAPT_RUNS of them. */
static volatile uint32_t g_tiny_thr = RA_TINY_STREAK, g_tiny_blk = RA_TINY_SECTORS;
static uint32_t          g_tn_cnt[RT_TB], g_tn_reads[RT_TB], g_tn_sec[RT_TB], g_tn_ended;
/* a round's sectors so far, of each kind; rounds weighed since learning
 * started (0: the first round is the small one), each kind and the tiny reads */
static uint32_t          g_ad_sec[2], g_ad_rounds[2], g_tn_rounds;
/* Which Adaptive state the counts are for: moved on each time it starts
 * afresh (another app in front, a setting), so a stream still going from the
 * app before - the home screen's, say - is not counted for the one now in
 * front when it ends (it was: a game's first round moved on it). */
static volatile uint32_t g_ad_gen;
/* A move is taken only if it brings more than its band (rt_band: a share of
 * the reads counted), and waits for a second round in a row that wants it
 * the same way (ad_settle) - up at once only where the one in effect loses.
 * Where thresholds weigh much the same (the model near nothing) each round's
 * few new runs swung it back and forth: NC 2 -> 4 -> 2 -> 5 -> 3, 2 <-> 3,
 * 2 <-> 8. Rounds cut short by their sectors also age the counts less
 * (ra_adapt_age), so a round weighs more than the last second. Any move in
 * the first rounds is taken at once (a loading screen: quick, not settled
 * yet). (0: none pending) */
#define RA_ADAPT_SETTLE_FROM 2u
static int               g_ad_pend_thr[2], g_ad_pend_sm[2];
static volatile uint32_t g_tn_moves;
/* A round due, for the worker to weigh (under g_ad_lock), and the storage
 * whose figures it takes: the large reads' kinds, the tiny reads. */
static int               g_ad_due[2], g_ad_dev[2], g_tn_due, g_tn_dev;
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

/* The threshold in effect for a stream of kind nc. */
static uint32_t ra_threshold(int nc)
{
	uint32_t t;
	int v;

	t = ra_base_threshold(nc);
	if (t == RA_THR_NONE || LD(g_ra_mode) != VM_RA_ADAPTIVE)
		return t;
	v = (int)t + LD(g_ra_adj[nc]);
	return v < (int)RA_THR_LO ? RA_THR_LO : v > (int)RA_THR_HI ? RA_THR_HI : (uint32_t)v;
}

/* On's threshold of the 32 KiB reads' stream for a threshold t: 16 reads at
 * the standard one, else four times t - never under 16. */
static uint32_t ra_small_on(uint32_t t, int nc)
{
	const uint32_t own = t == (nc ? RA_STREAK_NC : RA_STREAK) ? RA_SMALL_STREAK : t * RA_SMALL_FACTOR;

	return own < RA_SMALL_STREAK ? RA_SMALL_STREAK : own;
}

/* The one in effect, given t = ra_threshold(nc): with Adaptive its own,
 * 4..32 reads in fours, from where On has it. */
static uint32_t ra_small_threshold(int nc, uint32_t t)
{
	int v;

	if (t == RA_THR_NONE)
		return t;
	if (LD(g_ra_mode) != VM_RA_ADAPTIVE)
		return ra_small_on(t, nc);
	v = (int)ra_small_on(ra_base_threshold(nc), nc) + LD(g_rs_adj[nc]);
	return v < RT_SMALL_LO * 4 ? RT_SMALL_LO * 4 : v > RT_SMALL_HI * 4 ? RT_SMALL_HI * 4 : (uint32_t)v;
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
#define CAL_VERSION      2u              /* 1: without the tiny reads' figures */

typedef struct CalDev {
	uint16_t plain[2][CAL_BUCKETS], served[2][CAL_BUCKETS];
	uint32_t n_plain[2], n_served[2];
	uint32_t waste_us, waste_n;
	/* tiny reads: one copied from the block, and a block read (its sectors) */
	uint32_t tiny_hit_us, tiny_hit_n;
	uint32_t tiny_blk_us, tiny_blk_sec, tiny_blk_n;
} CalDev;
#define CAL_DEV_V1       ((uint32_t)offsetof(CalDev, tiny_hit_us))   /* version 1's */
#define CAL_TINY_MAX_US  16000u
#define CAL_TINY_KEEP    4096u
#define CAL_MIN_TINY     32u             /* hits, and blocks, before they are used */

static CalDev        g_cal[VM_DEV_COUNT];
static volatile int  g_cal_on = 1;     /* Tuning: Auto until the file says Off */
static volatile int  g_cal_dirty;            /* measured since the last save */
static volatile int  g_cal_loaded;           /* set under g_tune_lock */

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

	if (!CAL_MEASURING())
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

/* Tiny reads: a sum and a count kept (the last CAL_TINY_KEEP or so). */
static void cal_mean_add(uint32_t *sum, uint32_t *n, uint32_t v, uint32_t *extra, uint32_t ex)
{
	uint32_t c = LD(*n) + 1;

	if (v > CAL_TINY_MAX_US)
		v = CAL_TINY_MAX_US;
	ST(*sum, LD(*sum) + v);
	if (extra)
		ST(*extra, LD(*extra) + ex);
	if (c >= CAL_TINY_KEEP) {
		ST(*sum, LD(*sum) >> 1);
		if (extra)
			ST(*extra, LD(*extra) >> 1);
		c >>= 1;
	}
	ST(*n, c);
	ST(g_cal_dirty, 1);
}

/* A tiny read copied from its block (us), or the block read for one (us, its
 * sectors). Calibration on only. */
static void cal_tiny_hit(Dev *d, uint32_t us)
{
	CalDev *c = &g_cal[d - g_dev];

	cal_mean_add(&c->tiny_hit_us, &c->tiny_hit_n, us, NULL, 0);
}

static void cal_tiny_blk(Dev *d, uint32_t us, uint32_t sectors)
{
	CalDev *c = &g_cal[d - g_dev];

	cal_mean_add(&c->tiny_blk_us, &c->tiny_blk_n, us, &c->tiny_blk_sec, sectors);
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
			if (v.n_plain[k] >= VM_CAL_MIN_PLAIN)   /* the band's scale (rt_band) */
				cost->piece_us[k] = (int32_t)cal_median(c->plain[k], v.n_plain[k]);
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

/* A 128 KiB read on storage dev into memory kind nc: the median measured (the
 * standard way), else RT_PIECE_US - what a piece takes to be read, for what
 * the game's pauses can cover of it (rt_hide_pm). */
static uint32_t cal_piece_us(int dev, int nc)
{
	if (dev >= 0 && dev < VM_DEV_COUNT) {
		const CalDev *c = &g_cal[dev];
		const uint32_t n = LD(c->n_plain[nc]);

		if (n >= VM_CAL_MIN_PLAIN)
			return cal_median(c->plain[nc], n);
	}
	return RT_PIECE_US;
}

void vm_stage_cal(int dev, VmCal *out)
{
	RtCost c;

	cal_cost(dev, &c, out);
}

/* What a tiny read costs on storage dev: a card read of n sectors a + b n,
 * from the measured block reads (mean, of their mean size) and a 128 KiB read
 * (median, non-cacheable, else cached); a read from the block, measured. The
 * fixed figures for what is not measured enough. */
static void cal_tiny_cost(int dev, RtTinyCost *t)
{
	*t = rt_tiny_fixed;
	if (dev < 0 || dev >= VM_DEV_COUNT)
		return;
	{
		const CalDev *c = &g_cal[dev];
		const uint32_t bn = LD(c->tiny_blk_n), hn = LD(c->tiny_hit_n);
		int k = -1;

		if (LD(c->n_plain[1]) >= VM_CAL_MIN_PLAIN)
			k = 1;
		else if (LD(c->n_plain[0]) >= VM_CAL_MIN_PLAIN)
			k = 0;
		if (bn >= CAL_MIN_TINY && k >= 0) {
			const int64_t big = cal_median(c->plain[k], LD(c->n_plain[k]));   /* 256 sectors */
			const int64_t fm = LD(c->tiny_blk_us) / bn, bm = LD(c->tiny_blk_sec) / bn;

			if (bm < 256 && big > fm) {
				const int64_t b16 = (big - fm) * 16 / (256 - bm);
				const int64_t a = big - b16 * 256 / 16;

				t->b16_us = (int32_t)b16;
				t->a_us = (int32_t)(a > 0 ? a : 0);
			}
		}
		if (hn >= CAL_MIN_TINY)
			t->hit_us = (int32_t)(LD(c->tiny_hit_us) / hn);
	}
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
	uint32_t n_plain[2], n_served[2], n_waste, n_tiny_hit, n_tiny_blk;
	RtCost   cost;
	RtTinyCost tiny;
	int      have[2], have_waste;
} CalSaved;
static CalSaved g_cal_saved[VM_DEV_COUNT];   /* worker */

static void cal_tiny_cost(int dev, RtTinyCost *t);

static void cal_snapshot(void)
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		CalSaved *o = &g_cal_saved[i];
		VmCal v;

		cal_cost(i, &o->cost, &v);
		cal_tiny_cost(i, &o->tiny);
		o->n_tiny_hit = LD(g_cal[i].tiny_hit_n);
		o->n_tiny_blk = LD(g_cal[i].tiny_blk_n);
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
		{                                 /* tiny reads: the same way */
			RtTinyCost t;

			cal_tiny_cost(i, &t);
			if (cal_moved(t.a_us, o->tiny.a_us) || cal_moved(t.b16_us, o->tiny.b16_us) ||
			    cal_moved(t.hit_us, o->tiny.hit_us) ||
			    cal_grown(LD(g_cal[i].tiny_hit_n), o->n_tiny_hit, CAL_MIN_TINY) ||
			    cal_grown(LD(g_cal[i].tiny_blk_n), o->n_tiny_blk, CAL_MIN_TINY))
				return 1;
		}
	}
	return 0;
}

/* tuning.bin: the calibration (below, after what Adaptive keeps). */
static void cal_save_locked(void);
static void cal_load(void);
static int  g_tune_cal_due;                    /* the calibration to be written (g_tune_lock) */
/* tuning.bin, one at a time: the worker, and the overlay's Tuning switch
 * and reset (its own thread). */
static volatile int g_tune_lock;

static void tune_lock(void)
{
	while (__sync_lock_test_and_set(&g_tune_lock, 1))
		ksceKernelDelayThread(100);
}

/* An older version's calib.bin into g_cal (the calibration alone); 1 if it
 * was whole and right (else g_cal cleared). Read once, if there is no
 * tuning.bin yet; the first save of tuning.bin removes it. */
static int cal_read_file(const char *path)
{
	const SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	CalHead h;
	uint32_t c = 0x811C9DC5u, check = 0;
	int ok;

	if (fd < 0)
		return 0;
	memset(g_cal, 0, sizeof(g_cal));
	ok = ksceIoRead(fd, &h, sizeof(h)) == (int)sizeof(h) && h.magic == CAL_MAGIC &&
	     (h.version == CAL_VERSION || h.version == 1u) && h.devs == VM_DEV_COUNT;
	c = cal_fnv(c, &h, sizeof(h));
	/* version 1: each storage's counts without the tiny reads' (those start at 0) */
	for (int i = 0; i < VM_DEV_COUNT && ok; i++) {
		const uint32_t sz = h.version == 1u ? CAL_DEV_V1 : (uint32_t)sizeof(CalDev);

		ok = ksceIoRead(fd, &g_cal[i], sz) == (int)sz;
		c = cal_fnv(c, &g_cal[i], sz);
	}
	ok = ok && ksceIoRead(fd, &check, sizeof(check)) == (int)sizeof(check);
	ksceIoClose(fd);
	if (ok) {
		c ^= 0xA5C3A5C3u;
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

static void tune_forget_learned(void);

static void cal_reset_now(void)
{
	cal_load();
	memset(g_cal, 0, sizeof(g_cal));      /* the figures: the fixed ones again */
	tune_lock();
	tune_forget_learned();                /* ... and what was learned with them */
	g_tune_cal_due = 1;
	cal_save_locked();
	__sync_lock_release(&g_tune_lock);
	VM_KLOGI("stage: calibration and what was learned reset");
}

static void cal_set_now(int on)
{
	cal_load();
	ST(g_cal_on, on ? 1 : 0);
	tune_lock();
	g_tune_cal_due = 1;
	cal_save_locked();                    /* the switch kept, and what was measured */
	__sync_lock_release(&g_tune_lock);
	VM_KLOGI("stage: calibration %s", on ? "on" : "off");
}

/* What Adaptive has learned of an app (thresholds and the runs behind
 * them) is held in memory for the app before the one in front, so the same
 * launch, back in front (from the home screen), goes on from where it was -
 * with what it was learned under (read ahead's mode, the app's own
 * thresholds): taken up again only under the same. Nothing of it is kept in
 * a file: every launch learns from the start. Only one app learns at a time:
 * the one in front. */
/* ---- tuning.bin: the calibration ------------------------------------------
 *
 * One file on ux0 (the memory card or SD2Vita, not the internal flash), so
 * a start goes on from what was measured rather than from nothing:
 *   TuneHead | TuneCal
 * each with a check of its own. Written in place when a figure in use
 * moved; the whole file once, when there is none (or not this layout). */
#define TUNE_DIR      "ux0:data/vita-memio"
#define TUNE_FILE     TUNE_DIR "/tuning.bin"
#define TUNE_OLD      VM_DATA_DIR "/tuning.bin"    /* v2, on ur0: removed */
#define TUNE_MAGIC    0x4E54564Du                  /* "MVTN" */
#define TUNE_VERSION  8u                           /* 1: whole each time, 2: 12 apps,
                                                    * 3: no on/off, 4: on/off by rounds,
                                                    * 5: the last state kept, 6: the start
                                                    * (their mean), 7: the start's
                                                    * launches kept, 8: the calibration
                                                    * alone */
#define TUNE_SLOT_V3  328u                         /* versions 3 to 7's slots: their */
#define TUNE_SLOT_V4  360u                         /* calibration is read            */
#define TUNE_SLOT_V5  368u
#define TUNE_SLOT_V6  504u                         /* (6 and 7: the same size)       */
#define TUNE_TITLE    12u                          /* TuneApp.title, its 0 too */

typedef struct {
	char     title[12];
	uint32_t key;
	int8_t   ra_adj[2], rs_adj[2];
	uint8_t  tiny_thr, tiny_blk, sh_ad, sh_tn;
	uint16_t pos[2][RT_POS], run[2][RT_RUN], spos[2][RT_POS], srun[2][RT_RUN];
	uint16_t tn_cnt[RT_TB], tn_reads[RT_TB], tn_sec[RT_TB];
	RtEv     oo[3];                      /* on or off, from what it did: as it was */
	int64_t  oo_time[3];
	RtModelStretch oo_ms[3];             /* the model's say over the reads' stretch */
	uint32_t oo_gap[2][RT_GAP_B];        /* the game's pauses */
} TuneApp;

typedef struct {
	uint32_t magic, version, devs, cal_size, slot_size, check;
} TuneHead;

#define TUNE_CAL_SIZE  ((uint32_t)(4u + sizeof(CalDev) * VM_DEV_COUNT + 4u))   /* on, devs, check */
#define TUNE_CAL_OFF   ((SceOff)sizeof(TuneHead))

static int      g_tune_ok;                     /* the file is there, in this layout */
static int      g_tune_legacy;                 /* 1 calib.bin was read, 2 a v2 on ur0: removed */

static uint32_t tune_fnv(uint32_t c, const void *p, uint32_t n)
{
	const uint8_t *b = (const uint8_t *)p;

	for (uint32_t i = 0; i < n; i++)
		c = (c ^ b[i]) * 0x01000193u;
	return c;
}

/* An app as it is held, packed. */
typedef struct {
	TuneApp  app;                        /* app.title "": none                 */
	SceUID   pid;                        /* the launch its state is of         */
} AdHeld;

static char              g_ad_title[16];      /* whose g_ad is (under g_ad_lock) */
static uint32_t          g_ad_key;
/* Held, one of each: the last game (only one is suspended at a time) and the
 * home screen (always there, pid 0) - so a game put away to the home screen,
 * through system apps or not, is not pushed out, and the home screen goes on
 * from where it was each time. The system apps (NPXS*) are not held: they
 * learn afresh each time in front. */
static AdHeld            g_ad_game;
static AdHeld            g_ad_home;
static SceUID            g_ad_pid;            /* the launch g_ad is of           */
static volatile uint32_t g_ad_restored;       /* taken up again (log)            */

/* On or off, as measured (below, after ra_adapt_on): its state. */
#define OO_C     0
#define OO_NC    1
#define OO_TINY  2
#define OO_MIN_SEC     256u              /* a round's reads to be timed: 128 KiB  */
#define OO_MIN_SEC_TINY 16u              /* ... tiny ones: 16 sectors              */
static RtEv         g_oo[3];
static volatile int g_oo_off[3];
static volatile int g_ra_fewer[2];       /* C, NC: one piece less ahead (rt_ev_pieces) */
/* the kind's reads since its last round (the hooks add, the worker takes):
 * the standard way and served (as RtEv), its pieces for nothing */
typedef struct {
	uint64_t std_us, srv_us, held_us;    /* a round can last long (no stream ends) */
	uint32_t std_q, srv_q, waste;
	uint32_t gap[RT_GAP_B];              /* sectors read after a pause of each step (RT_GAP_B) */
} OoAcc;
static OoAcc        g_oo_acc[3];         /* atomics */
static int64_t      g_oo_time[3];        /* the reads' time, aged as the counts */
static RtModelStretch g_oo_ms[3];        /* the model over the reads' stretch (rt_model_stretch) */
static uint32_t     g_oo_gap[2][RT_GAP_B];   /* the pauses kept (worker's), aged as the counts */
static void oo_clear(void);

/* Afresh (g_ad_lock held). */
static void ra_adapt_clear(void)
{
	memset(&g_ad, 0, sizeof(g_ad));
	for (int k = 0; k < 2; k++) {
		ST(g_ra_adj[k], 0);
		ST(g_rs_adj[k], 0);
	}
	memset(g_tn_cnt, 0, sizeof(g_tn_cnt));
	memset(g_tn_reads, 0, sizeof(g_tn_reads));
	memset(g_tn_sec, 0, sizeof(g_tn_sec));
	g_tn_ended = 0;
	memset(g_ad_sec, 0, sizeof(g_ad_sec));
	memset(g_ad_rounds, 0, sizeof(g_ad_rounds));
	memset(g_ad_pend_thr, 0, sizeof(g_ad_pend_thr));
	memset(g_ad_pend_sm, 0, sizeof(g_ad_pend_sm));
	ST(g_ad_gen, LD(g_ad_gen) + 1);       /* streams still going: not this state's */
	g_tn_rounds = 0;
	ST(g_ad_due[0], 0);
	ST(g_ad_due[1], 0);
	ST(g_tn_due, 0);
	ST(g_tiny_thr, RA_TINY_STREAK);
	ST(g_tiny_blk, RA_TINY_SECTORS);
	oo_clear();
}


static int tune_title_is(const TuneApp *t, const char *title)
{
	return t->title[0] && !strncmp(t->title, title, TUNE_TITLE - 1);
}

/* The shift that fits every count of `v` in 16 bits. */
static uint8_t tune_shift(const uint32_t *v, int n, uint8_t s)
{
	uint32_t m = 0;

	for (int i = 0; i < n; i++)
		m |= v[i];
	while ((m >> s) > 0xFFFFu)
		s++;
	return s;
}

static void tune_put(uint16_t *o, const uint32_t *v, int n, uint8_t s)
{
	for (int i = 0; i < n; i++)
		o[i] = (uint16_t)(v[i] >> s);
}

static void tune_get(uint32_t *o, const uint16_t *v, int n, uint8_t s)
{
	for (int i = 0; i < n; i++)
		o[i] = (uint32_t)v[i] << s;
}

#define TUNE_N(a) ((int)(sizeof(a) / sizeof(uint32_t)))


/* What the one in front learned, packed (g_ad_lock held). */
static void tune_pack(TuneApp *t)
{
	uint8_t s = 0;

	memset(t, 0, sizeof(*t));
	memcpy(t->title, g_ad_title, TUNE_TITLE - 1);
	t->key = g_ad_key;
	for (int i = 0; i < 2; i++) {
		t->ra_adj[i] = (int8_t)LD(g_ra_adj[i]);
		t->rs_adj[i] = (int8_t)LD(g_rs_adj[i]);
	}
	t->tiny_thr = (uint8_t)LD(g_tiny_thr);
	t->tiny_blk = (uint8_t)LD(g_tiny_blk);
	s = tune_shift(&g_ad.pos[0][0], TUNE_N(g_ad.pos), s);
	s = tune_shift(&g_ad.run[0][0], TUNE_N(g_ad.run), s);
	s = tune_shift(&g_ad.spos[0][0], TUNE_N(g_ad.spos), s);
	s = tune_shift(&g_ad.srun[0][0], TUNE_N(g_ad.srun), s);
	t->sh_ad = s;
	tune_put(&t->pos[0][0], &g_ad.pos[0][0], TUNE_N(g_ad.pos), s);
	tune_put(&t->run[0][0], &g_ad.run[0][0], TUNE_N(g_ad.run), s);
	tune_put(&t->spos[0][0], &g_ad.spos[0][0], TUNE_N(g_ad.spos), s);
	tune_put(&t->srun[0][0], &g_ad.srun[0][0], TUNE_N(g_ad.srun), s);
	s = 0;
	s = tune_shift(g_tn_cnt, RT_TB, s);
	s = tune_shift(g_tn_reads, RT_TB, s);
	s = tune_shift(g_tn_sec, RT_TB, s);
	t->sh_tn = s;
	tune_put(t->tn_cnt, g_tn_cnt, RT_TB, s);
	tune_put(t->tn_reads, g_tn_reads, RT_TB, s);
	tune_put(t->tn_sec, g_tn_sec, RT_TB, s);
	memcpy(t->oo, g_oo, sizeof(t->oo));  /* off stays off for the same launch */
	memcpy(t->oo_time, g_oo_time, sizeof(t->oo_time));
	memcpy(t->oo_ms, g_oo_ms, sizeof(t->oo_ms));
	memcpy(t->oo_gap, g_oo_gap, sizeof(t->oo_gap));
}

/* g_ad_lock held, after ra_adapt_clear: what t holds, in effect. */
static void tune_restore(const TuneApp *t)
{
	for (int i = 0; i < 2; i++) {
		ST(g_ra_adj[i], t->ra_adj[i]);
		ST(g_rs_adj[i], t->rs_adj[i]);
	}
	ST(g_tiny_thr, t->tiny_thr);
	ST(g_tiny_blk, t->tiny_blk);
	tune_get(&g_ad.pos[0][0], &t->pos[0][0], TUNE_N(g_ad.pos), t->sh_ad);
	tune_get(&g_ad.run[0][0], &t->run[0][0], TUNE_N(g_ad.run), t->sh_ad);
	tune_get(&g_ad.spos[0][0], &t->spos[0][0], TUNE_N(g_ad.spos), t->sh_ad);
	tune_get(&g_ad.srun[0][0], &t->srun[0][0], TUNE_N(g_ad.srun), t->sh_ad);
	tune_get(g_tn_cnt, t->tn_cnt, RT_TB, t->sh_tn);
	tune_get(g_tn_reads, t->tn_reads, RT_TB, t->sh_tn);
	tune_get(g_tn_sec, t->tn_sec, RT_TB, t->sh_tn);
	g_tn_ended = 0;
	g_ad_rounds[0] = g_ad_rounds[1] = g_tn_rounds = 1;   /* learned: full rounds */
	memcpy(g_oo, t->oo, sizeof(g_oo));   /* on or off as it was: an off stays off */
	memcpy(g_oo_time, t->oo_time, sizeof(g_oo_time));
	memcpy(g_oo_ms, t->oo_ms, sizeof(g_oo_ms));
	memcpy(g_oo_gap, t->oo_gap, sizeof(g_oo_gap));
	for (int u = 0; u < 3; u++)
		ST(g_oo_off[u], g_oo[u].off);
	for (int k = 0; k < 2; k++)
		ST(g_ra_fewer[k], g_oo[k].fewer);
	ADD(g_ad_restored, 1);
}

/* Where an app is held: the home screen's place, a game's, or none (a
 * system app: NPXS*). */
static AdHeld *ad_held_for(const char *title)
{
	if (!strcmp(title, HOME_TITLE))
		return &g_ad_home;
	return ra_title_system(title) ? NULL : &g_ad_game;
}

/* What is in effect is `title`'s (""; none: the home screen with no
 * settings of its own, the global settings) under `key`. The same as
 * before: Adaptive goes on learning. Else what it learned is held (in its
 * place, ad_held_for), and the same launch held under the same key goes on
 * from there - its thresholds, and read ahead on or off for each kind as it
 * was; anything else starts afresh. */
static void ra_adapt_switch_pid(const char *title, uint32_t key, SceUID pid_in)
{
	const AdHeld *h = NULL;
	const SceUID pid = title[0] ? pid_in : 0;

	while (__sync_lock_test_and_set(&g_ad_lock, 1))
		ksceKernelDelayThread(10);
	if (key == g_ad_key && !strncmp(title, g_ad_title, sizeof(g_ad_title))) {
		__sync_lock_release(&g_ad_lock);
		return;
	}
	if (g_ad_title[0] && (g_ad_key & 0xFu) == VM_RA_ADAPTIVE) {
		AdHeld *const o = ad_held_for(g_ad_title);

		if (o) {
			tune_pack(&o->app);
			o->pid = g_ad_pid;
		}
	}
	ra_adapt_clear();
	if (title[0] && ad_held_for(title) && tune_title_is(&ad_held_for(title)->app, title))
		h = ad_held_for(title);
	/* the same launch: its pid (the home screen's is always 0, and always the same) */
	if (h && h->app.key == key && h->pid == pid && (pid || !strcmp(title, HOME_TITLE)))
		tune_restore(&h->app);            /* the same launch back in front: where it was */
	g_ad_pid = pid;
	{
		size_t i = 0;

		for (; i < sizeof(g_ad_title) - 1 && title[i]; i++)
			g_ad_title[i] = title[i];
		memset(g_ad_title + i, 0, sizeof(g_ad_title) - i);
	}
	g_ad_key = key;
	__sync_lock_release(&g_ad_lock);
}

/* The same, the launch being the app in front (the tests). */
static void __attribute__((unused)) ra_adapt_switch(const char *title, uint32_t key)
{
	ra_adapt_switch_pid(title, key, __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE));
}

/* Afresh, whatever is in effect (the tests). */
static void __attribute__((unused)) ra_adapt_reset(void)
{
	while (__sync_lock_test_and_set(&g_ad_lock, 1))
		ksceKernelDelayThread(10);
	ra_adapt_clear();
	__sync_lock_release(&g_ad_lock);
}

/* What was learned, forgotten (Tuning reset): what is held, and the one in
 * front starts afresh. */
static void tune_forget_learned(void)
{
	while (__sync_lock_test_and_set(&g_ad_lock, 1))
		ksceKernelDelayThread(10);
	memset(&g_ad_game, 0, sizeof(g_ad_game));
	memset(&g_ad_home, 0, sizeof(g_ad_home));
	ra_adapt_clear();
	__sync_lock_release(&g_ad_lock);
	g_tune_ok = 0;
}

/* The calibration part, at the file's position (sequentially): 1 if written. */
static int tune_write_cal(SceUID fd)
{
	const uint32_t on = (uint32_t)LD(g_cal_on);
	uint32_t c = tune_fnv(0x811C9DC5u, &on, sizeof(on));
	int ok = ksceIoWrite(fd, &on, sizeof(on)) == (int)sizeof(on);

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
		one.tiny_hit_us = LD(g_cal[i].tiny_hit_us);
		one.tiny_hit_n = LD(g_cal[i].tiny_hit_n);
		one.tiny_blk_us = LD(g_cal[i].tiny_blk_us);
		one.tiny_blk_sec = LD(g_cal[i].tiny_blk_sec);
		one.tiny_blk_n = LD(g_cal[i].tiny_blk_n);
		ok = ksceIoWrite(fd, &one, sizeof(one)) == (int)sizeof(one);
		c = tune_fnv(c, &one, sizeof(one));
	}
	c |= 1u;
	return ok && ksceIoWrite(fd, &c, sizeof(c)) == (int)sizeof(c);
}

/* The header this build writes. */
static void tune_head_of(TuneHead *h, uint32_t version, uint32_t slot)
{
	const TuneHead h0 = { TUNE_MAGIC, version, VM_DEV_COUNT, TUNE_CAL_SIZE, slot, 0 };

	*h = h0;
	h->check = tune_fnv(0x811C9DC5u, &h0, sizeof(h0)) | 1u;
}

static void tune_head(TuneHead *h)
{
	tune_head_of(h, TUNE_VERSION, 0u);
}


/* A new file - the header and the calibration - when there is none in this
 * layout (an older one's app slots after it: cut off). */
static int tune_write_all(void)
{
	TuneHead h;
	SceUID fd;
	int ok;

	tune_head(&h);
	ksceIoMkdir("ux0:data", 0777);
	ksceIoMkdir(TUNE_DIR, 0777);
	fd = ksceIoOpen(TUNE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd < 0)
		return 0;
	ok = ksceIoWrite(fd, &h, sizeof(h)) == (int)sizeof(h) && tune_write_cal(fd);
	ksceIoClose(fd);
	if (!ok)
		return 0;
	g_tune_ok = 1;
	return 1;
}

/* The calibration, if it changed since the file was written, where it lies
 * (g_tune_lock held). */
static void cal_save_locked(void)
{
	SceUID fd;

	if (!__atomic_load_n(&g_cal_loaded, __ATOMIC_ACQUIRE))
		return;
	ST(g_cal_dirty, 0);
	if (!g_tune_ok) {                     /* none yet (or not this layout) */
		if (!tune_write_all())
			return;
		cal_snapshot();
		g_tune_cal_due = 0;
	}
	if (g_tune_legacy) {                  /* the older files: in this one now */
		if (g_tune_legacy & 1) {
			ksceIoRemove(CAL_FILE);
			ksceIoRemove(CAL_FILE ".tmp");
		}
		if (g_tune_legacy & 2)
			ksceIoRemove(TUNE_OLD);
		g_tune_legacy = 0;
	}
	if (!g_tune_cal_due)
		return;
	fd = ksceIoOpen(TUNE_FILE, SCE_O_WRONLY, 0666);   /* in place: no truncation */
	if (fd < 0) {
		g_tune_ok = 0;                    /* gone: anew, next time */
		return;
	}
	if (ksceIoLseek(fd, TUNE_CAL_OFF, SCE_SEEK_SET) == TUNE_CAL_OFF && tune_write_cal(fd)) {
		g_tune_cal_due = 0;
		cal_snapshot();                   /* what the file holds now */
	}
	ksceIoClose(fd);
}

/* tuning.bin's header and calibration. This layout: 1; an older one (3 to
 * 7, app slots after the calibration): 2, its calibration used if its own
 * check passes, the file written anew at the next save. */
static int tune_read(void)
{
	const SceUID fd = ksceIoOpen(TUNE_FILE, SCE_O_RDONLY, 0);
	TuneHead h, want, v3, v4, v5, v6, v7;
	uint32_t on = 0, c, check = 0;
	int ok, old;

	if (fd < 0)
		return 0;
	tune_head(&want);
	tune_head_of(&v3, 3u, TUNE_SLOT_V3);
	tune_head_of(&v4, 4u, TUNE_SLOT_V4);
	tune_head_of(&v5, 5u, TUNE_SLOT_V5);
	tune_head_of(&v6, 6u, TUNE_SLOT_V6);
	tune_head_of(&v7, 7u, TUNE_SLOT_V6);
	ok = ksceIoRead(fd, &h, sizeof(h)) == (int)sizeof(h);
	old = ok && (!memcmp(&h, &v3, sizeof(h)) || !memcmp(&h, &v4, sizeof(h)) ||
	             !memcmp(&h, &v5, sizeof(h)) || !memcmp(&h, &v6, sizeof(h)) ||
	             !memcmp(&h, &v7, sizeof(h)));
	if (!ok || (!old && memcmp(&h, &want, sizeof(h)))) {
		ksceIoClose(fd);
		return 0;
	}
	/* the calibration */
	ok = ksceIoRead(fd, &on, sizeof(on)) == (int)sizeof(on);
	c = tune_fnv(0x811C9DC5u, &on, sizeof(on));
	ok = ok && ksceIoRead(fd, g_cal, sizeof(g_cal)) == (int)sizeof(g_cal);
	c = tune_fnv(c, g_cal, sizeof(g_cal)) | 1u;
	ok = ok && ksceIoRead(fd, &check, sizeof(check)) == (int)sizeof(check) && check == c;
	for (int i = 0; i < VM_DEV_COUNT && ok; i++)
		for (int k = 0; k < 2; k++)
			if (g_cal[i].n_plain[k] >= CAL_KEEP || g_cal[i].n_served[k] >= CAL_KEEP)
				ok = 0;
	ksceIoClose(fd);
	if (ok) {
		ST(g_cal_on, on ? 1 : 0);
	} else {
		memset(g_cal, 0, sizeof(g_cal));
		g_tune_cal_due = 1;               /* spoilt: written right at the next save */
	}
	if (old)
		g_tune_cal_due = 1;               /* written anew, as this layout */
	return old ? 2 : 1;
}

/* Once, by the worker: what was measured before; with no tuning.bin in this
 * layout, an older version's calib.bin (the calibration alone), moved into a
 * new tuning.bin at the first save (and the branch's v2 on ur0 removed). */
static void cal_load(void)
{
	SceUID fd;

	tune_lock();
	if (g_cal_loaded) {
		__sync_lock_release(&g_tune_lock);
		return;
	}
	{
		const int r = tune_read();

		g_tune_ok = r == 1;               /* 2: an older layout, its calibration kept: anew */
		if (!r && (cal_read_file(CAL_FILE) || cal_read_file(CAL_FILE ".tmp")))
			g_tune_legacy |= 1;
	}
	fd = ksceIoOpen(TUNE_OLD, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoClose(fd);
		g_tune_legacy |= 2;
	}
	cal_snapshot();                       /* what the file holds (or nothing) */
	__atomic_store_n(&g_cal_loaded, 1, __ATOMIC_RELEASE);
	__sync_lock_release(&g_tune_lock);
}

/* What tiny reads use now: Adaptive's (T, B), else the fixed ones (On). */
static uint32_t tiny_thr(void)
{
	return LD(g_ra_mode) == VM_RA_ADAPTIVE ? LD(g_tiny_thr) : RA_TINY_STREAK;
}

static uint32_t tiny_blk(void)
{
	return LD(g_ra_mode) == VM_RA_ADAPTIVE ? LD(g_tiny_blk) : RA_TINY_SECTORS;
}

static void cal_tiny_cost(int dev, RtTinyCost *t);

/* A run of tiny reads has ended: `reads` in a row, `sectors` in all; every
 * RA_ADAPT_RUNS of them (of two reads or more), (T, B) weighed again with
 * this storage's figures - moved if another is better by more than three
 * blocks read for one read. Adaptive only (another thread at it: lost). */
static void ra_tiny_end(const Dev *d, uint32_t reads, uint32_t sectors)
{
	int b;

	if (reads < 2 || LD(g_ra_mode) != VM_RA_ADAPTIVE || LD(g_verify) ||
	    __sync_lock_test_and_set(&g_ad_lock, 1))
		return;
	if (d->t_gen != g_ad_gen) {           /* begun for the app before: not counted */
		__sync_lock_release(&g_ad_lock);
		return;
	}
	b = rt_tiny_bucket(reads);
	g_tn_cnt[b] += RA_ADAPT_ONE;
	g_tn_reads[b] += reads * RA_ADAPT_ONE;
	g_tn_sec[b] += sectors * RA_ADAPT_ONE;
	/* a round's worth: weighed by the worker (ra_adapt_tick) */
	if (++g_tn_ended >= (g_tn_rounds && !g_oo[OO_TINY].off ? RA_ADAPT_RUNS : RA_ADAPT_RUNS_FIRST) &&
	    !LD(g_tn_due)) {
		ST(g_tn_due, 1);
		g_tn_dev = (int)(d - g_dev);
		__sync_lock_release(&g_ad_lock);
		ra_wake();
		return;
	}
	__sync_lock_release(&g_ad_lock);
}

/* What read ahead's own streams did, for Adaptive: counted where read ahead
 * counts them (ra_note), so both see the same runs. Only while Adaptive is in
 * effect (another thread at it: lost). */
static int ra_adapt_on(void)
{
	return LD(g_ra_mode) == VM_RA_ADAPTIVE;
}

/* Whether Adaptive learns now: as in effect, but not while Verify transfers
 * is on - it reads every transfer again (read ahead's served reads too), so
 * every time measured then is not what it is. What was learned stays in
 * effect meanwhile; nothing is counted, moved or switched. */
static int ra_learn_on(void)
{
	return ra_adapt_on() && !LD(g_verify);
}

/* ---- on or off, from what read ahead did (ra_test.h: RtEv) ----------------
 * For each kind - the large and 32 KiB reads into ordinary memory (OO_C),
 * into non-cacheable memory (OO_NC), the tiny reads (OO_TINY) - the state
 * (g_oo, under g_ad_lock: the worker's), the way in effect (g_oo_off: the
 * hooks look), and the kind's reads since its last round (g_oo_acc: the hooks
 * add a few words, the worker takes them - all else is the worker's, on the
 * system core). Only with Adaptive in effect. */
/* (OO_*, g_oo ...: with ra_adapt_clear, above) */

/* A read of kind u (< 0: none: device memory) done in `us`; `how`: 0 the
 * standard way, else served (all or partly; tiny: from the block, or the
 * block read for it). Hot path. */
static void oo_note(int u, int how, uint32_t nsec, uint32_t us)
{
	OoAcc *a;

	if (u < 0)
		return;
	a = &g_oo_acc[u];
	if (how) {
		ADD(a->srv_us, us);
		ADD(a->srv_q, u == OO_TINY ? 1u : nsec);
	} else {
		ADD(a->std_us, us);
		ADD(a->std_q, u == OO_TINY ? 1u : nsec);
	}
}

/* A piece read for nothing (kind u): what it held up. */
static void oo_waste(int u, uint32_t held_us)
{
	ADD(g_oo_acc[u].waste, 1);
	ADD(g_oo_acc[u].held_us, held_us);
}

/* A read of `nsec` sectors into memory kind nc (0, 1) that started at t0 and
 * took `us`, for a kind that is off (the caller looks): if it goes on a
 * stream, the pause before it is kept. The two
 * clocks are the on/off measure's own - nothing is read for this. Racing
 * threads at most blur a pause (a lost update). */
static void gap_note(Dev *d, int nc, uint32_t nsec, uint32_t t0, uint32_t us)
{
	const uint32_t last = LD(d->gap_end[nc]);
	uint32_t gap;

	ST(d->gap_end[nc], t0 + us);          /* a lost update only blurs a pause */
	if (!last || (LD(d->ra_streak) < 2 && LD(d->rs_streak) < 2))
		return;                              /* not on a stream: not what read ahead would cover */
	gap = t0 - last;
	if ((int32_t)gap < 0)
		gap = 0;
	ADD(g_oo_acc[nc].gap[rt_gap_bucket(gap)], nsec);
}

/* The worker (g_ad_lock held): kind nc's pauses of the round taken into what is
 * kept, the part of a piece's read they cover (`piece_us`), and the kept ones
 * aged as the counts are. Only a kind that is off keeps any: for one that is
 * on nothing is measured and what is left is dropped, so that each stay off
 * starts from the game as it is then. 0 until there is enough to say (the
 * kind is not turned on from nothing); `known` says which. */
static uint32_t oo_hide(int nc, uint32_t piece_us, int *known)
{
	uint32_t pm = 0;

	*known = 0;
	if (!g_oo[nc].off) {
		for (uint32_t b = 0; b < RT_GAP_B; b++)
			__atomic_store_n(&g_oo_acc[nc].gap[b], 0, __ATOMIC_RELAXED);
		memset(g_oo_gap[nc], 0, sizeof(g_oo_gap[nc]));
		return 1000;
	}
	for (uint32_t b = 0; b < RT_GAP_B; b++) {
		const uint32_t add = __atomic_exchange_n(&g_oo_acc[nc].gap[b], 0, __ATOMIC_RELAXED);

		g_oo_gap[nc][b] = (g_oo_gap[nc][b] > 0xFFFFFFFFu - add ? 0xFFFFFFFFu : g_oo_gap[nc][b] + add);
	}
	*known = rt_hide_known(g_oo_gap[nc]);
	if (*known)
		pm = rt_hide_pm(g_oo_gap[nc], piece_us);
	for (uint32_t b = 0; b < RT_GAP_B; b++)
		g_oo_gap[nc][b] -= g_oo_gap[nc][b] / 8;
	return pm;
}

/* Whether read ahead runs for kind u now. Hot path. */
static int oo_off(int u)
{
	return LD(g_oo_off[u]) && ra_adapt_on();
}

/* All afresh (g_ad_lock held): read ahead on for every kind, nothing kept. */
static void oo_clear(void)
{
	memset(g_oo, 0, sizeof(g_oo));
	memset(g_oo_time, 0, sizeof(g_oo_time));
	memset(g_oo_ms, 0, sizeof(g_oo_ms));
	ST(g_ra_fewer[0], 0);
	ST(g_ra_fewer[1], 0);
	for (int u = 0; u < 3; u++) {
		ST(g_oo_off[u], 0);
		ST(g_oo_acc[u].std_us, 0);
		ST(g_oo_acc[u].std_q, 0);
		ST(g_oo_acc[u].srv_us, 0);
		ST(g_oo_acc[u].srv_q, 0);
		ST(g_oo_acc[u].held_us, 0);
		ST(g_oo_acc[u].waste, 0);
		for (uint32_t b = 0; b < RT_GAP_B; b++)
			ST(g_oo_acc[u].gap[b], 0);
	}
	memset(g_oo_gap, 0, sizeof(g_oo_gap));
}

/* For the log: a round's look - the model's permille, what the reads said
 * (net, us, over the kind's time), the game's costs, the way now. */
typedef struct {
	int pm, pm_now, now, fewer, fewer_moved;
	int64_t net, time;
	uint32_t std, srv, held, hide, waste_pm;
} OoLog;

/* The worker, a round of kind u over (g_ad_lock held): `gain` what the model
 * (with this game's costs) says read ahead brings, the counts' scale. 1 if
 * it went on or off. */
static int oo_round(int u, int64_t gain, OoLog *lg)
{
	OoAcc a;
	int moved;
	int64_t pm, pml, t;
	uint32_t su, vu, hu;

	a.std_us = __atomic_exchange_n(&g_oo_acc[u].std_us, 0, __ATOMIC_RELAXED);
	a.std_q = __atomic_exchange_n(&g_oo_acc[u].std_q, 0, __ATOMIC_RELAXED);
	a.srv_us = __atomic_exchange_n(&g_oo_acc[u].srv_us, 0, __ATOMIC_RELAXED);
	a.srv_q = __atomic_exchange_n(&g_oo_acc[u].srv_q, 0, __ATOMIC_RELAXED);
	a.held_us = __atomic_exchange_n(&g_oo_acc[u].held_us, 0, __ATOMIC_RELAXED);
	a.waste = __atomic_exchange_n(&g_oo_acc[u].waste, 0, __ATOMIC_RELAXED);
	/* a round's times into 32 bits (a round of over an hour: as much
	 * kept of it as fits, the rates its own) */
	while (a.std_us > 0x3FFFFFFFu || a.srv_us > 0x3FFFFFFFu || a.held_us > 0x3FFFFFFFu) {
		a.std_us >>= 1;
		a.srv_us >>= 1;
		a.held_us >>= 1;
		a.std_q >>= 1;
		a.srv_q >>= 1;
		a.waste >>= 1;
	}
	su = (uint32_t)a.std_us;
	vu = (uint32_t)a.srv_us;
	hu = (uint32_t)a.held_us;
	g_oo_time[u] += ((int64_t)su + vu) * RA_ADAPT_ONE;
	t = g_oo_time[u] > 0 ? g_oo_time[u] : 1;
	pm = rt_pm_div(gain * 1000, t + (g_oo[u].off || gain < 0 ? 0 : gain));
	pml = rt_model_stretch(&g_oo_ms[u], gain, ((int64_t)su + vu) * RA_ADAPT_ONE, g_oo[u].off);
	/* back on if either says it brings (now, or over the stretch while off):
	 * a wrong off costs more than a wrong on */
	moved = rt_ev_round(&g_oo[u], u == OO_TINY, su, a.std_q, vu, a.srv_q, hu, a.waste,
	                    pm > pml ? pm : pml, &lg->net, &lg->time);
	if (moved)
		g_oo_ms[u].gain = g_oo_ms[u].time = 0;   /* the stretch from the change */
	g_oo_time[u] /= 2;                   /* aged as the counts are */
	ST(g_oo_off[u], g_oo[u].off);
	/* C, NC: how many pieces ahead, from how many it read for nothing (the
	 * share logged is what it was judged on: the counts restart at a change) */
	lg->fewer_moved = 0;
	lg->waste_pm = 0;
	if (u != OO_TINY) {
		const uint64_t w = (uint64_t)g_oo[u].pw * RA_PIECE_SECTORS;

		lg->waste_pm = w + g_oo[u].ps ? (uint32_t)(w * 1000 / (w + g_oo[u].ps)) : 0;
		lg->fewer_moved = rt_ev_pieces(&g_oo[u]);
		ST(g_ra_fewer[u], g_oo[u].fewer);
	}
	lg->fewer = g_oo[u].fewer;
	lg->pm = (int)pml;
	lg->pm_now = (int)pm;
	lg->now = g_oo[u].off;
	lg->std = g_oo[u].std_rate;
	lg->srv = g_oo[u].srv_rate;
	lg->held = g_oo[u].held_each;
	return moved;
}

/* A stream that goes on while its kind is off: what it read so far is taken into
 * the counts (as ra_adapt_end does, but no run ended: its length is counted when
 * it does), and a round is due once a first one's worth is in. So the way back on
 * does not wait for the end of a stream that may last minutes. Called once the
 * stream has RA_ADAPT_SECTORS_FIRST (1 MiB) of its own to give, so about once a
 * MiB read: one try-lock and ten adds on the reader's thread. The lock busy: the
 * counts are kept and it is tried again with the next read. Only for a kind that
 * is off - for one that is on, nothing changes. */
static void ra_adapt_flush(Dev *d, int small, int nc)
{
	uint32_t *pos;

	if (!ra_learn_on() || __sync_lock_test_and_set(&g_ad_lock, 1))
		return;
	if (d->ad_gen[small] != g_ad_gen) {   /* the app before's: not counted */
		memset(d->ad_pos[small], 0, sizeof(d->ad_pos[small]));
		d->ad_sec[small] = 0;
		__sync_lock_release(&g_ad_lock);
		return;
	}
	pos = small ? g_ad.spos[nc] : g_ad.pos[nc];
	for (int i = 0; i < RT_POS; i++)
		pos[i] += d->ad_pos[small][i];
	memset(d->ad_pos[small], 0, sizeof(d->ad_pos[small]));
	g_ad.total_sectors += d->ad_sec[small];
	g_ad_sec[nc] += d->ad_sec[small];
	d->ad_sec[small] = 0;
	if (g_ad_sec[nc] >= RA_ADAPT_SECTORS_FIRST && !LD(g_ad_due[nc])) {
		ST(g_ad_due[nc], 1);
		g_ad_dev[nc] = (int)(d - g_dev);
		__sync_lock_release(&g_ad_lock);
		ra_wake();
		return;
	}
	__sync_lock_release(&g_ad_lock);
}

/* A read of `n` sectors, the `at`-th in its stream (`small`: 32 KiB reads):
 * kept with the stream until it ends. No lock: the stream's own thread. */
static void ra_adapt_read(Dev *d, int small, uint32_t at, uint32_t n)
{
	if (!ra_learn_on())
		return;
	if (!d->ad_sec[small])
		d->ad_gen[small] = LD(g_ad_gen);  /* counted for the state in effect now */
	if (small) {
		const uint32_t b = (at - 1) / 4;

		d->ad_pos[1][b < RT_POS ? b : RT_POS - 1] += n * RA_ADAPT_ONE;
	} else {
		const uint32_t have = RT_PIECES * RT_PIECE_SECTORS;

		d->ad_pos[0][at < RT_POS ? at : RT_POS - 1] += (n < have ? n : have) * RA_ADAPT_ONE;
	}
	d->ad_sec[small] += n;
	/* a kind that is off: a long stream is the very case where read ahead would pay,
	 * and it is counted only when it ends - see ra_adapt_flush */
	if (d->ad_sec[small] >= RA_ADAPT_SECTORS_FIRST) {
		const int nc = small ? d->rs_nc : d->ra_nc;

		if (nc < 2 && oo_off(nc ? OO_NC : OO_C))
			ra_adapt_flush(d, small, nc);
	}
}

/* A stream of `len` reads has ended: its reads and its length counted; every
 * RA_ADAPT_RUNS of a kind (or RA_ADAPT_SECTORS read in them), its thresholds
 * weighed again. Not counted (and dropped) while Adaptive is not in effect or
 * another thread is at the counts. */
static void ra_adapt_end(Dev *d, int small, int nc, uint32_t len)
{
	const int ok = len && ra_learn_on() && !__sync_lock_test_and_set(&g_ad_lock, 1);
	uint32_t *pos;

	if (!ok || d->ad_gen[small] != g_ad_gen) {   /* (the app before's run: not counted) */
		memset(d->ad_pos[small], 0, sizeof(d->ad_pos[small]));
		d->ad_sec[small] = 0;
		if (ok)
			__sync_lock_release(&g_ad_lock);
		return;
	}
	nc = !!nc;
	pos = small ? g_ad.spos[nc] : g_ad.pos[nc];
	for (int i = 0; i < RT_POS; i++)
		pos[i] += d->ad_pos[small][i];
	memset(d->ad_pos[small], 0, sizeof(d->ad_pos[small]));
	g_ad.total_sectors += d->ad_sec[small];
	g_ad_sec[nc] += d->ad_sec[small];
	d->ad_sec[small] = 0;
	if (small) {
		const uint32_t b = len / 4;

		g_ad.srun[nc][b < RT_RUN ? b : RT_RUN - 1] += RA_ADAPT_ONE;
	} else {
		g_ad.run[nc][len < RT_RUN ? len : RT_RUN - 1] += RA_ADAPT_ONE;
	}
	/* a round's worth: weighed by the worker (ra_adapt_tick), not here */
	/* a kind that is off gets a round as soon as a first one would: the way back
	 * should not wait for a full round of what the game reads little of */
	if ((++g_ad.ended[nc] >= (g_ad_rounds[nc] && !g_oo[nc ? OO_NC : OO_C].off ? RA_ADAPT_RUNS : RA_ADAPT_RUNS_FIRST) ||
	     g_ad_sec[nc] >= (g_ad_rounds[nc] && !g_oo[nc ? OO_NC : OO_C].off ? RA_ADAPT_SECTORS : RA_ADAPT_SECTORS_FIRST)) &&
	    !LD(g_ad_due[nc])) {
		ST(g_ad_due[nc], 1);
		g_ad_dev[nc] = (int)(d - g_dev);
		__sync_lock_release(&g_ad_lock);
		ra_wake();
		return;
	}
	__sync_lock_release(&g_ad_lock);
}

/* What a round's pick `want` leaves in effect from `cur` (*pend: what a
 * round before wanted, 0 none; `lose`: the one in effect costs more than it
 * saves over the counts). Higher where it loses is taken at once - the
 * game's reads first. Any other move waits for a second round in a row that
 * wants it the same way, and then takes the smaller of the two: a large jump
 * up on a round's say alone went 2 -> 8 -> 2 where the model said nearly
 * nothing either way (PCSF00024, PCSF00007, GTA at +131 permille). A round
 * that keeps it as it is clears what is pending. */
static int ad_settle(int cur, int want, int *pend, int lose)
{
	const int up = want > cur, was = *pend ? (*pend > cur ? 1 : -1) : 0;

	if (want == cur) {
		*pend = 0;
		return cur;
	}
	if (up && lose) {
		*pend = 0;
		return want;
	}
	if (was == (up ? 1 : -1)) {
		const int w = up ? (want < *pend ? want : *pend) : (want > *pend ? want : *pend);

		*pend = 0;
		return w;
	}
	*pend = want;
	return cur;
}

/* The tiny reads counted for the round (g_ad_lock held). */
static uint32_t tn_reads_counted(void)
{
	uint32_t reads = 0;

	for (int i = 0; i < RT_TB; i++)
		reads += g_tn_reads[i] / RA_ADAPT_ONE;
	return reads;
}

/* Worker thread: the rounds that are due weighed - the threshold of each
 * kind of the large reads, (T, B) of the tiny ones - so a request only counts. */
static void ra_adapt_tick(void)
{
	/* what moved, logged once the lock is let go (log on) */
	int mv[2][4], tmv[4], oo_mv[3] = { 0, 0, 0 }, any = 0;
	OoLog ol[3];
	int ol_on[3] = { 0, 0, 0 };
	uint32_t hide[2] = { 1000, 1000 };
	int hide_known[2] = { 0, 0 };

	if (!LD(g_ad_due[0]) && !LD(g_ad_due[1]) && !LD(g_tn_due))
		return;                           /* (a stale look is caught next time) */
	memset(mv, 0, sizeof(mv));
	memset(tmv, 0, sizeof(tmv));
	while (__sync_lock_test_and_set(&g_ad_lock, 1))
		ksceKernelDelayThread(10);
	for (int nc = 0; nc < 2; nc++) {
		const uint32_t base = ra_base_threshold(nc), t = ra_threshold(nc);
		RtCost cost, ocost;
		int thr = (int)t, sm;

		if (!LD(g_ad_due[nc]))
			continue;
		ST(g_ad_due[nc], 0);
		/* the first round, from runs alone that read too little (a few short
		 * runs: a menu, not a game's reading): nothing moved on so little -
		 * the runs kept, the round waits for more */
		if (!g_ad_rounds[nc] && g_ad_sec[nc] < RA_ADAPT_SECTORS_FIRST) {
			g_ad.ended[nc] = 0;
			continue;
		}
		cal_cost(g_ad_dev[nc], &cost, NULL);   /* that storage's, if measured */
		{
			RtCost con = cost;

			rt_ev_cost(&g_oo[nc], nc, &con, &cost);   /* ... and this game's, where measured */
			cost.pieces = g_oo[nc].fewer ? RA_PIECES - g_oo[nc].fewer : 0;   /* as many ahead as it keeps */
		}
		/* what the game's pauses let a piece save, for a kind that is off only (for one
		 * that is on nothing is measured or looked up: oo_hide just drops what is kept).
		 * The device's figure scaled by it is for the on/off say alone (ocost): the
		 * thresholds below are learned from the unscaled one, as before */
		ocost = cost;
		hide[nc] = oo_hide(nc, g_oo[nc].off ? cal_piece_us(g_ad_dev[nc], nc) : 0, &hide_known[nc]);
		if (g_oo[nc].off)
			rt_hide_apply(&ocost, nc, hide[nc]);
		sm = (int)(ra_small_threshold(nc, t) / RA_SMALL_FACTOR);
		/* both sizes' thresholds, each on its own, in one pass: those to use on,
		 * or - for a kind that is off - those it would come back with, weighed
		 * only for whether it should (nothing moved while it is off) */
		if (ra_learn_on() && t != RA_THR_NONE) {
			const int sm0 = sm, was_off = g_oo[nc].off;
			int lose = 0;

			ra_adapt_pick(&g_ad, nc, &thr, &sm, RA_ADAPT_MARGIN(&cost), &cost, &lose);
			if (was_off) {
				g_ad_pend_thr[nc] = g_ad_pend_sm[nc] = 0;
			} else if (g_ad_rounds[nc] >= RA_ADAPT_SETTLE_FROM) {   /* a second round */
				thr = ad_settle((int)t, thr, &g_ad_pend_thr[nc], lose & 1);
				sm = ad_settle(sm0, sm, &g_ad_pend_sm[nc], lose & 2);
			}
			/* then on or off, from what it did (the model's say for off) */
			ol_on[nc] = 1;
			ol[nc].hide = hide_known[nc] ? hide[nc] : 0xFFFFFFFFu;     /* not measured: on, or too little */
			if (oo_round(nc, ra_adapt_gain(&g_ad, nc, thr, sm, &ocost), &ol[nc])) {
				oo_mv[nc] = 1;
				any = 1;
				for (int i = 0; i < VM_DEV_COUNT; i++)
					ST(g_dev[i].gap_end[nc], 0);    /* its pauses start from the next read */
			}
			if (ol[nc].fewer_moved)
				any = 1;
			/* the thresholds: in effect only for a kind that is on (back on: what
			 * this round found) */
			if (!g_oo[nc].off && (thr != (int)t || sm != sm0)) {
				mv[nc][0] = (int)t;
				mv[nc][1] = thr;
				mv[nc][2] = sm0 * RA_SMALL_FACTOR;
				mv[nc][3] = sm * RA_SMALL_FACTOR;
				any = 1;
				if (thr != (int)t) {
					ST(g_ra_adj[nc], thr - (int)base);
					ADD(g_ra_moves[nc], 1);
				}
				if (sm != sm0) {
					ST(g_rs_adj[nc], sm * RA_SMALL_FACTOR - (int)ra_small_on(base, nc));
					ADD(g_rs_moves[nc], 1);
				}
			}
		}
		ra_adapt_age(&g_ad, nc, RA_ADAPT_RUNS);
		g_ad_sec[nc] = 0;
		g_ad_rounds[nc]++;
	}
	if (LD(g_tn_due) && !g_tn_rounds && tn_reads_counted() < RA_ADAPT_TINY_FIRST) {
		ST(g_tn_due, 0);                  /* the first round: too few reads yet - wait for more */
		g_tn_ended = 0;
	} else if (LD(g_tn_due)) {
		RtTinyCost c;
		int t = (int)LD(g_tiny_thr), blk = (int)LD(g_tiny_blk);

		ST(g_tn_due, 0);
		cal_tiny_cost(g_tn_dev, &c);
		tmv[0] = t;
		tmv[1] = blk;
		/* as the large reads: (T, B) to use on, or to come back with - moved
		 * only while on */
		if (ra_learn_on()) {
			const int picked = rt_tiny_pick(g_tn_cnt, g_tn_reads, g_tn_sec, &t, &blk,
			                                (int64_t)RA_ADAPT_ONE * 3 *
			                                (rt_tiny_card(&c, (uint32_t)blk) - rt_tiny_card(&c, 1)), &c);

			ol_on[OO_TINY] = 1;
			if (oo_round(OO_TINY, rt_tiny_score(g_tn_cnt, g_tn_reads, g_tn_sec, t, blk, &c),
			             &ol[OO_TINY])) {
				oo_mv[OO_TINY] = 1;
				any = 1;
			}
			if (picked && !g_oo[OO_TINY].off) {
				ST(g_tiny_thr, (uint32_t)t);
				ST(g_tiny_blk, (uint32_t)blk);
				ADD(g_tn_moves, 1);
				tmv[2] = t;
				tmv[3] = blk;
				any = 1;
			}
		}
		for (int i = 0; i < RT_TB; i++) {
			g_tn_cnt[i] >>= 1;
			g_tn_reads[i] >>= 1;
			g_tn_sec[i] >>= 1;
		}
		g_tn_ended = 0;
		g_tn_rounds++;
	}
	__sync_lock_release(&g_ad_lock);
	/* a round's line only when it changed something of its kind (the log is
	 * on the internal storage: as few lines as can be); the minute's summary
	 * has where things stand */
	if (LD(g_log_diag))
		for (int u = 0; u < 3; u++)
			if (ol_on[u] && (oo_mv[u] || ol[u].fewer_moved ||
			                 (u == OO_TINY ? tmv[2] != 0
			                               : mv[u][0] != mv[u][1] || mv[u][2] != mv[u][3]))) {
				char hb[40] = "";

				if (u != OO_TINY && ol[u].hide != 0xFFFFFFFFu)
					snprintf(hb, sizeof(hb), ", pauses cover %u permille", (unsigned)ol[u].hide);
				VM_KLOGI("round %s: model %d permille (now %d); reads net %d ms of %u ms (standard %u, "
				         "R/A %u %s, piece for nothing %u us%s); %s",
				         u == OO_TINY ? "tiny" : u ? "NC" : "C", ol[u].pm, ol[u].pm_now,
				         (int)(ol[u].net / 1000), (unsigned)(ol[u].time / 1000),
				         (unsigned)ol[u].std, (unsigned)ol[u].srv,
				         u == OO_TINY ? "us a read" : "us/MiB", (unsigned)ol[u].held, hb,
				         ol[u].now ? "off" : "on");
			}
	if (any && LD(g_log_diag)) {
		for (int nc = 0; nc < 2; nc++)
			if (mv[nc][0] != mv[nc][1] || mv[nc][2] != mv[nc][3])
				VM_KLOGI("adaptive: %s from read %d -> %d, 32K from %d -> %d", nc ? "NC" : "C",
				         mv[nc][0], mv[nc][1], mv[nc][2], mv[nc][3]);
		if (tmv[2])
			VM_KLOGI("adaptive: tiny from read %d -> %d, block %d -> %d KiB", tmv[0], tmv[2],
			         tmv[1] / 2, tmv[3] / 2);
		for (int u = 0; u < 2; u++)
			if (ol_on[u] && ol[u].fewer_moved)
				VM_KLOGI("adaptive: %s read ahead %d pieces ahead (read for nothing %u permille)",
				         u ? "NC" : "C", RA_PIECES - ol[u].fewer,
				         (unsigned)ol[u].waste_pm);
		for (int u = 0; u < 3; u++)
			if (oo_mv[u])
				VM_KLOGI("adaptive: %s read ahead %s (reads net %d ms of %u ms, model %d permille)",
				         u == OO_TINY ? "tiny" : u ? "NC" : "C", ol[u].now ? "off" : "on",
				         (int)(ol[u].net / 1000), (unsigned)(ol[u].time / 1000), ol[u].pm);
	}
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
		if (wr) {
			/* a write running: whatever it read now could not be used - so
			 * nothing is read, the card is left to the write and to the reads
			 * that wait for it (a game saving) */
			pc->ok = 0;
			pc->skipped = 1;
			pc->held_us = 0;
			__atomic_store_n(&pc->wait_from, 0, __ATOMIC_RELAXED);
			ADD(g_ra_wskip, 1);
			__atomic_store_n(&pc->state, RA_FAILED, __ATOMIC_SEQ_CST);
			if (__atomic_exchange_n(&d->ra_waiting, 0, __ATOMIC_SEQ_CST))
				ksceKernelSignalSema(d->ra_done, 1);
			continue;
		}
		pc->skipped = 0;
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

/* Whether d's staging buffers hold pieces at all (used up ones too: they
 * are dropped before the buffers are used for anything else). */
static int ra_any(const Dev *d)
{
	for (int i = 0; i < RA_PIECES; i++)
		if (__atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) != RA_FREE)
			return 1;
	return 0;
}

/* A piece read and handed out to its end: kept, so a read that takes part
 * of it again (overlapping the stream's end, or read twice) still finds it,
 * until its slot is wanted for a new piece (ra_refill) or the pieces are
 * dropped. Not "ahead" of anything: it counts nowhere a piece in reserve
 * does (ra_ahead, ra_refill, ra_drop's figures). */
static int ra_spent(const RaPiece *pc)
{
	return __atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) == RA_READY && LD(pc->off) >= LD(pc->n);
}

/* Whether d has pieces still to hand out (queued, being read or read). */
static int ra_ahead(const Dev *d)
{
	for (int i = 0; i < RA_PIECES; i++)
		if (__atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) != RA_FREE && !ra_spent(&d->ra[i]))
			return 1;
	return 0;
}

/* Pieces of a stream still being read lie in d's staging buffers: its own,
 * or with Low kernel memory the other storage's (a small transfer then goes
 * the standard way rather than drop them; a look only - take decides). */
static int ra_live(const Dev *d)
{
	const Dev *o = d->shared ? __atomic_load_n(&g_sh.owner, __ATOMIC_RELAXED) : d;

	if (!o)
		return 0;
	return ra_ahead(o) &&
	       ksceKernelGetSystemTimeLow() - __atomic_load_n(&o->ra_last_us, __ATOMIC_RELAXED) <
	       RA_LIVE_US;
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
			if (st != RA_READY || d->ra[i].off < d->ra[i].n)
				any = 1;                    /* (a used up one: nothing dropped) */
			if (st == RA_READY) {
				waste += d->ra[i].n - d->ra[i].off;
				if (!d->ra[i].off) {
					cal_waste(d, &d->ra[i]);   /* none of it used */
					if (ra_learn_on())
						oo_waste(d->ra[i].nc ? OO_NC : OO_C, d->ra[i].held_us);
					Diag *const dg = LD(g_log_diag) ? DG() : NULL;

					if (dg) {
						LgKind *g = &dg->lk[d->ra[i].nc ? LK_NC : LK_C];

						ADD(g->waste, 1);
						ADD(g->held_us, (uint64_t)d->ra[i].held_us);
						if (__atomic_load_n(&d->ra[i].dirty, __ATOMIC_RELAXED))
							ADD(g->spoilt, 1);
					}
				}
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

/* Whether piece pc holds `at` for a read: where it goes on (its front), or -
 * read and handed out already - in the part before that, which stays in its
 * buffer until the piece is used up (a stream whose reads overlap the end of
 * the one before, or read again what they just read). */
static int ra_holds(const Dev *d, int i, uint32_t at, int full)
{
	const RaPiece *pc = &d->ra[i];
	const uint32_t s0 = LD(pc->sector), o = LD(pc->off), n = LD(pc->n);

	if (at < s0 || at - s0 >= n)
		return 0;
	if (at - s0 == o)
		return 1;                             /* (the front: checked when served) */
	/* the part handed out: only from a piece that can be (a write since, or
	 * too old - no use dropping the stream for it); a look from any thread
	 * sees what it can without the request's fields */
	if (at - s0 > o)
		return 0;
	return full ? ra_valid(d, i)
	            : __atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) == RA_READY &&
	              !__atomic_load_n(&pc->dirty, __ATOMIC_ACQUIRE);
}

/* The pieces that hold [sector, sector + n) from its start on, in turn
 * (idx), each from st[] sectors into it: how many sectors of the n asked
 * for they hold. 0: they do not hold its start. */
static uint32_t ra_cover(const Dev *d, void *part, io_fn io, uint32_t sector, uint32_t n,
                         int idx[RA_PIECES], uint32_t st[RA_PIECES], int *cnt)
{
	uint32_t at = sector, cov = 0;

	*cnt = 0;
	while (cov < n && *cnt < RA_PIECES) {
		int found = -1;

		for (int i = 0; i < RA_PIECES; i++) {
			const RaPiece *pc = &d->ra[i];

			if (__atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) != RA_FREE &&
			    pc->part == part && pc->io == io && ra_holds(d, i, at, 1))
				found = i;
		}
		if (found < 0)
			break;
		{
			const RaPiece *pc = &d->ra[found];
			const uint32_t from = at - pc->sector, have = pc->n - from;
			const uint32_t take_n = have < n - cov ? have : n - cov;

			idx[*cnt] = found;
			st[(*cnt)++] = from;
			cov += take_n;
			at += take_n;
		}
	}
	return cov;
}

/* Whether the pieces hold `sector` (ra_holds): a read there continues their
 * stream, whatever its size (a game reading one file in large and 32 KiB
 * reads in turn), or takes again what it just had from them. A look only,
 * from any thread - the request that takes the device decides (ra_cover). */
static int ra_at(const Dev *d, void *part, io_fn io, uint32_t sector)
{
	for (int i = 0; i < RA_PIECES; i++) {
		const RaPiece *pc = &d->ra[i];

		if (__atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) != RA_FREE && pc->part == part &&
		    pc->io == io && ra_holds(d, i, sector, 0))
			return 1;
	}
	return 0;
}

/* A read that overlaps the end of the stream so far: starting inside what
 * was read last (up to RA_OVER_MAX before the pieces) and going on past it -
 * a game, or its file system's clusters, reading a stream with each read
 * taking again the end of the one before (a sixth of PCSB00554's large
 * reads, 1 s of PCSF00024's). The sectors from its start to where the
 * pieces go on (0: none such): those come from the card, the rest from the
 * pieces. A look only, as ra_at. */
#define RA_OVER_MAX RA_PIECE_SECTORS
static uint32_t ra_over_head(const Dev *d, void *part, io_fn io, uint32_t sector, uint32_t n)
{
	uint32_t front = 0;
	int any = 0;

	/* the stream's front: where its pieces go on first (what it read last
	 * ends there) - not a later piece a read happens to reach into */
	for (int i = 0; i < RA_PIECES; i++) {
		const RaPiece *pc = &d->ra[i];

		if (__atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) != RA_FREE && pc->part == part &&
		    pc->io == io && LD(pc->off) < LD(pc->n)) {
			const uint32_t at = LD(pc->sector) + LD(pc->off);

			if (!any || at < front)
				front = at;
			any = 1;
		}
	}
	if (any && front > sector && front - sector < n && front - sector <= RA_OVER_MAX)
		return front - sector;
	return 0;
}

/* Whether a read [sector, sector + n) goes on from a stream that ends at
 * `end`: where it ended, or overlapping its end (see ra_over_head). */
static inline int ra_goes_on(uint32_t sector, uint32_t n, uint32_t end)
{
	return sector == end || (sector < end && end - sector < n && end - sector <= RA_OVER_MAX);
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

/* Whether the DMA controller would take each of k parts (src, sz sectors)
 * into p whole, as dma_try decides, and nothing is left for the CPU after it
 * (GPU-safe reads clean the cache after a DMA copy unless "Non-DMA"): then
 * this thread only waits for the controller, as the copier would. */
static int dma_whole(const uint8_t *p, const void *const *src, const uint32_t *sz, uint32_t k,
                     int clean)
{
	uint32_t at = 0;

	if (!k || dma_state() != VM_DMA_ON || (clean && LD(g_gpu_safe) != VM_GPUSAFE_NODMA))
		return 0;
	for (uint32_t i = 0; i < k; i++) {
		const uint32_t b = sz[i] * 512;

		if (b < VM_DMA_MIN_BYTES ||
		    (((uintptr_t)(p + (size_t)at * 512) | (uintptr_t)src[i] | (uintptr_t)b) &
		     (VM_DMA_ALIGN - 1u)))
			return 0;
		at += sz[i];
	}
	return 1;
}

/* Copy k ready parts (src, sz) into p on the copier, as offload_run does with
 * parts it reads; without a usable mapping this thread copies. Copies the DMA
 * controller makes whole are this thread's: the copier would only wait for
 * the controller too, after a mapping and two hand-overs. *own: the sectors
 * this thread copied. */
static void ra_copy_off(Dev *d, uint8_t *p, const void *const *src, const uint32_t *sz, uint32_t k,
                        int clean, OffMap *m, int last, uint32_t *own, uint32_t *cp_us)
{
	uint32_t done = 0, n = 0;
	int nomap = 1;

	if (dma_whole(p, src, sz, k, clean)) {
		const uint32_t c0 = ksceKernelGetSystemTimeLow();

		for (uint32_t i = 0; i < k; i++) {
			copy_out(p + (size_t)n * 512, src[i], sz[i] * 512, clean);
			n += sz[i];
		}
		if (!clean)
			vm_dsb();
		*own += n;
		*cp_us = ksceKernelGetSystemTimeLow() - c0;
		ADD(g_dma_here, 1);
		ADD(g_dma_here_kib, n / 2);
		return;
	}
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
	/* filled for the cnt pieces below; zeroed so the compiler sees them set
	 * (it cannot tell cnt > 0 whenever cov is) */
	const void *src[RA_PIECES] = { 0 };
	uint32_t st[RA_PIECES], sz[RA_PIECES] = { 0 },
	         cov = ra_cover(d, part, io, sector, n, idx, st, &cnt), w0, wait_us, cp_us = 0, at = 0;

	if (!cov)
		return 0;
	w0 = stats ? ksceKernelGetSystemTimeLow() : 0;
	for (int j = 0; j < cnt; j++)
		if (ra_wait(d, idx[j]) != RA_READY || !ra_valid(d, idx[j])) {
			const RaPiece *const pc = &d->ra[idx[j]];
			/* failed: the card's error; during a write: not read, the game was
			 * writing (ra_thread leaves the card to it) */
			const int why = LD(pc->state) != RA_READY ? (pc->skipped ? 1 : 0) :
			                !pc->ok ? 1 : LD(pc->dirty) ? 2 :
			                pc->epoch != LD(d->ra_epoch) ? 3 : 4;

			ADD(g_ra_bad_why[why], 1);
			ADD(g_ra_bad, 1);
			return 0;
		}
	wait_us = stats ? ksceKernelGetSystemTimeLow() - w0 : 0;
	for (int j = 0; j < cnt; j++) {
		const RaPiece *pc = &d->ra[idx[j]];
		const uint32_t have = pc->n - st[j];

		sz[j] = have < cov - at ? have : cov - at;
		src[j] = ra_ptr(d, idx[j]) + (size_t)st[j] * 512;
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

		/* handed out up to here (a read of what it had already moves nothing) */
		if (st[j] + sz[j] > pc->off)
			ST(pc->off, st[j] + sz[j]);       /* (ra_at looks at it unlocked) */
		/* used up: kept READY for a read of it again (ra_spent) */
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

		if (part == d->rs_part && ra_goes_on(sector, n, d->rs_end) && d->rs_streak &&
		    nc == d->rs_nc && (!ready || now - d->rs_us < RA_MAX_AGE_US)) {
			ST(d->rs_streak, d->rs_streak + (d->rs_streak < 1000));   /* (gap_note reads it unlocked) */
			d->rs_overlap = sector != d->rs_end;
		} else {
			ra_adapt_end(d, 1, d->rs_nc, d->rs_streak);
			ST(d->rs_streak, 1);
			d->rs_overlap = 0;
		}
		d->rs_us = now;
		ra_adapt_read(d, 1, d->rs_streak, n);
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
 * counts (then the pieces may be refilled after it). A read of one size that
 * goes on where the other size's stream is carries that stream on too - a
 * game reads one file in large and 32 KiB reads in turn (PCSF00007,
 * PCSG01179: a quarter of their reads' time), which broke both streams at
 * every change of size. A 32 KiB read that carries the large reads' stream
 * is that stream's for the refill. */
static int ra_note(Dev *d, void *part, uint32_t sector, uint32_t n, int nc)
{
	d->ra_noted_small = 0;
	if (n < RA_SMALL_SECTORS)
		return 0;                           /* neither part of a stream nor a break */
	/* wholly inside what the large reads' stream just read (read again, from
	 * the pieces if they still hold it): neither part of it nor a break */
	if (part == d->ra_last_part && d->ra_streak && nc == d->ra_nc && sector < d->ra_last_end &&
	    n <= d->ra_last_end - sector && d->ra_last_end - sector <= 2 * RA_PIECE_SECTORS)
		return 0;
	if (n < RA_MIN_SECTORS) {
		ra_note_small(d, part, sector, n, nc);
		d->ra_noted_small = 1;
		/* the large reads' stream goes on past it - into either memory: a
		 * file system may read the 32 KiB parts into its own (cached)
		 * buffer between a game's large non-cacheable reads (the tester's
		 * files on ux0) */
		if (part == d->ra_last_part && ra_goes_on(sector, n, d->ra_last_end) && d->ra_streak) {
			d->ra_last_end = sector + n;
			d->ra_noted_small = 0;
		}
	} else {
		const int ready = __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE);
		const uint32_t now = ready ? ksceKernelGetSystemTimeLow() : 0;

		if (part == d->ra_last_part && ra_goes_on(sector, n, d->ra_last_end) && d->ra_streak &&
		    nc == d->ra_nc && (!ready || now - d->ra_us < RA_MAX_AGE_US)) {
			ST(d->ra_streak, d->ra_streak + (d->ra_streak < 1000));   /* (gap_note reads it unlocked) */
			d->ra_overlap = sector != d->ra_last_end;
		} else {
			ra_adapt_end(d, 0, d->ra_nc, d->ra_streak);
			ST(d->ra_streak, 1);
			d->ra_overlap = 0;
		}
		ra_adapt_read(d, 0, d->ra_streak, n);
		d->ra_us = now;
		d->ra_nc = nc;
		d->ra_last_part = part;
		d->ra_last_end = sector + n;
		/* ... and the 32 KiB reads' stream past it (lost if another thread
		 * holds it: the next 32 KiB read then starts afresh) */
		if (!__sync_lock_test_and_set(&d->rs_lock, 1)) {
			if (part == d->rs_part && ra_goes_on(sector, n, d->rs_end) && d->rs_streak &&
			    nc == d->rs_nc)
				d->rs_end = sector + n;
			__sync_lock_release(&d->rs_lock);
		}
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
	int nc;
	uint32_t streak, thr, next, end;
	int want, live = 0, over;

	if (small) {
		/* the 32 KiB stream's fields: a read the standard way on another
		 * thread may note one at the same moment (ra_note_small) */
		if (__sync_lock_test_and_set(&d->rs_lock, 1))
			return;                           /* refilled after the next read */
		nc = d->rs_nc;
		streak = d->rs_streak;
		next = d->rs_end;
		over = d->rs_overlap;
		__sync_lock_release(&d->rs_lock);
	} else {
		nc = d->ra_nc;
		streak = d->ra_streak;
		next = d->ra_last_end;
		over = d->ra_overlap;
	}
	thr = ra_threshold(nc);

	if (small)
		thr = ra_small_threshold(nc, thr);
	if (streak < thr || oo_off(nc ? OO_NC : OO_C))
		return;                           /* (off as measured: the runs still counted) */
	/* the pieces are the large reads' stream's while it has any */
	if (small && !LD(d->ra_small) && ra_ahead(d))
		return;
	/* step by step (a stream may end at any time); Adaptive: a piece or two
	 * less ahead where many were read for nothing (rt_ev_pieces) */
	{
		const uint32_t ahead = (streak - thr) / (small ? RA_SMALL_FACTOR : 1) + 1;
		const int cap = RA_PIECES - (ra_adapt_on() ? LD(g_ra_fewer[nc]) : 0);

		want = ahead < (uint32_t)cap ? (int)ahead : cap;
	}
	ST(d->ra_small, small);
	end = over ? next : 0;          /* (0: no piece ends there - see below) */
	for (int i = 0; i < RA_PIECES; i++) {
		const RaPiece *pc = &d->ra[i];

		if (__atomic_load_n(&pc->state, __ATOMIC_ACQUIRE) != RA_FREE) {
			if (!ra_spent(pc))
				live++;
			if (pc->sector + pc->n > next)
				next = pc->sector + pc->n;
		}
	}
	while (live < want && next <= 0xFFFFFFFFu - RA_PIECE_SECTORS) {
		RaPiece *pc = NULL;

		/* a free slot, else the oldest used up piece's - in a stream whose
		 * reads overlap, not the one its last read ended with (its end where
		 * the stream is): the next read takes part of it again. That one's
		 * slot is taken on the next read (one piece less ahead till then,
		 * only with all three wanted - the card still has two to read). A
		 * stream read end to end (most) keeps all three ahead as before. */
		for (int i = 0; i < RA_PIECES; i++) {
			RaPiece *c = &d->ra[i];

			if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) == RA_FREE) {
				pc = c;
				break;
			}
			if (ra_spent(c) && c->sector + c->n != end &&
			    (!pc || (int32_t)(c->seq - pc->seq) < 0))
				pc = c;
		}
		if (!pc)
			return;
		/* free first: a look at it (ra_at) never sees the old state with
		 * the new piece's place */
		__atomic_store_n(&pc->state, RA_FREE, __ATOMIC_RELEASE);
		live++;
		pc->seq = ++d->ra_seq;
		ST(pc->sector, next);
		ST(pc->n, (uint32_t)RA_PIECE_SECTORS);
		ST(pc->off, 0);                   /* (ra_spent looks at it unlocked) */
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
	d->t_cached = 0;
	ST(d->t_end, LD(d->t_start));
	for (int i = 0; i < RA_PIECES; i++)
		__atomic_store_n(&d->ra[i].state, RA_FREE, __ATOMIC_RELEASE);
	__atomic_store_n(&d->ra_waiting, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&d->ra_ready, 0, __ATOMIC_RELEASE);
	ST(d->ra_made, 0);
}

/* The tiny reads' block if the device has none. Cached memory when the cache
 * can be cleaned and invalidated (g_dcache_cinv): a hit is then a copy out of
 * the cache, not out of uncached memory (~114 MB/s); after each fill its lines
 * are cleaned and invalidated (tiny_filled). Else, or if that memory is not
 * there, non-cacheable as before; without either the tiny reads read as
 * before. */
static void tiny_alloc(Dev *d)
{
	if (d->t_buf)
		return;
	d->t_cached = 0;
	if (g_dcache_cinv &&
	    alloc_buf(SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, 1, RA_TINY_SECTORS * 512, &d->t_uid,
	              &d->t_buf) >= 0) {
		/* nothing of the memory's last owner left in the cache: no line of
		 * it written back over what the card puts there */
		g_dcache_cinv(d->t_buf, RA_TINY_SECTORS * 512);
		d->t_cached = 1;
		return;
	}
	if (alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, RA_TINY_SECTORS * 512, &d->t_uid, &d->t_buf) < 0) {
		d->t_uid = -1;
		d->t_buf = NULL;
	}
}

/* A block just read into a cached t_buf: none of its lines stale in the cache
 * (read before the card wrote memory, or the CPU's look ahead while it did);
 * a line the driver wrote through the cache is written back first, so either
 * way the cache then holds nothing of it and the next read sees memory. */
static void tiny_filled(Dev *d, uint32_t blk)
{
	if (d->t_cached)
		g_dcache_cinv(d->t_buf, blk * 512);
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
	/* the tiny reads' block (already there if made at the start): without
	 * it they read as before */
	tiny_alloc(d);
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
	for (int i = 0; __atomic_load_n(dbusy(d), __ATOMIC_ACQUIRE); i++) {
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
		if (!reading && !__atomic_load_n(dbusy(d), __ATOMIC_ACQUIRE))
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
                        uint8_t *p, uint32_t n, int *ret, int *how, SceUID *pc)
{
	const int clean = LD(g_gpu_safe) && g_dcache_clean;
	const int cal = CAL_MEASURING();          /* calibration: block reads and hits timed */
	uint32_t h0;
	int wr, filled = 0;
	void *shadow;

	if ((LD(g_gpu_safe) && !g_dcache_clean) || !share_may(d, pc) ||
	    __sync_lock_test_and_set(dbusy(d), 1))
		return 0;                             /* (no cache clean: the standard path) */
	__sync_synchronize();                     /* pairs with ra_retire's fence */
	if (!__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) || !d->t_buf || !LD(d->enabled) ||
	    LD(d->fallback)) {
		__sync_lock_release(dbusy(d));
		return 0;
	}
	if (sector == d->t_next && part == d->t_part) {
		d->t_streak += d->t_streak < 1000;
		d->t_run_sec += n;
	} else {
		ra_tiny_end(d, d->t_streak, d->t_run_sec);   /* Adaptive: the run that ended */
		d->t_streak = 1;
		d->t_run_sec = n;
		d->t_gen = LD(g_ad_gen);              /* the run is the state's now in effect */
	}
	d->t_next = sector + n;
	if (LD(g_log_diag) && DG() && LD(d->t_end) != LD(d->t_start) && sector >= LD(d->t_start) &&
	    sector + n <= LD(d->t_end) && __atomic_load_n(&d->t_dirty, __ATOMIC_SEQ_CST))
		ADD(DG()->lk[LK_TINY].spoilt, 1);         /* in a block a write spoilt */
	if (oo_off(OO_TINY)) {                /* off as measured: the run still counted */
		ST(d->t_end, LD(d->t_start));
		__sync_lock_release(dbusy(d));
		return 0;
	}
	if (!(LD(d->t_end) != LD(d->t_start) && sector >= LD(d->t_start) && sector + n <= LD(d->t_end) &&
	      part == d->t_part && io == d->t_io && d->t_epoch == LD(d->ra_epoch) &&
	      !__atomic_load_n(&d->t_dirty, __ATOMIC_SEQ_CST) &&
	      ksceKernelGetSystemTimeLow() - d->t_us < RA_MAX_AGE_US)) {   /* last: only with a block */
		/* not in it: the block is of no use now */
		const uint32_t blk = tiny_blk();          /* at most RA_TINY_SECTORS */
		uint32_t b0;

		ST(d->t_end, LD(d->t_start));
		d->t_part = part;
		if (d->t_streak < tiny_thr() || n > blk || sector > 0xFFFFFFFFu - blk) {
			__sync_lock_release(dbusy(d));
			return 0;
		}
		/* as a piece: the range before the look at ra_wbusy, both in one order
		 * with a write's own steps */
		__atomic_store_n(&d->t_dirty, 0, __ATOMIC_SEQ_CST);
		__atomic_store_n(&d->t_start, sector, __ATOMIC_SEQ_CST);   /* before the end (stage_xfer) */
		__atomic_store_n(&d->t_end, sector + blk, __ATOMIC_SEQ_CST);
		wr = __atomic_load_n(&d->ra_wbusy, __ATOMIC_SEQ_CST);
		b0 = cal ? ksceKernelGetSystemTimeLow() : 0;
		if (wr || io(ref, part, sector, d->t_buf, blk) < 0) {
			/* a write now, or the card's end: this read the standard way */
			ST(d->t_end, sector);
			__sync_lock_release(dbusy(d));
			return 0;
		}
		tiny_filled(d, blk);
		d->t_io = io;
		d->t_epoch = LD(d->ra_epoch);
		d->t_us = ksceKernelGetSystemTimeLow();
		if (cal)
			cal_tiny_blk(d, d->t_us - b0, blk);   /* calibration: a block read */
		filled = 1;
	}
	*how = filled ? 2 : 1;
	h0 = cal && !filled ? ksceKernelGetSystemTimeLow() : 0;   /* only a read from the block */
	copy_out(p, (uint8_t *)d->t_buf + (size_t)(sector - LD(d->t_start)) * 512, n * 512, clean);
	if (!clean)
		vm_dsb();
	ADD(g_ra_tiny_reads, 1);
	if (cal && !filled)
		cal_tiny_hit(d, ksceKernelGetSystemTimeLow() - h0);   /* calibration: from the block */
	*ret = 0;
	shadow = take_shadow();                   /* verify mode: read again, compared */
	if (shadow) {
		if (ra_verify(d, io, ref, part, sector, p, n, clean, shadow))
			ST(d->t_end, LD(d->t_start));
		__sync_lock_release(&g_shadow_busy);
	}
	__sync_lock_release(dbusy(d));
	if (LD(d->fallback)) {
		void (*const fn)(void) = __atomic_load_n(&g_on_problem, __ATOMIC_ACQUIRE);

		if (fn)
			fn();
	}
	return 1;
}

/* ---- again cache -------------------------------------------------------------
 *
 * Games read again much of what they read a moment before, the standard
 * way each time (PCSF00024: a quarter of its 32 KiB reads, each the same 32
 * KiB it read within the last 384 KiB; PCSB01007: a fifth of its large
 * reads): 2.6 s of PCSF00024's 32 s of reading. A game reads from one
 * storage; the other's staging buffers (384 KiB) wait meanwhile. Kept
 * there, the reads of the size a game reads again are read into them and
 * copied to the game by the copier (DMA: 3 ms a MiB against the card's 54),
 * and read again, only copied.
 *   - Games only (title ids PCS*: not homebrew, Adrenaline, the system's
 *     apps), non-cacheable reads (what read ahead's pieces do not give).
 *   - The host storage's own requests come first: one taking its buffers
 *     empties the cache (gen); the cache uses them only while it holds that
 *     storage (a try: busy, it reads as without it), its pieces dropped.
 *     With no other storage on, a block of its own (made by the worker); with
 *     Low kernel memory (one buffer for both), none at all.
 *   - By size (32 KiB, large): kept only while the game reads again enough
 *     of them (rc_on_pm of the reads it reads the standard way, off again
 *     under rc_off_pm), and while what is kept is read again: of every
 *     RC_YIELD_N kept, at least rc_yield_pm read again from the cache - else
 *     off, and not on again for RC_HOLD looks (twice that the next time, up
 *     to RC_HOLD_MAX; back to RC_HOLD once a yield pays). The yield decides,
 *     not the share read again: PCSF00024 reads again 23 % of its 32 KiB
 *     reads, each from the cache, more than one per kept read; PCSF00007 6 %,
 *     a fifth of them; large reads need 25 % (WipEout 2048 kept them at 28 %,
 *     about even). A read read again counts for the size of the read it
 *     lies in (a 32 KiB read inside a 128 KiB one: for the 128 KiB reads), the
 *     size whose reads are kept. A kept read costs more than
 *     its copy: the DMA (3 ms a MiB; in the game's thread, ra_copy_off) - about
 *     6 % more than the card alone for 128 KiB, more for 32 KiB (PCSF00007,
 *     with the copier: 1060 kept, 190 read again, the game slower).
 *   - Writes mark what they touch (as read ahead's pieces); an entry is used
 *     for RA_MAX_AGE_US, of the same storage switch (epoch) only. */
static const uint32_t rc_on_pm[2] = { 100u, 100u };    /* 32 KiB, large  */
static const uint32_t rc_off_pm[2] = { 50u, 50u };
static const uint32_t rc_yield_pm[2] = { 300u, 250u };
#define RC_ROUND   128u                    /* reads of a size between two looks  */
/* kept between two looks at the yield: long enough for a game's spells
 * of reading nothing again (Gravity Rush: 76 % read again over the
 * session, yet off 8 times with 64) */
#define RC_YIELD_N 128u
#define RC_HOLD    4u                      /* looks off for a poor yield, doubled */
#define RC_HOLD_MAX 32u                    /* ... each time in a row, up to this  */

/* A game's title id: PCSA..PCSI and 5 digits (the system's apps are NPXS,
 * homebrew and Adrenaline anything else) - or the tester's, which measures
 * what games get. */
static int rc_title_game(const char *t)
{
	if (!strcmp(t, VM_TESTER_TITLE))
		return 1;
	if (t[0] != 'P' || t[1] != 'C' || t[2] != 'S' || t[3] < 'A' || t[3] > 'I')
		return 0;
	for (int i = 4; i < 9; i++)
		if (t[i] < '0' || t[i] > '9')
			return 0;
	return !t[9];
}

static void rc_clear(void)
{
	for (uint32_t i = 0; i < RC_UNITS; i++)
		__atomic_store_n(&g_rc.e[i].dev, NULL, __ATOMIC_SEQ_CST);
}

/* The storage whose buffers the cache may use for `d`'s reads (NULL: none
 * such - its own block). Under g_rc.lock. */
static Dev *rc_pick(const Dev *d)
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *h = &g_dev[i];

		if (h != d && !h->shared && LD(h->enabled) && !LD(h->fallback) &&
		    __atomic_load_n(&h->buf, __ATOMIC_ACQUIRE) && h->buf2)
			return h;
	}
	return NULL;
}

/* The cache's buffers, held for this request: 1 if they are (then
 * rc_let_go after). Under g_rc.lock. */
static int rc_hold(void)
{
	Dev *const h = g_rc.host;
	void *zb[2];

	if (!h) {
		uint8_t *const own = __atomic_load_n(&g_rc.own, __ATOMIC_ACQUIRE);

		if (!own) {
			__atomic_store_n(&g_rc.want_own, 1, __ATOMIC_RELAXED);   /* the worker makes it */
			return 0;
		}
		zb[0] = own;
		zb[1] = own + STAGE_BYTES;
	} else {
		if (__sync_lock_test_and_set(dbusy(h), 1))
			return 0;                         /* its own request: it comes first */
		__sync_synchronize();
		zb[0] = __atomic_load_n(&h->buf, __ATOMIC_ACQUIRE);
		zb[1] = h->buf2;
		if (!LD(h->enabled) || LD(h->fallback) || !zb[0] || !zb[1]) {
			__sync_lock_release(dbusy(h));
			return 0;
		}
		/* its own pieces are in them (waited for, if being read) */
		if (ra_any(h))
			ra_drop(h);
	}
	/* taken back by the host since, or other buffers: what was kept is gone */
	if (__atomic_load_n(&g_rc.gen, __ATOMIC_SEQ_CST) != g_rc.seen || zb[0] != g_rc.zb[0] ||
	    zb[1] != g_rc.zb[1]) {
		if (g_rc.seen != __atomic_load_n(&g_rc.gen, __ATOMIC_SEQ_CST))
			ADD(g_rc.lost, 1);
		rc_clear();
		g_rc.seen = __atomic_load_n(&g_rc.gen, __ATOMIC_SEQ_CST);
		g_rc.zb[0] = zb[0];
		g_rc.zb[1] = zb[1];
	}
	return 1;
}

static void rc_let_go(void)
{
	if (g_rc.host)
		__sync_lock_release(dbusy(g_rc.host));
}

static uint8_t *rc_at(uint32_t unit)
{
	return unit < RC_UNITS0 ? (uint8_t *)g_rc.zb[0] + (size_t)unit * RC_UNIT * 512
	                        : (uint8_t *)g_rc.zb[1] + (size_t)(unit - RC_UNITS0) * RC_UNIT * 512;
}

/* The entry holding all of [sector, sector + n) (-1: none). */
static int rc_find(const Dev *d, void *part, io_fn io, uint32_t sector, uint32_t n)
{
	const uint32_t now = ksceKernelGetSystemTimeLow();

	for (uint32_t i = 0; i < RC_UNITS; i++) {
		const RcEnt *e = &g_rc.e[i];

		if (__atomic_load_n(&e->dev, __ATOMIC_SEQ_CST) == d && e->ok && e->part == part &&
		    e->io == io && e->epoch == LD(d->ra_epoch) && sector >= e->sector &&
		    n <= e->n && sector - e->sector <= e->n - n && now - e->t < RA_MAX_AGE_US &&
		    !__atomic_load_n(&e->dirty, __ATOMIC_SEQ_CST))
			return (int)i;
	}
	return -1;
}

/* Room for `len` units, in one buffer: the entries there given up. */
static uint32_t rc_room(uint32_t len)
{
	uint32_t u = g_rc.next >= RC_UNITS ? 0 : g_rc.next;

	if (u < RC_UNITS0 ? u + len > RC_UNITS0 : u + len > RC_UNITS)
		u = u < RC_UNITS0 ? RC_UNITS0 : 0;    /* the next buffer's start */
	for (uint32_t i = 0; i < RC_UNITS; i++) {
		RcEnt *e = &g_rc.e[i];

		if (__atomic_load_n(&e->dev, __ATOMIC_SEQ_CST) && i < u + len && i + e->len > u)
			__atomic_store_n(&e->dev, NULL, __ATOMIC_SEQ_CST);
	}
	g_rc.next = u + len;
	return u;
}

/* Copy n sectors at src to the game's buffer p: by the copier as read ahead
 * does (`off`), else this thread. */
static void rc_copy(Dev *d, uint8_t *p, const uint8_t *src, uint32_t n, int off, OffMap *om,
                    int last, uint32_t *own)
{
	if (off) {
		const void *s1 = src;
		uint32_t cp_us = 0;

		ra_copy_off(d, p, &s1, &n, 1, 0, om, last, own, &cp_us);
	} else {
		copy_out(p, src, n * 512, 0);
		vm_dsb();
		*own += n;
	}
}

/* For the app in front: whether it is a game, and its counts afresh when
 * another came. Under g_rc.lock. */
static void rc_app(void)
{
	const SceUID fg = __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE);

	if (fg == g_rc.pid)
		return;
	{
		char t[16];

		g_rc.game = fg && fg_get(t) == fg && rc_title_game(t);
	}
	g_rc.pid = fg;
	memset(g_rc.hs, 0, sizeof(g_rc.hs));
	memset(g_rc.he, 0, sizeof(g_rc.he));
	memset(g_rc.hp, 0, sizeof(g_rc.hp));
	g_rc.reads[0] = g_rc.reads[1] = g_rc.again[0] = g_rc.again[1] = 0;
	memset(g_rc.ykept, 0, sizeof(g_rc.ykept));
	memset(g_rc.yhit, 0, sizeof(g_rc.yhit));
	memset(g_rc.hold, 0, sizeof(g_rc.hold));
	g_rc.hold_n[0] = g_rc.hold_n[1] = RC_HOLD;
	__atomic_store_n(&g_rc.on[0], 0, __ATOMIC_RELAXED);
	__atomic_store_n(&g_rc.on[1], 0, __ATOMIC_RELAXED);
	rc_clear();
}

/* A read of the game's (all of it) noted: whether it lay in one it read the
 * standard way lately, and (`std`) it was one such. Every RC_ROUND reads of
 * a size, whether the cache keeps that size. Under g_rc.lock. */
static void rc_note(void *part, uint32_t sector, uint32_t n, int std)
{
	const int k = n >= RA_MIN_SECTORS;
	int in = -1;

	for (uint32_t j = 0; j < RC_HIST && in < 0; j++)
		if (g_rc.hp[j] == part && g_rc.he[j] != g_rc.hs[j] && sector >= g_rc.hs[j] &&
		    sector < g_rc.he[j] && n <= g_rc.he[j] - sector)
			in = g_rc.hk[j];                  /* read again: for the size it lies in */
	g_rc.reads[k]++;
	if (in >= 0)
		g_rc.again[in]++;
	if (std && in < 0) {
		const uint32_t j = g_rc.hi++ % RC_HIST;

		g_rc.hp[j] = part;
		g_rc.hs[j] = sector;
		g_rc.he[j] = sector + n;
		g_rc.hk[j] = (uint8_t)k;
	}
	if (g_rc.reads[k] >= RC_ROUND) {
		const uint32_t pm = g_rc.again[k] * 1000u / g_rc.reads[k];
		const int was = __atomic_load_n(&g_rc.on[k], __ATOMIC_RELAXED);
		const int on = was ? pm >= rc_off_pm[k] : pm >= rc_on_pm[k] && !g_rc.hold[k];

		if (g_rc.hold[k])
			g_rc.hold[k]--;                   /* off for its yield: a few looks more */
		if (on != was) {
			__atomic_store_n(&g_rc.on[k], on, __ATOMIC_RELAXED);
			ADD(g_rc.moved_k[k], 1);
			g_rc.ykept[k] = g_rc.yhit[k] = 0;   /* its yield afresh */
		}
		g_rc.reads[k] /= 2;                   /* the older reads weigh less */
		g_rc.again[k] /= 2;
	}
}

/* A read of size k kept: every RC_YIELD_N, whether enough of them were read
 * again from the cache to pay for their copies (else that size off, and
 * held off a while). Under g_rc.lock. */
static void rc_yield(int k)
{
	if (++g_rc.ykept[k] < RC_YIELD_N)
		return;
	if (g_rc.yhit[k] * 1000u < g_rc.ykept[k] * rc_yield_pm[k]) {
		__atomic_store_n(&g_rc.on[k], 0, __ATOMIC_RELAXED);
		g_rc.hold[k] = g_rc.hold_n[k];
		g_rc.hold_n[k] = g_rc.hold_n[k] * 2 > RC_HOLD_MAX ? RC_HOLD_MAX : g_rc.hold_n[k] * 2;
		g_rc.ykept[k] = g_rc.yhit[k] = 0;
		ADD(g_rc.moved_k[k], 1);
		ADD(g_rc.yoff_k[k], 1);
		return;
	}
	g_rc.hold_n[k] = RC_HOLD;                 /* it pays: a short hold next time */
	g_rc.ykept[k] /= 2;                       /* the older ones weigh less */
	g_rc.yhit[k] /= 2;
}

/* The part [sector, sector + n) of a game's read (of `all` sectors) that
 * read ahead did not give: from the cache if it holds it, or read into the
 * cache (the size kept) and copied. 1 if done here (*ret: the read's
 * result), 0: the caller reads it as before. The device is the caller's. */
static int rc_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                   uint8_t *p, uint32_t n, uint32_t all, int off, OffMap *om, uint32_t *own,
                   int *ret)
{
	int done = 0, i;

	if (d->shared || __sync_lock_test_and_set(&g_rc.lock, 1))
		return 0;
	rc_app();
	if (!g_rc.game) {
		__sync_lock_release(&g_rc.lock);
		return 0;
	}
	/* another storage of the game's: the cache afresh for it */
	{
		Dev *const h = rc_pick(d);

		if (h != g_rc.host || d != g_rc.user) {
			__atomic_store_n(&g_rc.host, h, __ATOMIC_SEQ_CST);
			g_rc.user = d;
			rc_clear();
		}
	}
	if ((i = rc_find(d, part, io, sector, n)) >= 0 || (n == all && n <= 4 * RC_UNIT &&
	    __atomic_load_n(&g_rc.on[all >= RA_MIN_SECTORS], __ATOMIC_RELAXED))) {
		if (rc_hold()) {
			i = rc_find(d, part, io, sector, n);   /* (emptied, if taken back) */
			if (i >= 0) {
				const RcEnt *e = &g_rc.e[i];

				rc_copy(d, p, rc_at((uint32_t)i) + (size_t)(sector - e->sector) * 512, n, off,
				        om, 1, own);
				*ret = 0;
				done = 1;
				ADD(g_rc.hits, 1);
				ADD(g_rc.hit_kib, n / 2);
				ADD(g_rc.hits_k[e->k], 1);
				g_rc.yhit[e->k]++;               /* for the size it was kept for */
			} else if (n == all && n <= 4 * RC_UNIT &&
			           __atomic_load_n(&g_rc.on[all >= RA_MIN_SECTORS], __ATOMIC_RELAXED)) {
				const uint32_t len = (n + RC_UNIT - 1) / RC_UNIT, u = rc_room(len);
				RcEnt *e = &g_rc.e[u];

				/* as a piece: its range before the look at ra_wbusy, both
				 * in one order with a write's own steps */
				e->ok = 0;
				e->part = part;
				e->io = io;
				e->len = len;
				e->k = all >= RA_MIN_SECTORS;
				__atomic_store_n(&e->dirty, 0, __ATOMIC_SEQ_CST);
				__atomic_store_n(&e->sector, sector, __ATOMIC_SEQ_CST);
				__atomic_store_n(&e->n, n, __ATOMIC_SEQ_CST);
				__atomic_store_n(&e->dev, d, __ATOMIC_SEQ_CST);
				if (!__atomic_load_n(&d->ra_wbusy, __ATOMIC_SEQ_CST)) {
					*ret = io(ref, part, sector, rc_at(u), n);
					done = 1;
					if (*ret >= 0) {
						rc_copy(d, p, rc_at(u), n, off, om, 1, own);
						e->epoch = LD(d->ra_epoch);
						e->t = ksceKernelGetSystemTimeLow();
						e->ok = 1;
						ADD(g_rc.kept_k[e->k], 1);
						rc_yield(e->k);
					}
				}
				if (!done || *ret < 0)
					__atomic_store_n(&e->dev, NULL, __ATOMIC_SEQ_CST);
			}
			rc_let_go();
		}
	}
	__sync_lock_release(&g_rc.lock);
	return done;
}

/* After a game's read: noted for the cache's say (see rc_note). */
static void rc_seen(void *part, uint32_t sector, uint32_t n, int std)
{
	if (__sync_lock_test_and_set(&g_rc.lock, 1))
		return;                               /* a lost count: fine */
	rc_app();
	if (g_rc.game)
		rc_note(part, sector, n, std);
	__sync_lock_release(&g_rc.lock);
}

/* A read into non-cacheable memory (64 KiB and up) while read ahead is on:
 * from the pieces where they continue the stream (copied, by the copier as
 * the setting says; no cache to clean), the rest the standard way straight
 * into the buffer as before; then the stream noted and the pieces refilled.
 * The standard read's result is the caller's, as without read ahead. */
static uint32_t g_ra_nc_kib;              /* atomic: for the log */
static int ncd_job(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                   uint8_t *p, uint32_t nsec, SceUID pid, int *ret);
static SceUID ncd_wanted(uint32_t nsec, SceUID *pc);
static int ra_nc_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                      uint8_t *p, uint32_t nsec, uint32_t cal_t0, int cal_held, uint32_t *gotp,
                      SceUID *pc)
{
	/* the caller found it non-cacheable at both ends */
	OffMap om = { .uid = -1, .user = p, .len = nsec * 512, .first = -2, .mt = MEMTYPE_NC, .nc = 1 };
	uint32_t got = 0, own = 0, hd = 0;          /* hd: taken again from the card (overlap) */
	int ret = 0, stats, off, offload, bad = 0;
	void *shadow;

	if (!take(d, 1, nsec, pc)) {
		ADD(g_nc_stock_kib, nsec / 2);
		ret = io(ref, part, sector, p, nsec);
		ra_note_small_stock(d, part, sector, nsec, 1, ret);
		return ret;
	}
	if (!__atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE)) {
		__sync_lock_release(dbusy(d));          /* switched off meanwhile */
		ADD(g_nc_stock_kib, nsec / 2);
		return io(ref, part, sector, p, nsec);
	}
	shadow = take_shadow();                     /* verify mode: compared after */
	stats = LD(g_stats);
	offload = LD(g_offload);
	off = __atomic_load_n(&d->pipe_ready, __ATOMIC_ACQUIRE) &&
	      __atomic_load_n(&g_offload_state, __ATOMIC_ACQUIRE) >= 0 && !LD(g_nc_off_refused) &&
	      vm_offload_nc(offload) && (om.pid = req_pid(pc)) != KERNEL_PID;
	/* a 32 KiB read leaves the large reads' pieces alone (it goes straight
	 * into its own buffer, they are not in its way) - unless they go on where
	 * it starts: their stream, read in both sizes */
	/* overlapping the stream's end: what it takes again from the card first
	 * (then the read goes on from there, see ra_over_head) */
	if (ra_any(d) && !ra_at(d, part, io, sector) &&
	    (hd = ra_over_head(d, part, io, sector, nsec)) != 0) {
		ADD(g_nc_stock_kib, hd / 2);
		ret = io(ref, part, sector, p, hd);
		if (ret < 0) {
			hd = 0;
			ra_drop(d);
		} else {
			ADD(g_ra_over, 1);
		}
	}
	if (ret >= 0 && ra_any(d) &&
	    (nsec >= RA_MIN_SECTORS || LD(d->ra_small) || hd || ra_at(d, part, io, sector))) {
		got = ra_serve(d, part, io, sector + hd, p + (size_t)hd * 512, nsec - hd, 0, stats, off,
		               &om, nsec - hd, &own);
		*gotp = got;
		if (hd + got < nsec)
			ra_drop(d);                         /* elsewhere, or used up */
	}
	if (got && shadow)
		bad = ra_verify(d, io, ref, part, sector + hd, p + (size_t)hd * 512, got, 0, shadow);
	if (ret >= 0 && hd + got < nsec) {
		/* the rest: from the again cache (or through it), else "C + NC" on
		 * the system core, else the standard way */
		const uint32_t at = hd + got;
		const SceUID pid = shadow ? KERNEL_PID : ncd_wanted(nsec - at, pc);

		if (!shadow && off &&
		    rc_read(d, io, ref, part, sector + at, p + (size_t)at * 512, nsec - at, nsec, off,
		            &om, &own, &ret)) {
			/* done there */
		} else if (pid == KERNEL_PID ||
		           !ncd_job(d, io, ref, part, sector + at, p + (size_t)at * 512, nsec - at, pid,
		                    &ret)) {
			ret = io(ref, part, sector + at, p + (size_t)at * 512, nsec - at);
			ADD(g_nc_stock_kib, (nsec - at) / 2);
		}
	}
	if (om.uid >= 0)
		ksceKernelUserUnmap(om.uid);              /* the copier is done with it */
	if (ret >= 0 && !shadow)
		rc_seen(part, sector, nsec, !hd && !got);
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
	__sync_lock_release(dbusy(d));
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
 * into the game's buffer, with dbusy(d) already held. 1 if done here (*ret:
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
static SceUID ncd_wanted(uint32_t nsec, SceUID *pc)
{
	if (!vm_offload_direct(LD(g_offload)) || LD(g_verify) || nsec < NCD_MIN_SECTORS ||
	    __atomic_load_n(&g_ncd_state, __ATOMIC_ACQUIRE) < 0)
		return KERNEL_PID;
	return req_pid(pc);
}

/* ncd_job, taking the device first (0 if busy: the read stays standard). */
static int ncd_read(Dev *d, io_fn io, tai_hook_ref_t ref, void *part, uint32_t sector,
                    uint8_t *p, uint32_t nsec, SceUID pid, int *ret, SceUID *pc)
{
	int done;

	if (!share_may(d, pc))
		return 0;
	if (__sync_lock_test_and_set(dbusy(d), 1)) {
		ADD(g_ncd_busy, 1);
		return 0;
	}
	__sync_synchronize();                 /* pairs with pipe_retire's fence */
	if (!LD(d->enabled) || LD(d->fallback)) {
		__sync_lock_release(dbusy(d));
		return 0;
	}
	done = ncd_job(d, io, ref, part, sector, p, nsec, pid, ret);
	__sync_lock_release(dbusy(d));
	return done;
}

/* One request, either way. `rd_ref` is the read function, used by a write's
 * verify re-read (a read verifies with `ref` itself). */
static int stage_xfer_one(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                          void *part, uint32_t sector, void *buf, uint32_t nsec, XInfo *xi)
{
	uint8_t *p = buf;
	uint32_t left = nsec;
	int verify, ret = 0, mine = on_port(part, d->port);
	int mt0 = -2, mt1 = -2;               /* the buffer's memory type at its ends */
	int cal_held = 0;
	/* another app's request while one is in front: no read ahead for it
	 * (see g_bg_n); looked at only while read ahead is up */
	const int bg = mine && nsec && __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) &&
	               bg_req(&xi->pid);
	/* calibration on: when it started, and whether a piece held it up */
	/* (also with Adaptive: who a piece held up, for its on or off) */
	const uint32_t cal_m = mine && nsec && !bg && (CAL_MEASURING() || ra_learn_on())
	                       ? cal_start(d, nsec, &cal_held) : 0;
	const uint32_t cal_t0 = CAL_MEASURING() ? cal_m : 0;

	/* the largest read request seen, for the log (a lost update is fine) */
	if (mine && !write && nsec > __atomic_load_n(&g_rd_max, __ATOMIC_RELAXED))
		__atomic_store_n(&g_rd_max, nsec, __ATOMIC_RELAXED);

	if (mine && LD(g_count_all)) {
		if (write) {
			ADD(d->all_wr, (uint64_t)nsec * 512);
		} else {
			ADD(d->all_rd, (uint64_t)nsec * 512);
			/* the app in front's reads, per storage (the HUD shows its own) */
			if (nsec >= RA_MIN_SECTORS && __atomic_load_n(&g_ra_fg, __ATOMIC_RELAXED) && !bg)
				__atomic_fetch_add(&g_fg_sec[d - g_dev], nsec, __ATOMIC_RELAXED);
		}
	}
	if (bg && !write) {
		ADD(g_bg_n, 1);
		ADD(g_bg_kib, nsec / 2);
	}
	/* Tiny reads: from their own block, if it holds them */
	if (mine && !write && !bg && nsec && nsec < RA_SMALL_SECTORS &&
	    __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) &&
	    ra_tiny_read(d, io, ref, part, sector, p, nsec, &ret, &xi->tiny, &xi->pid))
		return ret;
	/* A non-cacheable caller buffer: the standard path is the faster one
	 * (see the top). Looked at only for transfers that would be staged. */
	if (mine && LD(d->enabled) && !LD(d->fallback) && __atomic_load_n(&d->buf, __ATOMIC_ACQUIRE) &&
	    nsec && nsec >= LD(g_min_sectors) &&
	    mt_uncached(mt0 = mt0 != -2 ? mt0 : ksceKernelGetMemBlockMemtypeByAddr(p)) &&
	    mt_uncached(mt1 = ksceKernelGetMemBlockMemtypeByAddr(p + nsec * 512 - 1))) {
		SceUID pid;
		int r;

		xi->nc = mt0 == MEMTYPE_NC && mt1 == MEMTYPE_NC ? 1 : 2;   /* (2: device memory) */

		/* ... unless read ahead holds it: a stream into non-cacheable
		 * memory, as games load their graphics data (what the pieces do not
		 * cover goes to the system core there too, with "C + NC") */
		if (!write && !bg && nsec >= RA_SMALL_SECTORS &&
		    __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE) && mt0 == MEMTYPE_NC && mt1 == MEMTYPE_NC)
			return ra_nc_read(d, io, ref, part, sector, p, nsec, cal_t0, cal_held, &xi->got,
			                  &xi->pid);
		/* "C + NC": read on the system core instead */
		if (!write && (pid = ncd_wanted(nsec, &xi->pid)) != KERNEL_PID &&
		    ncd_read(d, io, ref, part, sector, p, nsec, pid, &r, &xi->pid))
			return r;
		ADD(g_nc_stock_kib, nsec / 2);
		return io(ref, part, sector, buf, nsec);
	}
	/* Read ahead keeps pieces in the staging buffers: a small transfer goes
	 * the standard way meanwhile, instead of dropping them (a read elsewhere
	 * waits for the card either way; a write still makes them unusable). */
	if (mine && nsec < RA_MIN_SECTORS &&
	    (nsec < RA_SMALL_SECTORS || (!LD(d->ra_small) && !ra_at(d, part, io, sector) &&
	                                 !ra_over_head(d, part, io, sector, nsec))) &&
	    ra_live(d)) {
		ret = io(ref, part, sector, buf, nsec);
		if (!write && !bg)
			ra_note_small_stock(d, part, sector, nsec, 0, ret);
		return ret;
	}
	/* ... and another app's transfer of any size while the app has pieces
	 * (a slow stream of its own may still use them, see RA_MAX_AGE_US): a
	 * write the standard way still marks the ones it touches */
	if (bg && ra_ahead(d)) {
		if (!write)
			ADD(g_bg_kept, 1);
		return io(ref, part, sector, buf, nsec);
	}
	/* GPU-safe reads asked for, but this firmware has no cache clean to
	 * keep that promise: such reads take the standard path, which leaves
	 * the data in memory as promised. */
	if (!write && LD(g_gpu_safe) && !g_dcache_clean)
		return io(ref, part, sector, buf, nsec);
	if (!take(d, mine, nsec, &xi->pid)) {
		ret = io(ref, part, sector, buf, nsec);
		if (mine && !write && !bg)
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
	                (om.pid = req_pid(&xi->pid)) != KERNEL_PID;
	uint32_t sz[PIPE_MAX_PARTS], own = 0;
	int k = 0;
	const uint32_t sector0 = sector;
	/* read ahead: this read continues the stream from the pieces, if they
	 * hold it; anything else drops them first (they are in the buffers) */
	const int ra = !write && !bg && __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE);
	uint32_t ra_got = 0;                  /* of it from the pieces (calibration) */

	if (clean)
		ADD(g_gpu_cleaned_kib, left / 2);
	if (write && nsec >= RA_MIN_SECTORS && !bg_req(&xi->pid)) {
		ra_adapt_end(d, 0, d->ra_nc, d->ra_streak);
		ST(d->ra_streak, 0);                  /* a write ends the streams */
		if (!__sync_lock_test_and_set(&d->rs_lock, 1)) {
			ra_adapt_end(d, 1, d->rs_nc, d->rs_streak);
			ST(d->rs_streak, 0);
			__sync_lock_release(&d->rs_lock);
		}
	}
	/* overlapping the stream's end: what it takes again from the card, the
	 * rest from the pieces (see ra_over_head) */
	if (ra && ra_any(d) && !ra_at(d, part, io, sector)) {
		const uint32_t head = ra_over_head(d, part, io, sector, left);

		if (head) {
			ret = io(ref, part, sector, p, head);
			if (ret >= 0) {
				ADD(g_ra_over, 1);
				p += (size_t)head * 512;
				sector += head;
				left -= head;
			}
		}
	}
	if (ret >= 0 && ra_any(d)) {
		const uint32_t got = ra ? ra_serve(d, part, io, sector, p, left, clean, stats, off, &om,
		                                   left, &own)
		                        : 0;

		ra_got = got;
		xi->got = got;
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
		} else {
			/* Copy on the system core, unless the DMA controller can take
			 * every part whole: then this thread only waits for it, as the
			 * copier would - without the mapping and two hand-overs - and the
			 * card still reads the next part meanwhile (pipe_run). For that
			 * the split keeps every part a DMA copy (VM_DMA_MIN_BYTES up:
			 * a shorter last part would be the CPU's, on this core). */
			const int try_here = off && dma_state() == VM_DMA_ON &&
			                     !(clean && LD(g_gpu_safe) != VM_GPUSAFE_NODMA) &&
			                     !((uintptr_t)p & (VM_DMA_ALIGN - 1u));
			uint32_t done = 0;
			int here = 0;

			/* the parts: as pipe_plan says, else one */
			if (!(pipe &&
			      n >= (mode == VM_PIPE_AUTO ? PIPE_MIN_AUTO : PIPE_MIN_SECTORS) &&
			      (k = pipe_plan_min(d, n, plan_clean, sz,
			                         try_here ? VM_DMA_MIN_BYTES / 512 : PIPE_MIN_TAIL)) >= 2)) {
				k = 1;
				sz[0] = n;
			}
			if (try_here) {
				const void *src[PIPE_MAX_PARTS] = { 0 };

				for (int i = 0; i < k; i++)
					src[i] = (i & 1) ? d->buf2 : d->buf;
				here = dma_whole(p, src, sz, (uint32_t)k, clean);
			}
			if (off && !here) {
				uint32_t mine_n = 0;

				/* the buffer's memory type, read here: the copier, which
				 * maps it, is not in this process */
				off_memtype(&om);
				ret = offload_run(d, io, ref, part, sector, p, sz, (uint32_t)k, clean, stats,
				                  &om, left == n, &done, &mine_n);
				own += mine_n;
			} else if (k >= 2) {
				/* Overlapped, this chunk in 2-3 parts (see pipe_run). */
				ret = pipe_run(d, io, ref, part, sector, p, sz, (uint32_t)k, clean, stats,
				               &done);
				own += done;
			} else {
				ret = io(ref, part, sector, d->buf, n);
				if (ret >= 0) {
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
			}
			if (ret < 0) {
				p += (size_t)done * 512;    /* the parts already copied stay */
				sector += done;
				left -= done;
				break;
			}
			if (here) {
				ADD(g_dma_here_st, 1);
				ADD(g_dma_here_st_kib, n / 2);
			}
			if (k >= 2)
				ADD(g_pipe_reads, 1);
			if (k > 2)
				ADD(g_pipe_reads3, 1);
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
	if (!write && !bg && ret >= 0 && !left && !LD(d->fallback) && !problem &&
	    ra_note(d, part, sector0, nsec, 0)) {
		cal_read(d, 0, nsec, ra_got, cal_t0, cal_held);
		if (ra)
			ra_refill(d, io, ref, part);
	}
	if (verify)
		__sync_lock_release(&g_shadow_busy);
	__sync_lock_release(dbusy(d));
	if (ret < 0) {
		ADD(d->st.errors, 1);
		ST(d->fallback, 1);
		problem = 1;
	} else if (write) {
		ADD(d->st.writes, 1);
	} else {
		ADD(d->st.reads, 1);
		if (COUNT_CORES() && !left)
			diag_note(own);
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
static int stage_xfer_w(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                        void *part, uint32_t sector, void *buf, uint32_t nsec, XInfo *xi);

/* With the log on: one transfer, timed (Diag.lg). */
static void lg_note(Dev *d, int write, uint32_t nsec, uint32_t us, int col)
{
	Diag *const dg = DG();
	LgDev *g;

	if (!dg)
		return;
	g = &dg->lg[d - g_dev];
	if (write) {
		ADD(g->wr_n, 1);
		ADD(g->wr_sec, nsec);
		ADD(g->wr_us, (uint64_t)us);
		return;
	}
	{
		const int k = nsec < RA_SMALL_SECTORS ? LG_TINY : nsec < RA_MIN_SECTORS ? LG_SMALL : LG_LARGE;

		ADD(g->n[k], 1);
		ADD(g->sec[k], nsec);
		ADD(g->us[k], (uint64_t)us);
	}
	if (col) {
		ADD(g->col_n, 1);
		ADD(g->col_us, (uint64_t)us);
	}
	if (us > LD(g->max_us))
		ST(g->max_us, us);                /* a lost update is fine */
}

/* With the log on: a read of kind k (< 0: device memory) and how it went
 * (Diag.lk: 0 standard, 1 all from read ahead, 2 partly / tiny: 1 from the
 * block, 2 the block read for it). */
static void lk_note(int k, uint32_t nsec, uint32_t us, int col, int how)
{
	Diag *const dg = DG();
	LgKind *g;

	if (k < 0 || !dg)
		return;
	g = &dg->lk[k];
	ADD(g->n[how], 1);
	ADD(g->sec[how], nsec);
	ADD(g->us[how], (uint64_t)us);
	if (col) {
		ADD(g->col_n, 1);
		ADD(g->col_us, (uint64_t)us);
		if (us > LD(g->col_max))
			ST(g->col_max, us);           /* a lost update is fine */
	}
}

/* With the log on: where a read of the app in front lies (see PT_*). */
static void pt_note(Dev *d, void *part, uint32_t sector, uint32_t nsec, uint32_t us, int ra)
{
	Diag *const dg = DG();
	const int k = nsec < RA_SMALL_SECTORS ? LG_TINY : nsec < RA_MIN_SECTORS ? LG_SMALL : LG_LARGE;
	const uint32_t pt = (uint32_t)(uintptr_t)part;
	PtDev *t;
	int c = PT_FAR;
	uint32_t tot, since = 0xFFFFFFFFu, gap = 0xFFFFFFFFu, of = 0;

	if (!dg)
		return;
	t = &dg->pt[d - g_dev];
	tot = __atomic_fetch_add(&t->tot, nsec, __ATOMIC_RELAXED);   /* read before this one */
	if (LD(t->own_part[k]) == pt && LD(t->own_end[k]) == sector && sector) {
		c = PT_OWN;
	} else {
		const uint32_t e0 = sector + nsec;

		for (int j = 0; j < PT_HIST; j++) {
			const PtRead *h = &t->h[j];
			const uint32_t a = LD(h->start), e = LD(h->end), hs = LD(h->size);
			int w;

			if (LD(h->part) != pt || a == e)
				continue;
			if (sector == e) {
				w = (int)(hs & 3u) == k ? PT_SAME : PT_MIX;
			} else if (sector >= a && sector < e) {
				w = e0 > e ? PT_OVER : PT_IN;
				if (w == PT_IN) {
					/* read by the storage since (29 bits: wraps alike) */
					const uint32_t s = ((tot << 3) - (hs & ~7u)) >> 3;

					if (s < since) {
						since = s;
						of = hs;
					}
				}
			} else if (sector < a && sector + 2 * RA_PIECE_SECTORS >= a) {
				w = PT_BACK;
				if ((e0 >= a ? 0 : a - e0) < gap)
					gap = e0 >= a ? 0 : a - e0;
			} else if (sector > e && sector - e <= 2 * RA_PIECE_SECTORS) {
				w = PT_SKIP;
			} else {
				continue;
			}
			if (w < c)
				c = w;
		}
	}
	ADD(dg->pc.n[k][c], 1);
	ADD(dg->pc.us[k][c], (uint64_t)us);
	if (c == PT_IN) {
		ADD(dg->ag.n[k][PT_AGAIN_IN + (since <= 3 * RA_PIECE_SECTORS ? 0 : since <= 2048u ? 1 : 2)], 1);
		ADD(dg->ag.in_of[k][of & 3u], 1);
		if (of & 4u)
			ADD(dg->ag.in_of[k][PT_OF_RA], 1);
	}
	else if (c == PT_BACK)
		ADD(dg->ag.n[k][PT_AGAIN_BACK + (!gap ? 0 : gap <= RA_SMALL_SECTORS ? 1 : 2)], 1);
	{
		PtRead *h = &t->h[__atomic_fetch_add(&t->i, 1, __ATOMIC_RELAXED) % PT_HIST];

		ST(h->part, pt);
		ST(h->size, (uint32_t)k | (ra ? 4u : 0u) | (tot + nsec) << 3);
		ST(h->start, sector);
		ST(h->end, sector + nsec);
	}
	ST(t->own_part[k], pt);
	ST(t->own_end[k], sector + nsec);
}

/* The session's lines for where its reads lay (pt_note; `print` 0: none,
 * the first), then counted from here. The worker. */
static void pt_session(Diag *dg, int print)
{
	static const char *const what[3] = { "tiny", "32K", "large" };
	PtCount now;

	for (int k = 0; k < 3; k++)
		for (int c = 0; c < PT_N; c++) {
			now.n[k][c] = LD(dg->pc.n[k][c]);
			now.us[k][c] = LD(dg->pc.us[k][c]);
		}
	for (int k = 0; print && k < 3; k++) {
		uint32_t n[PT_N], all = 0;
		uint32_t ms[PT_N];

		for (int c = 0; c < PT_N; c++) {
			n[c] = now.n[k][c] - dg->pc_at.n[k][c];
			ms[c] = (uint32_t)((now.us[k][c] - dg->pc_at.us[k][c]) / 1000u);
			all += n[c];
		}
		if (all < 16)
			continue;
		/* per cent / ms each: under the log's 192 characters */
		VM_KLOGI("session order %s %u: own %u/%u same %u/%u mix %u/%u over %u/%u in %u/%u "
		         "back %u/%u skip %u/%u far %u/%u", what[k], (unsigned)all,
		         (unsigned)(n[0] * 100u / all), (unsigned)ms[0], (unsigned)(n[1] * 100u / all),
		         (unsigned)ms[1], (unsigned)(n[2] * 100u / all), (unsigned)ms[2],
		         (unsigned)(n[3] * 100u / all), (unsigned)ms[3], (unsigned)(n[4] * 100u / all),
		         (unsigned)ms[4], (unsigned)(n[5] * 100u / all), (unsigned)ms[5],
		         (unsigned)(n[6] * 100u / all), (unsigned)ms[6], (unsigned)(n[7] * 100u / all),
		         (unsigned)ms[7]);
	}
	/* how far the reads again and before lay (PtAgain), per cent of each */
	for (int k = 0; k < 3; k++) {
		uint32_t a[PT_AGAIN_N], f[4], in, bk;

		for (int b = 0; b < PT_AGAIN_N; b++)
			a[b] = __atomic_exchange_n(&dg->ag.n[k][b], 0, __ATOMIC_RELAXED);
		for (int b = 0; b < 4; b++)
			f[b] = __atomic_exchange_n(&dg->ag.in_of[k][b], 0, __ATOMIC_RELAXED);
		in = a[0] + a[1] + a[2];
		bk = a[3] + a[4] + a[5];
		if (!print || in + bk < 16)
			continue;
		/* in: how far, then in what (tiny / 32K / large reads, from R/A) */
		VM_KLOGI("session again %s: in %u (384K %u 1M %u more %u; in t %u s %u l %u R/A %u), "
		         "back %u (next %u 32K %u more %u)", what[k], (unsigned)in,
		         (unsigned)(in ? a[0] * 100u / in : 0), (unsigned)(in ? a[1] * 100u / in : 0),
		         (unsigned)(in ? a[2] * 100u / in : 0), (unsigned)(in ? f[0] * 100u / in : 0),
		         (unsigned)(in ? f[1] * 100u / in : 0), (unsigned)(in ? f[2] * 100u / in : 0),
		         (unsigned)(in ? f[3] * 100u / in : 0), (unsigned)bk,
		         (unsigned)(bk ? a[3] * 100u / bk : 0), (unsigned)(bk ? a[4] * 100u / bk : 0),
		         (unsigned)(bk ? a[5] * 100u / bk : 0));
	}
	dg->pc_at = now;
}

static int stage_xfer(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                      void *part, uint32_t sector, void *buf, uint32_t nsec)
{
	const int mine = nsec && on_port(part, d->port);
	const int lg = mine && LD(g_log_diag);
	/* Adaptive: the reads timed for "on or off, as measured" (oo_note; the
	 * rest is the worker's, on the system core) */
	const int ms = mine && !write && LD(d->enabled) && ra_learn_on();
	const uint32_t t0 = lg || ms ? ksceKernelGetSystemTimeLow() : 0;
	XInfo xi = { 0, 0, 0, REQ_PID_NONE };
	int col = 0, r;
	uint32_t us;

	if (lg && !write && __atomic_load_n(&d->ra_ready, __ATOMIC_ACQUIRE))
		for (int i = 0; i < RA_PIECES; i++)
			col |= __atomic_load_n(&d->ra[i].state, __ATOMIC_ACQUIRE) == RA_READING;
	r = stage_xfer_w(d, write, io, ref, rd_ref, part, sector, buf, nsec, &xi);
	if (!lg && !ms)
		return r;
	us = ksceKernelGetSystemTimeLow() - t0;
	if (lg) {
		lg_note(d, write, nsec, us, col);
		if (!write && r >= 0 && !bg_req(&xi.pid))
			pt_note(d, part, sector, nsec, us, xi.got != 0);   /* where it lay */
		if (!write && r >= 0)
			lk_note(nsec < RA_SMALL_SECTORS ? LK_TINY : xi.nc == 2 ? -1 : xi.nc, nsec, us, col,
			        nsec < RA_SMALL_SECTORS ? xi.tiny : !xi.got ? 0 : xi.got >= nsec ? 1 : 2);
	}
	if (ms && r >= 0)
		oo_note(nsec < RA_SMALL_SECTORS ? OO_TINY : xi.nc == 2 ? -1 : xi.nc,
		        nsec < RA_SMALL_SECTORS ? xi.tiny : xi.got != 0, nsec, us);
	if (ms && r >= 0 && nsec >= RA_SMALL_SECTORS && xi.nc < 2 && oo_off(xi.nc))
		gap_note(d, xi.nc, nsec, t0, us);        /* the pause before it, on a stream */
	return r;
}

static int stage_xfer_w(Dev *d, int write, io_fn io, tai_hook_ref_t ref, tai_hook_ref_t rd_ref,
                        void *part, uint32_t sector, void *buf, uint32_t nsec, XInfo *xi)
{
	int r;

	if (!write)
		return stage_xfer_one(d, 0, io, ref, rd_ref, part, sector, buf, nsec, xi);
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
		/* t_end first: the block's start is stored before its end
		 * (ra_tiny_read), so the start seen is that block's - read the other
		 * way round, an older start could hide part of the block from this
		 * write, and the block's old data be served after it */
		const uint64_t e = __atomic_load_n(&d->t_end, __ATOMIC_SEQ_CST);
		const uint64_t a = __atomic_load_n(&d->t_start, __ATOMIC_SEQ_CST);

		if (e != a && (uint64_t)sector < e && (uint64_t)sector + nsec > a)
			__atomic_store_n(&d->t_dirty, 1, __ATOMIC_SEQ_CST);
	}
	for (uint32_t i = 0; i < RC_UNITS; i++) {   /* the again cache's, as the pieces */
		RcEnt *e = &g_rc.e[i];

		if (__atomic_load_n(&e->dev, __ATOMIC_SEQ_CST) == d) {
			const uint64_t a = __atomic_load_n(&e->sector, __ATOMIC_SEQ_CST);
			const uint64_t z = a + __atomic_load_n(&e->n, __ATOMIC_SEQ_CST);

			if ((uint64_t)sector < z && (uint64_t)sector + nsec > a)
				__atomic_store_n(&e->dirty, 1, __ATOMIC_SEQ_CST);
		}
	}
	r = stage_xfer_one(d, 1, io, ref, rd_ref, part, sector, buf, nsec, xi);
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

/* At the start: device `d`'s staging buffer, and the overlapped reads' and
 * the tiny reads' buffers if what is set will use them (their threads come
 * later, from the worker, once the home screen is up). One that fails is
 * tried again when it is needed. */
static void prealloc(Dev *d)
{
	ensure_buf(d);
	if (!__atomic_load_n(&d->buf, __ATOMIC_ACQUIRE))
		return;
	if (LD(g_pipeline) || LD(g_offload) || LD(g_readahead))
		(void)ensure_buf2(d);                 /* tried again when needed */
	if (LD(g_readahead))
		tiny_alloc(d);
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
		f = 0;
		if (module_get_export_func(KERNEL_PID, "SceSysmem", LIB_CPU_DRIVER, NID_DCACHE_CINV,
		                           &f) >= 0 && f)
			g_dcache_cinv = (void (*)(void *, unsigned int))f;
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
	/* Every buffer the devices set on will use, now while contiguous memory
	 * is easy to find - though none comes on before the home screen
	 * (g_hold, vm_stage_go); the others get theirs when they are switched on
	 * (vm_stage_apply). */
	ST(g_live, 1);
	/* Low kernel memory: decided once, here - both storages set on */
	g_share_on = LD(g_lowmem_set) && LD(g_dev[VM_DEV_SD2VITA].want) &&
	             LD(g_dev[VM_DEV_MEMCARD].want) && !g_dev[VM_DEV_SD2VITA].buf &&
	             !g_dev[VM_DEV_MEMCARD].buf;
	g_dev[VM_DEV_SD2VITA].shared = g_dev[VM_DEV_MEMCARD].shared = g_share_on;
	if (g_share_on)
		VM_KLOGI("stage: low kernel memory: one buffer for the SD2Vita and the memory card");
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (LD(g_dev[i].want))
			prealloc(&g_dev[i]);
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

/* The home screen is up (main.c, guard.h): from now on devices come on as
 * set (the caller applies the settings next), and the app in front is
 * followed - the process events, hooked as PSVshell hooks them: SceProcessmgr's
 * import of ksceKernelInvokeProcEventHandler (an import, not an export: there
 * is none to hook; an import is one word to change, safe while it runs).
 * 1 the first time, 0 after. */
int vm_stage_go(void)
{
	if (!LD(g_hold) || !LD(g_live))
		return 0;
	g_proc_ref = 0;
	g_proc_hook = taiHookFunctionImportForKernel(KERNEL_PID, (tai_hook_ref_t *)&g_proc_ref, "SceProcessmgr",
	                                             LIB_PROCMGR_KERNEL, NID_PROC_EVENT,
	                                             ra_proc_event_hook);
	if (g_proc_hook >= 0)
		VM_KLOGI("stage: app events followed");
	else
		VM_KLOGW("stage: app events not hooked (0x%08x) - the per-app settings and read "
		         "ahead Auto follow no app", (unsigned)g_proc_hook);
	ST(g_hold, 0);
	return 1;
}

void vm_stage_fini(void)
{
	ST(g_live, 0);                      /* nothing allocated, nothing comes on, again */
	/* Stop taking the faster path, then wait for any transfer in flight. */
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		ST(g_dev[i].enabled, 0);
		ST(g_dev[i].unusable, 1);       /* and never allocate again */
	}
	for (int i = 0; i < VM_DEV_COUNT; i++)
		while (__atomic_load_n(dbusy(&g_dev[i]), __ATOMIC_ACQUIRE))
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
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];

		if (d->uid >= 0) {
			__atomic_store_n(&d->buf, NULL, __ATOMIC_RELEASE);
			ksceKernelFreeMemBlock(d->uid);
			d->uid = -1;
		}
		/* made at the start, never put to use (prealloc) */
		if (!d->pipe_made && d->uid2 >= 0) {
			ksceKernelFreeMemBlock(d->uid2);
			d->uid2 = -1;
			d->buf2 = NULL;
		}
		if (!d->ra_made && d->t_uid >= 0) {
			ksceKernelFreeMemBlock(d->t_uid);
			d->t_uid = -1;
			d->t_buf = NULL;
			d->t_cached = 0;
		}
	}
	if (g_rc.uid >= 0) {                  /* the again cache's own block */
		__atomic_store_n(&g_rc.own, NULL, __ATOMIC_RELEASE);
		ksceKernelFreeMemBlock(g_rc.uid);
		g_rc.uid = -1;
	}
	__atomic_store_n(&g_rc.host, NULL, __ATOMIC_SEQ_CST);
	g_rc.user = NULL;
	rc_clear();
	if (g_shadow_uid >= 0) {
		__atomic_store_n(&g_shadow, NULL, __ATOMIC_RELEASE);
		ksceKernelFreeMemBlock(g_shadow_uid);
		g_shadow_uid = -1;
	}
	/* Low kernel memory: the shared buffers, last */
	for (int i = 0; i < VM_DEV_COUNT; i++)
		if (g_dev[i].shared) {
			__atomic_store_n(&g_dev[i].buf, NULL, __ATOMIC_RELEASE);
			g_dev[i].buf2 = NULL;
		}
	if (g_sh.uid >= 0)
		ksceKernelFreeMemBlock(g_sh.uid);
	if (g_sh.uid2 >= 0)
		ksceKernelFreeMemBlock(g_sh.uid2);
	g_sh.uid = g_sh.uid2 = -1;
	g_sh.buf = g_sh.buf2 = NULL;
	__atomic_store_n(&g_sh.owner, NULL, __ATOMIC_RELAXED);
	g_dev[VM_DEV_SD2VITA].shared = g_dev[VM_DEV_MEMCARD].shared = g_share_on = 0;
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

static int app_in_front(void);

/* Put the settings in effect. Any thread. */
static void eff_update(void)
{
	VmApp a;
	int needed, pipeline, offload, gpusafe, ra;

	eff_lock();
	if (app_in_front())
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
	ST(g_ra_own, a.ra >= 0 ? 1 : 0);
	ST(g_readahead, ra != VM_RA_OFF ? 1 : 0);
	/* adaptive: whose settings these are, and what it learns under (the
	 * mode, the game's own thresholds - 0 none, 15 never) */
	{
		const int app = app_in_front();
		uint32_t key = (uint32_t)ra & 0xFu;

		for (int k = 0; k < 2; k++) {
			const uint32_t t = a.thr[k];

			key |= (t == RA_THR_NONE ? 15u : t & 0xFu) << (4 + 4 * k);
		}
		/* the pid read with the title: both are the loaded app's (g_ra_fg may
		 * have moved on since app_in_front looked - a process event) */
		ra_adapt_switch_pid(app ? g_app_title : "", key,
		                    app ? __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE) : 0);
	}
	/* The helper (and copier) wanted again after no setting wanted it: a
	 * fresh try at making them, whichever setting was switched on. */
	if (!needed && (pipeline || offload || ra))
		for (int i = 0; i < VM_DEV_COUNT; i++)
			ST(g_dev[i].pipe_failed, 0);
	__sync_lock_release(&g_eff_lock);
}

/* With the log on, once: what each of the plugin's blocks really takes - the
 * size asked for against what the system set aside for it, and in how many
 * physical stretches. */
static void mem_report_one(const char *what, int dev, SceUID uid, uint32_t asked)
{
	VmMemBlockInfoEx info;
	int r;

	if (uid < 0)
		return;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	r = ksceKernelMemBlockGetInfoEx(uid, &info);
	if (r < 0) {
		VM_KLOGI("stage: memory %s (dev %d): %u KiB asked, info 0x%08x", what, dev,
		         (unsigned)(asked / 1024), (unsigned)r);
		return;
	}
	VM_KLOGI("stage: memory %s dev %d: asked %u, set aside %u, mapped %u B; type 0x%08x; "
	         "%u stretches, the first %u B at 0x%08x", what, dev, (unsigned)asked,
	         (unsigned)info.alloc_map_size, (unsigned)info.mapped_size, (unsigned)info.type,
	         (unsigned)info.physical_range_count_written, (unsigned)info.physical_sizes[0],
	         (unsigned)(uintptr_t)info.physical_addresses[0]);
}

static void mem_report(void)
{
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const Dev *d = &g_dev[i];

		mem_report_one("staging", i, d->uid, STAGE_BYTES);
		mem_report_one("second buffer", i, d->uid2, PIPE_TAIL_SECTORS * 512);
		mem_report_one("tiny block", i, d->t_uid, RA_TINY_SECTORS * 512);
	}
	mem_report_one("shared staging", -1, g_sh.uid, STAGE_BYTES);
	mem_report_one("shared second buffer", -1, g_sh.uid2, PIPE_TAIL_SECTORS * 512);
	mem_report_one("log counts", -1, g_diag_uid, (uint32_t)sizeof(Diag));
	if (g_shadow_uid >= 0)
		mem_report_one("verify", -1, g_shadow_uid, STAGE_BYTES);
}

/* The log's block (see Diag): made once, the first time the log is on. */
static void diag_make(void)
{
	void *b = NULL;

	if (DG() || __sync_lock_test_and_set(&g_diag_busy, 1))
		return;
	if (!DG() && alloc_buf(SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW, 0, (sizeof(Diag) + 0xFFFu) & ~0xFFFu,
	                       &g_diag_uid, &b) >= 0) {
		memset(b, 0, sizeof(Diag));
		__atomic_store_n(&g_diag, (Diag *)b, __ATOMIC_RELEASE);   /* published last */
	} else if (!DG()) {
		g_diag_uid = -1;
		VM_KLOGW("stage: the log's counts not allocated: no session lines");
	}
	__sync_lock_release(&g_diag_busy);
}

void vm_stage_apply(const VmSettings *s)
{
	/* the internal storage never: its setting is reserved (VM_KEY_INTERNAL) */
	const int hold = LD(g_hold);
	const int32_t want[VM_DEV_COUNT] = { hold ? 0 : s->sd2vita, 0, hold ? 0 : s->memcard };

	for (int i = 0; i < VM_DEV_COUNT; i++) {
		Dev *d = &g_dev[i];

		if ((want[i] ? 1 : 0) != LD(d->enabled))
			ADD(d->ra_epoch, 1);  /* nothing read ahead before is used after */
		if (want[i] && !LD(d->enabled)) {
			ST(d->fallback, 0);   /* switching it back on is a fresh start */
			ST(d->alloc_err, 0);  /* ... and a fresh try for its buffer    */
		}
		ST(d->want, (i == VM_DEV_SD2VITA ? s->sd2vita : i == VM_DEV_MEMCARD ? s->memcard : 0) ? 1 : 0);
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
	if ((s->sysapps ? 1 : 0) != LD(g_sysapps)) {
		ST(g_sysapps, s->sysapps ? 1 : 0);
		__atomic_store_n(&g_app_reload, 1, __ATOMIC_RELEASE);   /* the worker: whose now */
		ra_wake();
	}
	ST(g_lowmem_set, s->lowmem ? 1 : 0);   /* used at the start only */
	if (s->log)
		diag_make();
	ST(g_log_diag, s->log && DG() ? 1 : 0);
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

	char ft[16];
	const SceUID now = fg_get(ft);        /* in front now, with its title */

	eff_lock();
	ra_app_global(&g_app_cur);
	if (!fg && LD(g_sysapps)) {
		memcpy(g_app_title, HOME_TITLE, sizeof(HOME_TITLE));   /* the home screen's */
	} else if (fg && fg == now) {
		memcpy(g_app_title, ft, sizeof(g_app_title));
	} else if (!fg || fg != __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE)) {
		g_app_title[0] = 0;
	}                                     /* else suspended, reloaded: its title kept */
	__sync_lock_release(&g_eff_lock);
	if (g_app_title[0] && ra_prof_load(g_app_title, &pf)) {
		eff_lock();
		g_app_cur = pf.app;
		__sync_lock_release(&g_eff_lock);
		if (ra_app_set_any(&pf.app))
			VM_KLOGI("stage: %s - its own settings (read ahead %d, copy %d, overlapped %d, "
			         "GPU-safe %d, start after %u / %u)", g_app_title, (int)pf.app.ra,
			         (int)pf.app.offload, (int)pf.app.pipe, (int)pf.app.gpusafe,
			         (unsigned)pf.app.thr[0], (unsigned)pf.app.thr[1]);
	}
	eff_lock();
	g_app_saved = g_app_cur;
	__sync_lock_release(&g_eff_lock);
	__atomic_store_n(&g_app_pid, fg, __ATOMIC_RELEASE);
	eff_update();
}

void vm_stage_app_now(VmAppNow *o)
{
	memset(o, 0, sizeof(*o));
	eff_lock();
	if (app_in_front()) {
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

/* Whether the settings loaded (g_app_cur) are those of what is in front:
 * an app, or the home screen (pid 0, HOME_TITLE) with VM_KEY_SYSAPPS on. */
static int app_in_front(void)
{
	const SceUID pid = __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE);

	return pid == __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE) && g_app_title[0];
}

int vm_stage_app_set(const VmApp *a)
{
	VmApp v;

	if (!a || !ra_app_ok(a))
		return VM_ERR_ARG;
	v = *a;
	/* its thresholds only mean something with read ahead on for it */
	if ((v.ra == VM_APP_GLOBAL ? LD(g_glob_ra) : v.ra) == VM_RA_OFF)
		v.thr[0] = v.thr[1] = 0;
	eff_lock();
	if (!app_in_front()) {                /* checked with the app: the worker loads under it */
		__sync_lock_release(&g_eff_lock);
		return VM_ERR_NOT_READY;
	}
	g_app_cur = v;
	__sync_lock_release(&g_eff_lock);
	eff_update();
	return VM_OK;
}

/* `title`'s settings `snap` into its profile (the worker, vm_stage_io_tick). */
static int app_save_now(const char *title, const VmApp *snap)
{
	RaProf pf;

	pf.app = *snap;
	{
		RaProf old;

		/* written only if it changes what the file holds (all global too:
		 * the file stays, no removing and making it again) */
		if (!ra_prof_load(title, &old) || memcmp(&old.app, &pf.app, sizeof(pf.app))) {
			ra_prof_save(title, &pf);
			if (!ra_prof_load(title, &old) || memcmp(&old.app, &pf.app, sizeof(pf.app))) {
				VM_KLOGW("stage: %s - its settings NOT saved (the card?)", title);
				return VM_ERR_IO;
			}
		}
	}
	eff_lock();
	if (app_in_front() && !strcmp(title, g_app_title))
		g_app_saved = *snap;
	__sync_lock_release(&g_eff_lock);
	VM_KLOGI("stage: %s - its settings saved", title);
	return VM_OK;
}

/* ---- what the overlay asks for, done by the worker from the top -----------
 *
 * The overlay's keys are handled deep in the worker's stack (the HUD
 * module's tick, its overlay, the action), and the file written for them -
 * tuning.bin made again (Tuning reset), a profile made or written - goes
 * through the file system's deepest path, and our own write hook, on top of
 * that: Triangle twice restarted the console (the worker has 6 KiB). So the
 * overlay only asks, and the worker does it from the top of its loop
 * (vm_stage_io_tick), its stack free, as for its other files. One of each
 * at a time; a newer ask replaces one not done yet. */
static volatile int g_rq_lock;
static int          g_rq_cal_reset;            /* under g_rq_lock          */
static int          g_rq_cal_set;              /* 0 none, 1 Off, 2 Auto    */
static int          g_rq_save;                 /* a profile: g_rq_title, g_rq_app */
static char         g_rq_title[sizeof(g_app_title)];
static VmApp        g_rq_app;

static void rq_lock(void)
{
	while (__sync_lock_test_and_set(&g_rq_lock, 1))
		;                                      /* a few words, never long */
}

void vm_stage_cal_reset(void)
{
	rq_lock();
	g_rq_cal_reset = 1;
	g_rq_cal_set = 0;                          /* the reset keeps the switch */
	__sync_lock_release(&g_rq_lock);
	ra_wake();
}

void vm_stage_cal_set(int on)
{
	ST(g_cal_on, on ? 1 : 0);                  /* shown at once; kept by the worker */
	rq_lock();
	g_rq_cal_set = on ? 2 : 1;
	__sync_lock_release(&g_rq_lock);
	ra_wake();
}

/* The app in front's settings as they are now, to be saved: VM_OK once asked
 * (the overlay shows it saved once it is: g_app_saved), or VM_ERR_NOT_READY. */
int vm_stage_app_save(void)
{
	char title[sizeof(g_app_title)];
	VmApp snap;
	int ok;

	/* whose and what, taken together: the worker may load another app's */
	eff_lock();
	ok = app_in_front();
	if (ok) {
		memcpy(title, g_app_title, sizeof(title));
		snap = g_app_cur;
	}
	__sync_lock_release(&g_eff_lock);
	if (!ok)
		return VM_ERR_NOT_READY;
	rq_lock();
	memcpy(g_rq_title, title, sizeof(g_rq_title));
	g_rq_app = snap;
	g_rq_save = 1;
	__sync_lock_release(&g_rq_lock);
	ra_wake();
	return VM_OK;
}

void vm_stage_io_tick(void)
{
	int reset, set, save;
	char title[sizeof(g_app_title)];
	VmApp snap;

	rq_lock();
	reset = g_rq_cal_reset;
	set = g_rq_cal_set;
	save = g_rq_save;
	if (save) {
		memcpy(title, g_rq_title, sizeof(title));
		snap = g_rq_app;
	}
	g_rq_cal_reset = g_rq_cal_set = g_rq_save = 0;
	__sync_lock_release(&g_rq_lock);
	if (reset)
		cal_reset_now();
	if (set)
		cal_set_now(set == 2);
	if (save)
		(void)app_save_now(title, &snap);
}

void vm_stage_on_problem(void (*fn)(void))
{
	__atomic_store_n(&g_on_problem, fn, __ATOMIC_RELEASE);
}

/* ---- the session lines (log on): what an app's time in front cost -------- */


static void lg_snap(LgSnap *o)
{
	const Diag *const dg = DG();

	memset(o, 0, sizeof(*o));
	if (!dg)
		return;
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const LgDev *g = &dg->lg[i];
		LgDev *t = &o->dev[i];

		for (int k = 0; k < 3; k++) {
			t->us[k] = LD(g->us[k]);
			t->n[k] = LD(g->n[k]);
			t->sec[k] = LD(g->sec[k]);
		}
		t->col_us = LD(g->col_us);
		t->col_n = LD(g->col_n);
		t->wr_us = LD(g->wr_us);
		t->wr_n = LD(g->wr_n);
		t->wr_sec = LD(g->wr_sec);
		t->max_us = 0;
	}
	for (int i = 0; i < 3; i++) {
		const LgKind *g = &dg->lk[i];
		LgKind *t = &o->k[i];

		for (int j = 0; j < 3; j++) {
			t->us[j] = LD(g->us[j]);
			t->n[j] = LD(g->n[j]);
			t->sec[j] = LD(g->sec[j]);
		}
		t->col_us = LD(g->col_us);
		t->held_us = LD(g->held_us);
		t->col_n = LD(g->col_n);
		t->waste = LD(g->waste);
		t->spoilt = LD(g->spoilt);
		t->col_max = 0;
	}
	o->hits = LD(g_ra_hits);
	o->kib = LD(g_ra_kib);
	o->drops = LD(g_ra_drops);
	o->waste_kib = LD(g_ra_waste_kib);
	o->bad = LD(g_ra_bad);
	o->wskip = LD(g_ra_wskip);
	o->nc_kib = LD(g_ra_nc_kib);
	o->tiny = LD(g_ra_tiny_reads);
	o->pipe = LD(g_pipe_reads);
	o->off = LD(g_off_reads);
	o->ncd = LD(g_ncd_reads);
	o->dma = LD(g_dma_copies);
	o->nc_stock = LD(g_nc_stock_kib);
	o->bg_n = LD(g_bg_n);
	o->bg_kib = LD(g_bg_kib);
	o->bg_kept = LD(g_bg_kept);
}

static const char *lg_ra_name(int m)
{
	return m == VM_RA_ADAPTIVE ? "Adaptive" : m == VM_RA_OFF ? "Off" : "On";
}

/* us per MiB of a kind (0: none read) */
static uint32_t lg_per_mib(uint64_t us, uint32_t sec)
{
	return sec ? (uint32_t)(us * 2048u / sec) : 0;
}

/* What `who` (in front for ms) cost, from snapshot a to now (b), with the
 * settings in effect for it (the next app's are not loaded yet). */
static void lg_session(const char *who, uint32_t ms, const LgSnap *a, const LgSnap *b)
{
	Diag *const dg = DG();                  /* there: a is its snapshot */
	const uint32_t t0 = ra_threshold(0), t1 = ra_threshold(1);
	int any = 0;

	for (int i = 0; i < VM_DEV_COUNT; i++) {
		const LgDev *x = &a->dev[i], *y = &b->dev[i];
		uint32_t n[3], sec[3], all_sec = 0;
		uint64_t us[3], all_us = 0;

		if (y->n[0] + y->n[1] + y->n[2] + y->wr_n == x->n[0] + x->n[1] + x->n[2] + x->wr_n)
			continue;
		if (!any)
			VM_KLOGI("session %s %u.%01us: R/A %s C%u NC%u, 32K C%u NC%u, tiny %u/%uK, "
			         "off %d%d%d; copy %d ovl %d gpu %d dma %d", who, (unsigned)(ms / 1000u),
			         (unsigned)(ms % 1000u / 100u), lg_ra_name(LD(g_ra_mode)), (unsigned)t0,
			         (unsigned)t1, (unsigned)ra_small_threshold(0, t0),
			         (unsigned)ra_small_threshold(1, t1), (unsigned)tiny_thr(),
			         (unsigned)(tiny_blk() / 2u), oo_off(OO_C), oo_off(OO_NC), oo_off(OO_TINY),
			         LD(g_offload), LD(g_pipeline), LD(g_gpu_safe),
			         dma_state() == VM_DMA_ON);
		any = 1;
		for (int k = 0; k < 3; k++) {
			n[k] = y->n[k] - x->n[k];
			sec[k] = y->sec[k] - x->sec[k];
			us[k] = y->us[k] - x->us[k];
			all_us += us[k];
			all_sec += sec[k];
		}
		VM_KLOGI("session dev %d: reads %u ms, %u MiB, %u us/MiB, max %u ms; during R/A "
		         "piece %u (%u ms); writes %u (%u KiB) %u ms", i, (unsigned)(all_us / 1000u),
		         (unsigned)(all_sec / 2048u), (unsigned)lg_per_mib(all_us, all_sec),
		         (unsigned)(__atomic_exchange_n(&dg->lg[i].max_us, 0, __ATOMIC_RELAXED) / 1000u),
		         (unsigned)(y->col_n - x->col_n), (unsigned)((y->col_us - x->col_us) / 1000u),
		         (unsigned)(y->wr_n - x->wr_n), (unsigned)((y->wr_sec - x->wr_sec) / 2u),
		         (unsigned)((y->wr_us - x->wr_us) / 1000u));
		VM_KLOGI("session dev %d: tiny %u (%u KiB) %u ms; 32K %u (%u MiB) %u us/MiB; "
		         "large %u (%u MiB) %u us/MiB", i, (unsigned)n[0], (unsigned)(sec[0] / 2u),
		         (unsigned)(us[0] / 1000u), (unsigned)n[1], (unsigned)(sec[1] / 2048u),
		         (unsigned)lg_per_mib(us[1], sec[1]), (unsigned)n[2], (unsigned)(sec[2] / 2048u),
		         (unsigned)lg_per_mib(us[2], sec[2]));
	}
	if (!any)
		return;
	/* by kind: the standard way against read ahead's, what it cost */
	for (int k = 0; k < 3; k++) {
		const LgKind *x = &a->k[k], *y = &b->k[k];
		uint32_t n[3], sec[3];
		uint64_t us[3];
		const uint32_t cmax = __atomic_exchange_n(&dg->lk[k].col_max, 0, __ATOMIC_RELAXED);

		for (int j = 0; j < 3; j++) {
			n[j] = y->n[j] - x->n[j];
			sec[j] = y->sec[j] - x->sec[j];
			us[j] = y->us[j] - x->us[j];
		}
		if (!n[0] && !n[1] && !n[2])
			continue;
		if (k == LK_TINY)
			VM_KLOGI("session tiny: standard %u (%u KiB) %u us each; from the block %u, %u us "
			         "each; blocks read %u, %u us each; spoilt by writes %u", (unsigned)n[0],
			         (unsigned)(sec[0] / 2u), (unsigned)(n[0] ? us[0] / n[0] : 0), (unsigned)n[1],
			         (unsigned)(n[1] ? us[1] / n[1] : 0), (unsigned)n[2],
			         (unsigned)(n[2] ? us[2] / n[2] : 0), (unsigned)(y->spoilt - x->spoilt));
		else
			VM_KLOGI("session %s: standard %u (%u MiB) %u us/MiB; from R/A %u (%u MiB) %u us/MiB; "
			         "partly %u (%u MiB) %u us/MiB", k ? "NC" : "C", (unsigned)n[0],
			         (unsigned)(sec[0] / 2048u), (unsigned)lg_per_mib(us[0], sec[0]), (unsigned)n[1],
			         (unsigned)(sec[1] / 2048u), (unsigned)lg_per_mib(us[1], sec[1]), (unsigned)n[2],
			         (unsigned)(sec[2] / 2048u), (unsigned)lg_per_mib(us[2], sec[2]));
		VM_KLOGI("session %s: during a piece %u, %u ms, longest %u us; pieces for nothing %u, "
		         "held reads %u ms, spoilt by writes %u", k == LK_TINY ? "tiny" : k ? "NC" : "C",
		         (unsigned)(y->col_n - x->col_n), (unsigned)((y->col_us - x->col_us) / 1000u),
		         (unsigned)cmax, (unsigned)(y->waste - x->waste),
		         (unsigned)((y->held_us - x->held_us) / 1000u),
		         k == LK_TINY ? 0u : (unsigned)(y->spoilt - x->spoilt));
	}
	VM_KLOGI("session R/A: hits %u (%u MiB, NC %u), dropped %u (%u MiB unused), unusable %u, "
	         "not read (a write) %u, tiny hits %u", (unsigned)(b->hits - a->hits),
	         (unsigned)((b->kib - a->kib) / 1024u), (unsigned)((b->nc_kib - a->nc_kib) / 1024u),
	         (unsigned)(b->drops - a->drops), (unsigned)((b->waste_kib - a->waste_kib) / 1024u),
	         (unsigned)(b->bad - a->bad), (unsigned)(b->wskip - a->wskip),
	         (unsigned)(b->tiny - a->tiny));
	VM_KLOGI("session: overlapped %u, core 3 %u, NC helper %u, NC standard %u MiB, DMA %u",
	         (unsigned)(b->pipe - a->pipe), (unsigned)(b->off - a->off),
	         (unsigned)(b->ncd - a->ncd), (unsigned)((b->nc_stock - a->nc_stock) / 1024u),
	         (unsigned)(b->dma - a->dma));
	if (b->bg_n != a->bg_n)
		VM_KLOGI("session: other apps' reads %u (%u MiB), no read ahead; the standard way to "
		         "keep the app's pieces %u", (unsigned)(b->bg_n - a->bg_n),
		         (unsigned)((b->bg_kib - a->bg_kib) / 1024u), (unsigned)(b->bg_kept - a->bg_kept));
}

/* The again cache's own block back to the system once nothing uses it:
 * the game it was made for left the front (it quit, or went to the home
 * screen - suspended, ev 4) or another storage lends its buffers now. Made
 * again when a game in front needs it (rc_hold). What it held is dropped (a
 * new block has none of it). A request in the cache: the next tick. Worker. */
static void rc_own_idle(void)
{
	SceUID uid;
	int lent;

	if (!__atomic_load_n(&g_rc.own, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&g_rc.want_own, __ATOMIC_RELAXED) ||
	    __sync_lock_test_and_set(&g_rc.lock, 1))
		return;
	if (g_rc.game && !g_rc.host && g_rc.pid &&
	    __atomic_load_n(&g_ra_fg, __ATOMIC_ACQUIRE) == g_rc.pid) {
		__sync_lock_release(&g_rc.lock);      /* its game is in front: kept */
		return;
	}
	lent = g_rc.host != NULL;
	__atomic_store_n(&g_rc.own, NULL, __ATOMIC_RELEASE);
	uid = g_rc.uid;
	g_rc.uid = -1;
	rc_clear();
	__sync_lock_release(&g_rc.lock);
	ksceKernelFreeMemBlock(uid);
	ADD(g_rc.own_freed, 1);
	VM_KLOGI("stage: again cache's own block freed (%s)",
	         lent ? "another storage lends its buffers" : "its game left the front");
}

void vm_stage_worker_tick(void)
{
	static uint32_t last_log_us, last_cleaned, last_nc, last_max;
	const uint32_t now_us = ksceKernelGetSystemTimeLow();

	if (!__atomic_load_n(&g_cal_loaded, __ATOMIC_ACQUIRE) && LD(g_live))
		cal_load();                       /* what was measured before, once */
	rc_own_idle();
	/* the again cache's own block, with no other storage to lend it buffers */
	if (__atomic_load_n(&g_rc.want_own, __ATOMIC_RELAXED) && LD(g_live) &&
	    !__atomic_load_n(&g_rc.own, __ATOMIC_ACQUIRE)) {
		void *b = NULL;

		__atomic_store_n(&g_rc.want_own, 0, __ATOMIC_RELAXED);
		if (alloc_buf(MEMBLOCK_KERNEL_NC_RW, 1, RC_UNITS * RC_UNIT * 512, &g_rc.uid, &b) >= 0) {
			__atomic_store_n(&g_rc.own, b, __ATOMIC_RELEASE);
			mem_report_one("again cache", -1, g_rc.uid, RC_UNITS * RC_UNIT * 512);
		} else {
			g_rc.uid = -1;
		}
	}
	ra_adapt_tick();                      /* Adaptive's rounds that are due */
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
			         "refused %u; in the request thread: R/A %u (%u MiB), staged %u (%u MiB)",
			         (unsigned)LD(g_dma_copies), (unsigned)(LD(g_dma_kib) / 1024),
			         (unsigned)(LD(g_dma_us) / 1000), (unsigned)LD(g_dma_mid),
			         (unsigned)(LD(g_dma_mid_us) / 1000), (unsigned)LD(g_dma_errors),
			         (unsigned)LD(g_dma_here), (unsigned)(LD(g_dma_here_kib) / 1024),
			         (unsigned)LD(g_dma_here_st), (unsigned)(LD(g_dma_here_st_kib) / 1024));
		}
		VM_KLOGI("stage: overlapped reads %u (%u in 3 parts), helper waited %u times %u ms",
		         (unsigned)LD(g_pipe_reads), (unsigned)LD(g_pipe_reads3),
		         (unsigned)__atomic_load_n(&g_pipe_waits, __ATOMIC_RELAXED),
		         (unsigned)(__atomic_load_n(&g_pipe_wait_us, __ATOMIC_RELAXED) / 1000));
		if (LD(g_readahead) || LD(g_ra_hits) || LD(g_ra_drops))
			VM_KLOGI("stage: read ahead %u reads %u MiB (%u MiB non-cacheable, %u overlapping), "
			         "dropped %u (%u MiB unused), unusable %u, NC copier refused %d (0x%08x)",
			         (unsigned)LD(g_ra_hits), (unsigned)(LD(g_ra_kib) / 1024),
			         (unsigned)(LD(g_ra_nc_kib) / 1024), (unsigned)LD(g_ra_over),
			         (unsigned)LD(g_ra_drops),
			         (unsigned)(LD(g_ra_waste_kib) / 1024), (unsigned)LD(g_ra_bad),
			         (int)LD(g_nc_off_refused), (unsigned)LD(g_nc_off_type));
		if (LD(g_rc.kept_k[0]) || LD(g_rc.kept_k[1]) || LD(g_rc.moved_k[0]) || LD(g_rc.moved_k[1])) {
			const Dev *const h = __atomic_load_n(&g_rc.host, __ATOMIC_RELAXED);

			char where[24];

			if (h)
				snprintf(where, sizeof(where), "in dev %d", (int)(h - g_dev));
			else
				snprintf(where, sizeof(where), "own block");
			/* by size: on or off, moved (of that for a poor yield), kept, read
			 * again from it - under the log's 192 characters */
			VM_KLOGI("stage: again cache %s: 32K %s %u/%u kept %u again %u; large %s %u/%u "
			         "kept %u again %u; %u MiB, taken back %u", where,
			         LD(g_rc.on[0]) ? "on" : "off", (unsigned)LD(g_rc.moved_k[0]),
			         (unsigned)LD(g_rc.yoff_k[0]), (unsigned)LD(g_rc.kept_k[0]),
			         (unsigned)LD(g_rc.hits_k[0]), LD(g_rc.on[1]) ? "on" : "off",
			         (unsigned)LD(g_rc.moved_k[1]), (unsigned)LD(g_rc.yoff_k[1]),
			         (unsigned)LD(g_rc.kept_k[1]), (unsigned)LD(g_rc.hits_k[1]),
			         (unsigned)(LD(g_rc.hit_kib) / 1024), (unsigned)LD(g_rc.lost));
		}
		if (LD(g_ra_mode) == VM_RA_ADAPTIVE) {
			const uint32_t t0 = ra_threshold(0), t1 = ra_threshold(1);

			VM_KLOGI("stage: read ahead adaptive: streak C %u NC %u, 32 KiB reads C %u NC %u now, "
			         "pieces ahead C %d NC %d; moved %u / %u and %u / %u times; taken up again %u",
			         (unsigned)t0, (unsigned)t1, (unsigned)ra_small_threshold(0, t0),
			         (unsigned)ra_small_threshold(1, t1),
			         RA_PIECES - LD(g_ra_fewer[0]), RA_PIECES - LD(g_ra_fewer[1]),
			         (unsigned)LD(g_ra_moves[0]), (unsigned)LD(g_ra_moves[1]),
			         (unsigned)LD(g_rs_moves[0]), (unsigned)LD(g_rs_moves[1]),
			         (unsigned)LD(g_ad_restored));
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
		{
			/* the block's memory: cached where it is (tiny_alloc) */
			const char *const tm = g_dev[VM_DEV_SD2VITA].t_cached || g_dev[VM_DEV_MEMCARD].t_cached ?
			                       "cached" : "non-cacheable";

			if (LD(g_ra_mode) == VM_RA_ADAPTIVE)
				VM_KLOGI("stage: tiny reads from their block %u (%s); adaptive: from read %u, "
				         "block %u KiB now, moved %u", (unsigned)LD(g_ra_tiny_reads), tm,
				         (unsigned)LD(g_tiny_thr), (unsigned)(LD(g_tiny_blk) / 2),
				         (unsigned)LD(g_tn_moves));
			else
				VM_KLOGI("stage: tiny reads from their block %u (%s)",
				         (unsigned)LD(g_ra_tiny_reads), tm);
		}
		for (int i = 0; i < VM_DEV_COUNT; i++) {
			const CalDev *c = &g_cal[i];
			const uint32_t hn = LD(c->tiny_hit_n), bn = LD(c->tiny_blk_n);
			RtTinyCost t;

			if (!hn && !bn)
				continue;
			cal_tiny_cost(i, &t);
			VM_KLOGI("stage: calibration dev %d tiny: from the block %u us (%u), a block %u us of "
			         "%u sectors (%u); in use %d + %d/16 us a sector, from the block %d", i,
			         (unsigned)(hn ? LD(c->tiny_hit_us) / hn : 0), (unsigned)hn,
			         (unsigned)(bn ? LD(c->tiny_blk_us) / bn : 0),
			         (unsigned)(bn ? LD(c->tiny_blk_sec) / bn : 0), (unsigned)bn, (int)t.a_us,
			         (int)t.b16_us, (int)t.hit_us);
		}
		if (g_stack_free_fn) {
			/* the least each has had free, of its stack (-1: no such thread):
			 * logged only if one has had under STACK_LOW_BYTES */
			int fr[VM_DEV_COUNT][3], low;
			const int wk = g_stack_free_fn(ksceKernelGetThreadId());

			low = wk >= 0 && wk < STACK_LOW_BYTES;
			for (int i = 0; i < VM_DEV_COUNT; i++) {
				const SceUID th[3] = { g_dev[i].th, g_dev[i].th2, g_dev[i].ra_th };

				for (int k = 0; k < 3; k++) {
					fr[i][k] = th[k] >= 0 ? g_stack_free_fn(th[k]) : -1;
					low |= fr[i][k] >= 0 && fr[i][k] < STACK_LOW_BYTES;
				}
			}
			if (low)
				VM_KLOGW("stage: stack low (bytes free of %u / %u): worker %d; SD2Vita helper %d "
				         "copier %d ahead %d; memory card %d %d %d", VM_STACK_WORKER, VM_STACK_DEV, wk,
				         fr[VM_DEV_SD2VITA][0], fr[VM_DEV_SD2VITA][1], fr[VM_DEV_SD2VITA][2],
				         fr[VM_DEV_MEMCARD][0], fr[VM_DEV_MEMCARD][1], fr[VM_DEV_MEMCARD][2]);
		}
		if (g_share_on)
			VM_KLOGI("stage: one buffer for both storages: handed over %u times; the standard "
			         "way: %u as it was taken, %u not the app in front's",
			         (unsigned)LD(g_sh.handovers), (unsigned)LD(g_sh.other_busy),
			         (unsigned)LD(g_sh.yielded));
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
	while (DG() && g_mapfail_logged < MAPFAIL_LOG && g_mapfail_logged < LD(g_mapfail_n) &&
	       __atomic_load_n(&DG()->mapfail[g_mapfail_logged].ready, __ATOMIC_ACQUIRE)) {
		const MapFail *f = &DG()->mapfail[g_mapfail_logged++];

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
		char ft[16];
		const SceUID fg = fg_get(ft);
		const uint32_t noid = __atomic_load_n(&g_fg_noid, __ATOMIC_RELAXED);

		if (fg != last_fg) {
			/* the session of the one before (log on), then the new one's */
			static char who[16];
			static uint64_t since_us;
			const uint64_t wide_us = (uint64_t)ksceKernelGetSystemTimeWide();
			Diag *const dg = DG();

			if (LD(g_log_diag) && dg) {
				LgSnap cur;

				lg_snap(&cur);
				if (since_us)
					lg_session(who[0] ? who : "the home screen",
					           (uint32_t)((wide_us - since_us) / 1000u), &dg->at, &cur);
				pt_session(dg, since_us != 0);   /* where its reads lay */
				dg->at = cur;
				since_us = wide_us;
			} else {
				since_us = 0;
			}
			if (fg)
				memcpy(who, ft, sizeof(who));
			else
				who[0] = 0;
			last_fg = fg;
			if (fg)
				VM_KLOGI("stage: in front: %s (pid 0x%08x)", ft, (unsigned)fg);
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
			char ft[16];
			const SceUID fg = fg_get(ft);

			VM_KLOGI("stage: v%s, app events %s, title ids %s, in front: %s", VM_BUILD_STR,
			         g_proc_hook >= 0 ? "followed" : "NOT hooked",
			         g_title_fn ? "there" : "MISSING", fg ? ft : "the home screen");
			mem_report();
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
		/* the home screen in front: its own (VM_KEY_SYSAPPS), loaded in
		 * the place of the app's - which comes back from its profile */
		const int home = !fg && LD(g_sysapps);

		if (__atomic_exchange_n(&g_app_reload, 0, __ATOMIC_ACQ_REL))
			app_load(fg || home ? fg : __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE));
		else if (fg && fg != __atomic_load_n(&g_app_pid, __ATOMIC_ACQUIRE))
			app_load(fg);
		else if (home && !app_in_front())
			app_load(0);
		else if (fg != last_fg)
			eff_update();
		/* tuning.bin: at a change of app, only if a figure in use moved
		 * (as few writes as can be) */
		if (fg != last_fg) {
			tune_lock();
			if (LD(g_cal_dirty) && cal_worth_saving())
				g_tune_cal_due = 1;
			if (g_tune_cal_due)
				cal_save_locked();
			__sync_lock_release(&g_tune_lock);
		}
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
				VM_KLOGI("stage: dev %d read ahead off (%s)", i,
				         !wanted ? "the device or the plugin off" :
				         LD(d->fallback) ? "a difference found: the standard way" :
				         LD(g_ra_own) ? "set Off for the app in front" : "set Off");
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
		o->set[k] = ra_base_threshold(k);
		o->mid[k] = ra_small_threshold(k, o->thr[k]);
	}
	o->tiny = tiny_thr();
	for (int u = 0; u < 3; u++)
		o->off |= (uint32_t)oo_off(u) << u;
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
	{
		char ft[16];

		if (!fg_get(ft))
			return;
		memcpy(o->title, ft, sizeof(o->title) - 1);
	}
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

void vm_stage_dma_stats(uint32_t *dma_sec, uint32_t *all_sec)
{
	*dma_sec = LD(g_cpo_dma_sec);
	*all_sec = LD(g_cpo_sec);
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