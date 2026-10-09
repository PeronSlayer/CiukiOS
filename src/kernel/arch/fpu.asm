; FPU state management routines: the only x87/SSE instructions allowed in
; the kernel image (docs/design/execution-abi.md, "FPU policy").
; SPDX-License-Identifier: GPL-2.0-only
bits 32
section .text

global fpu_fxsave, fpu_fxrstor, fpu_fnsave, fpu_frstor, fpu_reset_state

fpu_fxsave:
    mov eax, [esp + 4]
    fxsave [eax]
    ret
fpu_fxrstor:
    mov eax, [esp + 4]
    fxrstor [eax]
    ret
fpu_fnsave:
    mov eax, [esp + 4]
    fnsave [eax]
    ret
fpu_frstor:
    mov eax, [esp + 4]
    frstor [eax]
    ret

; void fpu_reset_state(int sse): FNINIT, and with SSE the default MXCSR and
; zeroed XMM registers, so the saved image is a fully initialised state.
fpu_reset_state:
    fninit
    mov eax, [esp + 4]
    test eax, eax
    jz .done
    sub esp, 4
    mov dword [esp], 0x1F80
    ldmxcsr [esp]
    add esp, 4
    xorps xmm0, xmm0
    xorps xmm1, xmm1
    xorps xmm2, xmm2
    xorps xmm3, xmm3
    xorps xmm4, xmm4
    xorps xmm5, xmm5
    xorps xmm6, xmm6
    xorps xmm7, xmm7
.done:
    ret

; Kernel-thread trampoline: EBX = function, ESI = argument.
extern task_exit
global kthread_trampoline
kthread_trampoline:
    push esi
    call ebx
    add esp, 4
    push dword 0
    call task_exit
.h: hlt
    jmp .h
