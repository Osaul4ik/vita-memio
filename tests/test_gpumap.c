/*
 * test_gpumap.c - the GPU-mapping watch (kernel/gpumap.c): it only installs
 * when asked and the flag file is there, the hooks pass the calls on and call
 * nothing else, never wait, drop rather than overflow, and the worker drains
 * everything in order, looking the addresses up itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/gpumap.c"

#include "fakes.h"
#include "vm_test.h"

static int lookups;                     /* sysmem lookups made */

int ksceKernelVAtoPA(const void *va, uintptr_t *pa)
{
	lookups++;
	*pa = (uintptr_t)va ^ 0x40000000u;
	return 0;
}
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size)
{
	(void)size;
	lookups++;
	return addr ? 0x1234 : -1;
}
int ksceKernelGetMemBlockMemtypeByAddr(const void *addr)
{
	lookups++;
	return addr ? 0x0D00 : -1;
}
SceUID ksceKernelGetProcessId(void) { return 0x40010003; }

static void reset(void)
{
	fakes_reset();
	g_ev_head = g_ev_tail = g_ev_lock = 0;
	g_maps = g_unmaps = g_dropped = 0;
	g_tried = g_logged = 0;
	g_h_map = g_h_unmap = -1;
	lookups = 0;
}

static void set_flag(int on)
{
	char p[512];

	snprintf(p, sizeof(p), "%s/gpumap.on", fakes_tmpdir_new());
	if (on) {
		FILE *f = fopen(p, "w");

		CHECK(f != NULL);
		if (f)
			fclose(f);
	}
}

static void test_installs_only_when_wanted(void)
{
	/* the switches on but no flag file: never hooks the GPU driver */
	reset();
	set_flag(0);
	for (uint32_t t = 0; t < 40; t++)
		vm_gpumap_tick(1, t);
	CHECK(fake_hook_calls == 0 && !g_tried);
	fakes_tmpdir_remove();

	reset();
	set_flag(1);
	vm_gpumap_tick(0, 10);
	CHECK(fake_hook_calls == 0 && !g_tried);         /* switches off */
	vm_gpumap_tick(1, 11);
	CHECK(fake_hook_calls == 0);                     /* looked for every 10 s */
	vm_gpumap_tick(1, 20);
	CHECK(fake_hook_calls == 2 && g_h_map >= 0 && g_h_unmap >= 0);
	vm_gpumap_tick(1, 30);
	CHECK(fake_hook_calls == 2);                     /* once */
	vm_gpumap_fini();
	CHECK(fake_hook_releases == 2 && g_h_map < 0);
	fakes_tmpdir_remove();
}

static void test_records_and_drains(void)
{
	reset();
	hk_map(0xC0FFEE, 0x20000000, 0x81000000, 0x10000, 3, 0, 0, 0);
	hk_unmap(0xC0FFEE, 0x20000000, 0x10000, 0, 0, 0, 0, 0);
	CHECK(g_maps == 1 && g_unmaps == 1 && g_ev_head == 2);
	CHECK(g_ev[0].kind == 1 && g_ev[0].pid == 0x40010003 && g_ev[0].a[2] == 0x81000000);
	CHECK(g_ev[0].a[4] == 3 && g_ev[1].kind == 2 && g_ev[1].a[2] == 0x10000);
	CHECK(lookups == 0);                             /* nothing inside the GPU driver */
	vm_gpumap_tick(0, 5);
	CHECK(g_ev_tail == 2 && g_logged == 2);
	CHECK(lookups == 3);                             /* the map, in the worker */
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
