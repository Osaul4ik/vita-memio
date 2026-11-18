/*
 * hud_text.h - pieces of the HUD's lines, kept free of kernel calls so the
 * host tests can check them (tests/test_overlay.c). Included by hud_main.c.
 */
#ifndef VITAMEMIO_HUD_TEXT_H
#define VITAMEMIO_HUD_TEXT_H

#include <stdint.h>

#include "../include/vitamemio_kernel_api.h"
#include "hud.h"

/* One kind's part of the read ahead line, `k` "C" or "NC":
 *   C:5:2/16/8   as set : now for large reads / 32 KiB reads / tiny reads
 *   NC:never     the app's "never" (its large and 32 KiB reads)
 * The tiny reads' is one for both kinds. */
static inline void hud_streak_kind(char *o, int cap, const char *k, uint32_t set, uint32_t now,
                                   uint32_t mid, uint32_t tiny, int off, int tiny_off)
{
	char t[8], n[8], m[8];

	if (now == VM_RA_NEVER) {
		snprintf(o, (size_t)cap, HUD_C_DIM "%s:never", k);
		return;
	}
	/* off as measured (Adaptive): "-" in its place */
	snprintf(n, sizeof(n), off ? "-" : "%u", (unsigned)(now > 99 ? 99 : now));
	snprintf(m, sizeof(m), off ? "-" : "%u", (unsigned)(mid > 99 ? 99 : mid));
	snprintf(t, sizeof(t), tiny_off ? "-" : "%u", (unsigned)(tiny > 99 ? 99 : tiny));
	snprintf(o, (size_t)cap, HUD_C_DIM "%.3s:" HUD_C_TEXT "%u" HUD_C_DIM ":" HUD_C_GOOD "%.2s/%.2s/%.2s",
	         k, (unsigned)(set > 99 ? 99 : set), n, m, t);
}

#endif /* VITAMEMIO_HUD_TEXT_H */
