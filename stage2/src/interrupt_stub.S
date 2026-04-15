.global stage2_interrupt_default_stub
.global stage2_exception_ud_stub
.global stage2_exception_gp_stub
.global stage2_exception_pf_stub
.global stage2_irq0_stub
.global stage2_irq1_stub

.extern stage2_exception_panic
.extern stage2_timer_on_irq0
.extern stage2_keyboard_on_irq1

.section .text

stage2_interrupt_default_stub:
1:
    cli
    hlt
    jmp 1b

stage2_exception_ud_stub:
    cli
    mov $6, %rdi
    xor %rsi, %rsi
    jmp stage2_exception_common_call

stage2_exception_gp_stub:
    cli
    mov (%rsp), %rsi
    add $8, %rsp
    mov $13, %rdi
    jmp stage2_exception_common_call

stage2_exception_pf_stub:
    cli
    mov (%rsp), %rsi
    add $8, %rsp
    mov $14, %rdi
    jmp stage2_exception_common_call

stage2_exception_common_call:
    cld
    call stage2_exception_panic
2:
    cli
    hlt
    jmp 2b

.macro PUSH_GPRS
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro POP_GPRS
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
.endm

stage2_irq0_stub:
    PUSH_GPRS
    cld
    call stage2_timer_on_irq0
    POP_GPRS
    iretq

stage2_irq1_stub:
    PUSH_GPRS
    cld
    call stage2_keyboard_on_irq1
    POP_GPRS
    iretq
