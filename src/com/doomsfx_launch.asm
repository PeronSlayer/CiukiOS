bits 16
org 0x0100

start:
    cld
    push cs
    pop ds
    push cs
    pop es

    mov dx, msg_begin
    call print_line
    call build_tail

    mov dx, dos4gw_path
    mov bx, dos4gw_param_block
    call exec_child
    jc .fail

    mov ax, 0x4C00
    int 0x21

.fail:
    mov dx, msg_fail
    call print_line
    mov ax, 0x4C01
    int 0x21

print_line:
    mov ah, 0x09
    int 0x21
    ret

build_tail:
    push ax
    push bx
    push cx
    push si
    push di
    mov di, dos4gw_tail_text
    mov si, base_tail_text
    mov cx, base_tail_len
.copy_base:
    lodsb
    stosb
    loop .copy_base
    mov bl, base_tail_len
    mov cl, [0x80]
    cmp cl, 0
    je .done
    mov si, 0x81
.skip_spaces:
    cmp cl, 0
    je .done
    lodsb
    dec cl
    cmp al, ' '
    je .skip_spaces
    cmp al, 9
    je .skip_spaces
    mov ah, al
    mov al, ' '
    stosb
    inc bl
    mov al, ah
.copy_arg:
    cmp bl, 126
    jae .done
    stosb
    inc bl
    cmp cl, 0
    je .done
    lodsb
    dec cl
    cmp al, 13
    je .done
    jmp .copy_arg
.done:
    mov [dos4gw_tail], bl
    mov al, 13
    stosb
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

exec_child:
    push ax
    push bx
    push dx
    push ds
    push es
    xor ax, ax
    mov [bx + 0], ax
    mov ax, ds
    mov [bx + 4], ax
    mov [bx + 8], ax
    mov [bx + 12], ax
    push ds
    pop es
    mov ax, 0x4B00
    int 0x21
    jc .done
    mov ax, 0x4D00
    int 0x21
    cmp ah, 0
    jne .child_fail
    cmp al, 0
    jne .child_fail
    clc
    jmp .done

.child_fail:
    stc

.done:
    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

msg_begin db '[DOOMSFX] LAUNCH DOS4GW', 13, 10, '$'
msg_fail db '[DOOMSFX] EXEC FAIL', 13, 10, '$'
dos4gw_path db '\SYSTEM\DRIVERS\DOS4GW.EXE', 0
base_tail_text db ' \APPS\DOOMAUD\DOOMSFX.LE'
base_tail_len equ $ - base_tail_text
dos4gw_tail db 0
dos4gw_tail_text times 127 db 0
dos4gw_fcb1 dw 0, 0
dos4gw_fcb2 dw 0, 0
dos4gw_param_block:
    dw 0
    dw dos4gw_tail
    dw 0
    dw dos4gw_fcb1
    dw 0
    dw dos4gw_fcb2
    dw 0
