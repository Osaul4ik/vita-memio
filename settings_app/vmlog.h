/*
 * vmlog.h - tiny logger for the user module (the Settings app).
 *
 * Appends timestamped lines to ur0:data/vita-memio/user.log (rotated to
 * user.old.log past VM_LOG_MAX_BYTES). It logs only problems (kernel module
 * not answering, Settings hooks or page not installed, a rejected setting),
 * so each line opens, appends and closes the file:
 * nothing stays open. Failure-tolerant: a line that cannot
 * be written is dropped.
 *
 * Same printf subset as the kernel logger: %s %d %i %u %x %X %c %p %%, with an
 * optional zero-pad width.
 */
#ifndef VITAMEMIO_TSLOG_H
#define VITAMEMIO_TSLOG_H

#include "../include/vitamemio.h"

#define VM_LOG_PATH       VM_DATA_DIR "/user.log"
#define VM_LOG_PATH_OLD   VM_DATA_DIR "/user.old.log"
#define VM_LOG_MAX_BYTES  (32 * 1024)

void vm_log(const char *fmt, ...);

#define VM_LOGI(...) vm_log(__VA_ARGS__)

#endif /* VITAMEMIO_TSLOG_H */
