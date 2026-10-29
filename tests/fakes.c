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
#include <sys/mman.h>
#include <unistd.h>

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "fakes.h"

int ts_failures;

uint32_t fake_now_us;
int fake_alloc_fail, fake_alloc_calls, fake_free_calls;
int fake_hook_fail_at, fake_hook_calls, fake_hook_releases;
int fake_rename_fail, fake_write_short;
int fake_mirror_fail, fake_mirror_calls, fake_mirror_wrong, fake_mirror_type;
int fake_dcache_missing, fake_dc_inv_calls, fake_dc_clean_inv_calls, fake_dc_bad_range;
int fake_va2pa_missing, fake_paddr_types_refused, fake_mirror_frees;
uint32_t fake_dc_inv_bytes;

/* ---- logging: swallowed ---------------------------------------------------- */
void ts_klog(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void ts_log(const char *fmt, ...) { (void)fmt; }

/* ---- time / threads -------------------------------------------------------- */
int ksceKernelDelayThread(unsigned us) { (void)us; return 0; }
uint32_t ksceKernelGetSystemTimeLow(void) { return fake_now_us; }

/* ---- memory blocks --------------------------------------------------------- */
/* Every block is a memfd mapped into memory, so a second block on the same
 * physical address can be a real second mapping at a different address, as on
 * the console. Fake physical address of block i: 0x40000000 + i MiB. */
static void  *g_blocks[64];
static size_t g_len[64];
static int    g_fd[64] = { [0 ... 63] = -1 };
static int    g_mirror_of[64];  /* a mirror block shows another block's memory */

static void *map_new(size_t size, int *fd_out)
{
	size_t len = (size + 4095) & ~(size_t)4095;
	int fd = memfd_create("fakeblock", 0);
	void *p;

	if (fd < 0 || ftruncate(fd, (off_t)len) != 0) {
		perror("memfd");
		exit(2);
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(2);
	}
	*fd_out = fd;
	return p;
}

#define FAKE_PA(i) (0x40000000u + (uint32_t)(i) * 0x100000u)

SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize size, void *opt)
{
	const SceKernelAllocMemBlockKernelOpt *o = opt;

	(void)name;
	if (o && (o->attr & SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PADDR)) {
		int target = -1;

		fake_mirror_calls++;
		fake_mirror_type = (int)type;
		/* fake_paddr_types_refused: how many types are refused before one works */
		if (fake_mirror_fail || fake_mirror_calls <= fake_paddr_types_refused)
			return -7;
		for (int i = 1; i < 64; i++)
			if (g_blocks[i] && !g_mirror_of[i] && FAKE_PA(i) == o->paddr)
				target = i;
		if (target < 0)
			return -8;
		for (int i = 1; i < 64; i++) {
			if (!g_blocks[i]) {
				size_t len = (size + 4095) & ~(size_t)4095;

				if (fake_mirror_wrong) {         /* new memory, as the "mirror" gave */
					g_blocks[i] = map_new(size, &g_fd[i]);
					g_len[i] = len;
					return i;                    /* its own physical address */
				} else {                         /* the same memory, a new address */
					g_blocks[i] = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
					                   g_fd[target], 0);
					if (g_blocks[i] == MAP_FAILED)
						abort();
				}
				g_len[i] = len;
				g_mirror_of[i] = target;
				return i;
			}
		}
		return -6;
	}
	fake_alloc_calls++;
	if (fake_alloc_fail)
		return -6;
	for (int i = 1; i < 64; i++) {
		if (!g_blocks[i]) {
			g_blocks[i] = map_new(size, &g_fd[i]);      /* zero-filled */
			g_len[i] = (size + 4095) & ~(size_t)4095;
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
	if (g_mirror_of[uid]) {
		/* A second mapping: the block it maps must still be there (it goes
		 * first). */
		if (!g_blocks[g_mirror_of[uid]])
			abort();
		fake_mirror_frees++;
	} else {
		for (int i = 1; i < 64; i++)      /* never free memory a mirror still shows */
			if (g_mirror_of[i] == uid)
				abort();
		fake_free_calls++;
	}
	munmap(g_blocks[uid], g_len[uid]);
	if (g_fd[uid] >= 0)
		close(g_fd[uid]);
	g_blocks[uid] = NULL;
	g_fd[uid] = -1;
	g_mirror_of[uid] = 0;
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

/* The cache functions (SceCpuForDriver): count the calls, check the ranges. */
static void fake_dc_inv(void *start, unsigned int size)
{
	fake_dc_inv_calls++;
	fake_dc_inv_bytes += size;
	if (((uintptr_t)start | size) & 0x3F)
		fake_dc_bad_range++;
}

static void fake_dc_clean_inv(void *start, unsigned int size)
{
	fake_dc_clean_inv_calls++;
	if (((uintptr_t)start | size) & 0x3F)
		fake_dc_bad_range++;
}

/* ksceKernelVAtoPA: blocks only. A second mapping reports its target's address. */
static int fake_va2pa(const void *va, uintptr_t *pa)
{
	for (int i = 1; i < 64; i++) {
		uint8_t *b = g_blocks[i];

		if (b && (const uint8_t *)va >= b && (const uint8_t *)va < b + g_len[i]) {
			int home = g_mirror_of[i] && g_fd[i] < 0 ? g_mirror_of[i] : i;

			*pa = FAKE_PA(home) + (uintptr_t)((const uint8_t *)va - b);
			return 0;
		}
	}
	return -1;
}

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func)
{
	(void)pid;
	if (!fake_va2pa_missing && !strcmp(modname, "SceSysmem") && libnid == 0x6F25E18A &&
	    funcnid == 0x8D160E65) {
		*func = (uintptr_t)fake_va2pa;
		return 0;
	}
	if (!fake_dcache_missing && !strcmp(modname, "SceSysmem") && libnid == 0x40ECDB0E) {
		if (funcnid == 0x8B4C26DF) { *func = (uintptr_t)fake_dc_inv; return 0; }   /* L1 */
		if (funcnid == 0x364E68A4) { *func = (uintptr_t)fake_dc_clean_inv; return 0; }
	}
	return -1;
}

void *ksceSdifGetSdContextGlobal(int port) { (void)port; return NULL; }

/* ---- file system: ur0:data/turbostorage/<x> -> <tmpdir>/<x> ---------------- */
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
	fake_alloc_fail = fake_alloc_calls = fake_free_calls = 0;
	fake_hook_fail_at = fake_hook_calls = fake_hook_releases = 0;
	fake_rename_fail = fake_write_short = 0;
	fake_mirror_fail = fake_mirror_calls = fake_mirror_wrong = fake_mirror_type = 0;
	fake_dcache_missing = fake_dc_inv_calls = fake_dc_clean_inv_calls = fake_dc_bad_range = 0;
	fake_dc_inv_bytes = 0;
	fake_va2pa_missing = fake_paddr_types_refused = fake_mirror_frees = 0;
}

static const char *map(const char *path, char *out, size_t n)
{
	static const char pre[] = "ur0:data/turbostorage/";

	if (strncmp(path, pre, sizeof(pre) - 1) != 0 || !g_tmp[0])
		return NULL;
	snprintf(out, n, "%s/%s", g_tmp, path + sizeof(pre) - 1);
	return out;
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
