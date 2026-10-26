/*
 * storageturbo_i18n.c - text for the Turbo Storage Settings page.
 *
 * MACHINE-TRANSLATED CONTENT. Only English (US/UK) is authoritative; the
 * other languages are machine translations so the page is legible in all 20
 * Vita system languages. Corrections from native speakers are welcome: edit
 * tools/gen_i18n.py and regenerate storageturbo_i18n_table.h.
 *
 * Needs SceRegistryMgr_stub_weak and SceAppUtil_stub_weak (both resident in
 * NPXS10015).
 */
#include "storageturbo_i18n.h"

extern int sceRegMgrGetKeyInt(const char *category, const char *name, int *value);
extern int sceAppUtilSystemParamGetInt(int paramId, int *value);

#define TS_SYSTEM_PARAM_ID_LANG 1

static const char *const g_tbl[TS_I18N_COUNT][TS_LANG_COUNT] = {
#include "storageturbo_i18n_table.h"
};

int ts_i18n_lang(void)
{
	int lang = -1;

	/* The registry is the canonical store and has no init gate. */
	if (sceRegMgrGetKeyInt("/CONFIG/SYSTEM", "language", &lang) >= 0 &&
	    lang >= 0 && lang < TS_LANG_COUNT)
		return lang;
	/* Only answers once the host app has called sceAppUtilInit(). */
	lang = -1;
	if (sceAppUtilSystemParamGetInt(TS_SYSTEM_PARAM_ID_LANG, &lang) >= 0 &&
	    lang >= 0 && lang < TS_LANG_COUNT)
		return lang;
	return TS_LANG_ENGLISH_US;
}

const char *ts_i18n_for(int id, int lang)
{
	const char *s;

	if (id < 0 || id >= TS_I18N_COUNT)
		return "";
	if (lang < 0 || lang >= TS_LANG_COUNT)
		lang = TS_LANG_ENGLISH_US;
	s = g_tbl[id][lang];
	if (!s)
		s = g_tbl[id][TS_LANG_ENGLISH_US];
	return s ? s : "";
}

const char *ts_i18n(int id)
{
	return ts_i18n_for(id, ts_i18n_lang());
}

int ts_i18n_to_u16(const char *utf8, unsigned short *out, int cap)
{
	const unsigned char *s = (const unsigned char *)utf8;
	int n = 0;

	if (!out || cap <= 0)
		return 0;
	if (!s) {
		out[0] = 0;
		return 0;
	}
	while (*s && n < cap - 1) {
		unsigned int c = *s, cp;

		if (c < 0x80) {
			cp = c;
			s += 1;
		} else if ((c & 0xe0) == 0xc0) {
			if ((s[1] & 0xc0) != 0x80) { s += 1; continue; }
			cp = ((c & 0x1f) << 6) | (s[1] & 0x3f);
			s += 2;
		} else if ((c & 0xf0) == 0xe0) {
			if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80) { s += 1; continue; }
			cp = ((c & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
			s += 3;
		} else if ((c & 0xf8) == 0xf0) {
			/* Outside the BMP (would need surrogates): drop it. */
			if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
			    (s[3] & 0xc0) != 0x80) { s += 1; continue; }
			s += 4;
			continue;
		} else {
			s += 1;   /* stray continuation byte */
			continue;
		}
		out[n++] = (unsigned short)cp;
	}
	out[n] = 0;
	return n;
}
