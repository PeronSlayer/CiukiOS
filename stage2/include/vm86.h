/*
 * vm86.h — OPENGEM-016 16-bit execution layer scaffolding.
 *
 * This header defines the ABI types for the v8086 monitor described in
 * docs/opengem-016-design.md. Implementation is currently scaffolding
 * only: no real mode-switch is performed, no guest task is entered.
 * Every function here is observability-only and safe to call from
 * long-mode host context.
 *
 * Phases:
 *   OPENGEM-017: mode-switch scaffold (this file, initial drop).
 *   OPENGEM-018: descriptor table shim data (GDT, TSS, IDT).
 *   OPENGEM-019: VM task descriptor.
 *   OPENGEM-020: INT dispatcher skeleton.
 *
 * Nothing in this header is called from the live boot path yet.
 */
#ifndef STAGE2_VM86_H
#define STAGE2_VM86_H

#include "types.h"

/*
 * Execution mode identifiers used by the three-level architecture
 * described in docs/opengem-016-design.md §5.1.
 */
typedef enum {
    VM86_MODE_HOST_LONG       = 1,  /* stage2 long-mode host         */
    VM86_MODE_COMPAT_PE32     = 2,  /* 32-bit protected-mode bridge  */
    VM86_MODE_GUEST_V8086     = 3   /* virtual-8086 guest            */
} vm86_mode_id;

/*
 * Trap frame exchanged between the PE compatibility host and the
 * INT dispatcher. Layout matches design §5.2.
 *
 * This structure is intentionally 32-bit register width even though
 * the guest is 16-bit: the v8086 task always exposes 32-bit register
 * state to the host (EAX/EBX/…) because the CPU preserves the upper
 * halves across the mode boundary.
 */
typedef struct vm86_trap_frame {
    u32 eax;
    u32 ebx;
    u32 ecx;
    u32 edx;
    u32 esi;
    u32 edi;
    u32 ebp;
    u32 esp;
    u32 eip;
    u32 eflags;
    u16 cs;
    u16 ds;
    u16 es;
    u16 fs;
    u16 gs;
    u16 ss;
} vm86_trap_frame;

/*
 * OPENGEM-017 scaffold probe. Emits the four observability markers
 * defined for this phase. Returns 1 on success, 0 on failure.
 */
int vm86_scaffold_probe(void);

#endif /* STAGE2_VM86_H */
