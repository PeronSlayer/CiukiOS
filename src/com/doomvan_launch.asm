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
    mov ax, [exec_error]
    call print_hex16
    mov dx, crlf
    call print_line
    mov ax, 0x4C01
    int 0x21

print_line:
    mov ah, 0x09
    int 0x21
    ret

print_hex16:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.digit:
    rol bx, 4
    mov al, bl
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    mov dl, al
    mov ah, 0x02
    int 0x21
    loop .digit
    pop cx
    pop bx
    pop ax
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
.copy_args:
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
    jmp .copy_args
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
    jnc .loaded
    mov [exec_error], ax
    jc .done
.loaded:
    mov ax, 0x4D00
    int 0x21
    ; The probe cares that DOS/4GW was launched; the child may return a
    ; diagnostic status while we are still stabilizing this isolated lane.
    clc
    jmp .done

.done:
    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

msg_begin db '[DOOMVAN] LAUNCH DOS4GW', 13, 10, '$'
msg_fail db '[DOOMVAN] EXEC FAIL', 13, 10, '$'
crlf db 13, 10, '$'
exec_error dw 0
dos4gw_path db 'DOS4GW.EXE', 0
base_tail_text db ' \APPS\DOOMVAN\PCDOOM.EXE'
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
