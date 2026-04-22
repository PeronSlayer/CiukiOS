.global efi_handoff
.type efi_handoff, @function

.section .text
efi_handoff:
    cli
    cld

    # SysV input:
    #   RDI = new_cr3
    #   RSI = entry point
    #   RDX = boot_info
    #   RCX = handoff_v0
    #
    # SysV output toward target:
    #   RDI = boot_info
    #   RSI = handoff_v0
    mov %rdi, %cr3      # arg1 SysV = new_cr3
    mov %rsi, %r15      # arg2 SysV = entry point (saved in R15)
    mov %rdx, %rdi      # arg3 SysV = boot_info
    mov %rcx, %rsi      # arg4 SysV = handoff_v0
    xor %rbp, %rbp

    # Debug marker before the jump
    mov $0xE9, %dx
    mov $'J', %al
    out %al, %dx

    jmp *%r15
