; DS:DX DOS dollar string; preserve the caller while printing and emitting
; a bounded VM-tagged record through the kernel-owned byte channel.
; SPDX-License-Identifier: GPL-2.0-only
record_dollar:
    pushf
    push ax
    push dx
    push si
    mov ah, 9
    int 0x21
    mov si, dx
    mov dx, 0xE9
.byte:
    lodsb
    cmp al, '$'
    je .done
    out dx, al
    jmp .byte
.done:
    pop si
    pop dx
    pop ax
    popf
    ret
