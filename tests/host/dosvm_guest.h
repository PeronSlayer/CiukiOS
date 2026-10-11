/* Interface between the f3-04 host runner (production DOS VM + DPMI code)
 * and its libx86emu guest CPU wrapper. SPDX-License-Identifier: GPL-2.0-only */
#ifndef DOSVM_GUEST_H
#define DOSVM_GUEST_H
#include <stdbool.h>
#include <stdint.h>

struct gcpu_state {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
    uint16_t cs, ss, ds, es, fs, gs;
    uint32_t ldt_limit;
    bool pm;                    /* ring-3 DPMI client, else V86 (run as real mode) */
};
struct gcpu_event {
    unsigned vector;            /* trap/fault vector delivered to dosvm_trap */
    uint32_t err, cr2;
    bool fault;                 /* EIP at the instruction (else after it) */
    bool executed_descriptor;
    unsigned executed;
    uint32_t dr6;               /* data breakpoint hits (trap after the instruction) */
    bool ss_shadow;             /* last instruction loaded SS: no interrupt before the next */
};
struct gcpu_ops {
    bool (*read)(uint32_t linear, void *buffer, unsigned bytes);
    bool (*write)(uint32_t linear, const void *buffer, unsigned bytes);
    void (*descriptor)(uint16_t selector, bool write, uint32_t d[2]);
    void (*unexpected)(const char *what);
    unsigned (*watch)(uint32_t linear, unsigned bytes, bool write); /* DR6 B0-B3 bits */
};
enum { GCPU_BUDGET, GCPU_TRAP, GCPU_FAULT };
void gcpu_init(const struct gcpu_ops *);
void gcpu_done(void);
int gcpu_run(struct gcpu_state *, unsigned max, struct gcpu_event *);
#endif
