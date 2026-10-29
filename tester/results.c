/*
 * results.c - see results.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/storageturbo.h"
#include "results.h"

#define TAIL_BYTES (96 * 1024)    /* the viewer shows the end of a long file */

int results_append(const char *path, const char *text)
{
	FILE *f = fopen(path, "ab");
	size_t n = strlen(text);
	int ok;

	if (!f)
		return -1;
	ok = fwrite(text, 1, n, f) == n;
	if (fclose(f) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

int results_clear(const char *path)
{
	FILE *f = fopen(path, "rb");

	if (!f)
		return 0;                     /* nothing to clear */
	fclose(f);
	return remove(path) == 0 ? 0 : -1;
}

long results_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fclose(f);
	return n;
}

int results_load(const char *path, char lines[][RESULTS_LINE_MAX], int max)
{
	FILE *f = fopen(path, "rb");
	long size;
	size_t got, start = 0;
	char *buf;
	int n = 0, all = 0;
	char (*tmp)[RESULTS_LINE_MAX];

	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	if (size <= 0) {
		fclose(f);
		return 0;
	}
	if (size > TAIL_BYTES) {
		fseek(f, size - TAIL_BYTES, SEEK_SET);
		size = TAIL_BYTES;
	} else {
		fseek(f, 0, SEEK_SET);
	}
	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return 0;
	}
	got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = 0;

	/* A cut-off first line of a long file is dropped. */
	if (got == TAIL_BYTES) {
		char *nl = memchr(buf, '\n', got);

		start = nl ? (size_t)(nl - buf) + 1 : got;
	}

	/* Split and wrap, keeping the last `max` lines in a ring. */
	tmp = malloc((size_t)max * RESULTS_LINE_MAX);
	if (!tmp) {
		free(buf);
		return 0;
	}
	for (size_t i = start; i < got;) {
		size_t e = i, len, pos = i;

		while (e < got && buf[e] != '\n')
			e++;
		len = e - i;
		if (len && buf[e - 1] == '\r')
			len--;                          /* CRLF files */
		do {
			size_t part = len > RESULTS_LINE_MAX - 1 ? RESULTS_LINE_MAX - 1 : len;
			char *dst = tmp[all % max];

			memcpy(dst, buf + pos, part);
			dst[part] = 0;
			for (size_t k = 0; k < part; k++)
				if ((unsigned char)dst[k] < 0x20 || (unsigned char)dst[k] > 0x7E)
					dst[k] = '?';
			all++;
			pos += part;
			len -= part;
		} while (len > 0);
		i = e + 1;
	}
	free(buf);

	n = all < max ? all : max;
	for (int k = 0; k < n; k++)
		memcpy(lines[k], tmp[(all - n + k) % max], RESULTS_LINE_MAX);
	free(tmp);
	return n;
}
