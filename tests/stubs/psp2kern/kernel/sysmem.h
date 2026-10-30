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
typedef struct SceKernelMemBlockInfoCore {
	unsigned int type, guid;
	const char  *name;
	void        *mapped_base;
	SceSize      mapped_size, alloc_map_size;
	int          extra_low, extra_high;
	unsigned int paging_type, partition_guid;
	void        *phy_mem_part;
} SceKernelMemBlockInfoCore;
typedef struct SceKernelMemBlockInfoEx {
	SceSize size;
	SceKernelMemBlockInfoCore core_info;
	SceSize physical_range_count, physical_range_count_written;
	void   *physical_addresses[16];
	SceSize physical_sizes[16];
} SceKernelMemBlockInfoEx;
int ksceKernelMemBlockGetInfoEx(SceUID uid, SceKernelMemBlockInfoEx *info);
int ksceKernelGetMemBlockMemtypeByAddr(const void *addr);
