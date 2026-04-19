/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPENGEM-033 - LIDT reversible trampoline.
 *
 * First phase that introduces a real LIDT opcode in stage2. The
 * routine is STILL gated behind an arm-flag (default disarmed) so
 * the default boot path never calls it. When invoked it performs:
 *
 *   1. pushfq / cli         - disable IF around the window
 *   2. sidt  [saved]        - snapshot current IDTR into caller slot
 *   3. lidt  [new]          - install the caller-provided shim IDTR
 *   4. lidt  [saved]        - IMMEDIATE restore of the original IDTR
 *   5. popfq                - restore caller IF
 *   6. retq                 - return to long-mode caller
 *
 * The "LIDT ping" deliberately reverts in the very next instruction
 * so no interrupt window exists where the shim IDTR is live with
 * IF enabled. Between step 3 and step 4 IF is cleared, so even on
 * real hardware a pending line-level IRQ cannot dispatch through
 * the shim's stub vectors.
 *
 * No IRETD / IRETQ is introduced. No CS change, no CR3 change,
 * no GDTR change. Caller arguments follow SysV AMD64:
 *   %rdi -> const vm86_idtr_image *new_idtr  (10 bytes, packed)
 *   %rsi ->       vm86_idtr_image *saved_out (10 bytes, packed)
 *
 * Returns 1 on success in %rax, 0 if any pointer is NULL.
 */

    .section .rodata
    .global vm86_lidt_ping_sentinel
vm86_lidt_ping_sentinel:
    .asciz "OPENGEM-033"

    .section .text
    .global vm86_lidt_ping_asm
    .type   vm86_lidt_ping_asm, @function
vm86_lidt_ping_asm:
    testq   %rdi, %rdi
    jz      .Lping_fail
    testq   %rsi, %rsi
    jz      .Lping_fail

    pushfq
    cli

    sidt    (%rsi)              /* save current IDTR            */
    lidt    (%rdi)               /* install caller-provided IDTR */
    lidt    (%rsi)               /* immediate restore            */

    popfq
    movq    $1, %rax
    retq

.Lping_fail:
    xorq    %rax, %rax
    retq
    .size  vm86_lidt_ping_asm, . - vm86_lidt_ping_asm

    .section .note.GNU-stack,"",@progbits
