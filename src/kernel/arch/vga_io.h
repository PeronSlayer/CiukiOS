/* Task-local VGA permissions, Intel SDM I/O bitmap (0 permits, 1 denies).
 * The caller initializes the whole map and sentinel to FFh once.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VGA_IO_H
#define CIUKI_VGA_IO_H
#include <stdint.h>
#include <stdbool.h>
static inline void arch_vga_bitmap(uint8_t *bitmap, bool enabled)
{
    for (unsigned i=0x3B0/8;i<0x3E0/8;i++) bitmap[i]=enabled ? 0 : 0xFF;
}
#endif
