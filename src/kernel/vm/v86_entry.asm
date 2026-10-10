; Intel SDM Vol. 3B 23.2.5/23.3.1, extended IRET frame, VME disabled:
; https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf
; SPDX-License-Identifier: GPL-2.0-only
bits 32
section .text
extern g_tss
extern trap_return

; void v86_enter(const struct v86_frame *frame, uint32_t *continuation)
; The TSS stack ends BELOW this call's saved C continuation. Using the top
; of the task stack would overwrite the live BIOS worker on the first IRQ.
global v86_enter
v86_enter:
    push ebp
    push ebx
    push esi
    push edi
    mov esi, [esp + 20]
    mov eax, [esp + 24]
    cli
    mov [eax], esp
    mov [g_tss + 4], esp
    sub esp, 92
    mov edi, esp
    mov ecx, 23
    cld
    rep movsd
    jmp trap_return

; Abandon only the monitor's trap frames and return to the saved worker.
; void v86_leave(uint32_t continuation), called with IF=0.
global v86_leave
v86_leave:
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
