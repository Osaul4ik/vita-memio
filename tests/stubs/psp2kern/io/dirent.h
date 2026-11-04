#pragma once
#include "../kernel/sysmem.h"
typedef struct SceIoDirent {
	unsigned char d_stat[0x58];
	char d_name[256];
	void *d_private;
	int dummy;
} SceIoDirent;
SceUID ksceIoDopen(const char *dirname);
int ksceIoDread(SceUID fd, SceIoDirent *dir);
int ksceIoDclose(SceUID fd);
