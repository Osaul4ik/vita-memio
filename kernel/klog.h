/*
 * klog.h - kernel-side logger for the SceVitaMemio kernel module.
 *
 * Primary sink is ksceDebugPrintf with a "[vitamemio]" tag (visible over a
 * kernel debug channel). In addition every line is appended to
 * ur0:data/vita-memio/kernel.log via ksceIo*, best-effort and
 * failure-tolerant. When the file passes VM_KLOG_MAX_BYTES it is renamed to
 * kernel.old.log and a fresh one is started, so an overnight run keeps about
 * twice that much history.
 *
 * What is logged: one line per start (build and storage driver), and
 * otherwise only problems - warnings and errors. Routine events are not.
 * Nothing at all is written unless logging is switched on in the settings.
 *
 * HOT-PATH RULE: the storage I/O hooks (kernel/stage.c) must NOT call
 * these. They only bump counters; the worker (kernel/main.c) does the
 * logging.
 *
 * The self-contained formatter means no libc (%s %d %i %u %x %X %c %p %%,
 * optional zero-pad width).
 */
#ifndef VITAMEMIO_KLOG_H
#define VITAMEMIO_KLOG_H

#include <stdint.h>

#include "../include/vitamemio.h"

#define VM_KLOG_ERR    0
#define VM_KLOG_WARN   1
#define VM_KLOG_INFO   2
#define VM_KLOG_DEBUG  3
#define VM_KLOG_TRACE  4

#ifndef VM_KLOG_DEFAULT_LEVEL
#define VM_KLOG_DEFAULT_LEVEL  VM_KLOG_DEBUG
#endif

#define VM_KLOG_DIR       VM_DATA_DIR
#define VM_KLOG_PATH      VM_KLOG_DIR "/kernel.log"
#define VM_KLOG_PATH_OLD  VM_KLOG_DIR "/kernel.old.log"
#define VM_KLOG_MAX_BYTES (128 * 1024)

void vm_klog_init(void);
void vm_klog_fini(void);
/* Logging on (1) or off (0), from the settings. Lines logged before the first
 * call are kept and written by it if logging is on. */
void vm_klog_set_enabled(int on);

void vm_klog(int level, const char *fmt, ...);

#define VM_KLOGE(...) vm_klog(VM_KLOG_ERR,   __VA_ARGS__)
#define VM_KLOGW(...) vm_klog(VM_KLOG_WARN,  __VA_ARGS__)
#define VM_KLOGI(...) vm_klog(VM_KLOG_INFO,  __VA_ARGS__)
#define VM_KLOGD(...) vm_klog(VM_KLOG_DEBUG, __VA_ARGS__)
#define VM_KLOGT(...) vm_klog(VM_KLOG_TRACE, __VA_ARGS__)

#endif /* VITAMEMIO_KLOG_H */
