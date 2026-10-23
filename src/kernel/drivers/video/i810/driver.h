/* Private i810 state, no userspace ABI. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VIDEO_I810_H
#define CIUKI_VIDEO_I810_H
#include "../native.h"
#include <ciuki/mm.h>
#define I810_PRIVATE_PAGES 17u
#define I810_RING_PAGES 16u
struct i810_engine {
    struct video_native native; /* first: accel hooks take this pointer */
    volatile uint8_t *ring, *cursor;
    uint32_t ring_phys, ring_bytes, tail, cursor_phys;
    uint32_t saved_ring[4], saved_cursor[3], saved_pixconf;
    uint8_t ring_saved[65536], cursor_saved[4096];
    bool ring_started, cursor_saved_valid;
    uint32_t dma_phys, aperture_bytes, gtt_first, gtt_root;
    uint32_t saved_pte[I810_PRIVATE_PAGES];
    unsigned gtt_modified;
    struct mm_pin pin;
};
int i810_engine_prepare(struct i810_engine *);
bool i810_gtt_region(struct i810_engine *, uint32_t, uint32_t, uint32_t *);
extern const struct display_driver i810_driver;
/* Physical allocation/cache/PCI boundary, replaced by the host device model. */
void *i810_dma_allocate(uint32_t *physical);
void i810_dma_free(uint32_t physical);
void i810_dma_publish(void);
uint32_t i810_aperture_bytes(const struct pci_func *);
#endif
