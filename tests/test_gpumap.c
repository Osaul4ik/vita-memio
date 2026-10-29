/*
 * test_gpumap.c - the GPU-mapping watch (kernel/gpumap.c): it only installs
 * when asked, passes the calls on, records what it can, never waits, drops
 * rather than overflows, and the worker drains everything in order.
 */
#include <stdlib.h>
#include <string.h>

#include "../kernel/gpumap.c"

#include "fakes.h"
#include "vm_test.h"

int ksceKernelVAtoPA(const void *va, uintptr_t *pa)
{
	*pa = (uintptr_t)va ^ 0x40000000u;
	return 0;
}
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size)
{
	(void)size;
	return addr ? 0x1234 : -1;
}
int ksceKernelGetMemBlockType(SceUID uid, unsigned int *type)
{
	*type = uid == 0x1234 ? 0x0C20D060u : 0;
	return 0;
}
SceUID ksceKernelGetProcessId(void) { return 0x40010003; }

static void reset(void)
{
	fakes_reset();
	g_ev_head = g_ev_tail = g_ev_lock = 0;
	g_maps = g_unmaps = g_dropped = 0;
	g_tried = g_logged = 0;
	g_h_map = g_h_unmap = -1;
}

static void test_installs_only_when_wanted(void)
{
	reset();
	vm_gpumap_tick(0, 1);
	CHECK(fake_hook_calls == 0 && !g_tried);
	vm_gpumap_tick(1, 2);
	CHECK(fake_hook_calls == 2 && g_h_map >= 0 && g_h_unmap >= 0);
	vm_gpumap_tick(1, 3);
	CHECK(fake_hook_calls == 2);                     /* once */
	vm_gpumap_fini();
	CHECK(fake_hook_releases == 2 && g_h_map < 0);
}

static void test_records_and_drains(void)
{
	reset();
	hk_map(0xC0FFEE, 0x20000000, 0x81000000, 0x10000, 3, 0, 0, 0);
	hk_unmap(0xC0FFEE, 0x20000000, 0x10000, 0, 0, 0, 0, 0);
	CHECK(g_maps == 1 && g_unmaps == 1 && g_ev_head == 2);
	CHECK(g_ev[0].kind == 1 && g_ev[0].pid == 0x40010003 && g_ev[0].a[2] == 0x81000000);
	CHECK(g_ev[0].pa == (0x81000000u ^ 0x40000000u) && g_ev[0].blk == 0x1234 &&
	      g_ev[0].type == 0x0C20D060u && g_ev[0].a[4] == 3);
	CHECK(g_ev[1].kind == 2 && g_ev[1].blk == 0);    /* unmap: no lookups */
	vm_gpumap_tick(0, 5);
	CHECK(g_ev_tail == 2 && g_logged == 2);
}

static void test_never_waits_and_drops_when_full(void)
{
	reset();
	for (int i = 0; i < EVENTS + 6; i++)
		hk_map(1, 2, 3, 4, 5, 0, 0, 0);
	CHECK(g_ev_head == EVENTS && g_dropped == 6 && g_maps == EVENTS + 6);
	g_ev_lock = 1;                                   /* someone is recording */
	vm_gpumap_tick(0, 7);                            /* the worker catches up */
	hk_map(1, 2, 3, 4, 5, 0, 0, 0);
	CHECK(g_dropped == 7 && g_ev_head == EVENTS);    /* dropped, did not wait */
	g_ev_lock = 0;
	hk_map(1, 2, 3, 4, 5, 0, 0, 0);
	CHECK(g_ev_head == EVENTS + 1);
	vm_gpumap_tick(0, 8);
	CHECK(g_ev_tail == g_ev_head);
	/* the log stops after LOG_MAX lines, the counting does not */
	g_logged = LOG_MAX;
	hk_map(1, 2, 3, 4, 5, 0, 0, 0);
	vm_gpumap_tick(0, 9);
	CHECK(g_logged == LOG_MAX && g_ev_tail == g_ev_head);
}

int main(void)
{
	RUN(test_installs_only_when_wanted);
	RUN(test_records_and_drains);
	RUN(test_never_waits_and_drops_when_full);
	printf(vm_failures ? "test_gpumap: %d FAILED\n" : "test_gpumap: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
