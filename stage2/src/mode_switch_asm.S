/*
 * OPENGEM-044 Task A stage-2 — mode-switch asm trampoline.
 *
 * Round-trip long-mode → legacy 32-bit PM → long-mode.
 *
 * C ABI:
 *   int mode_switch_asm_enter(mode_switch_scratch_t *scratch);
 *   %rdi = scratch pointer.
 *
 * Scratch layout (single source of truth: stage2/src/mode_switch.c).
 *
 * Legacy GDT (prebuilt by C before this function is called):
 *   0x00 null
 *   0x08 code64 flat  (L=1,D=0)
 *   0x10 code32 flat  (L=0,D=1)
 *   0x18 data32 flat  (L=0,D=1)
 *
 * Page-table prereq: stage2 .text/.data, scratch, pm32_stack_top must
 * all be identity-mapped in host long-mode CR3 (UEFI default holds for
 * stage2 loaded < 4 GiB).
 *
 * Boot-path isolation: only callable from mode_switch_run_legacy_pm
 * when BOTH API arm flag AND trampoline-live arm flag are held.
 */

.set CODE64_SEL, 0x08
.set CODE32_SEL, 0x10
.set DATA32_SEL, 0x18

.set SCR_RBX,         0x00
.set SCR_RBP,         0x08
.set SCR_R12,         0x10
.set SCR_R13,         0x18
.set SCR_R14,         0x20
.set SCR_R15,         0x28
.set SCR_RSP,         0x30
.set SCR_CR3,         0x38
.set SCR_CR4,         0x40
.set SCR_EFER_LO,     0x48
.set SCR_EFER_HI,     0x4C
.set SCR_CR0,         0x50
.set SCR_HOST_GDTR,   0x58
.set SCR_HOST_IDTR,   0x68
.set SCR_LEGACY_GDTR, 0x78
.set SCR_PM32_STACK,  0x88
.set SCR_BODY_FN,     0x90
.set SCR_BODY_USER,   0x98
.set SCR_RESULT,      0xA0
.set SCR_RFLAGS,      0xA8

.section .rodata
.globl mode_switch_asm_sentinel
mode_switch_asm_sentinel:
    .asciz "OPENGEM-044-A-ASM"

/* Debug trace: emit a single ASCII byte on port 0xE9 (debugcon).
 * Clobbers %al only. Safe in code32 and code64. */
.macro TRACE ch
    movb $\ch, %al
    outb %al, $0xE9
.endm

.section .text
.code64
.globl mode_switch_asm_enter
.type  mode_switch_asm_enter, @function
mode_switch_asm_enter:
    pushfq
    cli
    TRACE 'A'
    movq %rbx, SCR_RBX(%rdi)
    movq %rbp, SCR_RBP(%rdi)
    movq %r12, SCR_R12(%rdi)
    movq %r13, SCR_R13(%rdi)
    movq %r14, SCR_R14(%rdi)
    movq %r15, SCR_R15(%rdi)
    movq %rsp, SCR_RSP(%rdi)
    popq %rax
    movq %rax, SCR_RFLAGS(%rdi)

    movq %cr0, %rax
    movq %rax, SCR_CR0(%rdi)
    movq %cr3, %rax
    movq %rax, SCR_CR3(%rdi)
    movq %cr4, %rax
    movq %rax, SCR_CR4(%rdi)

    movl $0xC0000080, %ecx
    rdmsr
    movl %eax, SCR_EFER_LO(%rdi)
    movl %edx, SCR_EFER_HI(%rdi)

    sgdt SCR_HOST_GDTR(%rdi)
    sidt SCR_HOST_IDTR(%rdi)
    TRACE 'B'

    lgdt SCR_LEGACY_GDTR(%rdi)
    TRACE 'C'

    /* %r15 will survive the legacy-mode visit because R8-R15 values are
     * preserved across IA-32e exit (even if not addressable in legacy
     * 32-bit encoding). Inside compat/legacy .code32 we use %ebx as
     * scratch pointer (scratch lives in low 4 GiB). */
    movq %rdi, %r15
    movl %edi, %ebx

    pushq $CODE32_SEL
    leaq  ._ms_compat_hop(%rip), %rax
    pushq %rax
    lretq

.code32
._ms_compat_hop:
    TRACE 'D'
    movw $DATA32_SEL, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    TRACE 'E'
    movw $DATA32_SEL, %ax   /* reload: TRACE clobbered %al */
    movw %ax, %ss
    TRACE 'F'

    movl SCR_PM32_STACK(%ebx), %esp
    TRACE 'G'

    /* Leave IA-32e: CR0.PG=0 → clears EFER.LMA. */
    movl %cr0, %eax
    andl $0x7FFFFFFF, %eax
    movl %eax, %cr0
    TRACE 'H'

    movl $0xC0000080, %ecx
    rdmsr
    andl $0xFFFFFEFF, %eax        /* EFER.LME=0 */
    wrmsr
    TRACE 'I'

    movl %cr4, %eax
    andl $0xFFFFFFDF, %eax        /* CR4.PAE=0 */
    movl %eax, %cr4
    TRACE 'J'

    /* Legacy 32-bit PM, paging off. Call body(user).
     * NOTE: body is called with the SysV ABI for 32-bit, where the
     * caller also preserves ebx across the call — but we took a copy
     * in %r15 (preserved across mode transitions) so we can reload
     * %ebx from %r15 after the call using a fallback: reload from the
     * known static scratch address via the body itself (not needed
     * here because ebx IS callee-saved in cdecl). */
    movl SCR_BODY_USER(%ebx), %eax
    pushl %eax
    movl  SCR_BODY_FN(%ebx), %eax
    TRACE 'K'
    /* Sanity: relative call to a local micro-body. If 'm' appears the
     * CALL instruction and stack work in legacy PM. */
    call  ._ms_micro_body
    TRACE 'l'
    /* Dump %eax (body_fn) as 8 hex chars on 0xE9 so we can verify the
     * indirect target is sane before using it. Preserves %eax. */
    pushl %eax
    pushl %ecx
    movl  %eax, %ecx
    movl  $8, %edx
._ms_dump_eax:
    roll  $4, %ecx
    movl  %ecx, %eax
    andl  $0xF, %eax
    cmpl  $10, %eax
    jb    ._ms_dump_dec
    addl  $('a' - 10), %eax
    jmp   ._ms_dump_emit
._ms_dump_dec:
    addl  $'0', %eax
._ms_dump_emit:
    outb  %al, $0xE9
    decl  %edx
    jnz   ._ms_dump_eax
    popl  %ecx
    popl  %eax
    /* Real indirect call to body. */
    call  *%eax
    TRACE 'L'
    addl  $4, %esp
    movl  %eax, SCR_RESULT(%ebx)

    /* Re-enter IA-32e. */
    movl %cr4, %eax
    orl  $0x00000020, %eax        /* CR4.PAE=1 */
    movl %eax, %cr4
    TRACE 'M'

    movl SCR_CR3(%ebx), %eax
    movl %eax, %cr3
    TRACE 'N'

    movl $0xC0000080, %ecx
    rdmsr
    orl  $0x00000100, %eax        /* EFER.LME=1 */
    wrmsr
    TRACE 'O'

    movl %cr0, %eax
    orl  $0x80000000, %eax        /* CR0.PG=1 → IA-32e active */
    movl %eax, %cr0
    TRACE 'P'

    /* Compat mode active again. Far-return to CODE64_SEL target. */
    pushl $CODE64_SEL
    pushl $._ms_long_resume
    lretl

    /* .code32 local sanity routine for pre-call diagnostics. */
._ms_micro_body:
    movb $'m', %al
    outb %al, $0xE9
    retl

.code64
._ms_long_resume:
    TRACE 'Q'
    xorl %eax, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    lgdt SCR_HOST_GDTR(%r15)
    TRACE 'R'
    lidt SCR_HOST_IDTR(%r15)
    TRACE 'S'

    movq SCR_RSP(%r15), %rsp

    movq SCR_RBX(%r15), %rbx
    movq SCR_RBP(%r15), %rbp
    movq SCR_R12(%r15), %r12
    movq SCR_R13(%r15), %r13
    movq SCR_R14(%r15), %r14
    movq SCR_RESULT(%r15), %rax
    pushq SCR_RFLAGS(%r15)
    movq  SCR_R15(%r15), %r15
    popfq
    ret

.size mode_switch_asm_enter, . - mode_switch_asm_enter
