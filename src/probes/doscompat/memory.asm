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
    mov ax, [2]
    mov [initial_end], ax
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc fail
    mov ax, [initial_end]
    cmp ax, [2]
    jne fail
    mov ax, cs
    dec ax
    mov es, ax
    cmp [es:3], bx
    jne fail
    mov bx, 512
    mov ah, 0x48
    int 0x21
    jc fail
    mov [allocation], ax
    mov es, ax
    xor di, di
    mov cx, 4096
    mov ax, 0xA55A
    cld
    rep stosw
    push cs
    pop es
    mov bx, 0xFFFF
    mov ah, 0x4A
    int 0x21
    jnc fail
    cmp ax, 8
    jne fail
    mov es, [allocation]
    cmp word [es:8190], 0xA55A
    jne fail
    mov ah, 0x49
    int 0x21
    jc fail
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
initial_end dw 0
allocation dw 0
pass_msg db 'MEMORY PASS: initial PSP, current MCB, allocation isolation',13,10,'$'
fail_msg db 'MEMORY FAIL',13,10,'$'
align 16
stack times 512 db 0
stack_top:
image_end:
