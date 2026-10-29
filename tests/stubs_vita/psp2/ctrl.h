#pragma once
#include <psp2/types.h>
#define SCE_CTRL_UP 0x10
#define SCE_CTRL_DOWN 0x40
#define SCE_CTRL_LEFT 0x80
#define SCE_CTRL_RIGHT 0x20
#define SCE_CTRL_CROSS 0x4000
#define SCE_CTRL_CIRCLE 0x2000
#define SCE_CTRL_LTRIGGER 0x100
#define SCE_CTRL_RTRIGGER 0x200
#define SCE_CTRL_MODE_DIGITAL 0
typedef struct SceCtrlData { uint64_t timeStamp; unsigned buttons; uint8_t lx, ly, rx, ry; uint8_t reserved[16]; } SceCtrlData;
int sceCtrlSetSamplingMode(int mode);
int sceCtrlPeekBufferPositive(int port, SceCtrlData *pad, int n);
