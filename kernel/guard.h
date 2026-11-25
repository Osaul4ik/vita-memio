/*
 * guard.h - the boot guard (main.c), kept free of the rest so the host tests
 * can drive it (tests/test_guard.c).
 *
 * The plugin does nothing while the console starts: its storage hooks and
 * buffers are there, but every transfer goes the standard way, and no
 * thread or app event is set up (stage.c: vm_stage_go). It comes on as soon
 * as the home screen is up - SceShell loaded and showing frames (LiveArea) -
 * and the start counts as good once the plugin has then shown it works
 * (VM_GUARD_PROVEN).
 *
 * The file: one byte, '1' from module_start until that point, '0' after.
 * '1' found at module_start means the last start never got there: this one
 * is safe mode, nothing installed, until Settings > Advanced > Reset safe
 * mode. Written in place - never truncated, never deleted - so each change is
 * one small write on the internal storage (two a start): no block given back
 * and taken again, no directory entry a reset could lose (a lost delete
 * would look like a start that never finished). If the '1' cannot be
 * written and read back, the plugin stays off for this start: without it a
 * crash would not end in safe mode.
 */
#ifndef VITAMEMIO_GUARD_H
#define VITAMEMIO_GUARD_H

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>
#include <stdint.h>

/* The file's first byte (0: no file, or empty), its text in txt (printable,
 * for the log). */
static inline char vm_guard_read(const char *path, char *txt, int cap)
{
	const SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	int n = 0;

	for (int i = 0; i < cap; i++)
		txt[i] = 0;
	if (fd >= 0) {
		n = ksceIoRead(fd, txt, (SceSize)(cap - 1));
		ksceIoClose(fd);
	}
	if (n < 0)
		txt[0] = 0;
	for (int i = 0; txt[i]; i++)
		if (txt[i] < ' ' || txt[i] > '~')
			txt[i] = '.';
	return txt[0];
}

/* `c` as the first byte, in place. 0 or < 0. */
static inline int vm_guard_write(const char *path, const char *dir, char c)
{
	const int flags = SCE_O_WRONLY | SCE_O_CREAT;
	SceUID fd = ksceIoOpen(path, flags, 0666);
	int w;

	if (fd < 0) {                         /* its folder not there yet */
		ksceIoMkdir("ur0:data", 0777);
		ksceIoMkdir(dir, 0777);
		fd = ksceIoOpen(path, flags, 0666);
		if (fd < 0)
			return fd;
	}
	w = ksceIoWrite(fd, &c, 1);
	if (w == 1) {
		int st;

		/* on the card now: a hard reset right after (a start that hangs)
		 * must still find the '1' */
		ksceIoSyncByFd(fd, &st);
	}
	ksceIoClose(fd);
	if (w != 1)
		return -1;
	return 0;
}

/* The start's '1', written and read back: 0 only if it is there. */
static inline int vm_guard_arm(const char *path, const char *dir)
{
	char txt[4];

	if (vm_guard_write(path, dir, '1') < 0)
		return -1;
	return vm_guard_read(path, txt, (int)sizeof(txt)) == '1' ? 0 : -1;
}

/* ---- when it comes on, and when the start counts as good --------------------- *
 *
 * No timers: it comes on when the home screen is there, and the start counts
 * as good when the plugin has shown it works.
 */

/* The start is good once this many transfers took the faster path - the
 * plugin's own code ran and the console is still there - or once no storage
 * is on any more (none set, or gone back to the standard path after an
 * error: not a crash). */
#define VM_GUARD_PROVEN  64u

enum { VM_GP_BOOT, VM_GP_ON, VM_GP_DONE };            /* VmGuard.phase */
enum { VM_GA_NONE, VM_GA_GO, VM_GA_CLEAR };            /* vm_guard_step: what to do */
enum { VM_GW_FRAMES = 1, VM_GW_SHELL, VM_GW_BLIND };   /* VmGuard.why (the log) */

typedef struct {
	int phase;
	int why;                /* how the home screen was told: VM_GW_*          */
} VmGuard;

/* One look: `shell` 1 SceShell loaded, 0 not yet, -1 cannot tell on this
 * system; `frames` 1 SceShell shows a frame (LiveArea is up), 0 not yet, -1
 * cannot tell; `proven` 1 once the start counts as good (VM_GUARD_PROVEN).
 * VM_GA_GO: come on now; VM_GA_CLEAR: write '0'. */
static inline int vm_guard_step(VmGuard *g, int shell, int frames, int proven)
{
	switch (g->phase) {
	case VM_GP_BOOT:
		if (shell > 0 && frames > 0)
			g->why = VM_GW_FRAMES;             /* LiveArea up */
		else if (shell > 0 && frames < 0)
			g->why = VM_GW_SHELL;              /* the most that can be told here */
		else if (shell < 0)
			g->why = VM_GW_BLIND;              /* nothing can be told: as before */
		else
			return VM_GA_NONE;
		g->phase = VM_GP_ON;
		return VM_GA_GO;
	case VM_GP_ON:
		if (!proven)
			return VM_GA_NONE;
		g->phase = VM_GP_DONE;
		return VM_GA_CLEAR;
	default:
		return VM_GA_NONE;
	}
}

#endif /* VITAMEMIO_GUARD_H */
