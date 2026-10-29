/* Fakes shared by the host tests: clock, memory blocks, hooks and a file system
 * that maps the ur0:data/turbostorage directory into a temp directory. */
#pragma once
#include <stdint.h>

extern uint32_t fake_now_us;          /* ksceKernelGetSystemTimeLow()                */
extern int fake_alloc_fail, fake_alloc_calls, fake_free_calls;
extern int fake_hook_fail_at, fake_hook_calls, fake_hook_releases;
extern int fake_rename_fail;          /* ksceIoRename fails (power cut after remove) */
extern int fake_write_short;          /* ksceIoWrite writes only part of the data    */
/* Mirror (second mapping) blocks: fail, or map the wrong memory. */
extern int fake_mirror_fail, fake_mirror_calls, fake_mirror_wrong, fake_mirror_type;
/* Cache functions: missing, calls, misaligned ranges seen, bytes invalidated. */
extern int fake_dcache_missing, fake_dc_inv_calls, fake_dc_clean_inv_calls, fake_dc_bad_range;
extern uint32_t fake_dc_inv_bytes;
/* ksceKernelVAtoPA missing; types refused before a HAS_PADDR block works;
 * second mappings freed. */
extern int fake_va2pa_missing, fake_paddr_types_refused, fake_mirror_frees;

/* Fresh temp directory, exported as the "ur0:data/turbostorage" root. */
const char *fakes_tmpdir_new(void);
void fakes_tmpdir_remove(void);
void fakes_reset(void);
