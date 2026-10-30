#pragma once
#include "sysmem.h"
typedef unsigned int SceUInt;
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
int ksceKernelDelayThread(unsigned us);
uint32_t ksceKernelGetSystemTimeLow(void);
SceUID ksceKernelGetProcessId(void);
SceUID ksceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority,
                              SceSize stackSize, SceUInt attr, int cpuAffinityMask,
                              const void *option);
int ksceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int ksceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout);
int ksceKernelDeleteThread(SceUID thid);
SceUID ksceKernelCreateSema(const char *name, SceUInt attr, int initVal, int maxVal, void *option);
int ksceKernelDeleteSema(SceUID semaid);
int ksceKernelSignalSema(SceUID semaid, int signal);
int ksceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout);
/* only the fields the plugin reads, at their places in the real struct */
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
SceUID ksceKernelGetThreadId(void);
int ksceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info);
