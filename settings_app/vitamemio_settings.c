/*
 * vitamemio_settings.c - Settings-app (NPXS10015) front-end for vita-memio.
 *
 * HENkaku's 3.60 Settings recipe: hook sceKernelLoadStartModule in SceSettings to wait
 * for system_settings_core.suprx, then hook
 *   - scePafMiscLoadXmlLayout: splice a nested "vita-memio" setting_list into
 *     the System page (plugin="system_settings_plugin"), just before that
 *     page's own closing </setting_list>;
 *   - the SceRegMgr get/set/info in SceSystemSettingsCore: back the rows with a
 *     private /CONFIG/VITAMEMIO namespace that goes straight to the kernel
 *     module's syscalls (vmGetState / vmSetSetting), so a change applies and
 *     is saved immediately - no registry write;
 *   - scePafToplevelGetText: every title and hint carries an id resolved here.
 *     Each device row's hint is its live status, read from the kernel module
 *     whenever the page draws it.
 *
 * All hooks are import hooks (passthrough), so if anything doesn't match, the
 * page simply doesn't appear; nothing crashes.
 */
#include <psp2/kernel/modulemgr.h>
#include <taihen.h>

#include "../include/vitamemio.h"
#include "../include/vitamemio_kernel_api.h"
#include "vitamemio_i18n.h"
#include "settings_ui.h"
#include "vmlog.h"

/* ---- tiny local string helpers (no paf libc dependency) ---------------- */
static int s_len(const char *s){ int n=0; while(s&&s[n]) n++; return n; }
static int s_eqn(const char *a, const char *b, int n){
	for(int i=0;i<n;i++){ if(a[i]!=b[i]) return 0; if(!a[i]) return 1; } return 1; }
static int s_pfx(const char *s, const char *p){
	for(int i=0;p[i];i++){ if(s[i]!=p[i]) return 0; }
	return 1; }
static const char *mem_find(const char *h, int hn, const char *n, int nn){
	for(int i=0;i+nn<=hn;i++){ int j=0; while(j<nn&&h[i+j]==n[j]) j++; if(j==nn) return h+i; }
	return 0; }
static void s_cpy(char *d, const char *s, int n){ for(int i=0;i<n;i++) d[i]=s[i]; }

/* ---- the vita-memio page --------------------------------------------- */

/* Registry key names under /CONFIG/VITAMEMIO, in enum vm_key order. */
static const char *const g_keys[VM_KEY_COUNT] = {
	"sd2vita", "internal", "verify", "hud", "hud_pos", "devmode", "memcard",
};

#define ROW_LIST(k) \
	"<list id=\"vm_" k "\" title=\"vm_t_" k "\" icon=\"tex_spanner\" " \
	"hint=\"vm_h_" k "\" key=\"/CONFIG/VITAMEMIO/" k "\">"
#define ITEM(id, v) "<list_item id=\"" id "\" title=\"" id "\" value=\"" v "\"/>"

#define ROW_TOGGLE(k) \
	"<toggle_switch id=\"vm_" k "\" title=\"vm_t_" k "\" icon=\"tex_spanner\" " \
	"hint=\"vm_h_" k "\" key=\"/CONFIG/VITAMEMIO/" k "\"/>"

static const char X_GROUP[] =
	"<setting_list id=\"vitamemio_settings\" title=\"vm_t_title\" icon=\"tex_spanner\">"
	ROW_TOGGLE("sd2vita")
	ROW_TOGGLE("memcard")
	ROW_TOGGLE("internal")
	ROW_LIST("hud")
		ITEM("vm_i_hud0", "0") ITEM("vm_i_hud1", "1") ITEM("vm_i_hud2", "2")
	"</list>"
	ROW_LIST("hud_pos")
		ITEM("vm_i_pos0", "0") ITEM("vm_i_pos1", "1") ITEM("vm_i_pos2", "2") ITEM("vm_i_pos3", "3")
	"</list>"
	ROW_TOGGLE("devmode")
	"</setting_list>";

/* Accept a System page up to this size; leave it alone if it is ever bigger. */
#define VM_PAGE_ORIG_MAX 16384

static char g_pagebuf[VM_PAGE_ORIG_MAX + sizeof(X_GROUP)];

/* ---- text for ids (GetText) -------------------------------------------- */

/* The Settings renderer resolves title= and hint= through
 * scePafToplevelGetText and wants UTF-16; literal text in the XML shows blank.
 * A small ring of buffers, so the visible strings never share one. */
typedef unsigned short vm_u16;

static vm_u16 g_txt[6][576];
static int    g_txt_next = 0;

static vm_u16 *next_buf(void)
{
	vm_u16 *o = g_txt[g_txt_next];
	g_txt_next = (g_txt_next + 1) % 6;
	return o;
}

static const vm_u16 *fixed_text(int i18n_id)
{
	vm_u16 *o = next_buf();
	vm_i18n_to_u16(vm_i18n(i18n_id), o, (int)(sizeof(g_txt[0]) / sizeof(g_txt[0][0])));
	return o;
}

/* Small UTF-8 builder for the live status line. */
typedef struct { char s[1024]; int n; } Line;

static void l_put(Line *l, const char *t)
{
	while (*t && l->n < (int)sizeof(l->s) - 1) l->s[l->n++] = *t++;
	l->s[l->n] = 0;
}

/* A storage row's help text, plus why it isn't working when it isn't.
 * Speeds and counters are the HUD's and the command port's job. */
static const vm_u16 *device_hint(int dev, int help_id)
{
	VmState st;
	Line l = { {0}, 0 };
	int id = -1;

	if (vmGetState(&st) != VM_OK || st.magic != VM_STATE_MAGIC)
		id = VM_I18N_ST_NO_KERNEL;
	else if (st.disabled == VM_DIS_FIRMWARE)
		id = VM_I18N_ST_FIRMWARE;
	else if (st.disabled == VM_DIS_HOOKS)
		id = VM_I18N_ST_HOOKS;
	else if (st.disabled == VM_DIS_SAFE)
		id = VM_I18N_ST_SAFE;
	else if (st.dev[dev].status == VM_ST_FALLBACK)
		id = VM_I18N_ST_FALLBACK;
	else if (st.dev[dev].status == VM_ST_NO_BUFFER)
		id = VM_I18N_ST_NO_BUFFER;
	else if (dev == VM_DEV_MEMCARD && st.memcard_slow == 1)
		id = VM_I18N_ST_SLOW;

	l_put(&l, vm_i18n(help_id));
	if (id >= 0) {
		l_put(&l, " ");
		l_put(&l, vm_i18n(id));
	}
	vm_u16 *o = next_buf();
	vm_i18n_to_u16(l.s, o, (int)(sizeof(g_txt[0]) / sizeof(g_txt[0][0])));
	return o;
}

/* Row title / help ids per setting key; -1 = not on the page (verify is a
 * diagnostic, set over the command port only). */
static const signed char k_row[VM_KEY_COUNT] = {
	[VM_KEY_SD2VITA] = VM_I18N_ROW_SD2VITA, [VM_KEY_INTERNAL] = VM_I18N_ROW_INTERNAL,
	[VM_KEY_VERIFY] = -1, [VM_KEY_HUD] = VM_I18N_ROW_HUD,
	[VM_KEY_HUD_POS] = VM_I18N_ROW_HUD_POS, [VM_KEY_DEVMODE] = VM_I18N_ROW_DEVMODE,
	[VM_KEY_MEMCARD] = VM_I18N_ROW_MEMCARD,
};
static const signed char k_hint[VM_KEY_COUNT] = {
	[VM_KEY_SD2VITA] = VM_I18N_HINT_SD2VITA, [VM_KEY_INTERNAL] = VM_I18N_HINT_INTERNAL,
	[VM_KEY_VERIFY] = -1, [VM_KEY_HUD] = VM_I18N_HINT_HUD,
	[VM_KEY_HUD_POS] = VM_I18N_HINT_HUD_POS, [VM_KEY_DEVMODE] = VM_I18N_HINT_DEVMODE,
	[VM_KEY_MEMCARD] = VM_I18N_HINT_MEMCARD,
};

static int key_index(const char *name)
{
	for (int k = 0; k < VM_KEY_COUNT; k++)
		if (s_eqn(name, g_keys[k], s_len(g_keys[k]) + 1))
			return k;
	return -1;
}

/* Resolve one of our ids to UTF-16, or NULL if it isn't ours. */
static const vm_u16 *vm_text(const char *id)
{
	int k;
	if (!s_pfx(id, "vm_"))
		return 0;
	if (s_eqn(id, "vm_t_title", 11))
		return fixed_text(VM_I18N_TITLE);
	if (s_pfx(id, "vm_t_") && (k = key_index(id + 5)) >= 0 && k_row[k] >= 0)
		return fixed_text(k_row[k]);
	if (s_pfx(id, "vm_h_") && (k = key_index(id + 5)) >= 0 && k_hint[k] >= 0) {
		if (k == VM_KEY_SD2VITA)
			return device_hint(VM_DEV_SD2VITA, k_hint[k]);
		if (k == VM_KEY_INTERNAL)
			return device_hint(VM_DEV_INTERNAL, k_hint[k]);
		if (k == VM_KEY_MEMCARD)
			return device_hint(VM_DEV_MEMCARD, k_hint[k]);
		return fixed_text(k_hint[k]);
	}
	if (s_pfx(id, "vm_i_hud") && id[8] >= '0' && id[8] <= '2' && !id[9])
		return fixed_text(VM_I18N_ITEM_HUD_OFF + (id[8] - '0'));
	if (s_pfx(id, "vm_i_pos") && id[8] >= '0' && id[8] <= '3' && !id[9])
		return fixed_text(VM_I18N_ITEM_POS_TL + (id[8] - '0'));
	return 0;
}

/* ---- hooks ------------------------------------------------------------- */
static SceUID g_h_load=-1, g_h_xml=-1, g_h_get=-1, g_h_set=-1, g_h_info=-1, g_h_text=-1;
static tai_hook_ref_t r_load, r_xml, r_get, r_set, r_info, r_text;
static int g_installed = 0;   /* install the UI hooks once */

/* Exact category match (the NUL is compared too). */
static int is_ts(const char *cat){ return cat && s_eqn(cat, "/CONFIG/VITAMEMIO", 18); }

static const char CLOSE_LIST[] = "</setting_list>";

/* The </setting_list> closing the list that opens with `open`, found by
 * counting nesting. NULL if not found. */
static const char *list_close(const char *s, int size, const char *open, int open_n)
{
	static const char OPEN[] = "<setting_list";
	const char *end = s + size;
	const char *p = mem_find(s, size, open, open_n);
	int depth = 0;

	if (!p)
		return 0;
	while (p < end) {
		const char *o = mem_find(p, (int)(end - p), OPEN, (int)sizeof(OPEN) - 1);
		const char *cl = mem_find(p, (int)(end - p), CLOSE_LIST, (int)sizeof(CLOSE_LIST) - 1);
		if (!cl)
			return 0;
		if (o && o < cl) {
			depth++;
			p = o + sizeof(OPEN) - 1;
		} else {
			if (--depth == 0)
				return cl;
			p = cl + sizeof(CLOSE_LIST) - 1;
		}
	}
	return 0;
}

/* Where to splice in the System layout: second on the page, straight after
 * System Information (id_about_system). If that isn't found inside the
 * id_system list, last on the page instead. The layout also holds other
 * top-level lists (e.g. the error-details page) after id_system, so the
 * file's last </setting_list> is never used. NULL if id_system isn't found. */
static const char *system_insert_at(const char *s, int size)
{
	static const char SYS[] = "<setting_list id=\"id_system\"";
	static const char ABOUT[] = "<setting_list id=\"id_about_system\"";
	const char *sys_end = list_close(s, size, SYS, (int)sizeof(SYS) - 1);
	const char *about_end = list_close(s, size, ABOUT, (int)sizeof(ABOUT) - 1);

	if (!sys_end)
		return 0;
	if (about_end && about_end < sys_end)
		return about_end + sizeof(CLOSE_LIST) - 1;
	return sys_end;
}

/* scePafMiscLoadXmlLayout(out, buf, size, a4): splice our group into the
 * System page (Settings -> System). */
static int xml_hook(void *out, void *buf, int size, void *a4)
{
	static const char PLUG[] = "plugin=\"console_info_plugin\"";
	const char *s = (const char *)buf;

	if (s && size > 0 && mem_find(s, size, PLUG, (int)sizeof(PLUG) - 1)) {
		if (size > VM_PAGE_ORIG_MAX) {
			VM_LOGI("[SET] system page too big (%d > %d) - not touched",
			        size, VM_PAGE_ORIG_MAX);
			return TAI_CONTINUE(int, r_xml, out, buf, size, a4);
		}
		const char *ins = system_insert_at(s, size);
		int gl = (int)sizeof(X_GROUP) - 1;
		if (!ins)
			VM_LOGI("[SET] system page (size=%d): insert point not found - page not added",
			        size);
		if (ins) {
			int head = (int)(ins - s);
			s_cpy(g_pagebuf, s, head);
			s_cpy(g_pagebuf + head, X_GROUP, gl);
			s_cpy(g_pagebuf + head + gl, ins, size - head);
			buf = g_pagebuf; size = size + gl;
		}
	}
	return TAI_CONTINUE(int, r_xml, out, buf, size, a4);
}

/* RegMgr GET/SET/INFO for /CONFIG/VITAMEMIO -> the kernel module. */
static int get_hook(const char *cat, const char *name, int *val)
{
	if (is_ts(cat) && val) {
		VmState st;
		int k = name ? key_index(name) : -1;
		if (k >= 0 && vmGetState(&st) == VM_OK && st.magic == VM_STATE_MAGIC)
			*val = *vm_setting_field(&st.settings, k);
		else
			*val = 0;
		return 0;
	}
	return TAI_CONTINUE(int, r_get, cat, name, val);
}

static int set_hook(const char *cat, const char *name, int v)
{
	if (is_ts(cat)) {
		int k = name ? key_index(name) : -1;
		int r = k >= 0 ? vmSetSetting(k, v) : VM_ERR_ARG;
		if (r != VM_OK)
			VM_LOGI("[SET] %s = %d rejected (%d)", name ? name : "?", v, r);
		return 0;
	}
	return TAI_CONTINUE(int, r_set, cat, name, v);
}

static int info_hook(const char *cat, void *info, int unk)
{
	if (is_ts(cat)) { if (info) *(int *)((char *)info + 8) = 0x00040000; /* int */ return 0; }
	return TAI_CONTINUE(int, r_info, cat, info, unk);
}

/* scePafToplevelGetText(arg, char **msg) -> text for a label/hint id. */
static void *text_hook(void *arg, char **msg)
{
	if (msg && *msg) {
		const vm_u16 *t = vm_text(*msg);
		if (t) return (void *)t;
	}
	return TAI_CONTINUE(void *, r_text, arg, msg);
}

/* NID 0x2DCC4AFA as SceSettings imports it takes SIX args incl. int *status
 * (as in HENkaku's user.c). Wrong arity corrupts the shared hook chain. */
static SceUID load_hook(const char *path, SceSize args, void *argp,
                        int flags, void *option, int *status)
{
	SceUID ret = TAI_CONTINUE(SceUID, r_load, path, args, argp, flags, option, status);
	int match = (path && mem_find(path, s_len(path), "system_settings_core", 20)) ? 1 : 0;
	if (match && !g_installed) {
		g_installed = 1;
		g_h_xml  = taiHookFunctionImport(&r_xml, "SceSettings",
		              0x3D643CE8, 0x19FE55A8, xml_hook);              /* ScePafMisc */
		g_h_get  = taiHookFunctionImport(&r_get, "SceSystemSettingsCore",
		              0xC436F916, 0x16DDF3DC, get_hook);
		g_h_set  = taiHookFunctionImport(&r_set, "SceSystemSettingsCore",
		              0xC436F916, 0xD72EA399, set_hook);
		g_h_info = taiHookFunctionImport(&r_info,"SceSystemSettingsCore",
		              0xC436F916, 0x58421DD1, info_hook);
		g_h_text = taiHookFunctionImport(&r_text,"SceSystemSettingsCore",
		              0x4D9A9DD0, 0x19CEFDA7, text_hook);   /* scePafToplevelGetText */
		if (g_h_xml < 0 || g_h_get < 0 || g_h_set < 0 || g_h_info < 0 || g_h_text < 0)
			VM_LOGI("[SET] UI hooks not all installed: xml=0x%08x get=0x%08x set=0x%08x "
		        "info=0x%08x text=0x%08x", (unsigned)g_h_xml, (unsigned)g_h_get,
		        (unsigned)g_h_set, (unsigned)g_h_info, (unsigned)g_h_text);
	}
	return ret;
}

void vm_settings_hooks_install(void)
{
	g_h_load = taiHookFunctionImport(&r_load, "SceSettings",
	              0xCAE9ACE6, 0x2DCC4AFA, load_hook);   /* sceKernelLoadStartModule */
	if (g_h_load < 0)
		VM_LOGI("[SET] LoadStartModule import hook failed (0x%08x)", (unsigned)g_h_load);
}

void vm_settings_hooks_release(void)
{
	if (g_h_text>=0) { taiHookRelease(g_h_text, r_text); g_h_text=-1; }
	if (g_h_info>=0) { taiHookRelease(g_h_info, r_info); g_h_info=-1; }
	if (g_h_set>=0)  { taiHookRelease(g_h_set,  r_set);  g_h_set=-1;  }
	if (g_h_get>=0)  { taiHookRelease(g_h_get,  r_get);  g_h_get=-1;  }
	if (g_h_xml>=0)  { taiHookRelease(g_h_xml,  r_xml);  g_h_xml=-1;  }
	if (g_h_load>=0) { taiHookRelease(g_h_load, r_load); g_h_load=-1; }
}
