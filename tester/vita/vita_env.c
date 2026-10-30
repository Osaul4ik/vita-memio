/*
 * vita_env.c - see vita_env.h.
 */
#include <string.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include "../../include/vitamemio_kernel_api.h"
#include "vita_env.h"

/* ---- time and files ---------------------------------------------------------- */

static uint64_t v_now_us(void *ctx)
{
	return sceKernelGetProcessTimeWide();
}

static int v_open(void *ctx, const char *path, int create_trunc)
{
	int flags = SCE_O_RDWR | (create_trunc ? (SCE_O_CREAT | SCE_O_TRUNC) : 0);

	return sceIoOpen(path, flags, 0777);
}

static int v_pread(void *ctx, int h, void *buf, uint32_t n, uint64_t off)
{
	return sceIoPread(h, buf, n, (SceOff)off);
}

static int v_pwrite(void *ctx, int h, const void *buf, uint32_t n, uint64_t off)
{
	return sceIoPwrite(h, buf, n, (SceOff)off);
}

static int v_close(void *ctx, int h)
{
	return sceIoClose(h);
}

static int v_remove(void *ctx, const char *path)
{
	return sceIoRemove(path);
}

int vita_mkdirs(const char *dir)
{
	char p[BENCH_PATH_MAX];
	size_t n = strlen(dir), i;
	const char *colon = strchr(dir, ':');
	SceUID d;

	if (n == 0 || n >= sizeof(p))
		return -1;
	memcpy(p, dir, n + 1);
	/* "ux0:a/b/" -> make "ux0:a", then "ux0:a/b". Errors for existing folders are
	 * expected; the final check below decides. */
	for (i = colon ? (size_t)(colon - dir) + 1 : 0; i <= n; i++) {
		if (p[i] != '/' && p[i] != 0)
			continue;
		if (i > 0 && p[i - 1] != ':' && p[i - 1] != '/') {
			char keep = p[i];

			p[i] = 0;
			sceIoMkdir(p, 0777);
			p[i] = keep;
		}
	}
	if (p[n - 1] == '/')
		p[n - 1] = 0;
	d = sceIoDopen(p);
	if (d < 0)
		return -1;
	sceIoDclose(d);
	return 0;
}

static int v_mkdirs(void *ctx, const char *dir)
{
	return vita_mkdirs(dir);
}

/* ---- the plugin (read only) -------------------------------------------------- */

/* The tester only ever READS the plugin's state (vmGetState). It never calls
 * vmSetSetting: switching the plugin on or off is done by the user in Settings. */
static int v_plugin_get(void *ctx, BenchPluginState *out)
{
	VitaEnv *v = ctx;
	VmState st;

	memset(out, 0, sizeof(*out));
	memset(&st, 0, sizeof(st));
	if (vmGetState(&st) != VM_OK || st.magic != VM_STATE_MAGIC)
		return -1;
	out->available = 1;
	out->disabled = st.disabled;
	out->status = st.dev[v->dev].status;
	out->reads = st.dev[v->dev].reads;
	out->writes = st.dev[v->dev].writes;
	out->errors = st.dev[v->dev].errors;
	out->mismatches = st.dev[v->dev].mismatches;
	out->passthru = st.dev[v->dev].passthru;
	{
		/* vmGetSetting is newer than VmState: an older kernel module does not
		 * have it (the weak import then returns without an answer). */
		int s = vmGetSetting(VM_KEY_SMALL);

		out->small = s == 0 || s == 1 ? s : -1;
		s = vmGetSetting(VM_KEY_PIPELINE);
		out->pipeline = s >= VM_PIPE_OFF && s <= VM_PIPE_FIXED ? s : -1;
	}
	out->fingerprint = st.fingerprint;
	out->verify = st.settings.verify;
	out->verify_ready = st.verify_ready;
	out->verified = st.dev[v->dev].verified;
	{
		/* Optional: a kernel module older than this tester has no vmGetTiming
		 * (the weak import then just returns), so check the result and magic. */
		VmTiming tm;

		memset(&tm, 0, sizeof(tm));
		if (vmGetTiming(&tm) == VM_OK && tm.magic == VM_TIMING_MAGIC &&
		    tm.size == sizeof(tm)) {
			const VmTimingDev *t = &tm.dev[v->dev];

			out->timing_ok = 1;
			out->rd_sectors = t->rd_sectors;
			out->rd_io_us = t->rd_io_us;
			out->rd_copy_us = t->rd_copy_us;
			out->wr_sectors = t->wr_sectors;
			out->wr_io_us = t->wr_io_us;
			out->wr_copy_us = t->wr_copy_us;
		}
	}
	return 0;
}

/* ---- non-cacheable memory for the NC reads ---------------------------------- */

/* The kind of memory games give the graphics chip: the CPU writes go
 * straight to memory, with nothing left in the cache. */
static void *v_nc_alloc(void *ctx, uint32_t size)
{
	VitaEnv *v = ctx;
	void *base = NULL;
	SceUID uid;

	size = (size + 0xFFFFu) & ~0xFFFFu;
	uid = sceKernelAllocMemBlock("vmtest_nc", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
	                             size, NULL);
	if (uid < 0)
		return NULL;
	if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base) {
		sceKernelFreeMemBlock(uid);
		return NULL;
	}
	v->nc_uid = uid;
	return base;
}

static void v_nc_free(void *ctx, void *p)
{
	VitaEnv *v = ctx;

	(void)p;
	if (v->nc_uid >= 0) {
		sceKernelFreeMemBlock(v->nc_uid);
		v->nc_uid = -1;
	}
}

/* ---- the BenchEnv ---------------------------------------------------------------- */

static void v_progress(void *ctx, const BenchProgress *p)
{
	VitaEnv *v = ctx;

	if (v->on_progress)
		v->on_progress(p);
}

static int v_cancelled(void *ctx)
{
	VitaEnv *v = ctx;

	return v->poll_cancel ? v->poll_cancel() : 0;
}

void vita_env_init(VitaEnv *v, int dev,
                   void (*on_progress)(const BenchProgress *p),
                   int (*poll_cancel)(void))
{
	memset(v, 0, sizeof(*v));
	v->dev = dev;
	v->on_progress = on_progress;
	v->poll_cancel = poll_cancel;
	v->env.ctx = v;
	v->env.now_us = v_now_us;
	v->env.open = v_open;
	v->env.pread = v_pread;
	v->env.pwrite = v_pwrite;
	v->env.close = v_close;
	v->env.remove = v_remove;
	v->env.mkdirs = v_mkdirs;
	v->env.plugin_get = v_plugin_get;
	v->env.progress = v_progress;
	v->env.cancelled = v_cancelled;
	v->env.nc_alloc = v_nc_alloc;
	v->env.nc_free = v_nc_free;
	v->nc_uid = -1;
}
