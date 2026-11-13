#pragma once
#include <stdint.h>
#include <stddef.h>
/* What hud.c needs of SceDisplay, for the host tests. */
#define SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 0
typedef struct SceDisplayFrameBuf {
	size_t   size;
	void    *base;
	unsigned pitch;
	unsigned pixelformat;
	unsigned width;
	unsigned height;
} SceDisplayFrameBuf;
