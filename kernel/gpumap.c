/*
 * gpumap.c - finding out which memory games give the graphics chip (step one
 * of "GPU-safe reads, GPU memory only").
 *
 * GPU-safe reads clean every faster read's copy out of the CPU cache, which
 * costs about what the faster path saves. Only memory the GPU reads needs it.
 * The GPU driver (SceGpuEs4) maps memory into the GPU's MMU through
 * ksceGpuMmuMapMemory / ksceGpuMmuUnmapMemory (SceGpuEs4ForDriver); what
 * their arguments are exactly is not documented, so this build only watches:
 * a pass-through hook on both records each call (process, arguments, what the
 * "base" argument is in that process: physical address, memory block, memory
 * type) and the worker writes them to kernel.log. Nothing is changed.
 *
 * Installed only while both Enable log and GPU-safe reads are on. The hooks
 * forward eight arguments, so they pass on whatever the real functions take.
 */
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "gpumap.h"
#include "klog.h"

#define LIB_GPU_DRIVER  0x54802381   /* SceGpuEs4ForDriver     */
#define NID_GPU_MAP     0xEF4B16D3   /* ksceGpuMmuMapMemory    */
#define NID_GPU_UNMAP   0x9AAAD91C   /* ksceGpuMmuUnmapMemory  */

/* SceSysmemForDriver: the memory type at an address of the current address
 * space (masked with 0xFF00), or < 0. */
int ksceKernelGetMemBlockMemtypeByAddr(const void *addr);

typedef struct {
	uint32_t kind;              /* 1 map, 2 unmap */
	uint32_t pid;
	uint32_t a[5];              /* the first five arguments */
	int32_t  ret;
	uint32_t pa;                /* of a[2] ("base") in the calling process, or 0 */
	int32_t  blk;               /* its memory block, or < 0 */
	int32_t  memtype;           /* the memory type at a2 (0xFF00 mask), or < 0 */
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
	uintptr_t pa = 0;
	SceUID blk;

	/* What "base" (the third argument) is in the calling process. */
	memset(&e, 0, sizeof(e));
	e.kind = kind;
	e.pid = (uint32_t)ksceKernelGetProcessId();
	for (int k = 0; k < 5; k++)
		e.a[k] = a[k];
	e.ret = ret;
	if (kind == 1) {
		if (ksceKernelVAtoPA((const void *)(uintptr_t)a[2], &pa) < 0)
			pa = 0;
		blk = ksceKernelFindMemBlockByAddr((const void *)(uintptr_t)a[2], 0);
		e.pa = (uint32_t)pa;
		e.blk = blk;
		e.memtype = ksceKernelGetMemBlockMemtypeByAddr((const void *)(uintptr_t)a[2]);
	}
	/* Never wait in here (this runs inside the GPU driver): if another call
	 * is recording right now, or the worker is behind, only count it. */
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

void vm_gpumap_tick(int wanted, uint32_t uptime_s)
{
	if (wanted && !g_tried) {
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
			if (e->kind == 1)
				VM_KLOGI("gpumap: map pid=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
				         "a4=0x%08x ret=0x%08x | a2: pa=0x%08x blk=0x%08x memtype=0x%08x",
				         (unsigned)e->pid, (unsigned)e->a[0], (unsigned)e->a[1],
				         (unsigned)e->a[2], (unsigned)e->a[3], (unsigned)e->a[4],
				         (unsigned)e->ret, (unsigned)e->pa, (unsigned)e->blk,
				         (unsigned)e->memtype);
			else
				VM_KLOGI("gpumap: unmap pid=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x ret=0x%08x",
				         (unsigned)e->pid, (unsigned)e->a[0], (unsigned)e->a[1],
				         (unsigned)e->a[2], (unsigned)e->ret);
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
