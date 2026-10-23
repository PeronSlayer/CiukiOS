/* Task-local VGA permissions, Intel SDM I/O bitmap (0 permits, 1 denies).
 * The caller initializes the whole map and sentinel to FFh once.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VGA_IO_H
#define CIUKI_VGA_IO_H
#include <stdint.h>
#include <stdbool.h>
#include <ciuki/vga_ports.h>
static inline void arch_vga_bitmap(uint8_t *bitmap, bool enabled)
{
    /* Per-port bits: 3BCh-3BFh is a parallel port and stays denied. */
    for (unsigned port=CIUKI_VGA_MONO_FIRST;port<CIUKI_VGA_COLOR_END;port++) {
        bool vga=ciuki_vga_port(port);
        if (vga && enabled) bitmap[port/8]&=(uint8_t)~(1u<<(port%8));
        else bitmap[port/8]|=(uint8_t)(1u<<(port%8));
    }
}
#endif
