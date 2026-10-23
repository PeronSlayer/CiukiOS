/* SPDX-License-Identifier: MIT
 * G13 (ER-1, SY-8): text of a native program fault for the "illegal
 * operation" dialog's Details panel and for the crash file. Header-only so
 * the dialog (client.c) and the file writer (win32_crash.c) share one
 * layout. The record is W32_REPORT_NATIVE_FAULT of <ciuki/w32_diagnostics.h>:
 * code = vector | error code << 8, offset = faulting address. */
#ifndef DESK_FAULT_TEXT_H
#define DESK_FAULT_TEXT_H
#include <stdint.h>
#include <stdio.h>
#include <ciuki/w32_diagnostics.h>

static inline const char *desk_fault_vector_name(uint32_t vector)
{
    switch (vector) {
    case 0: return "divide error";
    case 6: return "invalid opcode";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "floating-point error";
    case 17: return "alignment check";
    case 19: return "SIMD floating-point exception";
    default: return "processor exception";
    }
}
/* One line: what happened, in words (also the crash file's second block). */
static inline int desk_native_fault_what(const struct ciuki_w32_fault *f, char *out, size_t cap)
{
    uint32_t vector = f->code & 0xffu, error = f->code >> 8;
    if (vector == 14)
        return snprintf(out, cap, "Page fault: %s of %s address %08X",
                        error & 2 ? "write" : "read", error & 1 ? "protected" : "unmapped", f->offset);
    if (vector == 13)
        return snprintf(out, cap, "General protection fault (selector %04X)", error & 0xffffu);
    return snprintf(out, cap, "%s", desk_fault_vector_name(vector));
}
/* Details panel, at most about 520 bytes; returns the length or a negative
 * value on truncation. */
static inline int desk_native_fault_text(const struct ciuki_w32_fault *f, char *out, size_t cap)
{
    char what[96];
    desk_native_fault_what(f, what, sizeof(what));
    int n = snprintf(out, cap,
        "Program: %.63s (PID %u, thread %u)\n%s\nException: %02X, error code %06X\nEIP: %08X\n"
        "EAX: %08X  EBX: %08X  ECX: %08X\nEDX: %08X  ESI: %08X  EDI: %08X\nEBP: %08X  ESP: %08X  EFLAGS: %08X\nStack:",
        f->exe, f->pid, f->tid, what, f->code & 0xffu, f->code >> 8, f->eip,
        f->registers[0], f->registers[1], f->registers[2], f->registers[3], f->registers[4], f->registers[5],
        f->registers[6], f->registers[7], f->registers[8]);
    for (unsigned i = 0; i < 8 && n > 0 && (size_t)n < cap; i++) {
        int k = f->valid & (1u << i) ? snprintf(out + n, cap - (size_t)n, "%s%08X", i % 4 ? " " : "\n", f->stack[i])
                                     : snprintf(out + n, cap - (size_t)n, "%s????????", i % 4 ? " " : "\n");
        if (k < 0 || (size_t)k >= cap - (size_t)n) return -1;
        n += k;
    }
    return n > 0 && (size_t)n < cap ? n : -1;
}
#endif
