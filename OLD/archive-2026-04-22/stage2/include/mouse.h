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

/*
 * OPENGEM-005 — Guarded OpenGEM session bridge.
 *
 * `int33_hooks_t` is an append-only surface that lets a DOS-native
 * OpenGEM build install its own INT 33h event callback(s) without
 * disturbing the stage2 fallback path. All fields are nullable.
 * Stage2 only calls non-null hooks; the fallback CiukiOS cursor
 * is quiesced automatically while an OpenGEM session is active.
 *
 * Append-only: new callback slots only at the tail of the struct;
 * never reorder existing ones. `version` lets future consumers
 * gate against minimum field count.
 */
typedef struct int33_hooks {
    u32  version;                       /* must be >= 1 */
    void (*on_session_enter)(void);     /* called before shell_run (OPENGEM-005) */
    void (*on_session_exit)(void);      /* called after shell_run returns     */
    void (*on_mouse_event)(u16 buttons, /* optional per-event tap; OpenGEM    */
                           i32 dx, i32 dy); /* implementers may synthesize INT 33h state */
} int33_hooks_t;

#define STAGE2_INT33_HOOKS_VERSION 1U

/*
 * Install or clear the hook table. Passing NULL clears hooks and
 * reactivates the default fallback cursor in mode 13h. The
 * function is idempotent and null-safe.
 *
 * Emits `[ mouse ] opengem hook installed` on a non-null install.
 */
void stage2_mouse_set_opengem_hooks(const int33_hooks_t *hooks);

/*
 * Called by the OpenGEM launcher helper to bracket the session.
 * Idempotent; safe to re-enter. Emits the frozen markers
 * `[ mouse ] opengem session: cursor disabled` and
 * `[ mouse ] opengem session: cursor restored`.
 */
void stage2_mouse_opengem_session_enter(void);
void stage2_mouse_opengem_session_exit(void);

/*
 * Read the "cursor quiesced" flag — 1 while an OpenGEM session owns
 * the screen, 0 otherwise. The mode-13h cursor blitter consults
 * this flag before painting. Always safe to call.
 */
int  stage2_mouse_opengem_cursor_quiesced(void);

#endif
