/*
 * vm_memblock.h - ksceKernelMemBlockGetInfoEx (SceSysmemForDriver, NID
 * 0x24A99FFF), which the pinned VitaSDK's stub library has but its headers do
 * not declare. The layout is that of SceKernelMemBlockInfoEx in newer
 * vita-headers (0xB8 bytes on the Vita); only `type` is used, and its use
 * checks it against the memory type at the block's address (kernel/stage.c),
 * so a wrong layout is refused, never trusted.
 */
#ifndef VITAMEMIO_VM_MEMBLOCK_H
#define VITAMEMIO_VM_MEMBLOCK_H

#include <stdint.h>
#include <psp2kern/kernel/sysmem.h>

typedef struct VmMemBlockInfoEx {
	SceSize  size;                  /* sizeof(VmMemBlockInfoEx)              */
	uint32_t type;                  /* SceKernelMemBlockType                 */
	uint32_t guid;
	const char *name;
	void    *mapped_base;
	SceSize  mapped_size;
	SceSize  alloc_map_size;
	int      extra_low;
	int      extra_high;
	uint32_t paging_type;
	uint32_t partition_guid;
	void    *phy_mem_part;
	SceSize  physical_range_count;
	SceSize  physical_range_count_written;
	void    *physical_addresses[16];
	SceSize  physical_sizes[16];
} VmMemBlockInfoEx;

int ksceKernelMemBlockGetInfoEx(SceUID uid, VmMemBlockInfoEx *info);

#endif /* VITAMEMIO_VM_MEMBLOCK_H */
