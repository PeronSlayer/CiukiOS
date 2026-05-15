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

exec_child:
    ; IN: DS:DX path ASCIIZ, DS:BX DOS EXEC parameter block
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

msg_begin db '[PMIRQSB] LAUNCH DOS4GW', 13, 10, '$'
msg_fail db '[PMIRQSB] EXEC FAIL', 13, 10, '$'
dos4gw_path db '\SYSTEM\DRIVERS\DOS4GW.EXE', 0
dos4gw_tail db 33, ' \SYSTEM\DRIVERS\PMIRQSB.LE PRIME', 13
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
