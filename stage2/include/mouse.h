#ifndef STAGE2_MOUSE_H
#define STAGE2_MOUSE_H

#include "types.h"

/*
 * SR-MOUSE-001 follow-up — PS/2 mouse driver (IRQ12).
 *
 * Low-level driver. Accumulates motion deltas and button state in
 * stage2, then exposes them to the INT 33h dispatcher in shell.c.
 * Absolute-position tracking + range clipping remains owned by the
 * INT 33h state (shell.c), keeping the DOS driver semantics isolated
 * from the hardware path.
 *
 * The driver is tolerant of hosts that never deliver IRQ12 (e.g.
 * headless QEMU without PS/2 mouse emulation): initialization is
 * best-effort and failures leave the state in a safe zero-motion,
 * zero-button fallback.
 */

/* Bit layout for the button mask returned by the driver:
 * bit0 = left, bit1 = right, bit2 = middle. */
#define STAGE2_MOUSE_BTN_LEFT   0x01U
#define STAGE2_MOUSE_BTN_RIGHT  0x02U
#define STAGE2_MOUSE_BTN_MIDDLE 0x04U

/*
 * stage2_mouse_init — best-effort probe/initialize the PS/2 mouse
 * through the 8042 controller. Unmasks IRQ12 and the PIC2 cascade
 * IRQ2 on success. Silent on hosts without a PS/2 mouse.
 * Returns 1 on success, 0 on failure (driver stays in safe fallback).
 */
int stage2_mouse_init(void);

/*
 * stage2_mouse_on_irq12 — IRQ12 entry (invoked from the stub).
 * Reads one byte from the 8042 data port, assembles 3-byte packets,
 * and updates internal deltas + button mask.
 */
void stage2_mouse_on_irq12(void);

/*
 * stage2_mouse_consume_deltas — atomically drain the accumulated
 * motion deltas and read the latest button mask. The deltas are
 * cleared on read; button mask is a snapshot (level state).
 */
void stage2_mouse_consume_deltas(i32 *out_dx, i32 *out_dy, u16 *out_buttons);

/* Diagnostics. */
u64 stage2_mouse_irq_count(void);
int stage2_mouse_is_present(void);

#endif
