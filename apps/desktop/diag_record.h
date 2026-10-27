/* fx-accept: one diagnostic line per desktop event, also in quiet boots.
 *
 * The native build writes the line through diagnostic call 2
 * (ciuki_raw_debug_write): the kernel prefixes "[user <task>] " and sends it
 * to the serial port, the console and the boot log, so the acceptance suite
 * (scripts/test/acceptance.py) and the T23 serial captures read the same
 * record. Host builds print it on stderr only when CIUKI_DESK_DIAG is set,
 * which keeps the host harnesses' output unchanged. Lines longer than the
 * 240 bytes debug_write accepts are dropped, never cut.
 * SPDX-License-Identifier: MIT */
#ifndef DESK_DIAG_RECORD_H
#define DESK_DIAG_RECORD_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __i386__
#include <ciuki/raw.h>
#endif

static inline void desk_diag_record(const char *line, int n)
{
    if (!line || n <= 0 || n > 240) return;
#ifdef __i386__
    (void)ciuki_raw_debug_write((uint32_t)(uintptr_t)line, (uint32_t)n, 0, 0, 0, 0);
#else
    if (getenv("CIUKI_DESK_DIAG")) fprintf(stderr, "%.*s\n", n, line);
#endif
}

/* A record token: [A-Za-z0-9._-] kept, any other byte '_', "-" when empty. */
static inline void desk_diag_token(char *out, size_t cap, const char *text)
{
    size_t n = 0;
    if (!cap) return;
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p && n + 1 < cap; p++) {
        unsigned char c = *p;
        out[n++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '_' || c == '-' ? (char)c : '_';
    }
    if (!n && cap > 1) out[n++] = '-';
    out[n] = 0;
}
#endif
