; HELLO.COM - a sample CiukiOS driver package (see DRIVER.INF).
; It stays resident and answers INT 2Fh AX=C000h with AX=C0DEh, BX='HI',
; so a program can find it. It reports on the screen and on COM1.
bits 16
org 0x100

start:
    jmp install

old_2f dd 0
handler:
    cmp ax,0xC000
    jne .chain
    mov ax,0xC0DE
    mov bx,'HI'
    iret
.chain:
    jmp far [cs:old_2f]
resident_end:

install:
    mov ax,0xC000                  ; already resident?
    int 0x2F
    cmp ax,0xC0DE
    je .again
    mov ax,0x352F
    int 0x21
    mov [old_2f],bx
    mov [old_2f+2],es
    mov dx,handler
    mov ax,0x252F
    int 0x21
    mov si,loaded_serial
    call serial
    mov dx,loaded_text
    mov ah,9
    int 0x21
    mov dx,(resident_end-start+0x100+15)/16
    mov ax,0x3100
    int 0x21
.again:
    mov dx,again_text
    mov ah,9
    int 0x21
    mov ax,0x4C00
    int 0x21

serial:
    lodsb
    test al,al
    jz .done
    mov ah,al
    mov dx,0x3FD
    mov cx,0x8000
.wait:
    in al,dx
    test al,0x20
    jnz .send
    loop .wait
.send:
    mov dx,0x3F8
    mov al,ah
    out dx,al
    jmp serial
.done:
    ret

loaded_serial db '[HELLO] driver loaded',13,10,0
loaded_text db 'HELLO sample driver loaded (INT 2Fh AX=C000h).',13,10,'$'
again_text db 'HELLO sample driver is already loaded.',13,10,'$'
