#pragma once
#include <psp2/types.h>
typedef struct SceDateTime { unsigned short year, month, day, hour, minute, second; unsigned microsecond; } SceDateTime;
int sceRtcGetCurrentClockLocalTime(SceDateTime *t);
