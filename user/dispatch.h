/*
 * dispatch.h - developer command server (user/dispatch.c), SceShell only.
 */
#ifndef STORAGETURBO_DISPATCH_H
#define STORAGETURBO_DISPATCH_H

/* Start the deferred server thread (returns at once; listens only while
 * Developer mode is on). */
void ts_dispatch_start(void);
void ts_dispatch_stop(void);

/* Run one command line; the reply (no trailing newline) goes to out. */
void ts_dispatch_exec(const char *line, char *out, int cap);

#endif /* STORAGETURBO_DISPATCH_H */
