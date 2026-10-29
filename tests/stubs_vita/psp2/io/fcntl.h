#pragma once
#include <psp2/types.h>
#define SCE_O_RDONLY 1
#define SCE_O_WRONLY 2
#define SCE_O_RDWR 3
#define SCE_O_CREAT 0x200
#define SCE_O_TRUNC 0x400
SceUID sceIoOpen(const char *f, int flags, SceMode mode);
int sceIoClose(SceUID fd);
int sceIoRead(SceUID fd, void *d, SceSize n);
int sceIoWrite(SceUID fd, const void *d, SceSize n);
int sceIoPread(SceUID fd, void *d, SceSize n, SceOff off);
int sceIoPwrite(SceUID fd, const void *d, SceSize n, SceOff off);
int sceIoRemove(const char *f);
