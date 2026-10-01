; BADHOOK.COM - test fixture: a driver that hooks IRQ 3 (INT 0Bh) and
; INT 66h, unmasks IRQ 3, and then fails (exit code 3) without undoing it.
bits 16
org 0x100
    mov dx,msg
    mov ah,9
    int 0x21
    mov ax,0x250B
    mov dx,handler
    int 0x21
    mov ax,0x2566
    int 0x21
    in al,0x21
    and al,0xF7
    out 0x21,al
    mov ax,0x4C03
    int 0x21
handler:
    iret
msg db '[FIXTURE] BADHOOK hooked INT 0Bh/66h, failing',13,10,'$'
