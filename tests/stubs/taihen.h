#pragma once
#include <stdint.h>
#include "psp2kern/kernel/sysmem.h"
typedef uintptr_t tai_hook_ref_t;
#define KERNEL_PID 0x10005
/* The tests call the stage functions with their own "original" function, so the
 * continuation macro is never the thing under test. */
#define TAI_CONTINUE(type, ref, ...) ({ (void)(ref); (type)0; })
int taiHookFunctionExportForKernel(int pid, tai_hook_ref_t *ref, const char *mod,
                                   uint32_t lib, uint32_t nid, const void *fn);
int taiHookReleaseForKernel(int uid, tai_hook_ref_t ref);
