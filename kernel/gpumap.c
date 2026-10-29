/*
 * gpumap.c - finding out which memory games give the graphics chip (step one
 * of "GPU-safe reads, GPU memory only").
 *
 * GPU-safe reads clean every faster read's copy out of the CPU cache, which
 * costs about what the faster path saves. Only memory the GPU reads needs it.
 * The GPU driver (SceGpuEs4) maps memory into the GPU's MMU through
 * ksceGpuMmuMapMemory / ksceGpuMmuUnmapMemory (SceGpuEs4ForDriver); what
 * their arguments are exactly is not documented, so this build only watches:
 * a pass-through hook on both records each call (process and arguments) and
 * the worker writes them to kernel.log, with what "base" is in the kernel's
 * address space (physical address, memory block, memory type) looked up
 * there. Nothing is changed.
 *
 * The hooks run inside the GPU driver, which may hold its own or the memory
 * manager's locks: they call nothing but the original and record the raw
 * arguments (an earlier build looked the address up in there, and the console
 * restarted when a game started).
 *
 * Installed only while Enable log and GPU-safe reads are on AND the file
 * ur0:data/vita-memio/gpumap.on exists, so GPU-safe reads alone never hook
 * the GPU driver. The hooks forward eight arguments, so they pass on whatever
 * the real functions take.
 */
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>
#include <taihen.h>

#include "../include/vitamemio.h"
#include "gpumap.h"
#include "klog.h"

#define LIB_GPU_DRIVER  0x54802381   /* SceGpuEs4ForDriver     */
#define NID_GPU_MAP     0xEF4B16D3   /* ksceGpuMmuMapMemory    */
#define NID_GPU_UNMAP   0x9AAAD91C   /* ksceGpuMmuUnmapMemory  */

#define GPUMAP_FLAG     VM_DATA_DIR "/gpumap.on"   /* the watch's own switch */

/* SceSysmemForDriver: the memory type at an address of the current address
 * space (masked with 0xFF00), or < 0. */
int ksceKernelGetMemBlockMemtypeByAddr(const void *addr);

typedef struct {
	uint32_t kind;              /* 1 map, 2 unmap */
	uint32_t pid;
	uint32_t a[5];              /* the first five arguments */
	int32_t  ret;
} GpuEvent;

#define EVENTS 64
static GpuEvent       g_ev[EVENTS];
static volatile int   g_ev_head;        /* written by the hooks, under g_ev_lock */
static volatile int   g_ev_tail;        /* advanced by the worker               */
static volatile int   g_ev_lock;
static volatile uint32_t g_maps, g_unmaps, g_dropped;

static SceUID         g_h_map = -1, g_h_unmap = -1;
static tai_hook_ref_t g_r_map, g_r_unmap;
static int            g_tried;
static int            g_logged;         /* events written so far this boot */

#define LOG_MAX 200                     /* then only the totals */

static void record(uint32_t kind, const uint32_t *a, int ret)
{
	GpuEvent e;

	/* Only the raw call: no lookups in here (see the top). */
	e.kind = kind;
	e.pid = (uint32_t)ksceKernelGetProcessId();
	for (int k = 0; k < 5; k++)
		e.a[k] = a[k];
	e.ret = ret;
	/* Never wait in here: if another call is recording right now, or the
	 * worker is behind, only count it. */
	if (__sync_lock_test_and_set(&g_ev_lock, 1)) {
		__sync_fetch_and_add(&g_dropped, 1);
		return;
	}
	if (g_ev_head - g_ev_tail >= EVENTS) {
		__sync_fetch_and_add(&g_dropped, 1);   /* the worker is behind */
	} else {
		g_ev[g_ev_head % EVENTS] = e;
		__sync_synchronize();               /* the entry before the index */
		g_ev_head++;
	}
	__sync_lock_release(&g_ev_lock);
}

static int hk_map(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                  uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
{
	uint32_t a[5] = { a0, a1, a2, a3, a4 };
	int r = TAI_CONTINUE(int, g_r_map, a0, a1, a2, a3, a4, a5, a6, a7);

	__sync_fetch_and_add(&g_maps, 1);
	record(1, a, r);
	return r;
}

static int hk_unmap(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                    uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7)
{
	uint32_t a[5] = { a0, a1, a2, a3, a4 };
	int r = TAI_CONTINUE(int, g_r_unmap, a0, a1, a2, a3, a4, a5, a6, a7);

	__sync_fetch_and_add(&g_unmaps, 1);
	record(2, a, r);
	return r;
}

static int flag_on(void)
{
	SceUID fd = ksceIoOpen(GPUMAP_FLAG, SCE_O_RDONLY, 0);

	if (fd < 0)
		return 0;
	ksceIoClose(fd);
	return 1;
}

/* What `va` is in the kernel's address space, looked up here in the worker. */
static void lookup(uint32_t va, uint32_t *pa, int32_t *blk, int32_t *memtype)
{
	uintptr_t p = 0;

	if (ksceKernelVAtoPA((const void *)(uintptr_t)va, &p) < 0)
		p = 0;
	*pa = (uint32_t)p;
	*blk = ksceKernelFindMemBlockByAddr((const void *)(uintptr_t)va, 0);
	*memtype = ksceKernelGetMemBlockMemtypeByAddr((const void *)(uintptr_t)va);
}

void vm_gpumap_tick(int wanted, uint32_t uptime_s)
{
	/* The flag file is looked for every 10 s while the switches are on. */
	if (wanted && !g_tried && uptime_s % 10 == 0 && flag_on()) {
		g_tried = 1;
		g_h_map = taiHookFunctionExportForKernel(KERNEL_PID, &g_r_map, "SceGpuEs4",
		                                         LIB_GPU_DRIVER, NID_GPU_MAP, hk_map);
		g_h_unmap = taiHookFunctionExportForKernel(KERNEL_PID, &g_r_unmap, "SceGpuEs4",
		                                           LIB_GPU_DRIVER, NID_GPU_UNMAP, hk_unmap);
		VM_KLOGI("gpumap: watching GPU memory mappings (map hook 0x%08x, unmap hook 0x%08x)",
		         (unsigned)g_h_map, (unsigned)g_h_unmap);
	}
	/* Write out what was recorded, in order. */
	while (g_ev_tail < g_ev_head) {
		const GpuEvent *e = &g_ev[g_ev_tail % EVENTS];

		if (g_logged < LOG_MAX) {
			g_logged++;
			if (e->kind == 1) {
				uint32_t pa;
				int32_t blk, memtype;

				lookup(e->a[2], &pa, &blk, &memtype);
				VM_KLOGI("gpumap: map pid=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
				         "a4=0x%08x ret=0x%08x | a2 (kernel view, later): pa=0x%08x "
				         "blk=0x%08x memtype=0x%08x",
				         (unsigned)e->pid, (unsigned)e->a[0], (unsigned)e->a[1],
				         (unsigned)e->a[2], (unsigned)e->a[3], (unsigned)e->a[4],
				         (unsigned)e->ret, (unsigned)pa, (unsigned)blk, (unsigned)memtype);
			} else {
				VM_KLOGI("gpumap: unmap pid=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x ret=0x%08x",
				         (unsigned)e->pid, (unsigned)e->a[0], (unsigned)e->a[1],
				         (unsigned)e->a[2], (unsigned)e->ret);
			}
		}
		__sync_synchronize();
		g_ev_tail++;
	}
	if (g_tried && uptime_s % 30 == 0)
		VM_KLOGI("gpumap: totals maps=%u unmaps=%u dropped=%u",
		         (unsigned)g_maps, (unsigned)g_unmaps, (unsigned)g_dropped);
}

void vm_gpumap_fini(void)
{
	if (g_h_map >= 0)
		taiHookReleaseForKernel(g_h_map, g_r_map);
	if (g_h_unmap >= 0)
		taiHookReleaseForKernel(g_h_unmap, g_r_unmap);
	g_h_map = g_h_unmap = -1;
}
