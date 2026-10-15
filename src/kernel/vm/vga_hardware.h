/* Standard VGA state transport. The caller owns/quiesces the adapter.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VGA_HARDWARE_H
#define CIUKI_VGA_HARDWARE_H
#include "virtual_vga.h"
struct vga_hardware_bus {
    void *arg;
    uint8_t (*in)(void *, uint16_t);
    void (*out)(void *, uint16_t, uint8_t);
    uint8_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint8_t);
};
void vga_hardware_capture(cvga_state *, const struct vga_hardware_bus *);
void vga_hardware_load(const cvga_state *, const struct vga_hardware_bus *);
/* Bit 0: misc/enable, 1: sequencer, 2: GC, 3: CRTC, 4: attribute. */
unsigned vga_hardware_verify(const cvga_state *, const struct vga_hardware_bus *);
#endif
