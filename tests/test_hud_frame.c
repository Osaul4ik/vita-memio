/*
 * test_hud_frame.c - the frame hook (kernel/hud.c) against the worker that
 * changes the HUD's text and its cache: every frame drawn while the text
 * keeps changing must carry a whole panel, the old text or the new one -
 * never a half-composed one and never none (the HUD blinked).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../kernel/hud.c"
#include "vm_test.h"

#define W  960
#define H  544
#define BG 0x40302010u
#define ROUNDS 3000

static const char k_a[3][VM_HUD_LINE_MAX] = { "SD2Vita 19.8 MB/s", "Core 3 C 12% NC 3%", "RA C 5:6 NC 5:4" };
static const char k_b[3][VM_HUD_LINE_MAX] = { "SD2Vita 26.1 MB/s", "Core 3 C 40% NC 9%", "RA C 5:3 NC 5:8" };

static uint32_t *g_ref_a, *g_ref_b;
static volatile int g_stop;
static volatile int g_bad, g_bare, g_frames;

static int no_game(const char *title_id) { (void)title_id; return 0; }

static void frame_on(uint32_t *fb, int index)
{
	SceDisplayFrameBuf p = { sizeof(p), fb, W, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, W, H };

	for (int i = 0; i < W * H; i++)
		fb[i] = BG;
	hook_set_framebuf(1, index, &p, 0);
}
static void frame(uint32_t *fb) { frame_on(fb, 0); }

/* the frames of one layer each: index 0 (a game's) and 1 (the system's) */
static void *reader(void *arg)
{
	uint32_t *fb = malloc(W * H * 4);
	const int index = (int)(intptr_t)arg;

	while (!g_stop) {
		frame_on(fb, index);
		if (memcmp(fb, g_ref_a, W * H * 4) && memcmp(fb, g_ref_b, W * H * 4)) {
			int bare = 1;

			for (int i = 0; i < W * H && bare; i++)
				bare = fb[i] == BG;
			__atomic_add_fetch(bare ? &g_bare : &g_bad, 1, __ATOMIC_RELAXED);
		}
		__atomic_add_fetch(&g_frames, 1, __ATOMIC_RELAXED);
	}
	free(fb);
	return NULL;
}

/* A new block put in as the cache, as cache_fit does when the panel grows. */
static void new_block(void)
{
	void *base = NULL;
	SceUID uid = ksceKernelAllocMemBlock("t", 0, 0x10000, NULL);

	if (uid >= 0 && ksceKernelGetMemBlockBase(uid, &base) >= 0)
		cache_swap(uid, base, 0x10000);
}

static void test_no_blink(void)
{
	pthread_t r[2];

	g_ref = 1;                         /* hooked */
	g_excl_running = no_game;          /* the system's layer drawn on too */
	g_ref_a = malloc(W * H * 4);
	g_ref_b = malloc(W * H * 4);
	/* both texts once, so the panel's width settles (it never shrinks while shown) */
	frame(g_ref_a);
	vm_hud_set_text(k_a, 3);
	vm_hud_set_text(k_b, 3);
	vm_hud_set_text(k_a, 3);
	frame(g_ref_a);
	vm_hud_set_text(k_b, 3);
	frame(g_ref_b);
	CHECK(memcmp(g_ref_a, g_ref_b, W * H * 4) != 0);
	CHECK(g_cache.px != NULL);         /* drawn from the cache */

	for (int i = 0; i < 2; i++)
		pthread_create(&r[i], NULL, reader, (void *)(intptr_t)i);
	for (int i = 0; i < ROUNDS; i++) {
		vm_hud_set_text(i & 1 ? k_b : k_a, 3);
		usleep(100);                   /* frames in between */
		if (i % 100 == 50) {
			new_block();               /* the cache swapped under the frames */
			vm_hud_set_text(i & 1 ? k_b : k_a, 3);   /* recomposed in it */
		}
	}
	g_stop = 1;
	for (int i = 0; i < 2; i++)
		pthread_join(r[i], NULL);
	printf("  %d frames: %d with a broken panel, %d without one\n", g_frames, g_bad, g_bare);
	CHECK(g_frames > 0);
	CHECK(g_bad == 0 && g_bare == 0);
	CHECK(g_readers == 0 && !(g_seq & 1));
	vm_hud_fini();
	CHECK(g_cache_uid < 0 && g_cache_stale < 0);
	free(g_ref_a);
	free(g_ref_b);
}

int main(void)
{
	RUN(test_no_blink);
	if (vm_failures) {
		printf("test_hud_frame: %d FAILED\n", vm_failures);
		return 1;
	}
	printf("test_hud_frame: ok\n");
	return 0;
}
