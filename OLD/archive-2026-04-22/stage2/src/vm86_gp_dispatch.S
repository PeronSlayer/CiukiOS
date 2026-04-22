/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPENGEM-035 - #GP dispatcher ISR stub (pending; never installed).
 *
 * This file introduces the SYMBOL that a future phase will install
 * into the PE32 IDT at vector 0x0D (VM86_IDT_VEC_GP). In OPENGEM-035
 * the symbol is defined but the body is an intentional halt loop:
 *   - no IDT points at it, so it cannot be reached by a real #GP;
 *   - should it ever be entered by mistake, the CPU halts in place
 *     instead of corrupting host state.
 *
 * The real implementation (OPENGEM-036+) will:
 *   - push all guest GPRs + segment selectors into a vm86_trap_frame;
 *   - read EIP/CS/EFLAGS/ESP/SS from the hardware-pushed #GP frame
 *     (CPU pushes these automatically when #GP is raised from v86);
 *   - call the C dispatcher (see vm86.c) to classify and act;
 *   - IRETD back into the guest with the updated frame, or HLT on
 *     VM86_GP_DISPATCH_ACTION_HLT.
 *
 * Safety contract enforced by scripts/test_vm86_gp_dispatch.sh:
 *   - exactly one .global symbol in this file;
 *   - body is hlt + jmp-in-place + nop pad (no IRETD, no IRETQ, no
 *     LIDT, no LGDT, no CR-write);
 *   - the sentinel "OPENGEM-035" lives in .rodata and is retained.
 */

    .section .rodata
    .global vm86_gp_dispatch_sentinel
vm86_gp_dispatch_sentinel:
    .asciz "OPENGEM-035"

    .section .text
    .global vm86_gp_dispatch_isr_stub
    .type   vm86_gp_dispatch_isr_stub, @function
vm86_gp_dispatch_isr_stub:
    .byte 0xF4                      /* hlt                          */
    .byte 0xEB, 0xFE                /* jmp .  (deterministic halt)  */
    .byte 0x90, 0x90, 0x90, 0x90, 0x90  /* nop pad to 8 bytes       */
    .size  vm86_gp_dispatch_isr_stub, . - vm86_gp_dispatch_isr_stub

    .section .note.GNU-stack,"",@progbits
