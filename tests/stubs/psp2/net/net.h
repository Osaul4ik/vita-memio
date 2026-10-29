#pragma once
#include <stdint.h>
typedef struct { uint8_t sin_len, sin_family; uint16_t sin_port; struct { uint32_t s_addr; } sin_addr; char pad[8]; } SceNetSockaddrIn;
typedef struct { char x[16]; } SceNetSockaddr;
#define SCE_NET_AF_INET 2
#define SCE_NET_SOCK_STREAM 1
#define SCE_NET_INADDR_ANY 0
#define SCE_NET_SOL_SOCKET 0xffff
#define SCE_NET_SO_RCVTIMEO 0x1006
#define SCE_NET_SO_SNDTIMEO 0x1005
int sceNetSocket(const char *n, int d, int t, int p);
int sceNetBind(int s, const SceNetSockaddr *a, unsigned l);
int sceNetListen(int s, int b);
int sceNetAccept(int s, SceNetSockaddr *a, unsigned *l);
int sceNetRecv(int s, void *b, unsigned n, int f);
int sceNetSend(int s, const void *b, unsigned n, int f);
int sceNetSocketClose(int s);
int sceNetSetsockopt(int s, int lvl, int opt, const void *v, unsigned l);
uint16_t sceNetHtons(uint16_t v);
uint32_t sceNetHtonl(uint32_t v);
