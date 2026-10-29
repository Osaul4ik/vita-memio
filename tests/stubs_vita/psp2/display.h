#pragma once
#include <psp2/types.h>
#define SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 0x00000000
#define SCE_DISPLAY_SETBUF_NEXTFRAME 1
typedef struct SceDisplayFrameBuf { SceSize size; void *base; unsigned pitch, pixelformat, width, height; } SceDisplayFrameBuf;
int sceDisplaySetFrameBuf(const SceDisplayFrameBuf *p, int sync);
int sceDisplayWaitVblankStart(void);
