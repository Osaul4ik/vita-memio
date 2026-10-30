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
int ksceKernelVAtoPA(const void *va, uintptr_t *pa);
SceUID ksceKernelFindMemBlockByAddr(const void *addr, SceSize size);
typedef unsigned int SceUInt32;
SceUID ksceKernelProcUserMap(SceUID pid, const char *name, int permission, const void *user_buf,
                             SceSize size, void **kernel_page, SceSize *kernel_size,
                             SceUInt32 *kernel_offset);
int ksceKernelUserUnmap(SceUID uid);
int ksceKernelGetMemBlockMemtypeByAddr(const void *addr);
