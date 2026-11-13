/*
 * guard.h - the boot guard's file (main.c): one byte, '1' from a start until
 * the home screen has been up a while, '0' after. Written in place - never
 * truncated, never deleted - so each change is one small write on the
 * internal storage: no block given back and taken again, no directory entry
 * that a reset could lose (a lost delete would look like a start that never
 * finished, and switch the plugin off for nothing). Kept free of the rest so
 * the host tests can drive it (tests/test_guard.c).
 */
#ifndef VITAMEMIO_GUARD_H
#define VITAMEMIO_GUARD_H

#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>

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

#endif /* VITAMEMIO_GUARD_H */
