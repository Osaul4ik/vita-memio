/*
 * pattern.h - the data the tester writes and checks.
 *
 * The content of every 4-byte word depends only on its position in the file, so
 * any read, at any offset and size, can be checked without keeping a copy of the
 * file. A transfer that returns wrong, missing or shifted data shows up as
 * mismatching words.
 *
 * Offsets and sizes must be multiples of 4 and the buffer 4-byte aligned (the
 * tester only uses multiples of 512 and malloc'd buffers).
 */
#ifndef STORAGETURBO_TESTER_PATTERN_H
#define STORAGETURBO_TESTER_PATTERN_H

#include <stdint.h>

static inline uint32_t tp_word(uint32_t i)
{
	uint32_t x = i * 2654435761u + 0x9E3779B9u;

	x ^= x >> 15;
	x *= 2246822519u;
	x ^= x >> 13;
	x *= 3266489917u;
	x ^= x >> 16;
	return x;
}

/* Fill `n` bytes that sit at file offset `off`. */
static inline void tp_fill(void *buf, uint64_t off, uint32_t n)
{
	uint32_t *w = (uint32_t *)buf;
	uint32_t first = (uint32_t)(off >> 2);

	for (uint32_t i = 0; i < n / 4; i++)
		w[i] = tp_word(first + i);
}

/* Number of words that differ from the pattern; *first_bad (if given) is the
 * file offset of the first one. */
static inline uint32_t tp_verify(const void *buf, uint64_t off, uint32_t n, uint64_t *first_bad)
{
	const uint32_t *w = (const uint32_t *)buf;
	uint32_t first = (uint32_t)(off >> 2), bad = 0;

	for (uint32_t i = 0; i < n / 4; i++) {
		if (w[i] != tp_word(first + i)) {
			if (!bad && first_bad)
				*first_bad = off + (uint64_t)i * 4;
			bad++;
		}
	}
	return bad;
}

#endif /* STORAGETURBO_TESTER_PATTERN_H */
