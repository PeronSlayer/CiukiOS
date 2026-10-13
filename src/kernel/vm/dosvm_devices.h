/* Per-VM device state; no physical I/O. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOSVM_DEVICES_H
#define CIUKI_DOSVM_DEVICES_H
#include <ciuki/dosvm.h>
#define DOSVM_PIT_CATCHUP 64u
#define DOSVM_KBD_BYTES 1152u /* legacy cvgp capacity: all held-key releases fit */
/* INT 33h callback continuation F000:1020 (INT FD; HLT). F000:1000-1011 is the
 * f3-03 EMS entry and its EMMXXXX0 name: sharing 1000h sent every callback
 * RETF into the EMS service (f3-01 host finding, 2026-10-11). */
#define DOSVM_MOUSE_RETURN 0x1020u
struct dosvm_pit_channel {
    uint32_t reload, phase, pending;
    uint16_t latch;
    uint8_t access, mode, write_phase, read_phase, latch_phase, write_byte, status;
    bool latched, status_latched, null_count, bcd, gate, running, out, load;
};
struct dosvm_pit {
    struct dosvm_pit_channel channel[3];
    uint8_t speaker;
    uint64_t edges, dropped_ticks, clocks;
};
struct dosvm_kbd {
    uint8_t bytes[DOSVM_KBD_BYTES], held[64];
    unsigned head, count;
    uint8_t command, output, controller_pending, keyboard_pending, scan_set, leds, last;
    bool scanning, enabled;
    uint8_t bios_prefix, shift;
    uint32_t reads; /* bytes consumed from port 60h, including synthetic INT 9 */
    uint64_t overflows;
};
struct dosvm_mouse {
    int32_t x, y, motion_x, motion_y, fraction_x, fraction_y, visible;
    uint16_t min_x, max_x, min_y, max_y, ratio_x, ratio_y;
    uint16_t mask, callback_cs, callback_ip, pending_mask, screen_mask, cursor_mask;
    uint8_t buttons, text_type;
    uint16_t pressed[3], released[3], press_x[3], press_y[3], release_x[3], release_y[3];
    bool active, saved_vif;
    struct v86_frame saved;
};
void dosvm_pit_init(struct dosvm *);
int dosvm_pit_port(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
void dosvm_pit_advance(struct dosvm *, uint64_t host_ticks);
void dosvm_kbd_init(struct dosvm *);
int dosvm_kbd_port(struct dosvm *, uint16_t, unsigned, bool, uint32_t *, void *);
bool dosvm_kbd_event(struct dosvm *, unsigned code, int value);
void dosvm_kbd_release(struct dosvm *);
int dosvm_kbd_bios(struct dosvm *);
bool dosvm_kbd_irq_ready(const struct dosvm *);
void dosvm_mouse_init(struct dosvm *);
void dosvm_mouse_event(struct dosvm *, int32_t dx, int32_t dy, unsigned buttons);
int dosvm_mouse_bios(struct dosvm *, struct v86_frame *);
int dosvm_mouse_dispatch(struct dosvm *, struct v86_frame *);
int dosvm_mouse_return(struct dosvm *, struct v86_frame *);
void dosvm_mouse_text(struct dosvm *);
#endif
