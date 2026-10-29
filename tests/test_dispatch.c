/*
 * test_dispatch.c - host tests for user/dispatch.c, the developer command port's
 * parser. Built twice: normally, and with -DTS_DEV_ALLOW_RISKY=1 (the diagnostics
 * build), because the two differ on `set internal` and `set verify`.
 *
 * The kernel calls (tsGetState, tsSetSetting, tsSelfTestPing) are fakes backed by
 * a TsSettings array; the sockets are never used.
 */
#include "../user/dispatch.c"

#include <stdlib.h>

#include "fakes.h"
#include "ts_test.h"

/* ---- fake kernel API ---------------------------------------------------------- */
static TsSettings g_set;

int tsSelfTestPing(void) { return (int)TS_SELFTEST_MAGIC; }

int tsGetState(TsState *out)
{
	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->magic = TS_STATE_MAGIC;
	memcpy(&out->settings, &g_set, sizeof(out->settings));   /* keys 0..6 */
	out->fingerprint = 0x2e7c52f7;
	out->uptime_s = 42;
	out->dev[TS_DEV_SD2VITA].status = TS_ST_ACTIVE;
	out->dev[TS_DEV_SD2VITA].reads = 7;
	out->dev[TS_DEV_SD2VITA].time_ms = 1234;
	out->dev[TS_DEV_SD2VITA].max_ms = 56;
	out->dev[TS_DEV_MEMCARD].status = TS_ST_FALLBACK;
	return TS_OK;
}

int tsGetSetting(int key)
{
	int32_t *f = ts_setting_field(&g_set, key);

	return f ? (int)*f : TS_ERR_ARG;
}

int tsGetCachedRead(int dev)
{
	static const int st[TS_DEV_COUNT] = { TS_CR_ACTIVE, TS_CR_OFF, TS_CR_UNAVAILABLE };

	return g_set.cached_read && dev >= 0 && dev < TS_DEV_COUNT ? st[dev] :
	       dev >= 0 && dev < TS_DEV_COUNT ? TS_CR_OFF : TS_ERR_ARG;
}

int tsSetSetting(int key, int value)
{
	if (!ts_setting_valid(key, value))
		return TS_ERR_ARG;
	*ts_setting_field(&g_set, key) = value;
	return TS_OK;
}

/* ---- unused socket/thread functions the file references ----------------------- */
int sceKernelDelayThread(unsigned us) { (void)us; return 0; }
SceUID sceKernelCreateThread(const char *n, int (*f)(SceSize, void *), int a, int b, int c, int d, void *e)
{ (void)n; (void)f; (void)a; (void)b; (void)c; (void)d; (void)e; return -1; }
int sceKernelStartThread(SceUID t, SceSize a, void *p) { (void)t; (void)a; (void)p; return -1; }
int sceKernelWaitThreadEnd(SceUID t, int *s, unsigned *o) { (void)t; (void)s; (void)o; return -1; }
int sceKernelDeleteThread(SceUID t) { (void)t; return -1; }
int sceNetSocket(const char *n, int d, int t, int p) { (void)n; (void)d; (void)t; (void)p; return -1; }
int sceNetBind(int s, const SceNetSockaddr *a, unsigned l) { (void)s; (void)a; (void)l; return -1; }
int sceNetListen(int s, int b) { (void)s; (void)b; return -1; }
int sceNetAccept(int s, SceNetSockaddr *a, unsigned *l) { (void)s; (void)a; (void)l; return -1; }
int sceNetRecv(int s, void *b, unsigned n, int f) { (void)s; (void)b; (void)n; (void)f; return -1; }
int sceNetSend(int s, const void *b, unsigned n, int f) { (void)s; (void)b; (void)n; (void)f; return -1; }
int sceNetSocketClose(int s) { (void)s; return 0; }
int sceNetSetsockopt(int s, int l, int o, const void *v, unsigned n) { (void)s; (void)l; (void)o; (void)v; (void)n; return 0; }
uint16_t sceNetHtons(uint16_t v) { return v; }
uint32_t sceNetHtonl(uint32_t v) { return v; }

/* ---- helpers ------------------------------------------------------------------ */
static char out[1024];

static const char *run(const char *line)
{
	memset(out, 'X', sizeof(out));
	ts_dispatch_exec(line, out, sizeof(out));
	return out;
}

static int has(const char *s, const char *sub) { return strstr(s, sub) != NULL; }

static void test_basic_commands(void)
{
	memset(&g_set, 0, sizeof(g_set));
	CHECK(has(run("help"), "commands:"));
	CHECK(has(run("version"), TS_VERSION_STR));
	CHECK(has(run("ping"), "ping ok"));
	CHECK(has(run("nonsense"), "unknown command"));
	CHECK(has(run(""), "unknown command"));
}

static void test_help_matches_build(void)
{
#ifdef TS_DEV_ALLOW_RISKY
	CHECK(has(run("help"), "internal verify"));
#else
	CHECK(!has(run("help"), "internal verify"));
	CHECK(has(run("help"), "Settings only"));
#endif
}

static void test_status(void)
{
	const char *r;

	memset(&g_set, 0, sizeof(g_set));
	r = run("status");
	CHECK(has(r, "turbo-storage") && has(r, "fingerprint=0x2e7c52f7"));
	CHECK(has(r, "sd2vita: active reads=7"));
	CHECK(has(r, "time=1234ms max=56ms"));
	CHECK(has(r, "memcard: fallback"));
	CHECK(has(r, "cached_read=0: sd2vita=off internal=off memcard=off"));
	CHECK(r[strlen(r) - 1] != '\n');          /* the server adds the newline */
	g_set.cached_read = 1;
	r = run("status");
	CHECK(has(r, "cached_read=1: sd2vita=active internal=off memcard=unavailable"));
	g_set.cached_read = 7;              /* what an older kernel module may answer */
	r = run("status");
	CHECK(has(r, "cached_read=?:\n") || has(r, "cached_read=?:"));
	CHECK(!has(r, "sd2vita=active internal"));
	g_set.cached_read = 0;
}

static void test_set_ordinary_keys(void)
{
	memset(&g_set, 0, sizeof(g_set));
	CHECK(has(run("set sd2vita 1"), "ok") && g_set.sd2vita == 1);
	CHECK(has(run("set memcard 1"), "ok") && g_set.memcard == 1);
	CHECK(has(run("set hud 2"), "ok") && g_set.hud == 2);
	CHECK(has(run("set hud_pos 3"), "ok") && g_set.hud_pos == 3);
	CHECK(has(run("set devmode 0"), "ok") && g_set.devmode == 0);
}

static void test_set_rejects_bad_input(void)
{
	memset(&g_set, 0, sizeof(g_set));
	CHECK(has(run("set hud 9"), "rejected") && g_set.hud == 0);
	CHECK(has(run("set sd2vita 2"), "rejected") && g_set.sd2vita == 0);
	CHECK(has(run("set bogus 1"), "usage"));
	CHECK(has(run("set sd2vita"), "usage"));
	CHECK(has(run("set sd2vita x"), "usage"));
	CHECK(has(run("set sd2vita -1"), "usage"));
	CHECK(has(run("set  sd2vita 1"), "usage") && g_set.sd2vita == 0);
	CHECK(has(run("set sd2vitaX 1"), "usage"));
}

/* A number far past int range used to overflow a signed int (undefined
 * behaviour); it must just be rejected as out of range. */
static void test_set_huge_number(void)
{
	memset(&g_set, 0, sizeof(g_set));
	CHECK(has(run("set sd2vita 99999999999999999999"), "rejected"));
	CHECK(g_set.sd2vita == 0);
	CHECK(has(run("set hud 4294967297"), "rejected"));   /* would wrap to 1 */
	CHECK(g_set.hud == 0);
}

static void test_risky_keys(void)
{
	memset(&g_set, 0, sizeof(g_set));
#ifdef TS_DEV_ALLOW_RISKY
	CHECK(has(run("set internal 1"), "ok") && g_set.internal == 1);
	CHECK(has(run("set verify 1"), "ok") && g_set.verify == 1);
	CHECK(has(run("set cached_read 1"), "ok") && g_set.cached_read == 1);
#else
	CHECK(has(run("set cached_read 1"), "not allowed") && g_set.cached_read == 0);
	CHECK(has(run("set internal 1"), "not allowed") && g_set.internal == 0);
	CHECK(has(run("set verify 1"), "not allowed") && g_set.verify == 0);
	CHECK(has(run("set internal 0"), "not allowed"));
#endif
}

/* Small reply buffers: the answer is cut, never overrun, and stays terminated. */
static void test_small_reply_buffers(void)
{
	const char *cmds[] = { "help", "status", "version", "ping", "set sd2vita 1", "x" };

	for (unsigned c = 0; c < sizeof(cmds) / sizeof(cmds[0]); c++) {
		for (int cap = 1; cap <= 40; cap++) {
			char *b = malloc(cap);          /* exact size: ASan sees an overrun */

			memset(&g_set, 0, sizeof(g_set));
			ts_dispatch_exec(cmds[c], b, cap);
			CHECK(memchr(b, 0, cap) != NULL);
			free(b);
		}
	}
}

int main(void)
{
	RUN(test_basic_commands);
	RUN(test_help_matches_build);
	RUN(test_status);
	RUN(test_set_ordinary_keys);
	RUN(test_set_rejects_bad_input);
	RUN(test_set_huge_number);
	RUN(test_risky_keys);
	RUN(test_small_reply_buffers);

#ifdef TS_DEV_ALLOW_RISKY
	printf(ts_failures ? "test_dispatch (diagnostics build): %d FAILED\n"
	                   : "test_dispatch (diagnostics build): ok\n", ts_failures);
#else
	printf(ts_failures ? "test_dispatch: %d FAILED\n" : "test_dispatch: ok\n", ts_failures);
#endif
	return ts_failures ? 1 : 0;
}
