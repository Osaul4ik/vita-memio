/*
 * guard.h - the boot guard (main.c), kept free of the rest so the host tests
 * can drive it (tests/test_guard.c).
 *
 * The plugin does nothing while the console starts: its storage hooks are
 * there, but every transfer goes the standard way, and no buffer, thread or
 * app event is set up (stage.c: vm_stage_go). It comes on as soon as the
 * home screen is up - SceShell loaded and showing frames (LiveArea) - and
 * the start counts as good once it has then run VM_GUARD_SETTLE_S seconds.
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
 * for the log) and its length in *len. */
static inline char vm_guard_read(const char *path, char *txt, int cap, int *len)
{
	const SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	int n = 0;

	for (int i = 0; i < cap; i++)
		txt[i] = 0;
	if (fd >= 0) {
		n = ksceIoRead(fd, txt, (SceSize)(cap - 1));
		ksceIoClose(fd);
	}
	*len = n > 0 ? n : 0;
	if (n < 0)
		txt[0] = 0;
	for (int i = 0; txt[i]; i++)
		if (txt[i] < ' ' || txt[i] > '~')
			txt[i] = '.';
	return txt[0];
}

/* `c` as the first byte, in place. A file longer than one byte (an older
 * version wrote a line) is cut to one, once; *len is then 1. 0 or < 0. */
static inline int vm_guard_write(const char *path, const char *dir, char c, int *len)
{
	const int flags = SCE_O_WRONLY | SCE_O_CREAT | (*len > 1 ? SCE_O_TRUNC : 0);
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
	ksceIoClose(fd);
	if (w != 1)
		return -1;
	*len = 1;
	return 0;
}

/* The start's '1', written and read back: 0 only if it is there. */
static inline int vm_guard_arm(const char *path, const char *dir, int *len)
{
	char txt[4];
	int n;

	if (vm_guard_write(path, dir, '1', len) < 0)
		return -1;
	return vm_guard_read(path, txt, (int)sizeof(txt), &n) == '1' ? 0 : -1;
}

/* ---- when it comes on, and when the start counts as good --------------------- */

/* After it comes on: the start is good once it has run this long. A setting
 * that makes the console restart soon after must end in safe mode, not in a
 * loop; the price: restarting by hand within this time leads to safe mode
 * once (Reset safe mode ends it). */
#define VM_GUARD_SETTLE_S     30u
/* SceShell loaded but its frames cannot be seen on this system: on after this. */
#define VM_GUARD_SHELL_WAIT_S 5u
/* SceShell loaded, its frames can be seen but none came: on after this anyway
 * (a display set up as nothing here expects). */
#define VM_GUARD_FRAMES_MAX_S 20u
/* SceShell cannot be looked up at all: on this long after the start. */
#define VM_GUARD_BLIND_S      60u

enum { VM_GP_BOOT, VM_GP_ON, VM_GP_DONE };         /* VmGuard.phase */
enum { VM_GA_NONE, VM_GA_GO, VM_GA_CLEAR };         /* vm_guard_step: what to do */
enum { VM_GW_FRAMES = 1, VM_GW_SHELL, VM_GW_BLIND };  /* VmGuard.why (the log) */

typedef struct {
	int      phase;
	int      shell_seen;
	uint32_t shell_at;      /* the uptime SceShell was first seen (BOOT)      */
	uint32_t on_at;         /* the uptime it came on                          */
	int      why;           /* how the home screen was told: VM_GW_*          */
} VmGuard;

/* One look (every 100 ms while it waits, every second after): `shell` 1
 * SceShell loaded, 0 not yet, -1 cannot tell; `frames` 1 SceShell shows
 * frames, 0 not yet, -1 cannot tell; `now` seconds since module_start.
 * VM_GA_GO: come on now; VM_GA_CLEAR: the start is good, write '0'. */
static inline int vm_guard_step(VmGuard *g, int shell, int frames, uint32_t now)
{
	switch (g->phase) {
	case VM_GP_BOOT:
		if (shell > 0) {
			if (!g->shell_seen) {
				g->shell_seen = 1;
				g->shell_at = now;
			}
			if (frames > 0)
				g->why = VM_GW_FRAMES;           /* LiveArea up: at once */
			else if (now - g->shell_at >= (frames < 0 ? VM_GUARD_SHELL_WAIT_S
			                                          : VM_GUARD_FRAMES_MAX_S))
				g->why = VM_GW_SHELL;
			else
				return VM_GA_NONE;
		} else if (shell < 0 && now >= VM_GUARD_BLIND_S) {
			g->why = VM_GW_BLIND;
		} else {
			g->shell_seen = 0;               /* not (yet, or again) */
			return VM_GA_NONE;
		}
		g->phase = VM_GP_ON;
		g->on_at = now;
		return VM_GA_GO;
	case VM_GP_ON:
		if (now - g->on_at < VM_GUARD_SETTLE_S)
			return VM_GA_NONE;
		g->phase = VM_GP_DONE;
		return VM_GA_CLEAR;
	default:
		return VM_GA_NONE;
	}
}

#endif /* VITAMEMIO_GUARD_H */
