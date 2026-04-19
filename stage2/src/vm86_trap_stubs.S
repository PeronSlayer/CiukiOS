/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPENGEM-032 - v8086 IDT trap stubs (observability only).
 *
 * Each of the eleven stubs below is a dead-code landing pad that
 * would be pointed to by the IDT shim image built in C.
 * No LIDT is ever issued in this phase, so none of these stubs
 * can execute on the default boot path.
 *
 * Safety contract:
 *   - Stubs must be reachable only via IDT dispatch. They are
 *     never called by C code.
 *   - Each stub body is a hlt + infinite jmp, so if any of them
 *     were ever entered by mistake the CPU halts deterministically
 *     instead of corrupting host state.
 *   - No IRET, no IRETD, no LIDT opcode appears in this file;
 *     that mutation is deferred to OPENGEM-033.
 *   - .global so dead-code elimination cannot strip them; the
 *     sentinel "OPENGEM-032" below is retained in .rodata.
 */

.section .rodata
.global vm86_trap_stubs_sentinel
vm86_trap_stubs_sentinel:
    .asciz "OPENGEM-032"

.section .text

.macro VM86_TRAP_STUB name
    .global \name
    .type   \name, @function
\name:
    .byte 0xF4                      /* hlt                          */
    .byte 0xEB, 0xFE                /* jmp .  (halt loop)           */
    .byte 0x90, 0x90, 0x90, 0x90, 0x90  /* nop pad to 8 bytes       */
    .size  \name, . - \name
.endm

VM86_TRAP_STUB vm86_trap_stub_de
VM86_TRAP_STUB vm86_trap_stub_ud
VM86_TRAP_STUB vm86_trap_stub_nm
VM86_TRAP_STUB vm86_trap_stub_ts
VM86_TRAP_STUB vm86_trap_stub_np
VM86_TRAP_STUB vm86_trap_stub_ss
VM86_TRAP_STUB vm86_trap_stub_gp
VM86_TRAP_STUB vm86_trap_stub_pf
VM86_TRAP_STUB vm86_trap_stub_sw20
VM86_TRAP_STUB vm86_trap_stub_sw21
VM86_TRAP_STUB vm86_trap_stub_unexpected

.section .note.GNU-stack,"",@progbits
