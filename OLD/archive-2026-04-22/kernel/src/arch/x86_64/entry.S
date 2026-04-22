.global _start
.extern kmain

.section .bss
.align 16
kernel_stack:
    .skip 16384
kernel_stack_top:

.section .text
_start:
    mov $0xE9, %dx
    mov $'K', %al
    out %al, %dx

    cld

    mov %rdi, %rbx              # save boot_info in RBX, not in RAX
    lea kernel_stack_top(%rip), %rsp

    mov $0xE9, %dx
    mov $'S', %al
    out %al, %dx

    xor %rbp, %rbp
    mov %rbx, %rdi

    mov $0xE9, %dx
    mov $'M', %al
    out %al, %dx

    call kmain

1:
    cli
    hlt
    jmp 1b
