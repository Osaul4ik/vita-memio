/*
 * vita_env.c - see vita_env.h.
 */
#include <string.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include "../../include/storageturbo_kernel_api.h"
#include "../restore.h"
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

/* ---- the plugin ---------------------------------------------------------------- */

static int get_state(TsState *st)
{
	memset(st, 0, sizeof(*st));
	if (tsGetState(st) != TS_OK || st->magic != TS_STATE_MAGIC)
		return -1;
	return 0;
}

static int v_plugin_get(void *ctx, BenchPluginState *out)
{
	VitaEnv *v = ctx;
	TsState st;

	memset(out, 0, sizeof(*out));
	if (get_state(&st) != 0)
		return -1;
	out->available = 1;
	out->disabled = st.disabled;
	out->status = st.dev[v->dev].status;
	out->reads = st.dev[v->dev].reads;
	out->writes = st.dev[v->dev].writes;
	out->errors = st.dev[v->dev].errors;
	out->mismatches = st.dev[v->dev].mismatches;
	out->fingerprint = st.fingerprint;
	return 0;
}

static void save_record(const TsSettings *s)
{
	RestoreRec r;
	SceUID fd;

	vita_mkdirs(TS_DATA_DIR "/");
	restore_pack(&r, s);
	fd = sceIoOpen(RESTORE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, &r, sizeof(r));
		sceIoClose(fd);
	}
}

/* Put `want` back; 0 when the plugin ends up with exactly those settings. */
static int apply_settings(const TsSettings *want)
{
	TsState st;
	int bad = 0;

	if (get_state(&st) != 0)
		return -1;
	for (int k = 0; k < TS_KEY_COUNT; k++) {
		TsSettings cur = st.settings, w = *want;

		if (*ts_setting_field(&cur, k) != *ts_setting_field(&w, k) &&
		    tsSetSetting(k, *ts_setting_field(&w, k)) != TS_OK)
			bad = 1;
	}
	if (bad || get_state(&st) != 0)
		return -1;
	return memcmp(&st.settings, want, sizeof(*want)) == 0 ? 0 : -1;
}

static int v_plugin_begin(void *ctx)
{
	VitaEnv *v = ctx;
	TsState st;

	if (get_state(&st) != 0)
		return -1;
	v->saved = st.settings;
	v->have_saved = 1;
	save_record(&v->saved);
	/* Verify mode re-reads and compares every transfer: it would distort the
	 * timings, and the tester does its own checking. */
	if (st.settings.verify && tsSetSetting(TS_KEY_VERIFY, 0) != TS_OK)
		return -1;
	return 0;
}

static int v_plugin_set(void *ctx, int on)
{
	VitaEnv *v = ctx;

	return tsSetSetting(bench_device_key(v->dev), on ? 1 : 0) == TS_OK ? 0 : -1;
}

static void v_plugin_end(void *ctx)
{
	VitaEnv *v = ctx;

	if (!v->have_saved)
		return;
	if (apply_settings(&v->saved) == 0) {
		sceIoRemove(RESTORE_PATH);        /* only once the settings are really back */
		v->have_saved = 0;
	}
}

int vita_env_recover(void)
{
	RestoreRec r;
	TsSettings s;
	SceUID fd = sceIoOpen(RESTORE_PATH, SCE_O_RDONLY, 0);
	int n;

	if (fd < 0)
		return 0;
	n = sceIoRead(fd, &r, sizeof(r));
	sceIoClose(fd);
	if (n != (int)sizeof(r) || restore_unpack(&r, &s) != 0) {
		sceIoRemove(RESTORE_PATH);        /* unreadable: nothing safe to restore */
		return 0;
	}
	if (apply_settings(&s) != 0)
		return -1;
	sceIoRemove(RESTORE_PATH);
	return 1;
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
	v->env.plugin_begin = v_plugin_begin;
	v->env.plugin_set = v_plugin_set;
	v->env.plugin_end = v_plugin_end;
	v->env.progress = v_progress;
	v->env.cancelled = v_cancelled;
}
