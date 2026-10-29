#pragma once
#include <psp2/types.h>
#define SCE_KERNEL_POWER_TICK_DEFAULT 0
uint64_t sceKernelGetProcessTimeWide(void);
int sceKernelPowerTick(int type);
int sceKernelExitProcess(int res);
