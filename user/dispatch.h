/*
 * dispatch.h - developer command server (user/dispatch.c), SceShell only.
 */
#ifndef VITAMEMIO_DISPATCH_H
#define VITAMEMIO_DISPATCH_H

/* Start the deferred server thread (returns at once; listens only while
 * Developer mode is on). */
void vm_dispatch_start(void);
void vm_dispatch_stop(void);

/* Run one command line; the reply (no trailing newline) goes to out. */
void vm_dispatch_exec(const char *line, char *out, int cap);

#endif /* VITAMEMIO_DISPATCH_H */
