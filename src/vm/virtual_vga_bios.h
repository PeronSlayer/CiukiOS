/* CiukiOS virtual VGA BIOS (INT 10h) operating on the software VGA model.
 *
 * Replaces physical video firmware for a monitored DOS session: the physical
 * display stays with the host. Functions update the guest BIOS Data Area
 * (0040:0049-008A) and INT 43h exactly where the IBM VGA BIOS does, so the
 * caller must snapshot/restore those bytes around a session. Guest buffers
 * (ES:DX DAC blocks, ES:BP fonts/strings) are reached through the bus.
 *
 * Implemented: 00h modes 00-03/0D/0E/10-13, 01-03, 05-0F, 10h (00-03, 07-10,
 * 12-13, 15, 17-1B), 11h (00-04, 10-14, 20-24, 30), 12h (BL=10/30-34/36),
 * 13h, 1Ah. VBE (4Fh), 1Bh, 1Ch and CGA/monochrome modes are deliberately
 * absent: registers are returned unchanged, exactly like firmware without
 * that function, and the call is counted as unsupported.
 */
#ifndef CIUKIOS_VIRTUAL_VGA_BIOS_H
#define CIUKIOS_VIRTUAL_VGA_BIOS_H

#include "vga_x86.h"

typedef struct cvbios_regs {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint16_t es, ds;
} cvbios_regs;

/* Far pointers (segment << 16 | offset) to firmware fonts, supplied by the
 * session owner from the real INT 10h/1130h before a session starts. Zero
 * means unavailable: text renders without glyphs and 11h ROM loads fail. */
enum { CVBIOS_FONT_8X8 = 0, CVBIOS_FONT_8X8_HIGH, CVBIOS_FONT_8X14,
       CVBIOS_FONT_8X16, CVBIOS_FONT_COUNT };

typedef struct cvbios {
    cvga_state *vga;
    const cvx_bus *bus;              /* guest linear memory, not the aperture */
    uint32_t font[CVBIOS_FONT_COUNT];
    uint32_t calls, unsupported, mode_sets;
    uint16_t last_unsupported;       /* AX of the latest unsupported call */
    uint16_t reserved;
} cvbios;

/* Default DAC tables of the IBM VGA BIOS: 1 = 64-entry CGA-compatible,
 * 2 = 64-entry EGA, 3 = 256-entry VGA. Returns entry count, 0 if invalid.
 * dest receives count*3 six-bit components. */
unsigned CVGA_CALL cvbios_default_palette(unsigned which, uint8_t *dest);
/* Returns 1 when handled, 0 when unsupported (registers untouched). */
int CVGA_CALL cvbios_int10(cvbios *b, cvbios_regs *r);
/* Current BIOS-visible mode from BDA 0040:0049. */
unsigned CVGA_CALL cvbios_current_mode(const cvbios *b);

#endif
