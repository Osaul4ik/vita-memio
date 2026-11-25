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
	fake_sync_calls = 0;
	CHECK(vm_guard_write(PATH, DIR, '1', &len) == 0 && len == 1 && size_of(dir) == 1);
	CHECK(fake_sync_calls == 1);             /* on the card before a hard reset */
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

/* The start's '1', written and read back; refused when it cannot be written
 * (the folder cannot be made): then the plugin stays off. */
static void test_arm(void)
{
	char txt[48];
	int len = 0;

	fakes_tmpdir_remove();
	fakes_tmpdir_new();
	CHECK(vm_guard_arm(PATH, DIR, &len) == 0 && vm_guard_read(PATH, txt, sizeof(txt), &len) == '1');
	CHECK(vm_guard_arm("ux9:nowhere/x/boot_guard", "ux9:nowhere/x", &len) < 0);
	fakes_tmpdir_remove();
}

/* When it comes on, and when the start counts as good: no timers. */
static void test_steps(void)
{
	VmGuard g;

	/* the console starts: nothing until SceShell shows a frame - then at once */
	memset(&g, 0, sizeof(g));
	CHECK(vm_guard_step(&g, 0, 0, 1) == VM_GA_NONE);        /* proven means nothing yet */
	CHECK(vm_guard_step(&g, 1, 0, 0) == VM_GA_NONE);        /* loaded, no frame yet */
	CHECK(vm_guard_step(&g, 1, 0, 0) == VM_GA_NONE && g.phase == VM_GP_BOOT);
	CHECK(vm_guard_step(&g, 1, 1, 0) == VM_GA_GO && g.why == VM_GW_FRAMES && g.phase == VM_GP_ON);
	/* on: good only once it has shown it works, then once */
	CHECK(vm_guard_step(&g, 1, 1, 0) == VM_GA_NONE);
	CHECK(vm_guard_step(&g, 0, 0, 0) == VM_GA_NONE && g.phase == VM_GP_ON);   /* no going back */
	CHECK(vm_guard_step(&g, 1, 1, 1) == VM_GA_CLEAR && g.phase == VM_GP_DONE);
	CHECK(vm_guard_step(&g, 1, 1, 1) == VM_GA_NONE);

	/* frames cannot be seen here: on as soon as SceShell is loaded */
	memset(&g, 0, sizeof(g));
	CHECK(vm_guard_step(&g, 0, -1, 0) == VM_GA_NONE);
	CHECK(vm_guard_step(&g, 1, -1, 0) == VM_GA_GO && g.why == VM_GW_SHELL);

	/* SceShell cannot be looked up at all: on at once, as before */
	memset(&g, 0, sizeof(g));
	CHECK(vm_guard_step(&g, -1, -1, 0) == VM_GA_GO && g.why == VM_GW_BLIND);
}

int main(void)
{
	RUN(test_guard);
	RUN(test_arm);
	RUN(test_steps);
	if (vm_failures) {
		printf("test_guard: %d FAILED\n", vm_failures);
		return 1;
	}
	printf("test_guard: ok\n");
	return 0;
}
