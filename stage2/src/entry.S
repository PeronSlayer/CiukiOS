.global _start
.extern stage2_main

.section .bss
.align 16
stage2_stack:
    .skip 16384
stage2_stack_top:

.section .text
_start:
    mov $0xE9, %dx
    mov $'2', %al
    out %al, %dx

    cld

    mov %rdi, %rbx
    mov %rsi, %r12
    lea stage2_stack_top(%rip), %rsp
    xor %rbp, %rbp

    mov %rbx, %rdi
    mov %r12, %rsi
    call stage2_main

1:
    cli
    hlt
    jmp 1b

