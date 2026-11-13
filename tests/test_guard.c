/*
 * test_guard.c - the boot guard's file (kernel/guard.h): one byte written in
 * place, the file never cut short again once it is one byte long, never
 * deleted; an older version's line cut to one byte once.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "../kernel/guard.h"
#include "fakes.h"
#include "vm_test.h"

#define DIR  "ur0:data/vita-memio"
#define PATH DIR "/boot_guard"

static long size_of(const char *dir)
{
	char p[512];
	struct stat st;

	snprintf(p, sizeof(p), "%s/boot_guard", dir);
	return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void test_guard(void)
{
	char txt[48];
	int len = -1;
	const char *dir;

	fakes_tmpdir_remove();
	dir = fakes_tmpdir_new();
	/* none yet: not a start that never finished */
	CHECK(vm_guard_read(PATH, txt, sizeof(txt), &len) == 0 && len == 0 && size_of(dir) == -1);
	/* a start, then the home screen: one byte, in place */
	CHECK(vm_guard_write(PATH, DIR, '1', &len) == 0 && len == 1 && size_of(dir) == 1);
	CHECK(vm_guard_read(PATH, txt, sizeof(txt), &len) == '1' && len == 1);
	CHECK(vm_guard_write(PATH, DIR, '0', &len) == 0 && size_of(dir) == 1);
	CHECK(vm_guard_read(PATH, txt, sizeof(txt), &len) == '0' && !strcmp(txt, "0"));
	/* an older version's line: read as before, cut to one byte at the next write */
	{
		char p[512];
		FILE *f;

		snprintf(p, sizeof(p), "%s/boot_guard", dir);
		f = fopen(p, "wb");
		fputs("1 up=1234 by=start\n", f);
		fclose(f);
	}
	CHECK(vm_guard_read(PATH, txt, sizeof(txt), &len) == '1' && len == 19 &&
	      !strcmp(txt, "1 up=1234 by=start."));
	CHECK(vm_guard_write(PATH, DIR, '0', &len) == 0 && len == 1 && size_of(dir) == 1);
	CHECK(vm_guard_read(PATH, txt, sizeof(txt), &len) == '0' && len == 1);
	fakes_tmpdir_remove();
}

int main(void)
{
	RUN(test_guard);
	if (vm_failures) {
		printf("test_guard: %d FAILED\n", vm_failures);
		return 1;
	}
	printf("test_guard: ok\n");
	return 0;
}
