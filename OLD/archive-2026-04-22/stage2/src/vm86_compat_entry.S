/* =====================================================================
 * OPENGEM-040 — compat-mode entry trampoline (arm-gated, never reached
 *               in the 040 boot path).
 *
 * Stages the 32-bit protected-mode entry path that drops the CPU into
 * v8086. Reached only if a caller supplies VM86_COMPAT_ENTRY_ARM_MAGIC
 * to the C wrapper AND holds the arm flag. No shell command wires
 * the wrapper in 040 — shell wiring arrives in OPENGEM-041.
 *
 * Inbound (SysV x86-64):
 *   rdi = const vm86_compat_task_image *img
 *   rsi = u64  host_cr3  (caller guarantees <= 0xFFFFFFFF)
 *   edx = (u16 v86_cs << 16) | u16 v86_ip
 *   ecx = (u16 v86_ss << 16) | u16 v86_sp
 *
 * Behavior when live body is reached (via 041 arm):
 *   1. cli
 *   2. stash args into vm86_compat_entry_scratch
 *   3. lgdt <scratch gdtr>
 *   4. far-return to compat32 via pushq CS32 / pushq &compat32 / lretq
 *   5. in compat32: reload data segs, lidt, ltr, push v86 IRETD
 *      frame, iretl
 * =====================================================================
 */

    .section .rodata
    .globl vm86_compat_entry_sentinel
vm86_compat_entry_sentinel:
    .asciz "OPENGEM-040"

    .section .data
    .align 8
    .globl vm86_compat_entry_scratch
vm86_compat_entry_scratch:
    .quad 0      /* [0x00] img pointer */
    .quad 0      /* [0x08] host_cr3 */
    .long 0      /* [0x10] cs:ip packed */
    .long 0      /* [0x14] ss:sp packed */
    .word 0      /* [0x18] gdtr limit */
    .word 0      /* [0x1A] pad */
    .long 0      /* [0x1C] gdtr base lo */
    .long 0      /* [0x20] gdtr base hi */
    .long 0      /* [0x24] pad */
    .word 0      /* [0x28] idtr limit */
    .word 0      /* [0x2A] pad */
    .long 0      /* [0x2C] idtr base lo */
    .long 0      /* [0x30] idtr base hi */
    .long 0      /* [0x34] pad */
    .word 0      /* [0x38] tss selector */
    .word 0      /* [0x3A] cs32 selector */
    .word 0      /* [0x3C] ds32 selector */
    .word 0      /* [0x3E] pad */

    .text
    .code64
    .globl vm86_compat_entry_trampoline
    .type  vm86_compat_entry_trampoline, @function
vm86_compat_entry_trampoline:
    cli

    /* Save args. */
    leaq    vm86_compat_entry_scratch(%rip), %rax
    movq    %rdi, 0x00(%rax)
    movq    %rsi, 0x08(%rax)
    movl    %edx, 0x10(%rax)
    movl    %ecx, 0x14(%rax)

    /* OPENGEM-040 gate: defensive hlt. OPENGEM-041 will remove this
     * jmp/hlt pair after runtime validation of the compat entry. */
    hlt
    jmp     vm86_compat_entry_trampoline

    /* Live body (staged, unreached in 040). */
    .globl vm86_compat_entry_body_live
vm86_compat_entry_body_live:
    /* Still long mode, IF=0, rax = &scratch. */
    leaq    0x18(%rax), %rbx
    lgdt    (%rbx)

    movzwl  0x3A(%rax), %ecx
    leaq    vm86_compat_entry_compat32(%rip), %rdx
    pushq   %rcx
    pushq   %rdx
    lretq

    .code32
    .globl vm86_compat_entry_compat32
vm86_compat_entry_compat32:
    /* 32-bit compat sub-mode. ebx preserved across lretq but we
     * reload from the symbol to be safe. */
    movl    $vm86_compat_entry_scratch, %ebx

    movzwl  0x3C(%ebx), %eax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss

    leal    0x28(%ebx), %ecx
    lidt    (%ecx)

    movzwl  0x38(%ebx), %ecx
    ltr     %cx

    /* v86 IRETD frame, top-to-bottom as CPU pops:
     *   EIP, CS, EFLAGS, ESP, SS, ES, DS, FS, GS. */
    pushl   $0                    /* GS */
    pushl   $0                    /* FS */
    pushl   $0                    /* DS */
    pushl   $0                    /* ES */

    movl    0x14(%ebx), %eax      /* ss:sp */
    movzwl  %ax, %ecx
    shrl    $16, %eax
    pushl   %eax                  /* SS */
    pushl   %ecx                  /* ESP */

    pushl   $0x00023202           /* EFLAGS: VM=1, IF=1, reserved-1 */

    movl    0x10(%ebx), %eax      /* cs:ip */
    movzwl  %ax, %ecx
    shrl    $16, %eax
    pushl   %eax                  /* CS */
    pushl   %ecx                  /* EIP */

    iretl

    hlt
    jmp     vm86_compat_entry_compat32

    .code64
    .size  vm86_compat_entry_trampoline, .-vm86_compat_entry_trampoline
