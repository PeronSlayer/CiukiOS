/* Fatal-error screen of the panic path (G13, ER-2): the Windows 98 "fatal
 * exception" text with a restart choice, and the PS/2 key decoder that picks
 * the choice. Pure parts are host-tested (tests/host/fatal_test.c).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FATAL_H
#define CIUKI_FATAL_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* One fatal event. frame=false is a panic() with only a reason string. */
struct fatal_report {
    bool frame;
    uint32_t vector, error, cs, eip, cr2;
    const char *task, *reason;
};
#define FATAL_SCREEN_BYTES 768u
/* The whole screen text (80 columns at most) into out; returns its length. */
size_t fatal_format(const struct fatal_report *, char *out, size_t capacity);

enum fatal_choice { FATAL_NONE, FATAL_RESTART, FATAL_STAY };
/* Keyboard controller byte decoder. translated: the 8042 converts set 2 to
 * set 1 (command byte bit 6); the kernel driver leaves it off. Ctrl+Alt+Del
 * and Enter restart; Esc keeps the screen. */
struct fatal_keys { bool translated, extended, release, ctrl, alt; };
enum fatal_choice fatal_key_feed(struct fatal_keys *, uint8_t byte);

/* Draws the screen on the console only (serial stays untouched), then waits
 * for the choice: restart through kernel_reboot, or halt for good. */
__attribute__((noreturn)) void fatal_screen(const struct fatal_report *);
#endif
