/*
 * hud_text.h - pieces of the HUD's lines, kept free of kernel calls so the
 * host tests can check them (tests/test_overlay.c). Included by hud_main.c.
 */
#ifndef VITAMEMIO_HUD_TEXT_H
#define VITAMEMIO_HUD_TEXT_H

#include <stdint.h>

#include "../include/vitamemio_kernel_api.h"
#include "hud.h"

/* One kind's part of the Streak line, `k` "C" or "NC":
 *   C:5:2/16/8   as set : now for large reads / 32 KiB reads / tiny reads
 *   NC:never     the app's "never" (its large and 32 KiB reads)
 * The tiny reads' is one for both kinds. */
static inline void hud_streak_kind(char *o, int cap, const char *k, uint32_t set, uint32_t now,
                                   uint32_t mid, uint32_t tiny)
{
	if (now == VM_RA_NEVER)
		snprintf(o, (size_t)cap, HUD_C_DIM "%s:never", k);
	else
		snprintf(o, (size_t)cap, HUD_C_DIM "%s:" HUD_C_TEXT "%u" HUD_C_DIM ":" HUD_C_GOOD "%u/%u/%u", k,
		         (unsigned)(set > 99 ? 99 : set), (unsigned)(now > 99 ? 99 : now),
		         (unsigned)(mid > 99 ? 99 : mid), (unsigned)(tiny > 99 ? 99 : tiny));
}

#endif /* VITAMEMIO_HUD_TEXT_H */
