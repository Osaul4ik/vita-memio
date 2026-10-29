/*
 * test_util.c - the small pure helpers: the shared printf subset (vm_fmt.h).
 */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "../include/vm_fmt.h"
#include "vm_test.h"

static int fmt(char *buf, int cap, const char *f, ...)
{
	va_list ap;
	int n;

	va_start(ap, f);
	n = vm_fmt_v(buf, cap, f, ap);
	va_end(ap);
	return n;
}

/* vm_fmt must print the same as the C library for what the loggers use. */
#define SAME(f, ...) do { \
	char a[128], b[128]; \
	fmt(a, sizeof(a), f, __VA_ARGS__); \
	snprintf(b, sizeof(b), f, __VA_ARGS__); \
	if (strcmp(a, b)) { printf("  FAIL %s:%d: \"%s\" -> \"%s\", libc \"%s\"\n", __FILE__, __LINE__, f, a, b); vm_failures++; } \
} while (0)

static void test_matches_libc(void)
{
	SAME("%d", 0);
	SAME("%d", 42);
	SAME("%d", -42);
	SAME("%d", INT_MAX);
	SAME("%d", INT_MIN);
	SAME("%i", -7);
	SAME("%u", 0u);
	SAME("%u", UINT_MAX);
	SAME("%x", 0xdeadbeefu);
	SAME("%X", 0xdeadbeefu);
	SAME("%08x", 0x1234u);
	SAME("%06u", 42u);
	SAME("%05d", 42);
	SAME("%05d", -42);
	SAME("%s", "hello");
	SAME("%c", 'z');
	SAME("100%%%s", "");
	SAME("[%u.%06u] %s %d", 12u, 345u, "x", -1);
}

static void test_odd_formats(void)
{
	char b[64];

	fmt(b, sizeof(b), "%s", (const char *)0);
	CHECK(strcmp(b, "(null)") == 0);
	fmt(b, sizeof(b), "a%qb");
	CHECK(strcmp(b, "a%qb") == 0);            /* unknown conversion is kept */
	fmt(b, sizeof(b), "end%");
	CHECK(strcmp(b, "end") == 0);             /* a lone % at the end is dropped */
	fmt(b, sizeof(b), "%p", (void *)(uintptr_t)0xabc);
	CHECK(strcmp(b, "0x00000abc") == 0);
}

/* Any buffer size: always terminated, never written past cap. */
static void test_truncation(void)
{
	for (int cap = 1; cap <= 24; cap++) {
		char *b = malloc(cap);                /* exact size: ASan sees an overrun */
		int n = fmt(b, cap, "%s-%d-%08x", "abcdefgh", -123456, 0xCAFEu);

		CHECK(n >= 0 && n <= cap - 1);
		CHECK(b[n] == 0);
		CHECK(strlen(b) == (size_t)n);
		free(b);
	}
	CHECK(fmt(NULL, 0, "x") == 0);            /* zero capacity: nothing to write to */
}

int main(void)
{
	RUN(test_matches_libc);
	RUN(test_odd_formats);
	RUN(test_truncation);

	printf(vm_failures ? "test_util: %d FAILED\n" : "test_util: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
