/*
 * main.c - the vita-memio tester app (a normal Vita application, built into
 * vitamemio_tester.vpk together with the plugin).
 *
 * It ONLY tests. It never switches the plugin or changes any of its settings:
 * the user turns the plugin on or off for a device in Settings > Devices >
 * vita-memio (verify mode too),
 * then runs the test here. A test runs the same storage tests several times in
 * the plugin state found at the start (bench.c), checks every byte, appends the
 * result together with that plugin state to ur0:data/vita-memio/results.txt,
 * shows it, and the file can be cleared. Run it once with the plugin OFF and once
 * ON to compare.
 *
 * The kernel module is not required: the import of its SceVitaMemio library is
 * weak, so without it the app still starts, the plugin state shows as unknown and
 * the test still measures the storage (vmGetState's result is always checked).
 */
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>

#include "../include/vitamemio_kernel_api.h"
#include "bench.h"
#include "report.h"
#include "results.h"
#include "vita/ui.h"
#include "vita/vita_env.h"

/* The newlib heap: back buffer (2 MiB), the test's 1 MiB buffer, report text. */
int _newlib_heap_size_user = 16 * 1024 * 1024;

_Static_assert(REPORT_LINE_MAX == RESULTS_LINE_MAX, "the viewer shows both kinds of lines");

static const char *const k_roots[] = {
	"ux0:MemioTest/", "uma0:MemioTest/", "imc0:MemioTest/", "ur0:MemioTest/", "grw0:MemioTest/",
};
#define N_ROOTS ((int)(sizeof(k_roots) / sizeof(k_roots[0])))
/* The folder that usually belongs to each device (enum vm_dev order). */
static const int k_root_default[VM_DEV_COUNT] = { 0, 3, 0 };

static const BenchProfile *const k_profiles[] = { &bench_profile_quick, &bench_profile_standard };
#define N_PROFILES 2

static int g_dev = VM_DEV_SD2VITA;
static int g_root = 0;
static int g_profile = 0;

static BenchResult g_res;
static char g_report[32768];
static char g_screen[REPORT_LINES_MAX][REPORT_LINE_MAX];
static char g_view[RESULTS_LINES_MAX][RESULTS_LINE_MAX];
static char g_plugin_build[48];   /* the kernel module's build, for the report */

/* ---- small screens ----------------------------------------------------------- */

#define HINT_BACK "\x02 back"

static uint32_t line_color(const char *l)
{
	if (strstr(l, "FAIL") || strstr(l, "NOT as expected"))
		return UI_BAD;
	if (strstr(l, "PASS"))
		return UI_GOOD;
	if (strstr(l, "WARNING"))
		return UI_WARN;
	if (!strncmp(l, "csv", 3))
		return UI_DIM;
	if (!strncmp(l, "====", 4) || !strncmp(l, "----", 4))
		return UI_EDGE;
	return UI_FG;
}

/* Scrollable text. `from_end` starts at the last page. */
static void viewer(const char *title, char (*lines)[RESULTS_LINE_MAX], int n, int from_end,
                   const char *foot)
{
	const int rows = UI_ROWS - 7;                /* panel rows 3..UI_ROWS-3 */
	int top = from_end && n > rows ? n - rows : 0;

	for (;;) {
		char pos[48], hints[UI_COLS + 1];
		unsigned k;

		snprintf(pos, sizeof(pos), "lines %d-%d of %d", n ? top + 1 : 0,
		         top + rows < n ? top + rows : n, n);
		ui_header(title, pos);
		ui_panel(1, 3, UI_COLS - 2, rows + 2, NULL);
		for (int i = 0; i < rows && top + i < n; i++)
			ui_text(3, 4 + i, line_color(lines[top + i]), lines[top + i]);
		if (n > rows) {                             /* a scroll bar on the right */
			int y0 = UI_TOP + 4 * UI_CH, h = rows * UI_CH;
			int bh = h * rows / n, by = y0 + (h - bh) * top / (n - rows);

			ui_fill((UI_COLS - 2) * UI_CW, y0, 3, h, UI_EDGE);
			ui_fill((UI_COLS - 2) * UI_CW, by, 3, bh < 6 ? 6 : bh, UI_ACCENT);
		}
		snprintf(hints, sizeof(hints), "\x03 scroll   \x05 \x06 page   " HINT_BACK "   %s", foot);
		ui_footer(hints);
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

static void box_lines(const char *title, uint32_t color, const char *l1, const char *l2,
                      const char *l3)
{
	ui_panel(10, 8, UI_COLS - 20, 9, title);
	ui_text(14, 10, color, l1);
	if (l2) ui_text(14, 12, UI_FG, l2);
	if (l3) ui_text(14, 13, UI_FG, l3);
}

static void message(uint32_t color, const char *l1, const char *l2, const char *l3)
{
	for (;;) {
		ui_header("tester", "");
		box_lines(NULL, color, l1, l2, l3);
		ui_footer("\x01 ok");
		ui_present();
		if (ui_poll() & (UI_OK | UI_BACK))
			return;
	}
}

static int confirm(const char *title, const char *l1, const char *l2, const char *l3)
{
	for (;;) {
		unsigned k;

		ui_header("tester", "");
		box_lines(title, UI_WARN, l1, l2, l3);
		ui_footer("\x01 yes   \x02 no");
		ui_present();
		k = ui_poll();
		if (k & UI_OK) return 1;
		if (k & UI_BACK) return 0;
	}
}

/* ---- the plugin, live (read only) ---------------------------------------------------- */

/* What one device is doing, as a short word and its colour. */
static const char *dev_state(const VmState *st, int dev, uint32_t *color)
{
	if (st->disabled != VM_DIS_NONE) { *color = UI_DIM; return "inactive"; }
	switch (st->dev[dev].status) {
	case VM_ST_ACTIVE:    *color = UI_GOOD; return "ON";
	case VM_ST_OFF:       *color = UI_DIM;  return "off";
	case VM_ST_FALLBACK:  *color = UI_BAD;  return "FELL BACK";
	case VM_ST_NO_BUFFER: *color = UI_WARN; return "no buffer";
	default:              *color = UI_DIM;  return "inactive";
	}
}

/* One line about the plugin's verify mode and "small transfers too". */
static void verify_line(const VmState *st, char *out, size_t cap, uint32_t *color)
{
	int sm = vmGetSetting(VM_KEY_SMALL);
	const char *small = sm == 1 ? "ON" : sm == 0 ? "off" : "?";

	if (!st->settings.verify) {
		*color = UI_DIM;
		snprintf(out, cap, "verify: off   small transfers too: %s   (Settings > ... > Advanced)", small);
	} else if (!st->verify_ready) {
		*color = UI_WARN;
		snprintf(out, cap, "verify: ON, no check buffer yet - nothing checked   small transfers: %s", small);
	} else {
		*color = UI_ACCENT;
		snprintf(out, cap, "verify: ON - faster transfers re-read and compared   small transfers: %s", small);
	}
}

/* The PLUGIN panel: one line per device, then verify mode. 7 rows from `row`. */
static void plugin_panel(int row, int hl_dev)
{
	VmState st;
	char vl[UI_COLS + 1];
	uint32_t c;

	ui_panel(1, row, UI_COLS - 2, 7, "PLUGIN");
	memset(&st, 0, sizeof(st));
	if (vmGetState(&st) != VM_OK || st.magic != VM_STATE_MAGIC) {
		ui_text(4, row + 2, UI_WARN, "The kernel module does not answer: the plugin state is unknown.");
		ui_text(4, row + 3, UI_DIM, "Tests still run and measure the storage.");
		return;
	}
	ui_text(4, row + 1, UI_DIM, "device");
	ui_text(16, row + 1, UI_DIM, "state");
	ui_text(28, row + 1, UI_DIM, "faster read");
	ui_text(43, row + 1, UI_DIM, "faster write");
	ui_text(58, row + 1, UI_DIM, "errors");
	ui_text(67, row + 1, UI_DIM, "verified");
	ui_text(79, row + 1, UI_DIM, "mismatches");
	for (int d = 0; d < VM_DEV_COUNT; d++) {
		const VmDevState *ds = &st.dev[d];
		int r = row + 2 + d;
		const char *w = dev_state(&st, d, &c);

		if (d == hl_dev)
			ui_text(2, r, UI_ACCENT, ">");
		ui_text(4, r, d == hl_dev ? UI_FG : UI_DIM, bench_device_name(d));
		ui_text(16, r, c, w);
		ui_textf(28, r, UI_FG, "%u MiB", (unsigned)ds->read_mib);
		ui_textf(43, r, UI_FG, "%u MiB", (unsigned)ds->write_mib);
		ui_textf(58, r, ds->errors ? UI_BAD : UI_FG, "%u", (unsigned)ds->errors);
		ui_textf(67, r, st.settings.verify ? UI_FG : UI_DIM, "%u", (unsigned)ds->verified);
		ui_textf(79, r, ds->mismatches ? UI_BAD : UI_FG, "%u", (unsigned)ds->mismatches);
	}
	verify_line(&st, vl, sizeof(vl), &c);
	ui_text(4, row + 6, c, vl);
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

	ui_header("tester", "running");
	ui_panel(1, 3, UI_COLS - 2, 5, "TEST");
	ui_textf(4, 4, UI_FG, "device  %s", bench_device_name(g_cfg->device));
	ui_textf(40, 4, UI_FG, "folder  %s", g_cfg->dir);
	ui_textf(4, 5, UI_FG, "profile %s (%u MiB file, %u runs)", g_cfg->profile->name,
	         (unsigned)g_cfg->profile->file_mib, (unsigned)g_cfg->profile->rounds);
	ui_textf(4, 6, UI_ACCENT, "run %d of %d", p->run_index + 1, p->run_total);
	ui_textf(40, 6, UI_FG, "plugin  %s", bench_plugin_mode_name(p->plugin_mode));

	ui_panel(1, 9, UI_COLS - 2, 8, "PROGRESS");
	ui_textf(4, 10, UI_FG, "%s", phase[p->phase >= 0 && p->phase <= BP_CLEANUP ? p->phase : 0]);
	if (p->phase == BP_METRIC && p->metric >= 0)
		ui_textf(40, 10, UI_ACCENT, "%s", bench_metric_name[p->metric]);
	ui_bar(4, 12, UI_COLS - 8, p->permille, UI_ACCENT);
	ui_text(4, 14, UI_DIM, "everything");
	ui_bar(4, 15, UI_COLS - 8, overall, UI_GOOD);

	plugin_panel(18, g_cfg->device);
	ui_footer("\x02 hold to stop   the test file is removed also when stopped;"
	          " plugin settings are not touched");
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

	if (g_dev == VM_DEV_INTERNAL &&
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
	/* the kernel module's own build; an older module (0.8.0 and before)
	 * does not have the call, and the weak import leaves the text empty */
	memset(g_plugin_build, 0, sizeof(g_plugin_build));
	if (vmGetBuild(g_plugin_build, (int)sizeof(g_plugin_build)) != VM_OK || !g_plugin_build[0])
		snprintf(g_plugin_build, sizeof(g_plugin_build), "unknown (0.8.0 or older)");
	meta.plugin_version = g_plugin_build;
	meta.folder = cfg.dir;
	report_format(&g_res, &meta, g_report, sizeof(g_report));
	vita_mkdirs(VM_DATA_DIR "/");
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

enum { M_DEVICE, M_FOLDER, M_PROFILE, M_RUN, M_VIEW, M_CLEAR, M_EXIT, M_COUNT };

static void menu(void)
{
	int sel = M_RUN;

	for (;;) {
		char item[M_COUNT][UI_COLS + 1];
		long sz = results_size(RESULTS_PATH);
		const BenchProfile *pr = k_profiles[g_profile];
		unsigned k;

		snprintf(item[M_DEVICE], sizeof(item[0]), "Device          %s", bench_device_name(g_dev));
		snprintf(item[M_FOLDER], sizeof(item[0]), "Test folder     %s", k_roots[g_root]);
		snprintf(item[M_PROFILE], sizeof(item[0]), "Profile         %s: %u MiB file, %u runs",
		         pr->name, (unsigned)pr->file_mib, (unsigned)pr->rounds);
		snprintf(item[M_RUN], sizeof(item[0]), "Run test        plugin as set in Settings");
		if (sz < 0)
			snprintf(item[M_VIEW], sizeof(item[0]), "Saved results   none yet");
		else
			snprintf(item[M_VIEW], sizeof(item[0]), "Saved results   view (%ld KiB)", (sz + 1023) / 1024);
		snprintf(item[M_CLEAR], sizeof(item[0]), "Clear results");
		snprintf(item[M_EXIT], sizeof(item[0]), "Exit");

		ui_header("tester", "v" TESTER_BUILD);
		plugin_panel(3, g_dev);
		ui_panel(1, 11, UI_COLS - 2, 8, "TEST");
		for (int i = 0; i < M_COUNT; i++) {
			int row = 12 + i;
			int choice = i == M_DEVICE || i == M_FOLDER || i == M_PROFILE;

			if (i == sel)
				ui_select(2, row, UI_COLS - 4);
			ui_text(5, row, i == sel ? UI_FG : UI_DIM, item[i]);
			if (choice && i == sel)
				ui_text(UI_COLS - 8, row, UI_ACCENT, "\x04");
		}
		ui_text(3, 21, UI_DIM, "Pick the device the folder is on (SD2Vita and the memory card are usually ux0:).");
		ui_text(3, 22, UI_DIM, "If they do not match, the test says so.");
		ui_text(3, 24, UI_DIM, "The tester never changes the plugin. Switch it in Settings > Devices > vita-memio");
		ui_text(3, 25, UI_DIM, "(verify mode: Advanced), then run the test again to compare.");
		ui_text(3, 26, UI_DIM, "Every run is appended to ur0:data/vita-memio/results.txt.");
		ui_footer("\x03 choose   \x04 change   \x01 select");
		ui_present();

		k = ui_poll();
		if ((k & UI_UP) && sel > 0) sel--;
		if ((k & UI_DOWN) && sel < M_COUNT - 1) sel++;
		if (sel == M_DEVICE && (k & (UI_LEFT | UI_RIGHT))) {
			g_dev = (g_dev + ((k & UI_RIGHT) ? 1 : VM_DEV_COUNT - 1)) % VM_DEV_COUNT;
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
