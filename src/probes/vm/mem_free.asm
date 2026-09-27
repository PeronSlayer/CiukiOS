; MEMFREE.COM: report the largest allocatable conventional block (INT 21h/48h).
bits 16
org 100h
    mov bx,(end_of_image-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    mov bx,0FFFFh
    mov ah,48h
    int 21h
    mov ax,bx                        ; largest block in paragraphs
    mov cx,4
    mov di,digits
.hex:
    rol ax,4
    mov dl,al
    and dl,15
    add dl,'0'
    cmp dl,'9'
    jbe .store
    add dl,7
.store:
    mov [di],dl
    inc di
    loop .hex
    mov dx,text
    mov ah,9
    int 21h
    mov ax,4C00h
    int 21h
text db '[MEMFREE] largest block paragraphs='
digits db '0000',13,10,'$'
end_of_image:
