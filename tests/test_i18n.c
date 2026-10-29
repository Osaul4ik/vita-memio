/*
 * test_i18n.c - the Settings page text: the UTF-8 -> UTF-16 conversion, and a
 * check over the whole generated translation table (valid UTF-8, English present,
 * every string fits the page's buffers). Run it after editing tools/gen_i18n.py.
 */
#include "../settings_app/storageturbo_i18n.c"

#include <stdlib.h>
#include <string.h>

#include "ts_test.h"

/* the registry / app-util calls the language lookup makes */
static int fake_lang = 1;
int sceRegMgrGetKeyInt(const char *c, const char *n, int *v) { (void)c; (void)n; *v = fake_lang; return 0; }
int sceAppUtilSystemParamGetInt(int id, int *v) { (void)id; *v = 1; return 0; }

/* sizeof the Settings page's text buffers (storageturbo_settings.c: g_txt[..][576]) */
#define PAGE_UNITS 576

static unsigned short *to16(const char *s, int cap, int *n)
{
	unsigned short *o = malloc(cap * sizeof(*o));    /* exact size for ASan */

	*n = ts_i18n_to_u16(s, o, cap);
	return o;
}

/* a heap copy of exactly strlen+1 bytes, so reading past the NUL is caught */
static char *dup_exact(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	memcpy(d, s, n);
	return d;
}

static void test_conversion(void)
{
	int n;
	unsigned short *o;
	char *s;

	s = dup_exact("abc");
	o = to16(s, 16, &n);
	CHECK(n == 3 && o[0] == 'a' && o[2] == 'c' && o[3] == 0);
	free(o); free(s);

	s = dup_exact("\xC3\xA9");                       /* U+00E9 */
	o = to16(s, 16, &n);
	CHECK(n == 1 && o[0] == 0xE9);
	free(o); free(s);

	s = dup_exact("\xE6\x97\xA5\xE6\x9C\xAC");       /* U+65E5 U+672C */
	o = to16(s, 16, &n);
	CHECK(n == 2 && o[0] == 0x65E5 && o[1] == 0x672C);
	free(o); free(s);

	s = dup_exact("a\xF0\x9F\x98\x80z");             /* outside the BMP: dropped */
	o = to16(s, 16, &n);
	CHECK(n == 2 && o[0] == 'a' && o[1] == 'z');
	free(o); free(s);

	/* bad input must neither crash nor read past the terminator */
	const char *bad[] = { "\x80", "\xC3", "\xC3(", "\xE6\x97", "\xE6\x97(", "\xF0\x9F\x98",
	                      "\xF0", "\xFF\xFE", "a\xE6", "\xC0\x80" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		s = dup_exact(bad[i]);
		o = to16(s, 16, &n);
		CHECK(n >= 0 && n < 16 && o[n] == 0);
		free(o); free(s);
	}
}

static void test_conversion_limits(void)
{
	int n;
	unsigned short *o;

	o = to16("abcdef", 4, &n);                       /* room for 3 + NUL */
	CHECK(n == 3 && o[3] == 0);
	free(o);

	o = to16("abc", 1, &n);                          /* only the NUL fits */
	CHECK(n == 0 && o[0] == 0);
	free(o);

	CHECK(ts_i18n_to_u16("abc", NULL, 8) == 0);
	CHECK(ts_i18n_to_u16("abc", (unsigned short[1]){ 7 }, 0) == 0);
	unsigned short z[2] = { 9, 9 };
	CHECK(ts_i18n_to_u16(NULL, z, 2) == 0 && z[0] == 0);
}

/* strict UTF-8: no overlongs, surrogates or code points past U+10FFFF */
static int valid_utf8(const char *str, int *units)
{
	const unsigned char *s = (const unsigned char *)str;

	*units = 0;
	while (*s) {
		unsigned c = *s, cp;
		int extra;

		if (c < 0x80) { cp = c; extra = 0; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
		else return 0;
		for (int i = 1; i <= extra; i++) {
			if ((s[i] & 0xC0) != 0x80)
				return 0;
			cp = (cp << 6) | (s[i] & 0x3F);
		}
		if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
		    (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
			return 0;
		*units += cp >= 0x10000 ? 2 : 1;
		s += extra + 1;
	}
	return 1;
}

static void test_table(void)
{
	for (int id = 0; id < TS_I18N_COUNT; id++) {
		const char *en = g_tbl[id][TS_LANG_ENGLISH_US];

		CHECK(en != NULL && en[0]);
		for (int lang = 0; lang < TS_LANG_COUNT; lang++) {
			const char *t = g_tbl[id][lang];
			int units;

			if (!t)
				continue;                    /* falls back to English */
			if (!valid_utf8(t, &units) || units >= PAGE_UNITS) {
				printf("  FAIL id %d lang %d: bad UTF-8 or %d units\n", id, lang, units);
				ts_failures++;
			}
			CHECK(strcmp(ts_i18n_for(id, lang), t) == 0);
		}
	}
	CHECK(ts_i18n_for(-1, 1)[0] == 0 && ts_i18n_for(TS_I18N_COUNT, 1)[0] == 0);
	CHECK(strcmp(ts_i18n_for(TS_I18N_TITLE, 999), ts_i18n_for(TS_I18N_TITLE, TS_LANG_ENGLISH_US)) == 0);
}

/* The device rows show "<help> <why it is not working>" in one buffer. */
static void test_device_hint_fits(void)
{
	static const int hints[] = { TS_I18N_HINT_SD2VITA, TS_I18N_HINT_INTERNAL, TS_I18N_HINT_MEMCARD };
	static const int why[] = { TS_I18N_ST_NO_KERNEL, TS_I18N_ST_FIRMWARE, TS_I18N_ST_HOOKS,
	                           TS_I18N_ST_SAFE, TS_I18N_ST_FALLBACK, TS_I18N_ST_NO_BUFFER,
	                           TS_I18N_ST_SLOW };

	for (unsigned h = 0; h < sizeof(hints) / sizeof(hints[0]); h++)
		for (unsigned w = 0; w < sizeof(why) / sizeof(why[0]); w++)
			for (int lang = 0; lang < TS_LANG_COUNT; lang++) {
				int a, b;

				valid_utf8(ts_i18n_for(hints[h], lang), &a);
				valid_utf8(ts_i18n_for(why[w], lang), &b);
				if (a + 1 + b >= PAGE_UNITS) {
					printf("  FAIL hint %u + status %u lang %d: %d units, page holds %d\n",
					       h, w, lang, a + 1 + b, PAGE_UNITS - 1);
					ts_failures++;
				}
			}
}

static void test_language_lookup(void)
{
	fake_lang = 0;  CHECK(ts_i18n_lang() == 0);
	fake_lang = 19; CHECK(ts_i18n_lang() == 19);
	fake_lang = 99; CHECK(ts_i18n_lang() == 1);      /* out of range: via app util -> 1 */
	fake_lang = -3; CHECK(ts_i18n_lang() == 1);
	fake_lang = 1;
}

int main(void)
{
	RUN(test_conversion);
	RUN(test_conversion_limits);
	RUN(test_table);
	RUN(test_device_hint_fits);
	RUN(test_language_lookup);

	printf(ts_failures ? "test_i18n: %d FAILED\n" : "test_i18n: ok\n", ts_failures);
	return ts_failures ? 1 : 0;
}
