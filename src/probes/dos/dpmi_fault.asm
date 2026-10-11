; f3-04 fault termination (DPMIFLT.COM): a 16-bit client allocates memory,
; a callback and a watchpoint, nests 0301h -> real mode -> callback, then
; executes a privileged instruction with no exception handler. The default
; handler for 0Dh is fatal (DPMI 0.9 10.4): the host terminates this VM only
; and unwinds every transition without guest help. "DPMIFLT BEGIN" marks the
; start; reaching "DPMIFLT FAIL" means the fault was not taken.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 100h

start:
    cld
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, stack_top
    mov [rseg], ax
    mov bx, (image_end - $$ + 100h + 15) >> 4
    mov ah, 4Ah
    int 21h
    mov ax, 1687h
    int 2Fh
    test ax, ax
    jnz failed
    mov [entry], di
    mov [entry + 2], es
    push ds
    pop es
    xor ax, ax
    call far [entry]
    jc failed
    mov [pm_ds], ds
    mov si, msg_begin
    call emit
    mov bx, 2                     ; 128 KiB
    xor cx, cx
    mov ax, 0501h
    int 31h
    jc failed
    push ds
    mov ax, cs
    mov ds, ax
    mov si, cb_proc
    mov ax, ss
    mov es, ax
    mov di, cbs
    mov ax, 0303h
    int 31h
    pop ds
    push ds
    pop es
    jc failed
    mov [cb_addr], dx
    mov [cb_addr + 2], cx
    mov di, rmcs                  ; 0301h into real mode, which calls back
    mov cx, 32h
    xor al, al
    rep stosb
    mov ax, [rseg]
    mov [rmcs + 2Ch], ax
    mov word [rmcs + 2Ah], rm_call
    mov word [rmcs + 20h], 0002h
    xor cx, cx
    mov di, rmcs
    mov ax, 0301h
    int 31h
failed:
    mov si, msg_fail
    call emit
    mov ax, 4C01h
    int 21h

cb_proc:                          ; protected mode, locked stack, two levels deep
    lgdt [cs:gdt_area]            ; #GP(0), no handler: fatal for this VM
    iret

rm_call:
    call far [cs:cb_addr]
    retf

emit:
    mov dx, 0E9h
.byte:
    lodsb
    test al, al
    jz .done
    out dx, al
    jmp .byte
.done:
    mov al, 10
    out dx, al
    ret

msg_begin db 'DPMIFLT BEGIN', 0
msg_fail db 'DPMIFLT FAIL', 0
align 4
entry dd 0
cb_addr dd 0
gdt_area dd 0, 0
rseg dw 0
pm_ds dw 0
rmcs times 32h db 0
cbs times 32h db 0
align 16
stack times 512 db 0
stack_top:
image_end:
