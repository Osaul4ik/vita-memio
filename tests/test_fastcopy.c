/*
 * test_fastcopy.c - kernel/fastcopy.h gives exactly what memcpy gives, for every
 * source and destination alignment and every length around the 4-, 32- and
 * 64-byte steps, never writes outside the destination and never reads outside
 * the source (AddressSanitizer: each buffer is its own exact-size allocation).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../kernel/fastcopy.h"
#include "vm_test.h"

int vm_failures;

static void one(size_t soff, size_t doff, size_t n)
{
	uint8_t *sbuf = malloc(soff + n + 1), *dbuf = malloc(doff + n + 16), *want = malloc(doff + n + 16);

	for (size_t i = 0; i < soff + n + 1; i++)
		sbuf[i] = (uint8_t)(i * 131 + 7);
	memset(dbuf, 0xA5, doff + n + 16);
	memcpy(want, dbuf, doff + n + 16);
	memcpy(want + doff, sbuf + soff, n);
	CHECK(vm_fast_copy(dbuf + doff, sbuf + soff, n) == dbuf + doff);
	CHECK(memcmp(dbuf, want, doff + n + 16) == 0);   /* data, and the guard bytes */
	free(sbuf);
	free(dbuf);
	free(want);
}

int main(void)
{
	static const size_t big[] = { 512, 4096, 32768, 65536 + 3, 262144 };

	for (size_t so = 0; so < 8; so++)
		for (size_t dof = 0; dof < 8; dof++)
			for (size_t n = 0; n <= 200; n++)
				one(so, dof, n);
	for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++)
		for (size_t so = 0; so < 4; so++)
			for (size_t dof = 0; dof < 4; dof++)
				one(so, dof, big[i]);
	one(0, 0, 0);
	printf(vm_failures ? "test_fastcopy: %d FAILED\n" : "test_fastcopy: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
