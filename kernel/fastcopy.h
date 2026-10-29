/*
 * fastcopy.h - the CPU copy out of a staging buffer into a caller's buffer
 * (reads only: for writes into staging the system memcpy measured faster).
 *
 * Why not plain memcpy: the staging buffer is non-cacheable, and on the
 * Cortex-A9 every load from non-cacheable memory goes out to the bus on its own.
 * The faster path's own timing showed the copy out of it at ~114 MB/s (4.5 us of
 * every ~28 us 512-byte block on SD2Vita reads). Loading 8 words at a time
 * (one LDM of 32 bytes) lets the bus fetch them as one burst instead of eight
 * single reads.
 *
 * No NEON/VFP: kernel code must not touch the FP registers, which belong to the
 * user thread that is waiting on this I/O. On ARM the burst loop is a short
 * LDM/STM asm block; the host build runs the same steps in C. Both are checked
 * by tests/test_fastcopy.c (the ARM one under qemu: make -C tests arm).
 *
 * Only word-aligned buffers take the burst loop; anything else (a caller's
 * odd-aligned buffer, a tail under 4 bytes) is left to memcpy, so the result is
 * always byte-for-byte what memcpy would give (tests/test_fastcopy.c).
 */
#ifndef STORAGETURBO_FASTCOPY_H
#define STORAGETURBO_FASTCOPY_H

#include <stddef.h>
#include <stdint.h>

/* GCC turns a plain copy loop back into a memcpy call (loop distribution), which
 * would undo the point of this function: keep it a loop. */
#if defined(__GNUC__) && !defined(__clang__)
#define TS_NO_MEMCPY_IDIOM __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define TS_NO_MEMCPY_IDIOM
#endif

/* A word that may alias any other type (the buffers hold plain bytes). */
typedef uint32_t __attribute__((may_alias)) ts_word_t;

static inline TS_NO_MEMCPY_IDIOM void *ts_fast_copy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	if ((((uintptr_t)d | (uintptr_t)s) & 3u) == 0) {
		ts_word_t *dw = (ts_word_t *)d;
		const ts_word_t *sw = (const ts_word_t *)s;

#if defined(__arm__)
		/* 64 bytes per pass as two 8-register LDM/STM pairs. Written out, because
		 * GCC turns the C loop below into LDRD/STRD pairs (8 bytes per access)
		 * instead of one 32-byte burst. r7 and r11 (possible frame pointers),
		 * sp, lr and pc are not used; the compiler keeps sw/dw out of the
		 * clobbered registers. */
		while (n >= 64) {
			__asm__ volatile(
				"ldmia %[s]!, {r3, r4, r5, r6, r8, r9, r10, r12}\n\t"
				"stmia %[d]!, {r3, r4, r5, r6, r8, r9, r10, r12}\n\t"
				"ldmia %[s]!, {r3, r4, r5, r6, r8, r9, r10, r12}\n\t"
				"stmia %[d]!, {r3, r4, r5, r6, r8, r9, r10, r12}\n\t"
				: [s] "+r" (sw), [d] "+r" (dw)
				:
				: "r3", "r4", "r5", "r6", "r8", "r9", "r10", "r12", "memory");
			n -= 64;
		}
#else
		/* Host build (tests): the same steps in C. */
		while (n >= 64) {
			uint32_t a0 = sw[0], a1 = sw[1], a2 = sw[2], a3 = sw[3];
			uint32_t a4 = sw[4], a5 = sw[5], a6 = sw[6], a7 = sw[7];

			dw[0] = a0; dw[1] = a1; dw[2] = a2; dw[3] = a3;
			dw[4] = a4; dw[5] = a5; dw[6] = a6; dw[7] = a7;
			a0 = sw[8];  a1 = sw[9];  a2 = sw[10]; a3 = sw[11];
			a4 = sw[12]; a5 = sw[13]; a6 = sw[14]; a7 = sw[15];
			dw[8] = a0;  dw[9] = a1;  dw[10] = a2; dw[11] = a3;
			dw[12] = a4; dw[13] = a5; dw[14] = a6; dw[15] = a7;
			sw += 16;
			dw += 16;
			n -= 64;
		}
#endif
		while (n >= 4) {
			*dw++ = *sw++;
			n -= 4;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	if (n)
		memcpy(d, s, n);
	return dst;
}

#endif /* STORAGETURBO_FASTCOPY_H */
