/*
 * main.c - the Turbo Storage tester app (a normal Vita application, built into
 * storageturbo_tester.vpk together with the plugin).
 *
 * It ONLY tests. It never switches the plugin or changes any of its settings:
 * the user turns the plugin on or off for a device in Settings > Turbo Storage,
 * then runs the test here. A test runs the same storage tests several times in
 * the plugin state found at the start (bench.c), checks every byte, appends the
 * result together with that plugin state to ur0:data/turbostorage/results.txt,
 * shows it, and the file can be cleared. Run it once with the plugin OFF and once
 * ON to compare.
 *
 * The kernel module is not required: the import of its SceStorageTurbo library is
 * weak, so without it the app still starts, the plugin state shows as unknown and
 * the test still measures the storage (tsGetState's result is always checked).
 */
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>

#include "../include/storageturbo_kernel_api.h"
#include "bench.h"
#include "report.h"
#include "results.h"
#include "vita/ui.h"
#include "vita/vita_env.h"

/* The newlib heap: back buffer (2 MiB), the test's 1 MiB buffer, report text. */
int _newlib_heap_size_user = 16 * 1024 * 1024;

#define TITLE "Turbo Storage tester"

_Static_assert(REPORT_LINE_MAX == RESULTS_LINE_MAX, "the viewer shows both kinds of lines");

static const char *const k_roots[] = {
	"ux0:TurboTest/", "uma0:TurboTest/", "imc0:TurboTest/", "ur0:TurboTest/", "grw0:TurboTest/",
};
#define N_ROOTS ((int)(sizeof(k_roots) / sizeof(k_roots[0])))
/* The folder that usually belongs to each device (enum ts_dev order). */
static const int k_root_default[TS_DEV_COUNT] = { 0, 3, 0 };

static const BenchProfile *const k_profiles[] = { &bench_profile_quick, &bench_profile_standard };
#define N_PROFILES 2

static int g_dev = TS_DEV_SD2VITA;
static int g_root = 0;
static int g_profile = 0;

static BenchResult g_res;
static char g_report[32768];
static char g_screen[REPORT_LINES_MAX][REPORT_LINE_MAX];
static char g_view[RESULTS_LINES_MAX][RESULTS_LINE_MAX];

/* ---- small screens ----------------------------------------------------------- */

static void header(const char *title)
{
	ui_clear();
	ui_row_bg(0, UI_SELBG);
	ui_text(1, 0, UI_ACCENT, TITLE);
	ui_textf(UI_COLS - 1 - (int)strlen(title), 0, UI_FG, "%s", title);
}

static void footer(const char *text)
{
	ui_text(1, UI_ROWS - 1, UI_DIM, text);
}

static uint32_t line_color(const char *l)
{
	if (strstr(l, "FAIL") || strstr(l, "FAILED") || strstr(l, "NOT as expected"))
		return UI_BAD;
	if (strstr(l, "PASS"))
		return UI_GOOD;
	if (!strncmp(l, "csv", 3))
		return UI_DIM;
	if (!strncmp(l, "====", 4) || !strncmp(l, "----", 4))
		return UI_ACCENT;
	return UI_FG;
}

/* Scrollable text. `from_end` starts at the last page. */
static void viewer(const char *title, char (*lines)[RESULTS_LINE_MAX], int n, int from_end,
                   const char *foot)
{
	const int rows = UI_ROWS - 3;
	int top = from_end && n > rows ? n - rows : 0;

	for (;;) {
		unsigned k;

		header(title);
		for (int i = 0; i < rows && top + i < n; i++)
			ui_text(1, 2 + i, line_color(lines[top + i]), lines[top + i]);
		ui_textf(1, UI_ROWS - 1, UI_DIM, "lines %d-%d of %d   up/down, L/R page   %s",
		         n ? top + 1 : 0, top + rows < n ? top + rows : n, n, foot);
		ui_present();
		k = ui_poll();
		if (k & UI_BACK)
			return;
		if ((k & UI_UP) && top > 0) top--;
		if ((k & UI_DOWN) && top + rows < n) top++;
		if (k & UI_PGUP) top = top > rows ? top - rows : 0;
		if (k & UI_PGDN) top = top + 2 * rows < n ? top + rows : (n > rows ? n - rows : 0);
	}
}

static void message(uint32_t color, const char *l1, const char *l2, const char *l3)
{
	for (;;) {
		header("");
		ui_text(2, 5, color, l1);
		if (l2) ui_text(2, 7, UI_FG, l2);
		if (l3) ui_text(2, 8, UI_FG, l3);
		footer("press cross or circle");
		ui_present();
		if (ui_poll() & (UI_OK | UI_BACK))
			return;
	}
}

static int confirm(const char *title, const char *l1, const char *l2, const char *l3)
{
	for (;;) {
		unsigned k;

		header(title);
		ui_text(2, 5, UI_WARN, l1);
		if (l2) ui_text(2, 7, UI_FG, l2);
		if (l3) ui_text(2, 8, UI_FG, l3);
		footer("cross: yes    circle: no");
		ui_present();
		k = ui_poll();
		if (k & UI_OK) return 1;
		if (k & UI_BACK) return 0;
	}
}

/* ---- running a test -------------------------------------------------------------- */

static int poll_cancel(void)
{
	static int latched;

	if (ui_back_down())
		latched = 1;
	return latched;
}

static const BenchConfig *g_cfg;
static uint64_t g_last_draw;
static int g_last_phase = -2, g_last_metric = -2, g_last_run = -2;

static void on_progress(const BenchProgress *p)
{
	static const char *const phase[] = {
		"preparing", "measuring", "reading the whole file back", "cleaning up",
	};
	uint64_t now = sceKernelGetProcessTimeWide();
	uint32_t overall;
	int change = p->phase != g_last_phase || p->metric != g_last_metric ||
	             p->run_index != g_last_run;

	sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   /* no screen-off or sleep mid-test */
	if (!change && p->permille < 1000 && now - g_last_draw < 100000)
		return;
	g_last_draw = now;
	g_last_phase = p->phase;
	g_last_metric = p->metric;
	g_last_run = p->run_index;

	/* Per run: BM_COUNT metrics, then the read-back of the whole file. */
	{
		uint64_t step = p->phase == BP_FINAL ? BM_COUNT :
		                p->phase == BP_CLEANUP ? BM_COUNT + 1 :
		                (uint64_t)(p->metric > 0 ? p->metric : 0);

		overall = p->run_total > 0 ?
		          (uint32_t)(((uint64_t)p->run_index * (BM_COUNT + 1) + step) * 1000 /
		                     ((uint64_t)p->run_total * (BM_COUNT + 1))) : 0;
		if (overall > 1000)
			overall = 1000;
	}

	header("running");
	ui_textf(2, 3, UI_FG, "device: %s    folder: %s", bench_device_name(g_cfg->device), g_cfg->dir);
	ui_textf(2, 4, UI_FG, "profile: %s (%u MiB file, %u runs)", g_cfg->profile->name,
	         (unsigned)g_cfg->profile->file_mib, (unsigned)g_cfg->profile->rounds);
	ui_textf(2, 7, UI_ACCENT, "run %d of %d    plugin %s", p->run_index + 1, p->run_total,
	         bench_plugin_mode_name(p->plugin_mode));
	ui_textf(2, 9, UI_FG, "%s", phase[p->phase >= 0 && p->phase <= BP_CLEANUP ? p->phase : 0]);
	if (p->phase == BP_METRIC && p->metric >= 0)
		ui_textf(2, 10, UI_FG, "%s", bench_metric_name[p->metric]);
	ui_bar(2, 12, 76, p->permille, UI_ACCENT);
	ui_text(2, 14, UI_DIM, "everything:");
	ui_bar(2, 15, 76, overall, UI_GOOD);
	ui_text(2, 18, UI_DIM, "Hold circle to stop. The test file is removed, also when you stop.");
	ui_text(2, 19, UI_DIM, "The plugin settings are not touched.");
	ui_present();
}

static void timestamp(char *out, size_t cap)
{
	SceDateTime t;

	memset(&t, 0, sizeof(t));
	sceRtcGetCurrentClockLocalTime(&t);
	snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day, t.hour,
	         t.minute, t.second);
}

static void run_test(void)
{
	static BenchConfig cfg;
	VitaEnv venv;
	ReportMeta meta;
	char stamp[48], foot[96];
	int rc, n, saved;

	if (g_dev == TS_DEV_INTERNAL &&
	    !confirm("internal storage", "Internal storage is the console's own storage.",
	             "The test writes one file in the folder and removes it, but the plugin",
	             "is experimental there. Run the test?"))
		return;

	cfg.device = g_dev;
	cfg.profile = k_profiles[g_profile];
	cfg.dir = k_roots[g_root];
	g_cfg = &cfg;
	g_last_phase = g_last_metric = g_last_run = -2;
	vita_env_init(&venv, g_dev, on_progress, poll_cancel);

	memset(&g_res, 0, sizeof(g_res));
	rc = bench_run(&venv.env, &cfg, &g_res);
	(void)rc;

	/* Wait until the back button is released, so it does not close the next screen. */
	while (ui_back_down())
		ui_present();
	ui_poll();

	timestamp(stamp, sizeof(stamp));
	meta.timestamp = stamp;
	meta.plugin_version = TS_VERSION_STR;
	meta.folder = cfg.dir;
	report_format(&g_res, &meta, g_report, sizeof(g_report));
	vita_mkdirs(TS_DATA_DIR "/");
	saved = results_append(RESULTS_PATH, g_report) == 0;

	n = report_screen_lines(&g_res, g_screen, REPORT_LINES_MAX);
	snprintf(foot, sizeof(foot), saved ? "saved to results.txt" : "COULD NOT SAVE results.txt");
	viewer(g_res.failed ? "test failed" : g_res.cancelled ? "test stopped" : "result", g_screen, n, 0, foot);
}

/* ---- the menu ---------------------------------------------------------------------- */

static void show_results(void)
{
	int n = results_load(RESULTS_PATH, g_view, RESULTS_LINES_MAX);

	if (n <= 0) {
		message(UI_FG, "No results saved yet.", NULL, NULL);
		return;
	}
	viewer("saved results", g_view, n, 1, "");
}

static void clear_results(void)
{
	long sz = results_size(RESULTS_PATH);
	char l2[80];

	if (sz < 0) {
		message(UI_FG, "There are no saved results.", NULL, NULL);
		return;
	}
	snprintf(l2, sizeof(l2), "results.txt is %ld KiB. This cannot be undone.", (sz + 1023) / 1024);
	if (!confirm("clear results", "Delete all saved results?", l2, NULL))
		return;
	if (results_clear(RESULTS_PATH) == 0)
		message(UI_GOOD, "Results deleted.", NULL, NULL);
	else
		message(UI_BAD, "Could not delete results.txt.", NULL, NULL);
}

/* What the plugin does for the selected device right now (read only). */
static void plugin_line(char *out, size_t cap)
{
	TsState st;
	BenchPluginState ps;
	const char *dev = bench_device_name(g_dev);

	memset(&st, 0, sizeof(st));
	if (tsGetState(&st) != TS_OK || st.magic != TS_STATE_MAGIC) {
		snprintf(out, cap, "plugin: the kernel module does not answer (state unknown)");
		return;
	}
	memset(&ps, 0, sizeof(ps));
	ps.available = 1;
	ps.disabled = st.disabled;
	ps.status = st.dev[g_dev].status;
	switch (bench_plugin_mode(&ps)) {
	case BPM_ON:
		snprintf(out, cap, "plugin for %s: ON%s", dev, st.settings.verify ? " (verify mode is on)" : "");
		break;
	case BPM_OFF:
		snprintf(out, cap, "plugin for %s: OFF", dev);
		break;
	case BPM_INACTIVE:
		snprintf(out, cap, "plugin: inactive (safe mode, unknown firmware or no hooks)");
		break;
	default:
		snprintf(out, cap, "plugin for %s: fell back to stock or has no buffer", dev);
		break;
	}
}

enum { M_DEVICE, M_FOLDER, M_PROFILE, M_RUN, M_VIEW, M_CLEAR, M_EXIT, M_COUNT };

static void menu(void)
{
	int sel = M_RUN;

	for (;;) {
		char pl[UI_COLS + 1], item[M_COUNT][UI_COLS + 1];
		long sz = results_size(RESULTS_PATH);
		const BenchProfile *pr = k_profiles[g_profile];
		unsigned k;

		plugin_line(pl, sizeof(pl));
		snprintf(item[M_DEVICE], sizeof(item[0]), "Device         < %s >", bench_device_name(g_dev));
		snprintf(item[M_FOLDER], sizeof(item[0]), "Test folder    < %s >", k_roots[g_root]);
		snprintf(item[M_PROFILE], sizeof(item[0]), "Profile        < %s: %u MiB file, %u runs >",
		         pr->name, (unsigned)pr->file_mib, (unsigned)pr->rounds);
		snprintf(item[M_RUN], sizeof(item[0]), "Run test (plugin as set in Settings)");
		if (sz < 0)
			snprintf(item[M_VIEW], sizeof(item[0]), "View saved results (none yet)");
		else
			snprintf(item[M_VIEW], sizeof(item[0]), "View saved results (%ld KiB)", (sz + 1023) / 1024);
		snprintf(item[M_CLEAR], sizeof(item[0]), "Clear saved results");
		snprintf(item[M_EXIT], sizeof(item[0]), "Exit");

		header("v" TESTER_VERSION);
		ui_text(2, 3, UI_DIM, pl);
		for (int i = 0; i < M_COUNT; i++) {
			int row = 6 + i * 2;

			if (i == sel)
				ui_row_bg(row, UI_SELBG);
			ui_text(2, row, i == sel ? UI_FG : UI_DIM, item[i]);
		}
		ui_text(2, 22, UI_DIM, "Pick the device that the folder is on (SD2Vita and the memory card");
		ui_text(2, 23, UI_DIM, "are usually ux0:). If they do not match, the test says so.");
		ui_text(2, 25, UI_DIM, "The test never changes the plugin. Turn it on or off for the device");
		ui_text(2, 26, UI_DIM, "in Settings > Turbo Storage, then run the test again to compare.");
		ui_text(2, 27, UI_DIM, "Every run is appended to ur0:data/turbostorage/results.txt.");
		footer("up/down: choose   left/right: change   cross: select");
		ui_present();

		k = ui_poll();
		if ((k & UI_UP) && sel > 0) sel--;
		if ((k & UI_DOWN) && sel < M_COUNT - 1) sel++;
		if (sel == M_DEVICE && (k & (UI_LEFT | UI_RIGHT))) {
			g_dev = (g_dev + ((k & UI_RIGHT) ? 1 : TS_DEV_COUNT - 1)) % TS_DEV_COUNT;
			g_root = k_root_default[g_dev];
		}
		if (sel == M_FOLDER && (k & (UI_LEFT | UI_RIGHT)))
			g_root = (g_root + ((k & UI_RIGHT) ? 1 : N_ROOTS - 1)) % N_ROOTS;
		if (sel == M_PROFILE && (k & (UI_LEFT | UI_RIGHT)))
			g_profile = (g_profile + 1) % N_PROFILES;
		if (k & UI_OK) {
			switch (sel) {
			case M_RUN:   run_test(); break;
			case M_VIEW:  show_results(); break;
			case M_CLEAR: clear_results(); break;
			case M_EXIT:  return;
			default: break;
			}
		}
	}
}

int main(void)
{
	if (ui_init() != 0)
		return 1;
	menu();
	ui_shutdown();
	sceKernelExitProcess(0);
	return 0;
}
