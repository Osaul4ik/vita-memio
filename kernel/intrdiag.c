/*
 * intrdiag.c - DIAGNOSTIC (branch ncdirect, not for release): which hardware
 * interrupts fire, how often, on which core and for how long.
 *
 * 20 s after start (still inside the boot guard's window, so a start that
 * goes wrong here ends in safe mode) every registered hardware interrupt
 * (codes 32-255; the timers and inter-core signals below 32 are left alone)
 * gets a hook that counts it per core, times it and calls the system's own
 * handler. Once a minute the busiest ones since the last line are logged with
 * the cores they are routed to. Nothing is changed in how they are handled.
 */
#ifdef VM_INTRDIAG
#include <stdint.h>
#include <psp2kern/kernel/intrmgr.h>
#include <psp2kern/kernel/threadmgr.h>

#include "intrdiag.h"
#include "klog.h"

#define ID_FIRST  32
#define ID_COUNT  256
#define ID_TOP    6

static SceKernelIntrHookHandler g_old[ID_COUNT];
static uint8_t  g_hooked[ID_COUNT];
static uint32_t g_n[ID_COUNT][4], g_us[ID_COUNT][4];      /* atomics */
static uint32_t g_last[ID_COUNT];
static volatile int g_on;

static inline uint32_t core(void)
{
	uint32_t m;

	__asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(m));   /* MPIDR */
	return m & 3;
}

static int id_hook(int code, void *ctx, int prio)
{
	const uint32_t c = core(), t0 = ksceKernelGetSystemTimeLow();
	const int r = g_old[code](code, ctx, prio);

	__atomic_fetch_add(&g_us[code][c], ksceKernelGetSystemTimeLow() - t0, __ATOMIC_RELAXED);
	__atomic_fetch_add(&g_n[code][c], 1, __ATOMIC_RELAXED);
	return r;
}

static uint32_t total(int code)
{
	uint32_t t = 0;

	for (int c = 0; c < 4; c++)
		t += __atomic_load_n(&g_n[code][c], __ATOMIC_RELAXED);
	return t;
}

static int id_thread(SceSize args, void *argp)
{
	int hooked = 0;

	(void)args; (void)argp;
	ksceKernelDelayThread(20 * 1000 * 1000);
	for (int code = ID_FIRST; code < ID_COUNT; code++) {
		int tgt = 0;

		if (ksceKernelGetIntrTarget(code, &tgt) < 0)
			continue;
		if (ksceKernelRegisterIntrHookHandler(code, id_hook, &g_old[code]) == 0 && g_old[code]) {
			g_hooked[code] = 1;
			hooked++;
		}
	}
	g_on = 1;
	VM_KLOGI("intrdiag: %d interrupts counted", hooked);
	for (;;) {
		int top[ID_TOP], ntop = 0;
		uint32_t d[ID_COUNT];

		ksceKernelDelayThread(60 * 1000 * 1000);
		if (!g_on)
			break;
		for (int code = 0; code < ID_COUNT; code++) {
			const uint32_t t = g_hooked[code] ? total(code) : 0;

			d[code] = t - g_last[code];
			g_last[code] = t;
		}
		for (int k = 0; k < ID_TOP; k++) {        /* the busiest, by count */
			int best = -1;

			for (int code = 0; code < ID_COUNT; code++) {
				int taken = 0;

				for (int j = 0; j < ntop; j++)
					taken |= top[j] == code;
				if (!taken && d[code] && (best < 0 || d[code] > d[best]))
					best = code;
			}
			if (best < 0)
				break;
			top[ntop++] = best;
		}
		for (int k = 0; k < ntop; k++) {
			const int code = top[k];
			int tgt = 0;

			ksceKernelGetIntrTarget(code, &tgt);
			VM_KLOGI("intrdiag: 0x%02x target 0x%x, %u in the last minute; total by core "
			         "%u/%u/%u/%u, us %u/%u/%u/%u", code, (unsigned)tgt, (unsigned)d[code],
			         (unsigned)g_n[code][0], (unsigned)g_n[code][1], (unsigned)g_n[code][2],
			         (unsigned)g_n[code][3], (unsigned)g_us[code][0], (unsigned)g_us[code][1],
			         (unsigned)g_us[code][2], (unsigned)g_us[code][3]);
		}
	}
	return 0;
}

void vm_intrdiag_start(void)
{
	SceUID th = ksceKernelCreateThread("SceVitaMemioIntrDiag", id_thread, 0x60, 0x2000, 0,
	                                   0x00080000, NULL);

	if (th >= 0)
		ksceKernelStartThread(th, 0, NULL);
}

void vm_intrdiag_stop(void)
{
	g_on = 0;
	for (int code = 0; code < ID_COUNT; code++)
		if (g_hooked[code]) {
			ksceKernelReleaseIntrHookHandler(code);
			g_hooked[code] = 0;
		}
}
#endif
