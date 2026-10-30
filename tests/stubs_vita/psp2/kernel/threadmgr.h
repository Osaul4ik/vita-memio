#pragma once
#include <psp2/types.h>
/* Only the fields the tester reads, at their places in the real struct. */
typedef struct SceKernelThreadInfo {
	SceSize  size;
	SceUID   processId;
	char     name[32];
	unsigned attr, status;
	void    *entry, *stack;
	int      stackSize, initPriority, currentPriority, initCpuAffinityMask,
	         currentCpuAffinityMask, currentCpuId, lastExecutedCpuId;
	unsigned waitType;
	SceUID   waitId;
	int      exitStatus;
	uint64_t runClocks;
	unsigned intrPreemptCount, threadPreemptCount, threadReleaseCount;
	int      changeCpuCount, fNotifyCallback, reserved;
} SceKernelThreadInfo;
SceUID sceKernelGetThreadId(void);
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info);
