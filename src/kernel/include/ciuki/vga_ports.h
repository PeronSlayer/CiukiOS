/* Legacy VGA I/O ownership, one definition for every claimant.
 *
 * The decoded VGA register file is two ranges, not one: the monochrome
 * aliases 3B0h-3BBh and the colour/common block 3C0h-3DFh. 3BCh-3BFh is
 * not VGA; 3BCh is the first parallel-port base of the PC (BIOS LPT1 on
 * many portables, the T23 among them). Windows 9x/NT assigns a VGA adapter
 * exactly these two ranges and leaves 3BCh to the parallel port. Claiming
 * 3B0h-3DFh as one span makes the display driver collide with LPT1 there
 * and refuses acceleration (T23 trial 9a60215f, directive fx-1).
 *
 * Ends are exclusive, as everywhere in the resource registry. A driver that
 * never touches the monochrome aliases claims only the colour range.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VGA_PORTS_H
#define CIUKI_VGA_PORTS_H
#include <stdint.h>
#include <stdbool.h>

#define CIUKI_VGA_MONO_FIRST  0x3B0u
#define CIUKI_VGA_MONO_END    0x3BCu /* exclusive; 3BCh.. belongs to a parallel port */
#define CIUKI_VGA_COLOR_FIRST 0x3C0u
#define CIUKI_VGA_COLOR_END   0x3E0u /* exclusive */

static inline bool ciuki_vga_mono_port(uint32_t port)
{ return port >= CIUKI_VGA_MONO_FIRST && port < CIUKI_VGA_MONO_END; }
static inline bool ciuki_vga_color_port(uint32_t port)
{ return port >= CIUKI_VGA_COLOR_FIRST && port < CIUKI_VGA_COLOR_END; }
/* True when the port is a VGA register (either range). */
static inline bool ciuki_vga_port(uint32_t port)
{ return ciuki_vga_mono_port(port) || ciuki_vga_color_port(port); }
/* True when every byte of an access of `width` bytes is a VGA register. */
static inline bool ciuki_vga_port_span(uint32_t port, unsigned width)
{
    if (!width || width > 4) return false;
    for (unsigned i = 0; i < width; i++) if (!ciuki_vga_port(port + i)) return false;
    return true;
}
#endif
