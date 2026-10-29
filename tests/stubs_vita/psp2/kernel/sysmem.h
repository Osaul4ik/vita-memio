#pragma once
#include <psp2/types.h>
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0x09408060
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE 0x0C208060
SceUID sceKernelAllocMemBlock(const char *n, int type, SceSize size, void *opt);
int sceKernelGetMemBlockBase(SceUID id, void **base);
int sceKernelFreeMemBlock(SceUID id);
