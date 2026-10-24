/* Standard VGA state transport. The caller owns/quiesces the adapter.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VGA_HARDWARE_H
#define CIUKI_VGA_HARDWARE_H
#include <stdbool.h>
#include "virtual_vga.h"
struct vga_hardware_bus {
    void *arg;
    uint8_t (*in)(void *, uint16_t);
    void (*out)(void *, uint16_t, uint8_t);
    uint8_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint8_t);
};
void vga_hardware_capture(cvga_state *, const struct vga_hardware_bus *);
/* fx-22: registers plus the selected 4 KiB plane pages (bit n = offsets
 * n*4096.., all planes); `expired` is polled between pages. Returns the
 * pages captured. */
#define VGA_HARDWARE_ALL_PAGES 0xFFFFu
uint16_t vga_hardware_capture_pages(cvga_state *, const struct vga_hardware_bus *, uint16_t pages,
                                    bool (*expired)(void *), void *);
void vga_hardware_load(const cvga_state *, const struct vga_hardware_bus *);
/* Native providers bracket the planar transfer with their chipset aperture
 * controls and keep scanout blank until panel/streams have been restored. */
void vga_hardware_read_registers(cvga_state *, const struct vga_hardware_bus *, bool dac8);
void vga_hardware_capture_planes(cvga_state *, const struct vga_hardware_bus *);
void vga_hardware_load_blanked(const cvga_state *, const struct vga_hardware_bus *);
void vga_hardware_unblank(const cvga_state *, const struct vga_hardware_bus *);
/* Bit 0: misc/enable, 1: sequencer, 2: GC, 3: CRTC, 4: attribute. */
unsigned vga_hardware_verify(const cvga_state *, const struct vga_hardware_bus *);
#endif
