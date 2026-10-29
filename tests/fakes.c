/*
 * fakes.c - host-side stand-ins for the kernel and SDK functions the tested
 * files call. Nothing here is the code under test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "fakes.h"

int vm_failures;

uint32_t fake_now_us;
int fake_alloc_fail, fake_alloc_calls, fake_free_calls;
int fake_hook_fail_at, fake_hook_calls, fake_hook_releases;
int fake_rename_fail, fake_write_short;

/* ---- logging: swallowed ---------------------------------------------------- */
void vm_klog(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void vm_log(const char *fmt, ...) { (void)fmt; }

/* ---- time / threads -------------------------------------------------------- */
int ksceKernelDelayThread(unsigned us) { (void)us; return 0; }
int fake_time_calls;
uint32_t ksceKernelGetSystemTimeLow(void) { fake_time_calls++; return fake_now_us; }

/* ---- memory blocks --------------------------------------------------------- */
static void *g_blocks[64];

SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize size, void *opt)
{
	(void)name; (void)type; (void)opt;
	fake_alloc_calls++;
	if (fake_alloc_fail)
		return -6;
	for (int i = 1; i < 64; i++) {
		if (!g_blocks[i]) {
			g_blocks[i] = calloc(1, size);
			return i;
		}
	}
	return -6;
}

int ksceKernelGetMemBlockBase(SceUID uid, void **base)
{
	if (uid <= 0 || uid >= 64 || !g_blocks[uid])
		return -1;
	*base = g_blocks[uid];
	return 0;
}

int ksceKernelFreeMemBlock(SceUID uid)
{
	if (uid <= 0 || uid >= 64 || !g_blocks[uid])
		return -1;
	free(g_blocks[uid]);
	g_blocks[uid] = NULL;
	fake_free_calls++;
	return 0;
}

/* ---- taiHEN ---------------------------------------------------------------- */
int taiHookFunctionExportForKernel(int pid, tai_hook_ref_t *ref, const char *mod,
                                   uint32_t lib, uint32_t nid, const void *fn)
{
	(void)pid; (void)mod; (void)lib; (void)nid; (void)fn;
	fake_hook_calls++;
	if (fake_hook_fail_at && fake_hook_calls == fake_hook_fail_at)
		return -1;
	*ref = 0x1000 + fake_hook_calls;
	return 100 + fake_hook_calls;
}

int taiHookReleaseForKernel(int uid, tai_hook_ref_t ref)
{
	(void)uid; (void)ref;
	fake_hook_releases++;
	return 0;
}

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func)
{
	(void)pid; (void)modname; (void)libnid; (void)funcnid; (void)func;
	return -1;
}

void *ksceSdifGetSdContextGlobal(int port) { (void)port; return NULL; }

/* ---- file system: ur0:data/vita-memio/<x> -> <tmpdir>/<x> ---------------- */
static char g_tmp[256];

const char *fakes_tmpdir_new(void)
{
	snprintf(g_tmp, sizeof(g_tmp), "/tmp/ts-test-XXXXXX");
	if (!mkdtemp(g_tmp)) {
		perror("mkdtemp");
		exit(2);
	}
	return g_tmp;
}

void fakes_tmpdir_remove(void)
{
	char cmd[300];

	if (g_tmp[0]) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp);
		if (system(cmd)) {}
		g_tmp[0] = 0;
	}
}

void fakes_reset(void)
{
	fake_now_us = 0;
	fake_time_calls = 0;
	fake_alloc_fail = fake_alloc_calls = fake_free_calls = 0;
	fake_hook_fail_at = fake_hook_calls = fake_hook_releases = 0;
	fake_rename_fail = fake_write_short = 0;
}

static const char *map(const char *path, char *out, size_t n)
{
	static const char pre[] = "ur0:data/vita-memio/";

	if (!g_tmp[0])
		return NULL;
	if (!strncmp(path, pre, sizeof(pre) - 1)) {
		snprintf(out, n, "%s/%s", g_tmp, path + sizeof(pre) - 1);
		return out;
	}
	return NULL;
}

SceUID ksceIoOpen(const char *path, int flags, int mode)
{
	char p[512];
	int f = 0;

	if (!map(path, p, sizeof(p)))
		return -1;
	if ((flags & SCE_O_RDONLY) && (flags & SCE_O_WRONLY))
		f |= O_RDWR;
	else if (flags & SCE_O_WRONLY)
		f |= O_WRONLY;
	else
		f |= O_RDONLY;
	if (flags & SCE_O_CREAT)  f |= O_CREAT;
	if (flags & SCE_O_TRUNC)  f |= O_TRUNC;
	if (flags & SCE_O_APPEND) f |= O_APPEND;
	int fd = open(p, f, mode ? mode : 0666);
	return fd < 0 ? -1 : fd;
}

int ksceIoRead(SceUID fd, void *buf, SceSize n) { return (int)read(fd, buf, n); }

int ksceIoWrite(SceUID fd, const void *buf, SceSize n)
{
	if (fake_write_short && n > 4)
		n = n / 2;                 /* a write cut short (card full, power cut) */
	return (int)write(fd, buf, n);
}

int ksceIoClose(SceUID fd) { return close(fd); }

int ksceIoMkdir(const char *path, int mode) { (void)path; (void)mode; return 0; }

int ksceIoRemove(const char *path)
{
	char p[512];

	return map(path, p, sizeof(p)) ? unlink(p) : -1;
}

/* Strict like an exFAT rename: it does not replace an existing target. */
int ksceIoRename(const char *from, const char *to)
{
	char a[512], b[512];

	if (fake_rename_fail || !map(from, a, sizeof(a)) || !map(to, b, sizeof(b)))
		return -1;
	if (access(b, F_OK) == 0)
		return -1;
	return rename(a, b);
}
