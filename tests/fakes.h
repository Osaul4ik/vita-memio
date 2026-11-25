/* Fakes shared by the host tests: clock, memory blocks, hooks and a file system
 * that maps the ur0:data/vita-memio directory into a temp directory. */
#pragma once
#include <stdint.h>

extern uint32_t fake_now_us;          /* ksceKernelGetSystemTimeLow()                */
extern int fake_time_calls;           /* ... and how often it was called             */
extern int fake_alloc_fail, fake_alloc_calls, fake_free_calls;
extern unsigned fake_alloc_max;       /* > 0: a block bigger than this refused */
extern unsigned fake_alloc_type_fail; /* > 0: a block of this type refused        */
extern unsigned fake_block_type[64];  /* each block's type                        */
extern int fake_alloc_fail_after;     /* > 0: that many more made, then refused  */
extern int fake_alloc_fail_nth;       /* > 0: only the call that many on refused */
extern int fake_sync_calls;           /* ksceIoSyncByFd calls */
unsigned fake_block_size(int uid);
int fake_blocks_live(void);           /* blocks made and not freed */
extern int fake_hook_fail_at, fake_hook_calls, fake_hook_releases;
extern int fake_rename_fail;          /* ksceIoRename fails (power cut after remove) */
extern int fake_prof_writes;          /* files opened to write under .../ra/        */
extern int fake_tune_writes;          /* ... and tuning.bin                         */
extern int fake_tune_bytes;           /* bytes written to it                        */
extern int fake_write_short;          /* ksceIoWrite writes only part of the data    */
extern int fake_memtype;              /* ksceKernelGetMemBlockMemtypeByAddr()        */
extern int fake_memtype_calls;
extern int fake_thread_fail, fake_sema_fail;  /* thread / the n-th semaphore fails */
extern int fake_threads_live, fake_semas_live;
extern int fake_thread_fail_from;     /* from the n-th thread creation on, all fail */
int fake_sema_count(int id);
extern int fake_pid_calls;            /* ksceKernelGetProcessId() calls            */
extern int fake_pid, fake_usermap_err, fake_usermap_deny, fake_usermap_calls, fake_usermap_live;
extern int fake_meminfo_type, fake_meminfo_err, fake_meminfo_calls;
extern uintptr_t fake_noblock_lo, fake_noblock_hi;   /* in no memory block */
extern void *fake_usermap_elsewhere;       /* a semaphore's count now, or -1 */
extern int fake_thread_fail_masked, fake_thread_last_mask;  /* a core mask refused / used */
extern const void *fake_memtype_from; /* from this address on (if set) ...          */
extern const void *fake_memtype_to;   /* ... up to this one (if set) ...            */
extern int fake_memtype_above;        /* ... the memory type is this one             */

/* Fresh temp directory, exported as the "ur0:data/vita-memio" root. */
const char *fakes_tmpdir_new(void);
const char *fakes_tmpdir(void);
const char *fake_log_find(const char *part);   /* the latest log line holding it */
void fake_log_clear(void);              /* the one made last */
void fakes_tmpdir_remove(void);
void fakes_reset(void);

#include <pthread.h>
extern pthread_t fake_usermap_thread, fake_unmap_thread;
