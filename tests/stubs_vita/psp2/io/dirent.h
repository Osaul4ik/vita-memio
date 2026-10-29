#pragma once
#include <psp2/types.h>
SceUID sceIoDopen(const char *d);
int sceIoDclose(SceUID fd);
