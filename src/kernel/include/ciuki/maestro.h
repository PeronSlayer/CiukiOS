/* Private Maestro engine/capture interface. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MAESTRO_H
#define CIUKI_MAESTRO_H
#include <ciuki/audio_backend.h>
#define MAESTRO_ARENA_BYTES 0x20000u
#define MAESTRO_PLAY_OFFSET 0x1000u
#define MAESTRO_MIX_OFFSET 0x9000u
#define MAESTRO_CAPTURE_OFFSET 0x10000u
extern const struct audio_dma_ops maestro_ops;
bool maestro_match(const struct pci_func *, enum audio_backend_kind *);
bool maestro_kind(enum audio_backend_kind);
int maestro_restore(struct audio_dma_device *);
/* Kernel-owned copied S16LE stereo diagnostic capture; no public/guest ABI.
 * source is AC'97 MIC=0 or LINE=4; token is invalidated by every full stop.
 * Capture stop proves only its four APUs idle; playback keeps its lease. */
int maestro_capture_start(struct audio_dma_device *, unsigned rate, unsigned source, uint32_t *token);
int maestro_capture_read(struct audio_dma_device *, uint32_t token, struct audio_frame *, unsigned frames);
int maestro_capture_stop(struct audio_dma_device *, uint32_t token);
#endif
