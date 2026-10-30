/* Fakes shared by the host tests: clock, memory blocks, hooks and a file system
 * that maps the ur0:data/vita-memio directory into a temp directory. */
#pragma once
#include <stdint.h>

extern uint32_t fake_now_us;          /* ksceKernelGetSystemTimeLow()                */
extern int fake_time_calls;           /* ... and how often it was called             */
extern int fake_alloc_fail, fake_alloc_calls, fake_free_calls;
extern int fake_hook_fail_at, fake_hook_calls, fake_hook_releases;
extern int fake_rename_fail;          /* ksceIoRename fails (power cut after remove) */
extern int fake_write_short;          /* ksceIoWrite writes only part of the data    */
extern int fake_memtype;              /* ksceKernelGetMemBlockMemtypeByAddr()        */
extern int fake_memtype_calls;
extern int fake_thread_fail, fake_sema_fail;  /* thread / the n-th semaphore fails */
extern int fake_threads_live, fake_semas_live;
extern const void *fake_memtype_from; /* from this address on (if set) ...          */
extern int fake_memtype_above;        /* ... the memory type is this one             */

/* Fresh temp directory, exported as the "ur0:data/vita-memio" root. */
const char *fakes_tmpdir_new(void);
void fakes_tmpdir_remove(void);
void fakes_reset(void);
