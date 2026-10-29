#pragma once
#include "../kernel/sysmem.h"
#define SCE_O_RDONLY 0x0001
#define SCE_O_WRONLY 0x0002
#define SCE_O_APPEND 0x0100
#define SCE_O_CREAT  0x0200
#define SCE_O_TRUNC  0x0400
SceUID ksceIoOpen(const char *path, int flags, int mode);
int ksceIoRead(SceUID fd, void *buf, SceSize n);
int ksceIoWrite(SceUID fd, const void *buf, SceSize n);
int ksceIoClose(SceUID fd);
int ksceIoRemove(const char *path);
int ksceIoRename(const char *from, const char *to);
