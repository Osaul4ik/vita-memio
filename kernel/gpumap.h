/*
 * gpumap.h - watching the memory games map for the GPU (see gpumap.c).
 */
#ifndef VITAMEMIO_GPUMAP_H
#define VITAMEMIO_GPUMAP_H

#include <stdint.h>

/* From the 1 Hz worker: installs the watch the first time `wanted` is set
 * and writes what it recorded to the log. */
void vm_gpumap_tick(int wanted, uint32_t uptime_s);
void vm_gpumap_fini(void);

#endif /* VITAMEMIO_GPUMAP_H */
