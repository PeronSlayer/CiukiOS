/* =====================================================================
 * OPENGEM-041 — live v86 entry (double arm-gated, staged).
 *
 * This file stages an UNGUARDED version of the 040 trampoline. It is
 * reachable only through vm86_compat_entry_enter_v86() which requires
 * BOTH the 040 arm magic AND the 041 live-arm magic to be held.
 *
 * Boot path is untouched. No shell command wires the entry in 041 —
 * shell integration is the next milestone (gem.exe loader).
 *
 * Contract:
 *   - Caller must have run vm86_compat_entry_prepare() to a
 *     verified compat-task image.
 *   - Caller must have patched img->tss.cr3 with the host CR3.
 *   - Caller is responsible for populating the scratch cs:ip / ss:sp
 *     fields before calling this entry.
 *
 * Inbound (SysV x86-64): no args; all state is already in the scratch
 * block written by vm86_compat_entry_prepare + cs_ip/ss_sp patch.
 *
 * This function DOES NOT RETURN. After iretl the CPU is in v8086.
 * ===================================================================== */

    .section .rodata
    .globl vm86_compat_entry_live_sentinel
vm86_compat_entry_live_sentinel:
    .asciz "OPENGEM-041"

    .text
    .code64
    .globl vm86_compat_entry_enter_asm
    .type  vm86_compat_entry_enter_asm, @function
vm86_compat_entry_enter_asm:
    cli

    /* rax = &vm86_compat_entry_scratch (published by 040). */
    leaq    vm86_compat_entry_scratch(%rip), %rax

    /* LGDT from scratch+0x18. */
    leaq    0x18(%rax), %rbx
    lgdt    (%rbx)

    /* Far-return into compat32. */
    movzwl  0x3A(%rax), %ecx          /* CS32 */
    leaq    vm86_compat_entry_live_compat32(%rip), %rdx
    pushq   %rcx
    pushq   %rdx
    lretq

    .code32
    .globl vm86_compat_entry_live_compat32
vm86_compat_entry_live_compat32:
    movl    $vm86_compat_entry_scratch, %ebx

    movzwl  0x3C(%ebx), %eax          /* DS32 */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    leal    0x28(%ebx), %ecx
    lidt    (%ecx)

    movzwl  0x38(%ebx), %ecx          /* TSS selector */
    ltr     %cx

    /* v86 IRETD frame. */
    pushl   $0                         /* GS */
    pushl   $0                         /* FS */
    pushl   $0                         /* DS */
    pushl   $0                         /* ES */

    movl    0x14(%ebx), %eax           /* ss:sp */
    movzwl  %ax, %ecx
    shrl    $16, %eax
    pushl   %eax                       /* SS */
    pushl   %ecx                       /* ESP */

    pushl   $0x00023202                /* EFLAGS: VM=1, IF=1, reserved-1 */

    movl    0x10(%ebx), %eax           /* cs:ip */
    movzwl  %ax, %ecx
    shrl    $16, %eax
    pushl   %eax                       /* CS */
    pushl   %ecx                       /* EIP */

    iretl

    /* Unreachable. */
    hlt
    jmp     vm86_compat_entry_live_compat32

    .code64
    .size  vm86_compat_entry_enter_asm, .-vm86_compat_entry_enter_asm
