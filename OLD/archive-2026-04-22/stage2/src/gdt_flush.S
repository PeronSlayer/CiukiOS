.global stage2_gdt_apply

.section .text
stage2_gdt_apply:
    lgdt (%rdi)

    mov %si, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss

    pushq %rdx
    lea 1f(%rip), %rax
    pushq %rax
    lretq

1:
    mov %cx, %ax
    ltr %ax
    ret
