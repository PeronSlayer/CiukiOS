; SPDX-License-Identifier: GPL-2.0-only
; i386 same-stack saved context, no task switch or exception-stack copying.
bits 32
section .text
global ndis_context_save
global ndis_context_recover
ndis_context_save:
    mov eax,[esp+4]
    lea edx,[esp+4]
    mov [eax],edx
    mov [eax+4],ebx
    mov [eax+8],esi
    mov [eax+12],edi
    mov [eax+16],ebp
    mov edx,[esp]
    mov [eax+20],edx
    pushfd
    pop edx
    mov [eax+24],edx
    xor eax,eax
    ret
ndis_context_recover:
    mov esp,[eax]
    mov ebx,[eax+4]
    mov esi,[eax+8]
    mov edi,[eax+12]
    mov ebp,[eax+16]
    mov edx,[eax+20]
    push dword [eax+24]
    popfd
    mov eax,1
    jmp edx
section .note.GNU-stack noalloc noexec nowrite progbits
