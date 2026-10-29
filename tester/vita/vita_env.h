/*
 * vita_env.h - the BenchEnv for a real console: sceIo files, the process timer,
 * and the SceStorageTurbo syscalls for switching the plugin.
 */
#ifndef STORAGETURBO_TESTER_VITA_ENV_H
#define STORAGETURBO_TESTER_VITA_ENV_H

#include <stddef.h>

#include "../bench.h"

typedef struct VitaEnv {
	BenchEnv   env;
	int        dev;                              /* enum ts_dev under test        */
	void     (*on_progress)(const BenchProgress *p);
	int      (*poll_cancel)(void);               /* 1 when the user asked to stop */
	TsSettings saved;
	int        have_saved;
} VitaEnv;

void vita_env_init(VitaEnv *v, int dev,
                   void (*on_progress)(const BenchProgress *p),
                   int (*poll_cancel)(void));

/* At app start: if an earlier test was interrupted, put the user's plugin
 * settings back. Returns 1 when something was restored, 0 when there was
 * nothing to do, -1 when the restore failed (the record is kept for the next
 * start). */
int vita_env_recover(void);

/* Create a folder and all its parents ("ux0:TurboTest/"). 0 when it exists after. */
int vita_mkdirs(const char *dir);

#endif
