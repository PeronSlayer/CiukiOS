; VECPROBE.COM - test fixture: reports INT 0Bh, INT 66h, INT 60h and the PIC
; masks on COM1 ("[VECPROBE] 0B=SSSS:OOOO 66=SSSS:OOOO 60=SSSS:OOOO PIC=MMMM
; PKT60=0/1"), so a gate can check what a driver left behind.
bits 16
org 0x100
    mov si,tag
    call text
    mov al,0x0B
    call vector
    mov al,0x66
    call vector
    mov al,0x60
    call vector
    mov si,pic
    call text
    in al,0xA1
    call hex2
    in al,0x21
    call hex2
    mov si,pkt
    call text
    ; "PKT DRVR" at INT 60h + 3?
    xor ax,ax
    mov es,ax
    les di,[es:0x60*4]
    add di,3
    mov si,sig
    mov cx,8
    repe cmpsb
    mov al,'0'
    jne .no
    mov al,'1'
.no:
    call char
    mov si,crlf
    call text
    mov ax,0x4C00
    int 0x21
; AL = vector: " VV=SSSS:OOOO"
vector:
    push ax
    mov al,' '
    call char
    pop ax
    push ax
    call hex2
    mov al,'='
    call char
    pop ax
    xor ah,ah
    shl ax,2
    mov bx,ax
    xor ax,ax
    mov es,ax
    mov ax,[es:bx+2]
    call hex4
    mov al,':'
    call char
    mov ax,[es:bx]
    jmp hex4
hex4:
    push ax
    mov al,ah
    call hex2
    pop ax
hex2:
    push ax
    shr al,4
    call digit
    pop ax
digit:
    and al,0x0F
    add al,'0'
    cmp al,'9'
    jbe char
    add al,7
char:
    push dx
    push ax
    mov dx,0x3FD
.wait:
    in al,dx
    test al,0x20
    jz .wait
    pop ax
    mov dx,0x3F8
    out dx,al
    pop dx
    ret
text:
    cs lodsb
    test al,al
    jz .done
    call char
    jmp text
.done:
    ret
tag db '[VECPROBE]',0
pic db ' PIC=',0
pkt db ' PKT60=',0
crlf db 13,10,0
sig db 'PKT DRVR'
