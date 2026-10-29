/* Host-test stubs of the VitaSDK headers: only what the tested files use. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int SceUID;
typedef uint32_t SceSize;
typedef int64_t SceOff;
typedef struct { SceSize size; uint32_t attr; } SceKernelAllocMemBlockKernelOpt;
#define SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT 1
#define SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW 0x1020D006
SceUID ksceKernelAllocMemBlock(const char *name, unsigned type, SceSize size, void *opt);
int ksceKernelGetMemBlockBase(SceUID uid, void **base);
int ksceKernelFreeMemBlock(SceUID uid);
