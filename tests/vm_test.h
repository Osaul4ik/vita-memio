/* Tiny check/run macros for the host tests. */
#pragma once
#include <stdio.h>
extern int vm_failures;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); vm_failures++; } } while (0)
#define RUN(f)   do { printf("%s\n", #f); f(); } while (0)
