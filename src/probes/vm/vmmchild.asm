; VMMCHILD.COM - runs in a second DOS VM (VM manager gate). Prints
; "[VM1] n" on COM1 in a busy loop that never yields, then exits with 42.
bits 16
cpu 386
org 100h
start:
    xor bp,bp
.loop:
    mov si,msg
    call text
    mov ax,bp
    call hex
    mov si,crlf
    call text
    call wait_tick
    inc bp
    cmp bp,20
    jb .loop
    mov ax,4C2Ah
    int 21h
; Busy wait (no HLT, no DOS) until the BIOS tick count moves on.
wait_tick:
    push es
    push 40h
    pop es
    mov eax,[es:6Ch]
    mov ecx,400000000
.w:
    cmp eax,[es:6Ch]
    jne .d
    dec ecx
    jnz .w
.d:
    pop es
    ret
text:
    lodsb
    test al,al
    jz .done
    call char
    jmp text
.done:
    ret
char:
    push dx
    push ax
    mov dx,3FDh
.wait:
    in al,dx
    test al,20h
    jz .wait
    pop ax
    mov dx,3F8h
    out dx,al
    pop dx
    ret
hex:
    push cx
    mov cx,4
.digit:
    rol ax,4
    push ax
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe .out
    add al,7
.out:
    call char
    pop ax
    loop .digit
    pop cx
    ret
msg db '[VM1] ',0
crlf db 13,10,0
