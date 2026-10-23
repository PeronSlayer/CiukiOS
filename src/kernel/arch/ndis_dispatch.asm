; Position independent ring-3 dispatcher, copied into a pinned read-only page.
; Kernel supplies EAX=callback, EBX=address of trusted-published argument vector,
; ECX=token, EDX=generation. All callbacks are stdcall, at most six arguments.
; SPDX-License-Identifier: GPL-2.0-only
BITS 32
SECTION .rodata
GLOBAL ndis_dispatch_start, ndis_dispatch_end
ndis_dispatch_start:
    cld
    push edx
    push ecx
    mov edi, eax
    mov esi, ebx
    mov ecx, [esi]                    ; bounded argument count
    cmp ecx, 6
    ja .fault
    test ecx, ecx
    jz .call
.args:
    push dword [esi+ecx*4]
    loop .args
.call:
    call edi
    mov esi, eax                      ; callback result, kernel validates kind
    pop ecx
    pop ebx
    mov eax, 0x4e440002               ; NDIS_USER_COMPLETE
    int 0x80
.fault:
    ud2                              ; completion must abandon this stack
ndis_dispatch_end:
SECTION .note.GNU-stack noalloc noexec nowrite progbits
