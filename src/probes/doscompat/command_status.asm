bits 16
org 0x100
start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4A
    int 0x21
    mov ax, cs
    mov [param+4], ax
    mov [param+8], ax
    mov [param+12], ax
    mov dx, command
    mov bx, param
    mov ax, 0x4B00
    int 0x21
    jc fail
    mov ah, 0x4D
    int 0x21
    cmp ax, 37
    jne fail
    mov dx, pass_msg
    mov ah, 9
    int 0x21
    mov ax, 0x4C00
    int 0x21
fail:
    push cs
    pop ds
    mov dx, fail_msg
    mov ah, 9
    int 0x21
    mov ax, 0x4C01
    int 0x21
command db '\COMMAND.COM', 0
tail db tail_end - tail - 1
    db ' /C EXIT37.COM'
tail_end db 13
param dw 0, tail, 0, 0, 0, 0, 0
pass_msg db 'COMMAND STATUS PASS',13,10,'$'
fail_msg db 'COMMAND STATUS FAIL',13,10,'$'
align 16
stack times 512 db 0
stack_top:
image_end:
