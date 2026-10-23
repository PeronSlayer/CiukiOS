/* Private C-Media transport state/API. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CMIPCI_H
#define CIUKI_CMIPCI_H
#include <stdint.h>
#include <stdbool.h>
struct audio_frame;
struct audio_dma_device;
struct cmipci_state {
    struct audio_frame *pcm;
    uint32_t phys, rate, misc, ext;
    uint64_t observed, progressed, produced, consumed, irqs, overruns;
    unsigned frame, model;
    uint8_t mixer[8], native[2];
    bool running, failed, saved;
};
/* Thread context, copied kernel buffers only; no public recording ABI.
 * Reads return whole 256-frame periods, or -EAGAIN; stale DMA is -EIO. */
int cmipci_capture_start(struct audio_dma_device *, unsigned rate);
int cmipci_capture_read(struct audio_dma_device *, struct audio_frame *);
int cmipci_capture_stop(struct audio_dma_device *);
int cmipci_capture_prove(struct audio_dma_device *);
int cmipci_restore(struct audio_dma_device *);
#endif
