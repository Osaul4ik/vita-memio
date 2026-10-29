#include <string.h>

#include "restore.h"

static uint32_t sum_of(const RestoreRec *r)
{
	const int32_t *v = (const int32_t *)&r->settings;
	uint32_t h = 0x811C9DC5u ^ r->magic;

	for (int i = 0; i < TS_KEY_COUNT; i++)
		h = (h ^ (uint32_t)v[i]) * 16777619u;
	return h;
}

void restore_pack(RestoreRec *r, const TsSettings *s)
{
	memset(r, 0, sizeof(*r));
	r->magic = RESTORE_MAGIC;
	r->settings = *s;
	r->sum = sum_of(r);
}

int restore_unpack(const RestoreRec *r, TsSettings *out)
{
	TsSettings tmp = r->settings;

	if (r->magic != RESTORE_MAGIC || r->sum != sum_of(r))
		return -1;
	for (int k = 0; k < TS_KEY_COUNT; k++)
		if (!ts_setting_valid(k, *ts_setting_field(&tmp, k)))
			return -1;
	*out = tmp;
	return 0;
}
