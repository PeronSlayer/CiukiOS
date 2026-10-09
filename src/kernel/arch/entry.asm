; Ciuki VMM entry. The loader enters at the physical address of _start with
; paging off, EAX = "CIUK", EBX = physical address of ciuki_boot_info.
; Build a temporary directory mapping 0-16 MiB twice (identity and at
; 0xC0000000), enable paging, jump high, and call kmain.
; docs/design/boot-memory.md "Early kernel sequence".
; SPDX-License-Identifier: GPL-2.0-only

bits 32

%define KERNEL_VBASE 0xC0000000
%define V2P(x) ((x) - KERNEL_VBASE)

section .text.entry
global _start
extern kmain

_start:
    cli
    cld
    mov esi, eax                    ; preserve magic
    mov edi, ebx                    ; preserve boot info physical address

    ; Four page tables cover 0..16 MiB.
    mov ecx, 4096
    xor eax, eax
    mov edx, V2P(boot_pt)
.fill_pt:
    mov ebx, eax
    or ebx, 0x003                   ; present, writable
    mov [edx], ebx
    add eax, 4096
    add edx, 4
    loop .fill_pt

    ; Directory entries 0..3 (identity) and 768..771 (high half).
    mov edx, V2P(boot_pd)
    mov eax, V2P(boot_pt)
    or eax, 0x003
    xor ecx, ecx
.fill_pd:
    mov [edx + ecx * 4], eax
    mov [edx + 768 * 4 + ecx * 4], eax
    add eax, 4096
    inc ecx
    cmp ecx, 4
    jb .fill_pd

    ; CR4 in a known state before paging: no PAE (the directory would be
    ; read as a PDPT), no VME/PVI, PSE/PGE off until the kernel decides.
    mov eax, cr4
    and eax, 0xFFFFF94C             ; clear VME(0) PVI(1) PSE(4) PAE(5) PGE(7) OSFXSR(9) OSXMMEXCPT(10)
    mov cr4, eax
    mov eax, V2P(boot_pd)
    mov cr3, eax
    mov eax, cr0
    or eax, 0x80010000              ; PG | WP
    mov cr0, eax

    lea eax, [.high]
    jmp eax
.high:
    mov esp, boot_stack_top
    xor ebp, ebp
    push edi                        ; boot info physical address
    push esi                        ; magic
    call kmain
.halt:
    cli
    hlt
    jmp .halt

section .bss
align 4096
global boot_pd
boot_pd:    resb 4096
boot_pt:    resb 4096 * 4
global boot_stack_bottom, boot_stack_top
boot_stack_bottom:
            resb 16384
boot_stack_top:
