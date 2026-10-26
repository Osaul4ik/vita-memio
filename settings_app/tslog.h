/*
 * tslog.h - tiny logger for the user module (both instances: SceShell and the
 * Settings app).
 *
 * Appends timestamped lines to ur0:data/turbostorage/user.log (rotated to
 * user.old.log past TS_LOG_MAX_BYTES). It logs only on start, page loads,
 * setting changes and developer-server events, so each line opens, appends and
 * closes the file: nothing stays open. Failure-tolerant: a line that cannot
 * be written is dropped.
 *
 * Same printf subset as the kernel logger: %s %d %i %u %x %X %c %p %%, with an
 * optional zero-pad width.
 */
#ifndef STORAGETURBO_TSLOG_H
#define STORAGETURBO_TSLOG_H

#include "../include/storageturbo.h"

#define TS_LOG_PATH       TS_DATA_DIR "/user.log"
#define TS_LOG_PATH_OLD   TS_DATA_DIR "/user.old.log"
#define TS_LOG_MAX_BYTES  (32 * 1024)

void ts_log(const char *fmt, ...);

#define TS_LOGI(...) ts_log(__VA_ARGS__)

#endif /* STORAGETURBO_TSLOG_H */
