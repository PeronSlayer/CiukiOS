#ifndef CIUKIOS_SESSION_GPU_LEGACY_H
#define CIUKIOS_SESSION_GPU_LEGACY_H

#include <stdint.h>

/* VM_FB mode-packet BPP bit: the active VBE mode uses a banked window. */
#define CVLEGACY_VBE_BANKED_FLAG 0x8000U

/* Mode-preserving scanout-base backend; firmware establishes the VBE mode. */
uint32_t cvlegacy_bind(uint32_t physical, uint32_t bytes,
                       uint32_t width, uint32_t height,
                       uint32_t pitch, uint32_t bpp);
/* True only for a validated QEMU-compatible PCI framebuffer BAR alias. */
uint32_t cvlegacy_vbe_alias(uint32_t physical);
uint32_t cvlegacy_present(uint32_t yoffset);
uint32_t cvlegacy_release(void);

#endif
