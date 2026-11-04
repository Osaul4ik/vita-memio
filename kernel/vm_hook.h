/*
 * vm_hook.h - a hook's continuation, safely.
 *
 * taiHEN makes a hook live a moment before it stores the ref (patches.c,
 * tai_hook_func_abs), and TAI_CONTINUE reads through the ref at once. A call
 * that lands in that window - hooks are put in while the system runs - waits
 * for the ref here (bounded). 0: still none; the caller then fails that one
 * call rather than jump through NULL.
 */
#ifndef VITAMEMIO_HOOK_H
#define VITAMEMIO_HOOK_H

#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

/* SCE_ERROR_ERRNO_EIO: what a hooked call returns with no ref to go on with. */
#define VM_HOOK_NO_REF  ((int)0x80010005)

static inline tai_hook_ref_t vm_hook_ref(const volatile tai_hook_ref_t *ref)
{
	for (int i = 0; i < 2000 && *ref == 0; i++)
		ksceKernelDelayThread(10);
	return *ref;
}

#endif /* VITAMEMIO_HOOK_H */
