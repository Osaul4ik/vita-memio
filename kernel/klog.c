/*
 * klog.c - kernel-side logger implementation. See klog.h.
 *
 * Primary sink: ksceDebugPrintf("[vita-memio] ...").
 * Secondary sink: an append-only, size-capped ur0 logfile via ksceIo*, opened
 * best-effort and disabled silently on any failure. One rotation step
 * (kernel.log -> kernel.old.log) keeps the previous chunk.
 *
 * Both sinks only run while logging is switched on (Settings > Advanced >
 * Enable log, VmSettings.log). Until the settings are known at boot, lines are
 * held in a small buffer: written out if logging turns out to be on, dropped
 * if it is off.
 *
 * A single mutex guards the fd. The formatter is a self-contained printf subset
 * (no libc in kernel), the same one the Settings-page logger uses.
 */
#include <psp2kern/kernel/threadmgr.h>   /* ksceKernelGetSystemTimeWide, mutex   */
#include <psp2kern/kernel/debug.h>       /* ksceDebugPrintf                      */
#include <psp2kern/io/fcntl.h>           /* ksceIoOpen/Write/Close/Lseek/Remove  */
#include <psp2kern/io/stat.h>            /* ksceIoMkdir                          */
#include <psp2kern/kernel/sysclib.h>     /* memcpy                               */
#include <stdarg.h>

#include "klog.h"
#include "../include/vm_fmt.h"

static SceUID   g_mtx = -1;
static int      g_inited = 0;
static int      g_level  = VM_KLOG_DEFAULT_LEVEL;
static SceUID   g_fd = -1;
static int      g_file_disabled = 0;
static uint32_t g_file_bytes = 0;
static int      g_enabled = -1;        /* -1: not known yet, 0: off, 1: on   */

/* Lines logged before the settings were loaded. */
#define EARLY_LINES 8
static char g_early[EARLY_LINES][192];
static int  g_early_len[EARLY_LINES];
static int  g_early_n;

static void lock(void)   { if (g_mtx >= 0) ksceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) ksceKernelUnlockMutex(g_mtx, 1); }

/* ---- file plumbing (best-effort) ---------------------------------------- */

static void file_open_locked(void)
{
	if (g_file_disabled || g_fd >= 0)
		return;
	ksceIoMkdir("ur0:data", 0777);
	ksceIoMkdir(VM_KLOG_DIR, 0777);

	SceUID fd = ksceIoOpen(VM_KLOG_PATH,
	                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0) {
		g_file_disabled = 1;
		return;
	}
	SceOff end = ksceIoLseek(fd, 0, SCE_SEEK_END);
	g_file_bytes = end > 0 ? (uint32_t)end : 0;
	g_fd = fd;
}

static void file_write_locked(const char *line, int len)
{
	if (g_file_disabled)
		return;
	if (g_fd < 0)
		file_open_locked();
	if (g_fd < 0)
		return;

	if (g_file_bytes + (uint32_t)len + 1 > VM_KLOG_MAX_BYTES) {
		/* Rotate: keep one previous chunk as kernel.old.log, start afresh. */
		ksceIoClose(g_fd);
		ksceIoRemove(VM_KLOG_PATH_OLD);
		ksceIoRename(VM_KLOG_PATH, VM_KLOG_PATH_OLD);
		g_fd = ksceIoOpen(VM_KLOG_PATH,
		                  SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		g_file_bytes = 0;
		if (g_fd < 0) { g_file_disabled = 1; return; }
	}

	int w1 = ksceIoWrite(g_fd, line, len);
	int w2 = ksceIoWrite(g_fd, "\n", 1);
	if (w1 < 0 || w2 < 0) {
		ksceIoClose(g_fd);
		g_fd = -1;
		g_file_disabled = 1;
		return;
	}
	g_file_bytes += (uint32_t)w1 + (uint32_t)w2;
}

/* ---- public API --------------------------------------------------------- */

void vm_klog_init(void)
{
	if (g_inited)
		return;
	g_level = VM_KLOG_DEFAULT_LEVEL;
	g_file_disabled = 0;
	g_file_bytes = 0;
	g_fd = -1;
	if (g_mtx < 0)
		g_mtx = ksceKernelCreateMutex("VmKLogMtx", 0, 0, NULL);
	g_inited = 1;
}

static void emit_locked(const char *line, int len)
{
	ksceDebugPrintf("%s\n", line);
	file_write_locked(line, len);
}

void vm_klog_set_enabled(int on)
{
	on = on ? 1 : 0;
	lock();
	if (on != g_enabled) {
		g_enabled = on;
		if (on) {
			g_file_disabled = 0;           /* a fresh chance for the file */
			for (int i = 0; i < g_early_n; i++)
				emit_locked(g_early[i], g_early_len[i]);
		} else if (g_fd >= 0) {
			ksceIoClose(g_fd);
			g_fd = -1;
		}
		g_early_n = 0;
	}
	unlock();
}

void vm_klog_fini(void)
{
	if (!g_inited)
		return;
	lock();
	if (g_fd >= 0) { ksceIoClose(g_fd); g_fd = -1; }
	unlock();
	if (g_mtx >= 0) { ksceKernelDeleteMutex(g_mtx); g_mtx = -1; }
	g_inited = 0;
}

static const char *level_name(int level)
{
	switch (level) {
	case VM_KLOG_ERR:   return "ERR";
	case VM_KLOG_WARN:  return "WARN";
	case VM_KLOG_INFO:  return "INFO";
	case VM_KLOG_DEBUG: return "DEBUG";
	case VM_KLOG_TRACE: return "TRACE";
	default:            return "?";
	}
}

void vm_klog(int level, const char *fmt, ...)
{
	if (level > g_level || !fmt || g_enabled == 0)   /* off: not even formatted */
		return;

	char line[192];
	int pos = 0;

	uint64_t t = (uint64_t)ksceKernelGetSystemTimeWide();  /* microseconds        */
	uint32_t secs = (uint32_t)(t / 1000000u);
	uint32_t us   = (uint32_t)(t % 1000000u);

	/* "[vita-memio] [<s>.<us>] <LVL> " prefix. */
	const char *tag = "[vita-memio] [";
	while (*tag && pos < (int)sizeof(line) - 1) line[pos++] = *tag++;
	pos = vm_fmt_uint(line, sizeof(line), pos, secs, 10, 0, 0, 0);
	if (pos < (int)sizeof(line) - 1) line[pos++] = '.';
	pos = vm_fmt_uint(line, sizeof(line), pos, us, 10, 0, 6, 1);
	if (pos < (int)sizeof(line) - 1) line[pos++] = ']';
	if (pos < (int)sizeof(line) - 1) line[pos++] = ' ';
	const char *lvl = level_name(level);
	while (*lvl && pos < (int)sizeof(line) - 1) line[pos++] = *lvl++;
	if (pos < (int)sizeof(line) - 1) line[pos++] = ' ';

	va_list ap;
	va_start(ap, fmt);
	pos += vm_fmt_v(line + pos, (int)sizeof(line) - pos, fmt, ap);
	va_end(ap);

	if (pos > (int)sizeof(line) - 1)
		pos = (int)sizeof(line) - 1;
	line[pos] = 0;

	/* Both sinks only while logging is on; before that is known, hold it. */
	if (!g_inited)
		return;
	lock();
	if (g_enabled > 0)
		emit_locked(line, pos);
	else if (g_enabled < 0 && g_early_n < EARLY_LINES) {
		memcpy(g_early[g_early_n], line, sizeof(line));
		g_early_len[g_early_n++] = pos;
	}
	unlock();
}
