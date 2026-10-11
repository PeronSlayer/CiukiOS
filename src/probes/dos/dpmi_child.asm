; f3-04 nested DPMI client (DPMICH16.COM / DPMICH32.COM): started by
; DPMI16/DPMI32 through INT 21h/4B00h while the parent is suspended in a
; 0300h call. Enters with the parent's width, allocates a block, selectors,
; a callback and a watchpoint, hooks INT 61h, then terminates in protected
; mode without releasing anything. Exit code 2Ah = success, 1-9 = stage.
; SPDX-License-Identifier: GPL-2.0-only
%ifndef BITS32
%define BITS32 0
%endif
bits 16
org 100h

start:
    cld
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, stack_top
    mov bx, (image_end - $$ + 100h + 15) >> 4
    mov ah, 4Ah
    int 21h
    mov al, 1
    jc rm_exit
    mov ax, 1687h
    int 2Fh
    test ax, ax
    mov al, 2
    jnz rm_exit
    mov [entry], di
    mov [entry + 2], es
    push ds
    pop es
    mov ax, BITS32
    call far [entry]
    mov al, 3
    jc rm_exit
    mov [pm_ds], ds
    mov bx, 1                     ; 64 KiB block
    xor cx, cx
    mov ax, 0501h
    int 31h
    mov al, 4
    jc pm_exit
    mov cx, 2
    mov ax, 0000h
    int 31h
    mov al, 5
    jc pm_exit
    mov bl, 61h
    mov cx, cs
    mov edx, child_int61
    mov ax, 0205h
    int 31h
    mov al, 6
    jc pm_exit
    int 61h
    cmp byte [hits], 1
    mov al, 7
    jne pm_exit
    push ds
    mov ax, cs
    mov ds, ax
    mov esi, child_int61
    mov ax, ss
    mov es, ax
    mov edi, cbs
    mov ax, 0303h
    int 31h
    pop ds
    push ds
    pop es
    mov al, 8
    jc pm_exit
    mov bx, ds
    mov ax, 0006h
    int 31h
    add dx, hits
    adc cx, 0
    mov bx, cx
    mov cx, dx
    mov dx, 0101h
    mov ax, 0B00h
    int 31h
    mov al, 9
    jc pm_exit
    mov al, 2Ah
pm_exit:
    mov ah, 4Ch
    int 21h
rm_exit:
    mov ah, 4Ch
    int 21h

child_int61:
    push ds
    push ax
    mov ax, [cs:pm_ds]
    mov ds, ax
    inc byte [hits]
    pop ax
    pop ds
%if BITS32
    iretd
%else
    iret
%endif

align 4
entry dd 0
pm_ds dw 0
hits db 0
align 2
cbs times 32h db 0
align 16
stack times 512 db 0
stack_top:
image_end:
