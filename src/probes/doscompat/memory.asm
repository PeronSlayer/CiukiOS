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
    cmp byte [0x80], 0
    je parent
    jmp checks                          ; the child run (any tail)

; Parent: shrink, then run this program as a child, which repeats every
; check (an AH=48h block must lie above the child's own block, too).
parent:
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc fail
    mov [params + 4], cs
    mov [params + 8], cs
    mov [params + 12], cs
    mov ah, 0x62                        ; own PSP: environment for the name
    int 0x21
    mov es, bx
    mov es, [es:0x2C]
    xor di, di
    xor al, al
    mov cx, 0x8000
    cld
.env_end:
    repne scasb
    scasb
    jne .env_end
    add di, 2                           ; the string count, then the path
    push ds
    push es
    pop ds
    mov dx, di
    mov bx, params
    push cs
    pop es
    mov ax, 0x4B00
    int 0x21
    pop ds
    jc fail
    mov ah, 0x4D
    int 0x21
    test al, al
    jnz fail_child
checks:
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
    ; The block lies above this program's own (resized) block and its MCB.
    mov dx, cs
    add dx, (image_end - $$ + 0x100 + 15) >> 4
    cmp ax, dx
    jbe overlap
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
    ; The largest free block, too (it starts right above this program).
    mov bx, 0xFFFF
    mov ah, 0x48
    int 0x21
    jnc fail
    cmp ax, 8
    jne fail
    mov ah, 0x48
    int 0x21
    jc fail
    mov dx, cs
    add dx, (image_end - $$ + 0x100 + 15) >> 4
    cmp ax, dx
    jbe overlap
    mov es, ax
    mov ah, 0x49
    int 0x21
    jc fail
    push cs
    pop es
    cmp byte [0x80], 0                  ; the child only reports its code
    jne .passed
    mov dx, pass_msg
    mov ah, 9
    int 0x21
.passed:
    mov ax, 0x4C00
    int 0x21
overlap:
    push cs
    pop ds
    mov dx, overlap_msg
    mov ah, 9
    int 0x21
fail_child:
    push cs
    pop ds
    mov dx, child_msg
    mov ah, 9
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
child_tail db 2,' C',13
params dw 0,child_tail,0,0x5C,0,0x6C,0
child_msg db 'MEMORY child run failed',13,10,'$'
allocation dw 0
pass_msg db 'MEMORY PASS: initial PSP, current MCB, allocation isolation',13,10,'$'
fail_msg db 'MEMORY FAIL',13,10,'$'
overlap_msg db 'MEMORY AH=48h returned memory inside the caller',13,10,'$'
align 16
stack times 512 db 0
stack_top:
image_end:
