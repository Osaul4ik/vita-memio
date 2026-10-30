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
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
                             int stackSize, unsigned attr, int cpuAffinityMask, const void *option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int sceKernelWaitThreadEnd(SceUID thid, int *stat, unsigned *timeout);
int sceKernelDeleteThread(SceUID thid);
int sceKernelDelayThread(unsigned us);
