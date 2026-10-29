#pragma once
typedef int SceUID;
typedef unsigned SceSize;
int sceKernelDelayThread(unsigned us);
SceUID sceKernelCreateThread(const char *n, int (*f)(SceSize, void *), int prio, int stack, int attr, int cpu, void *opt);
int sceKernelStartThread(SceUID t, SceSize a, void *p);
int sceKernelWaitThreadEnd(SceUID t, int *st, unsigned *to);
int sceKernelDeleteThread(SceUID t);
