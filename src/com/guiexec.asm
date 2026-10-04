; Trusted console utility wrapper. Keep the desktop mode and suppress console
; rendering for the duration of a nested EXEC; diagnostics still reach COM1.
bits 16
cpu 386
org 0x100
start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    mov bx,(program_end-$$+0x100+15)/16
    mov ah,0x4A
    int 0x21
    jc bad
    xor cx,cx
    mov cl,[0x80]
    mov si,0x81
.space:
    test cx,cx
    jz bad
    cmp byte [si],' '
    jne .name
    inc si
    dec cx
    jmp .space
.name:
    mov di,path
    xor bx,bx
.copy:
    jcxz .args
    lodsb
    dec cx
    cmp al,' '
    je .args
    cmp bx,79
    jae bad
    stosb
    inc bx
    jmp .copy
.args:
    mov byte [di],0
    mov [tail],cl
    mov di,tail+1
    rep movsb
    mov byte [di],13
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov ax,0x3521
    int 0x21
    mov [old21],bx
    mov [old21+2],es
    mov ax,0x3510
    int 0x21
    mov [old10],bx
    mov [old10+2],es
    mov ax,0x3529
    int 0x21
    mov [old29],bx
    mov [old29+2],es
    mov dx,quiet21
    mov ax,0x2521
    int 0x21
    mov dx,quiet10
    mov ax,0x2510
    int 0x21
    mov dx,quiet29
    mov ax,0x2529
    int 0x21
    push cs
    pop es
    mov dx,path
    mov bx,params
    mov ax,0x4B00
    int 0x21
    jc .restore
    mov ah,0x4D
    int 0x21
.restore:
    mov [cs:result],al
    push cs
    pop ds
    lds dx,[old10]
    mov ax,0x2510
    int 0x21
    push cs
    pop ds
    lds dx,[old29]
    mov ax,0x2529
    int 0x21
    push cs
    pop ds
    lds dx,[old21]
    mov ax,0x2521
    int 0x21
    mov al,[cs:result]
    mov ah,0x4C
    int 0x21
bad:
    mov ax,0x4CFF
    int 0x21

quiet10:
    cmp ah,0x0E
    je .teletype
    ; A nested console program must not change the desktop video mode or
    ; draw/scroll text over it. These BIOS output functions return harmlessly.
    cmp ah,0x00
    je .suppress
    cmp ah,0x01
    je .suppress
    cmp ah,0x02
    je .suppress
    cmp ah,0x05
    je .suppress
    cmp ah,0x06
    je .suppress
    cmp ah,0x07
    je .suppress
    cmp ah,0x09
    je .suppress
    cmp ah,0x0A
    je .suppress
    cmp ah,0x0B
    je .suppress
    cmp ah,0x0C
    je .suppress
    cmp ah,0x13
    je .suppress
.chain:
    jmp far [cs:old10]
.teletype:
    call serial
    iret
.suppress:
    iret
quiet29:
    ; DOS fast console output passes the character in AL.
    call serial
    iret
quiet21:
    cmp ah,2
    je .char
    cmp ah,9
    je .string
    cmp ah,6
    jne .write_test
    cmp dl,0xFF
    jne .char
.write_test:
    cmp ah,0x40
    jne .chain
    cmp bx,1
    je .write
    cmp bx,2
    je .write
.chain:
    jmp far [cs:old21]
.char:
    mov al,dl
    call serial
    iret
.string:
    push ax
    push si
    cld
    mov si,dx
.next:
    lodsb
    cmp al,'$'
    je .end_string
    call serial
    jmp .next
.end_string:
    pop si
    pop ax
    iret
.write:
    push si
    push cx
    cld
    mov si,dx
    jcxz .written
.byte:
    lodsb
    call serial
    loop .byte
.written:
    pop cx
    mov ax,cx
    pop si
    push bp
    mov bp,sp
    and word [ss:bp+6],0xFFFE
    pop bp
    iret
serial:
    push ax
    push cx
    push dx
    mov ah,al
    mov dx,0x3FD
    mov cx,0x1000
.wait:
    in al,dx
    test al,0x20
    jnz .send
    loop .wait
.send:
    mov al,ah
    mov dx,0x3F8
    out dx,al
    pop dx
    pop cx
    pop ax
    ret
old21 dd 0
old10 dd 0
old29 dd 0
result db 0
params dw 0,tail,0,0x5C,0,0x6C,0
path times 80 db 0
tail times 128 db 0
times 512 db 0
stack_top:
program_end:
