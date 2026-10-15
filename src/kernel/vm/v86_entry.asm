; Intel SDM Vol. 3B 23.2.5/23.3.1, extended IRET frame, VME disabled:
; https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf
; SPDX-License-Identifier: GPL-2.0-only
bits 32
section .text
extern g_tss
extern trap_return
extern crit_begin
extern crit_end
extern crit_label

; void v86_enter(const struct v86_frame *frame, uint32_t *continuation)
; The TSS stack ends BELOW this call's saved C continuation. Using the top
; of the task stack would overwrite the live BIOS worker on the first IRQ.
; f3-04: a frame without EFLAGS.VM is a ring-3 DPMI client frame, the
; 76-byte trap_frame whose SS:ESP/EFLAGS/CS:EIP end at the same TSS ESP0
; (no V86 segment words). trap_return selects the matching IRET path.
global v86_enter
v86_enter:
    push ebp
    push ebx
    push esi
    push edi
    cli
    call crit_begin
    push dword v86_entry_label
    call crit_label
    add esp, 4
    mov esi, [esp + 20]
    mov eax, [esp + 24]
    mov [eax], esp
    mov [g_tss + 4], esp
    mov ecx, 23
    test dword [esi + 64], 0x20000
    jnz .copy
    mov ecx, 19
.copy:
    lea edx, [ecx * 4]
    sub esp, edx
    mov edi, esp
    cld
    rep movsd
    jmp trap_return

; Abandon only the monitor's trap frames and return to the saved worker.
; void v86_leave(uint32_t continuation), called with IF=0.
global v86_leave
v86_leave:
    call crit_end
    mov esp, [esp + 4]
    pop edi
    pop esi
    pop ebx
    pop ebp
    sti
    ret

; Copied as data into the reserved scratch page; never executes in ring 0.
; SeaBIOS rel-1.16.3 invoke_mouse_handler: status, X, Y, zero Z then far CALL.
; https://raw.githubusercontent.com/coreboot/seabios/rel-1.16.3/src/mouse.c
; At entry [SP+10]=status, [SP+8]=X, [SP+6]=Y. Preserve all caller state.
section .rodata
v86_entry_label: db 'v86_enter', 0
bits 16
global biosvm_mouse_stub, biosvm_mouse_stub_end
biosvm_mouse_stub:
    push bp
    mov bp, sp
    pushf
    cli
    pusha
    push ds
    push cs
    pop ds
    mov bx, [0x400]             ; producer head (0..31)
    cmp bx, 31
    ja .bad
    cmp word [0x402], 31
    ja .bad
    mov dx, bx
    inc dx
    and dx, 31
    cmp dx, [0x402]             ; consumer tail
    je .full
    imul bx, bx, 3
    mov al, [ss:bp + 12]
    mov [bx + 0x406], al
    mov al, [ss:bp + 10]
    mov [bx + 0x407], al
    mov al, [ss:bp + 8]
    mov [bx + 0x408], al
    mov [0x400], dx             ; publish complete packet
    jmp .done
.full:
    inc word [0x404]
    jmp .done
.bad:
    mov word [0x400], 0
    mov word [0x402], 0
    inc word [0x404]
.done:
    pop ds
    popa
    popf
    pop bp
    retf
biosvm_mouse_stub_end:
