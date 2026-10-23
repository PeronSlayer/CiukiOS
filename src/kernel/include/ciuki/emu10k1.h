/* Private F4 EMU voice/ADC interface. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_EMU10K1_H
#define CIUKI_EMU10K1_H
#include <ciuki/audio_backend.h>
extern const struct audio_dma_ops emu10k1_ops;
bool emu10k1_match(const struct pci_func *, enum audio_backend_kind *);
bool emu10k1_kind(enum audio_backend_kind);
unsigned emu10k1_aux_pages(enum audio_backend_kind);
uint32_t emu10k1_dma_mask(enum audio_backend_kind);
int emu10k1_codec_write(struct audio_dma_device *, uint16_t, uint16_t);
int emu10k1_restore(struct audio_dma_device *);
/* Kernel buffers only. Capture is deliberately absent from the public audio
 * and virtual SB16 ABIs. Halting the transport also halts capture. */
int emu10k1_capture_start(struct audio_dma_device *, unsigned rate);
int emu10k1_capture_read(struct audio_dma_device *, struct audio_frame *, unsigned frames);
int emu10k1_capture_stop(struct audio_dma_device *);
#endif
