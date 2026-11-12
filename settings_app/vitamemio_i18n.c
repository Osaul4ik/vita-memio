/*
 * vitamemio_i18n.c - text for the vita-memio Settings page (English only, see
 * vitamemio_i18n.h; the strings are in tools/gen_i18n.py).
 */
#include "vitamemio_i18n.h"

static const char *const g_tbl[VM_I18N_COUNT] = {
#include "vitamemio_i18n_table.h"
};

const char *vm_i18n(int id)
{
	const char *s;

	if (id < 0 || id >= VM_I18N_COUNT)
		return "";
	s = g_tbl[id];
	return s ? s : "";
}

int vm_i18n_to_u16(const char *utf8, unsigned short *out, int cap)
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
