/*
 * fakes.c - host-side stand-ins for the kernel and SDK functions the tested
 * files call. Nothing here is the code under test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
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
uint32_t ksceKernelGetSystemTimeLow(void)
{
	__sync_fetch_and_add(&fake_time_calls, 1);
	return __sync_fetch_and_add(&fake_now_us, 0);
}

/* Real threads and counting semaphores (pthreads), so code that hands work to
 * another thread is tested with real concurrency. */
int fake_thread_fail, fake_sema_fail, fake_threads_live, fake_semas_live;

typedef struct { int used; pthread_mutex_t m; pthread_cond_t c; int count, max; } FakeSema;
static FakeSema g_semas[32];

SceUID ksceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, void *option)
{
	(void)name; (void)attr; (void)option;
	if (fake_sema_fail && --fake_sema_fail == 0)
		return -0x20;
	for (int i = 1; i < 32; i++)
		if (!g_semas[i].used) {
			g_semas[i].used = 1;
			pthread_mutex_init(&g_semas[i].m, NULL);
			pthread_cond_init(&g_semas[i].c, NULL);
			g_semas[i].count = initVal;
			g_semas[i].max = maxVal;
			fake_semas_live++;
			return 0x100 + i;
		}
	return -0x21;
}

static FakeSema *sema_of(SceUID id)
{
	int i = id - 0x100;

	return i > 0 && i < 32 && g_semas[i].used ? &g_semas[i] : NULL;
}

int ksceKernelDeleteSema(SceUID id)
{
	FakeSema *s = sema_of(id);

	if (!s)
		return -1;
	pthread_mutex_destroy(&s->m);
	pthread_cond_destroy(&s->c);
	s->used = 0;
	fake_semas_live--;
	return 0;
}

int ksceKernelSignalSema(SceUID id, int signal)
{
	FakeSema *s = sema_of(id);

	if (!s)
		return -1;
	pthread_mutex_lock(&s->m);
	if (s->count + signal > s->max) {
		pthread_mutex_unlock(&s->m);
		return -2;                          /* as the kernel: over the maximum */
	}
	s->count += signal;
	pthread_cond_broadcast(&s->c);
	pthread_mutex_unlock(&s->m);
	return 0;
}

int ksceKernelWaitSema(SceUID id, int signal, SceUInt *timeout)
{
	FakeSema *s = sema_of(id);

	(void)timeout;
	if (!s)
		return -1;
	pthread_mutex_lock(&s->m);
	while (s->count < signal)
		pthread_cond_wait(&s->c, &s->m);
	s->count -= signal;
	pthread_mutex_unlock(&s->m);
	return 0;
}

typedef struct {
	int used, started;
	pthread_t t;
	SceKernelThreadEntry entry;
	SceSize arglen;
	unsigned char args[64] __attribute__((aligned(16)));
} FakeThread;
static FakeThread g_threads[16];

static void *thread_main(void *p)
{
	FakeThread *t = p;

	t->entry(t->arglen, t->args);
	return NULL;
}

SceUID ksceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
                              SceSize stackSize, SceUInt attr, int cpuAffinityMask,
                              const void *option)
{
	(void)name; (void)initPriority; (void)stackSize; (void)attr; (void)cpuAffinityMask;
	(void)option;
	if (fake_thread_fail)
		return -0x30;
	for (int i = 1; i < 16; i++)
		if (!g_threads[i].used) {
			memset(&g_threads[i], 0, sizeof(g_threads[i]));
			g_threads[i].used = 1;
			g_threads[i].entry = entry;
			return 0x200 + i;
		}
	return -0x31;
}

static FakeThread *thread_of(SceUID id)
{
	int i = id - 0x200;

	return i > 0 && i < 16 && g_threads[i].used ? &g_threads[i] : NULL;
}

int ksceKernelStartThread(SceUID id, SceSize arglen, void *argp)
{
	FakeThread *t = thread_of(id);

	if (!t || t->started || arglen > sizeof(t->args))
		return -1;
	memcpy(t->args, argp, arglen);           /* copied, as the kernel does */
	t->arglen = arglen;
	if (pthread_create(&t->t, NULL, thread_main, t) != 0)
		return -2;
	t->started = 1;
	fake_threads_live++;
	return 0;
}

int ksceKernelWaitThreadEnd(SceUID id, int *stat, SceUInt *timeout)
{
	FakeThread *t = thread_of(id);

	(void)stat; (void)timeout;
	if (!t || !t->started)
		return -1;
	pthread_join(t->t, NULL);
	t->started = 0;
	fake_threads_live--;
	return 0;
}

int ksceKernelDeleteThread(SceUID id)
{
	FakeThread *t = thread_of(id);

	if (!t || t->started)
		return -1;                          /* only an ended thread */
	t->used = 0;
	return 0;
}

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
	fake_memtype = 0xD000;          /* ordinary cached memory */
	fake_memtype_calls = 0;
	fake_memtype_from = NULL;
	fake_memtype_above = 0xD000;
	fake_thread_fail = fake_sema_fail = 0;
}

int fake_memtype = 0xD000, fake_memtype_calls;
const void *fake_memtype_from;
int fake_memtype_above = 0xD000;

int ksceKernelGetMemBlockMemtypeByAddr(const void *addr)
{
	fake_memtype_calls++;
	if (fake_memtype_from && (const char *)addr >= (const char *)fake_memtype_from)
		return fake_memtype_above;
	return fake_memtype;
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
