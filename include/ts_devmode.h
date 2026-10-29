/*
 * ts_devmode.h - the Developer mode auto-off rule, kept free of kernel calls so
 * the host tests (tests/) can run it.
 *
 * The 1 Hz worker (kernel/main.c) calls ts_devmode_tick() once a second with the
 * seconds since the module started and the current Developer mode setting. It
 * keeps the "on since" second in *since (0 = not counting).
 */
#ifndef STORAGETURBO_DEVMODE_H
#define STORAGETURBO_DEVMODE_H

#include <stdint.h>

#include "storageturbo.h"

/* Returns 1 when Developer mode has been on for TS_DEVMODE_MINUTES and must be
 * switched off now (the counter restarts), else 0. `now_s` must be >= 1. */
static inline int ts_devmode_tick(uint32_t now_s, int devmode_on, uint32_t *since)
{
	if (!devmode_on) {
		*since = 0;
		return 0;
	}
	if (!*since) {
		*since = now_s;
		return 0;
	}
	if (now_s - *since >= (uint32_t)TS_DEVMODE_MINUTES * 60u) {
		*since = 0;
		return 1;
	}
	return 0;
}

#endif /* STORAGETURBO_DEVMODE_H */
