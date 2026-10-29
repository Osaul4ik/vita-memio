/*
 * sim.c - runs the real tester app (tester/main.c, ui.c, vita_env.c and the engine)
 * on a Linux PC against a fake console: sceIo* map to plain files in the current
 * directory, the plugin is a small fake (settings, status, counters that grow on
 * large transfers while a device is ON; read only, as for the real tester), the
 * controller follows a script and every screen can be written out as an image.
 *
 *   SIM_SCRIPT="30:cross 40:down*2"    frame:button[+button][*frames], 2 frames by default
 *   SIM_SNAP="12,80,200"               write snap_<frame>.ppm at those frames
 *   SIM_SNAP_CHANGE=1                  write a snapshot whenever the screen differs (max 80)
 *   SIM_EXIT_FRAME=n                   exit after n frames
 *   SIM_SD2VITA=0|1                    the plugin's SD2Vita setting (default 1)
 *   SIM_NOBUF=1                        a device that is on reports "no buffer"
 *
 * This is a test aid for the screens and the control flow, not a Vita emulator: it
 * says nothing about real speeds or about the console's drivers.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/rtc.h>

#include "../../include/storageturbo_kernel_api.h"

/* ---- files -------------------------------------------------------------------- */

SceUID sceIoOpen(const char *f, int flags, SceMode mode)
{
	int fl = (flags & SCE_O_RDWR) == SCE_O_RDWR ? O_RDWR : (flags & SCE_O_WRONLY) ? O_WRONLY : O_RDONLY;

	if (flags & SCE_O_CREAT) fl |= O_CREAT;
	if (flags & SCE_O_TRUNC) fl |= O_TRUNC;
	return open(f, fl, 0644) ;
}
int sceIoClose(SceUID fd) { return close(fd); }
int sceIoRead(SceUID fd, void *d, SceSize n) { return (int)read(fd, d, n); }
int sceIoWrite(SceUID fd, const void *d, SceSize n) { return (int)write(fd, d, n); }
int sceIoRemove(const char *f) { return unlink(f); }
int sceIoMkdir(const char *d, SceMode m) { return mkdir(d, 0755); }
SceUID sceIoDopen(const char *d) { DIR *p = opendir(d); return p ? (SceUID)(long)dirfd(p) : -1; }
int sceIoDclose(SceUID fd) { return close(fd); }

/* The fake plugin. */
static TsSettings g_set = { 1, 0, 1, 0, 1, 0, 0 };
static TsDevState g_dev[TS_DEV_COUNT];
static int g_sim_fail_nobuf;

static void count(int write, SceSize n)
{
	for (int d = 0; d < TS_DEV_COUNT; d++) {
		int on = d == TS_DEV_SD2VITA ? g_set.sd2vita : d == TS_DEV_INTERNAL ? g_set.internal : g_set.memcard;

		if (on && n >= 32768) {
			if (write) g_dev[d].writes++; else g_dev[d].reads++;
		}
	}
}

int sceIoPread(SceUID fd, void *d, SceSize n, SceOff off) { count(0, n); return (int)pread(fd, d, n, off); }
int sceIoPwrite(SceUID fd, const void *d, SceSize n, SceOff off) { count(1, n); return (int)pwrite(fd, d, n, off); }

int tsGetState(TsState *o)
{
	memset(o, 0, sizeof(*o));
	o->size = sizeof(*o);
	o->magic = TS_STATE_MAGIC;
	o->settings = g_set;
	o->fingerprint = 0xC0FFEE01;
	for (int d = 0; d < TS_DEV_COUNT; d++) {
		int on = d == TS_DEV_SD2VITA ? g_set.sd2vita : d == TS_DEV_INTERNAL ? g_set.internal : g_set.memcard;

		o->dev[d] = g_dev[d];
		o->dev[d].status = on ? (g_sim_fail_nobuf ? TS_ST_NO_BUFFER : TS_ST_ACTIVE) : TS_ST_OFF;
	}
	return TS_OK;
}

/* No tsSetSetting here on purpose: the tester must never change a plugin
 * setting, so if it ever calls it, building the simulator fails to link. */
int tsSelfTestPing(void) { return TS_SELFTEST_MAGIC; }

/* Timing: 20 us io and 5 us copy per block for reads, 30 / 2 for writes. */
int tsGetTiming(TsTiming *o)
{
	memset(o, 0, sizeof(*o));
	o->size = sizeof(*o);
	o->magic = TS_TIMING_MAGIC;
	for (int d = 0; d < TS_DEV_COUNT; d++) {
		o->dev[d].rd_sectors = g_dev[d].reads * 64;
		o->dev[d].rd_io_us = g_dev[d].reads * 64 * 20;
		o->dev[d].rd_copy_us = g_dev[d].reads * 64 * 5;
		o->dev[d].wr_sectors = g_dev[d].writes * 64;
		o->dev[d].wr_io_us = g_dev[d].writes * 64 * 30;
		o->dev[d].wr_copy_us = g_dev[d].writes * 64 * 2;
	}
	return TS_OK;
}

/* ---- time, memory ---------------------------------------------------------------- */

uint64_t sceKernelGetProcessTimeWide(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
int sceKernelPowerTick(int t) { return 0; }
int sceKernelExitProcess(int r) { exit(r); }
int sceRtcGetCurrentClockLocalTime(SceDateTime *t)
{
	t->year = 2026; t->month = 10; t->day = 28; t->hour = 12; t->minute = 30; t->second = 11;
	return 0;
}
static void *g_fb;
SceUID sceKernelAllocMemBlock(const char *n, int type, SceSize size, void *opt)
{
	g_fb = malloc(size);
	return g_fb ? 7 : -1;
}
int sceKernelGetMemBlockBase(SceUID id, void **b) { *b = g_fb; return 0; }
int sceKernelFreeMemBlock(SceUID id) { free(g_fb); g_fb = NULL; return 0; }

/* ---- the controller and the display ------------------------------------------------ */

static int g_frame;
static struct { int frame, buttons, dur; } g_script[256];
static int g_nscript;

static int button_bit(const char *n)
{
	static const struct { const char *n; int b; } t[] = {
		{ "up", SCE_CTRL_UP }, { "down", SCE_CTRL_DOWN }, { "left", SCE_CTRL_LEFT },
		{ "right", SCE_CTRL_RIGHT }, { "cross", SCE_CTRL_CROSS }, { "circle", SCE_CTRL_CIRCLE },
		{ "l", SCE_CTRL_LTRIGGER }, { "r", SCE_CTRL_RTRIGGER },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(n, t[i].n)) return t[i].b;
	fprintf(stderr, "sim: unknown button %s\n", n);
	exit(2);
}

static void parse_script(const char *s)
{
	char *copy = strdup(s), *save = NULL;

	for (char *tok = strtok_r(copy, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		int fr, dur = 2, b = 0;
		char *colon = strchr(tok, ':'), *star;

		if (!colon) continue;
		fr = atoi(tok);
		if ((star = strchr(colon, '*'))) { dur = atoi(star + 1); *star = 0; }
		for (char *n = strtok(colon + 1, "+"); n; n = strtok(NULL, "+"))
			b |= button_bit(n);
		g_script[g_nscript].frame = fr; g_script[g_nscript].buttons = b; g_script[g_nscript].dur = dur;
		g_nscript++;
	}
	free(copy);
}

int sceCtrlSetSamplingMode(int m) { return 0; }
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int n)
{
	memset(pad, 0, sizeof(*pad));
	for (int i = 0; i < g_nscript; i++)
		if (g_frame >= g_script[i].frame && g_frame < g_script[i].frame + g_script[i].dur)
			pad->buttons |= (unsigned)g_script[i].buttons;
	return 1;
}

static int g_snap_change, g_exit_frame = -1, g_nsnaps, g_snap_max = 80;
static int g_snaps[64], g_nsnap_frames;
static uint32_t g_last_hash;

static void dump(const uint32_t *px)
{
	char name[64];
	FILE *f;

	snprintf(name, sizeof(name), "snap_%04d.ppm", g_frame);
	f = fopen(name, "wb");
	if (!f) return;
	fprintf(f, "P6\n960 544\n255\n");
	for (int i = 0; i < 960 * 544; i++) {
		unsigned char c[3] = { px[i] & 0xFF, (px[i] >> 8) & 0xFF, (px[i] >> 16) & 0xFF };

		fwrite(c, 1, 3, f);
	}
	fclose(f);
	g_nsnaps++;
}

int sceDisplaySetFrameBuf(const SceDisplayFrameBuf *p, int sync)
{
	const uint32_t *px = p->base;
	int want = 0;
	uint32_t h = 2166136261u;

	for (int i = 0; i < g_nsnap_frames; i++)
		if (g_snaps[i] == g_frame) want = 1;
	if (g_snap_change && g_nsnaps < g_snap_max) {
		for (int i = 0; i < 960 * 544; i += 7) h = (h ^ px[i]) * 16777619u;
		if (h != g_last_hash) want = 1;
		g_last_hash = h;
	}
	if (want) dump(px);
	return 0;
}

int sceDisplayWaitVblankStart(void)
{
	g_frame++;
	if (g_exit_frame >= 0 && g_frame >= g_exit_frame)
		exit(0);
	return 0;
}

int tester_main(void);

int main(int argc, char **argv)
{
	const char *s;

	if ((s = getenv("SIM_SCRIPT"))) parse_script(s);
	if ((s = getenv("SIM_SNAP"))) {
		char *c = strdup(s);

		for (char *t = strtok(c, ","); t && g_nsnap_frames < 64; t = strtok(NULL, ","))
			g_snaps[g_nsnap_frames++] = atoi(t);
	}
	g_snap_change = getenv("SIM_SNAP_CHANGE") != NULL;
	if ((s = getenv("SIM_EXIT_FRAME"))) g_exit_frame = atoi(s);
	if ((s = getenv("SIM_SNAP_MAX"))) g_snap_max = atoi(s);
	g_sim_fail_nobuf = getenv("SIM_NOBUF") != NULL;
	if ((s = getenv("SIM_SD2VITA"))) g_set.sd2vita = atoi(s);   /* "as set in Settings" */
	return tester_main();
}
