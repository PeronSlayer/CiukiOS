/* SPDX-License-Identifier: GPL-2.0-only */
.text
.globl abi_probe
.type abi_probe,@function
abi_probe:
    push %ebp
    mov %esp,%ebp
    push %ebx
    push %esi
    push %edi
    mov %esp,%esi
    push $4
    push $3
    call abi_std
    cmp %esp,%esi
    jne .Lfail
    cmp $34,%eax
    jne .Lfail
    mov $2,%ecx
    mov $3,%edx
    push $4
    call abi_kf
    cmp %esp,%esi
    jne .Lfail
    cmp $234,%eax
    jne .Lfail
    push $3
    push $9
    call abi_cdecl
    add $8,%esp
    cmp %esp,%esi
    jne .Lfail
    cmp $6,%eax
    jne .Lfail
    call abi_callback_probe
    cmp %esp,%esi
    jne .Lfail
    test %eax,%eax
    jne .Lfail
    cmpl $7,abi_tick_count
    jne .Lfail
    xor %eax,%eax
    jmp .Ldone
.Lfail:
    mov $1,%eax
.Ldone:
    mov %esi,%esp
    pop %edi
    pop %esi
    pop %ebx
    leave
    ret
.globl _start
.type _start,@function
_start:
    call abi_main
    mov %eax,%ebx
    mov $1,%eax
    int $0x80
.section .note.GNU-stack,"",@progbits
