/*
 * vm86_snapshot.S — OPENGEM-031 CPU-state capture helpers.
 *
 * Reads current CR0/CR3/CR4, EFER via RDMSR, and the GDTR/IDTR
 * via SGDT/SIDT into a caller-supplied vm86_cpu_snapshot struct.
 * No CPU state is modified.
 *
 * ABI: System V AMD64. rdi = pointer to vm86_cpu_snapshot.
 */

.section .text
.global vm86_snapshot_capture_asm

vm86_snapshot_capture_asm:
    testq   %rdi, %rdi
    jz      .Lfail

    movl    $0x00000310, 0(%rdi)
    movl    $0, 4(%rdi)

    mov     %cr0, %rax
    mov     %rax, 8(%rdi)
    mov     %cr3, %rax
    mov     %rax, 16(%rdi)
    mov     %cr4, %rax
    mov     %rax, 24(%rdi)

    pushq   %rcx
    pushq   %rdx
    movl    $0xC0000080, %ecx
    rdmsr
    shlq    $32, %rdx
    orq     %rdx, %rax
    mov     %rax, 32(%rdi)
    popq    %rdx
    popq    %rcx

    subq    $16, %rsp
    sgdt    (%rsp)
    movw    (%rsp), %ax
    movw    %ax, 40(%rdi)
    movw    $0, 42(%rdi)
    movl    $0, 44(%rdi)
    movq    2(%rsp), %rax
    movq    %rax, 48(%rdi)
    addq    $16, %rsp

    subq    $16, %rsp
    sidt    (%rsp)
    movw    (%rsp), %ax
    movw    %ax, 56(%rdi)
    movw    $0, 58(%rdi)
    movl    $0, 60(%rdi)
    movq    2(%rsp), %rax
    movq    %rax, 64(%rdi)
    addq    $16, %rsp

    movl    $1, %eax
    ret

.Lfail:
    xorl    %eax, %eax
    ret
