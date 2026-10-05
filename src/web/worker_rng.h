#ifndef CIUK_WORKER_RNG_H
#define CIUK_WORKER_RNG_H

#include <stdint.h>

/* Map and initialise the 64 KiB virtio DMA arena at the supplied physical
 * base. Returns 0 on success, -1 on absent/invalid device or mapping. */
int wrng_init(uint32_t physical_dma_base);
/* Returns 0 before initialization or after success, otherwise the failing
 * initialization stage: 1 PCI discovery, 2 BAR, 3 device features, 4 queue,
 * 5 physical mapping, 6 selector allocation, 7 selector setup, 8 layout,
 * 9 device status. */
int wrng_last_error(void);
/* Returns 0 on success, -1 on bounded-wait/device/buffer failure. */
int wrng_read(void *output, uint32_t bytes);
void wrng_shutdown(void);

#endif
