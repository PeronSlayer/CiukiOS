; Convert a completed one-byte-per-pixel 16-colour band to VGA mode 12h.
; All horizontal bounds are multiples of eight; the band stride is its width.
; No VRAM read or read/modify/write is needed. Each caller owns its state.
%macro VGA_INDEXED_PRESENT 10
%1:
    pushad
    push fs
    push es
    mov ax,[%4]
    sub ax,[%3]
    shr ax,3
    jz %%done
    mov [%7],ax
    mov ax,[%6]
    sub ax,[%5]
    jz %%done
    mov [%8],ax
    mov ax,[%5]
    imul ax,80
    mov bx,[%3]
    shr bx,3
    add ax,bx
    mov [%9],ax
    mov fs,[%2]
    mov ax,0xA000
    mov es,ax
    mov dx,0x3CE
    mov ax,0x0001             ; enable set/reset: off
    out dx,ax
    mov ax,0x0003             ; data rotate: copy
    out dx,ax
    mov ax,0x0005             ; write mode 0
    out dx,ax
    mov ax,0xFF08             ; bit mask: all eight pixels
    out dx,ax
    mov byte [%10],1
%%plane:
    mov dx,0x3C4
    mov al,2
    mov ah,[%10]
    out dx,ax
    xor si,si
    mov di,[%9]
    mov bp,[%8]
%%row:
    mov bx,[%7]
%%byte:
    xor ah,ah
    mov cx,8
%%pixel:
    shl ah,1
    mov al,[fs:si]
    test al,[%10]
    jz %%zero
    or ah,1
%%zero:
    inc si
    loop %%pixel
    mov [es:di],ah
    inc di
    dec bx
    jnz %%byte
    add di,80
    sub di,[%7]
    dec bp
    jnz %%row
    shl byte [%10],1
    cmp byte [%10],16
    jb %%plane
    mov ax,0x0F02             ; leave all planes enabled
    out dx,ax
%%done:
    pop es
    pop fs
    popad
    ret
%endmacro
