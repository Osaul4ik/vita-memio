/*
 * vita_env.h - the BenchEnv for a real console: sceIo files, the process timer,
 * and a read-only look at the plugin's state (SceVitaMemio vmGetState). The
 * tester never changes a plugin setting.
 */
#ifndef VITAMEMIO_TESTER_VITA_ENV_H
#define VITAMEMIO_TESTER_VITA_ENV_H

#include <stddef.h>

#include <psp2/kernel/sysmem.h>

#include "../bench.h"

typedef struct VitaEnv {
	BenchEnv   env;
	int        dev;                              /* enum vm_dev under test        */
	void     (*on_progress)(const BenchProgress *p);
	int      (*poll_cancel)(void);               /* 1 when the user asked to stop */
	SceUID     nc_uid;                           /* the NC reads' memory block    */
	SceUID     mem_uid[3];                       /* DIAGNOSTIC: by BENCH_MEM_*    */
} VitaEnv;

void vita_env_init(VitaEnv *v, int dev,
                   void (*on_progress)(const BenchProgress *p),
                   int (*poll_cancel)(void));

/* Create a folder and all its parents ("ux0:MemioTest/"). 0 when it exists after. */
int vita_mkdirs(const char *dir);

#endif
