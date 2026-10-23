; SPDX-License-Identifier: GPL-2.0-only
bits 32
section .text
extern test_shim_page, test_shim_divide, test_shim_invalid
extern test_nested, test_locks, test_shim_timeout
global foreign_start, foreign_end, foreign_sum, foreign_page, foreign_divide
global foreign_invalid, foreign_shim_page, foreign_shim_divide, foreign_shim_invalid
global foreign_nested, foreign_locks, foreign_spin, foreign_shim_timeout
foreign_start:
foreign_sum:
    mov eax,[esp+4]
    add eax,[esp+8]
    ret 8
foreign_page:
    xor eax,eax
    mov eax,[eax]
    ret 8
foreign_divide:
    xor ecx,ecx
    xor edx,edx
    mov eax,1
    div ecx
    ret 8
foreign_invalid:
    ud2
    ret 8
foreign_shim_page:
    call test_shim_page
    ret 8
foreign_shim_divide:
    call test_shim_divide
    ret 8
foreign_shim_invalid:
    call test_shim_invalid
    ret 8
foreign_nested:
    call test_nested
    ret 8
foreign_locks:
    call test_locks
    ud2
    ret 8
foreign_spin:
    jmp foreign_spin
foreign_shim_timeout:
    call test_shim_timeout
    ret 8
foreign_end:
section .note.GNU-stack noalloc noexec nowrite progbits
