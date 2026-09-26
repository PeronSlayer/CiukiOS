/* CiukiOS software VGA device. No host ports, BIOS calls, allocator or clock.
 *
 * This is a device model, NOT a DOS VM or a replacement BIOS. A monitor must
 * route every guest VGA port and A0000-BFFFF access here, including DPMI maps.
 * Directly mapping these planes into a guest does NOT implement VGA latches.
 *
 * Reference: Chips & Technologies 82C452 Data Sheet, revision 2.1, September
 * 1991, standard VGA registers pp. 31-64:
 * https://www.dosdays.co.uk/media/c_and_t/82C452_VGA_Controller_Sep91.pdf
 * Cross-check: QEMU v9.2.0 hw/display/vga.c, vga_mem_readb/writeb and scanout:
 * https://github.com/qemu/qemu/blob/v9.2.0/hw/display/vga.c
 * Physical chain-4 organization follows DOSBox-X's hardware-tested generic
 * VGA_ChainedVGA_Slow_Handler (not the distinct Tseng ET4000 behavior):
 * https://github.com/joncampbell123/dosbox-x/blob/dosbox-x-v2025.05.03/src/hardware/vga_memory.cpp
 * Scanout byte/word/dword strides are cross-checked against vga_draw.cpp at
 * the same tag. Chipset-specific C&T/Tseng extended address XORs are excluded.
 * This implementation uses PHYSICAL plane addresses. Unlike QEMU's compacted
 * representation, CRTC word/dword addressing changes scanout, not CPU access.
 * For example chained CPU bytes 0..7 occupy plane addresses 0 and 4; an
 * unchained Mode X framebuffer uses consecutive addresses in each plane.
 */
#ifndef CIUKIOS_VIRTUAL_VGA_H
#define CIUKIOS_VIRTUAL_VGA_H

#include <stdint.h>

#define CVGA_PLANE_SIZE 65536UL
#define CVGA_STATE_BYTES 262992UL
#define CVGA_BLINK_VISIBLE 1u
#define CVGA_CURSOR_VISIBLE 2u

#if defined(__WATCOMC__)
#define CVGA_CALL __cdecl
#else
#define CVGA_CALL
#endif

typedef struct cvga_state {
    uint8_t plane[4][CVGA_PLANE_SIZE];
    uint8_t latch[4];
    uint8_t seq[5], gc[9], crtc[25], attr[21];
    uint8_t dac[256][3];              /* six bits per component */
    uint8_t seq_index, gc_index, crtc_index, attr_index, attr_data_phase;
    uint8_t misc, feature, enable;
    uint8_t dac_mask, dac_index, dac_component, dac_read_mode;
    uint32_t changes;                /* monotonically wrapping invalidation */
} cvga_state;
typedef char cvga_state_layout_must_match[(sizeof(cvga_state) == CVGA_STATE_BYTES) ? 1 : -1];

typedef struct cvga_geometry {
    unsigned width, height;          /* repeated rows removed only when identical */
    unsigned scan_repeat;            /* physical scanlines per returned row */
    unsigned text, char_width, char_height;
    unsigned blank;                  /* presenter must output black, not DAC[0] */
} cvga_geometry;

void CVGA_CALL cvga_init(cvga_state *v);
/* Register presets only; DAC and font are supplied by the guest/BIOS adapter.
 * mode may contain bit 7 (preserve video memory). Other modes return 0 without
 * changing state. preserve_vram also overrides clearing. No BIOS-data updates.
 */
int CVGA_CALL cvga_set_bios_mode(cvga_state *v, unsigned mode, int preserve_vram);
/* Installs one exact caller-provided 256 x 16 font into plane 2, font bank 0. */
void CVGA_CALL cvga_load_font_8x16(cvga_state *v, const uint8_t *font4096);
/* status1 is supplied by monitor timing: bit 0 display-disabled, bit 3 retrace.
 * It is never advanced as a side effect of polling a port.
 */
uint8_t CVGA_CALL cvga_read_port(cvga_state *v, uint16_t port, uint8_t status1);
void CVGA_CALL cvga_write_port(cvga_state *v, uint16_t port, uint8_t value);
/* Full physical guest address, not an unchecked offset. Out-of-aperture reads
 * return FF and writes are ignored. Read accesses load all four VGA latches.
 */
uint8_t CVGA_CALL cvga_read_vram(cvga_state *v, uint32_t address);
void CVGA_CALL cvga_write_vram(cvga_state *v, uint32_t address, uint8_t value);
/* Supported scanout: normal-address VGA text, 16-colour planar graphics,
 * mode 13h and unchained 256-colour (Mode X), with CRTC start, pitch, split,
 * horizontal panning, font banks, cursor and blink. CGA shift/interleave,
 * custom shift-load/count divisors, VBE and chipset extensions return 0.
 * Extended-memory-disable compatibility mappings are deliberately unsupported;
 * memory accesses return FF/ignore writes while SR04 bit 1 is clear.
 */
int CVGA_CALL cvga_get_geometry(const cvga_state *v, cvga_geometry *g);
/* No partial output: insufficient capacity or unsupported mode returns 0.
 * Outputs DAC indexes after attribute lookup and pixel mask, not RGB. A
 * blanked screen emits zero indexes; geometry.blank tells the presenter to
 * bypass the DAC and use black even when palette entry zero is nonblack.
 * Blink/cursor phases are explicitly supplied; no timer or host coupling.
 */
unsigned CVGA_CALL cvga_render_row8(const cvga_state *v, unsigned y, uint8_t *dest,
                                   unsigned capacity, unsigned frame_flags);

#endif
