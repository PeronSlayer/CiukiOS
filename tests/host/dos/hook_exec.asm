; H50 checked EXEC rollback, DTA/PSP restoration and original overlay loader.
bits 16
org 0x100
start:
    cld
    push cs
    pop ds
    push cs
    pop es
    mov bx,(image_end-$$+0x10f)/16
    mov ah,0x4a
    int 0x21
    jc fail
    mov [params+4],ds
    mov [params+8],ds
    mov [params+12],ds
    mov dx,dta
    mov ah,0x1a
    int 0x21
    mov dx,relative_dir
    mov ah,0x3b
    int 0x21
    jc fail
    mov dx,tsr
    mov bx,params
    mov ax,0x4b00
    int 0x21
    jc fail
    mov ax,0x1216
    xor bx,bx
    int 0x2f
    jc fail
    mov ax,[es:di]
    mov [refs],ax
    push cs
    pop es
    mov dx,toobig
    mov bx,params
    mov ax,0x4b00
    int 0x21
    jnc fail
    cmp ax,8
    jne fail
    mov ax,0x1216
    xor bx,bx
    int 0x2f
    jc fail
    mov ax,[es:di]
    cmp ax,[refs]
    jne fail
    push cs
    pop es
    mov dx,missing
    mov bx,params
    mov ax,0x4b00
    int 0x21
    jnc fail
    cmp ax,2
    jne fail
    mov dx,badrel
    mov bx,params
    mov ax,0x4b00
    int 0x21
    jnc fail
    cmp ax,11
    jne fail
    mov ah,0x62
    int 0x21
    mov ax,cs
    cmp ax,bx
    jne fail
    mov ah,0x2f
    int 0x21
    cmp bx,dta
    jne fail
    mov ax,es
    mov bx,cs
    cmp ax,bx
    jne fail
    mov bx,64
    mov ah,0x48
    int 0x21
    jc fail
    mov [overlay],ax
    mov [allocated],ax
    mov dx,goodrel
    mov bx,overlay
    mov ax,0x4b03
    int 0x21
    jc fail
    mov es,[allocated]
    cmp word [es:0],0x1151
    jne fail
    mov ah,0x49
    int 0x21
    jc fail
    mov si,passed
    jmp report
fail:
    mov si,failed
report:
    lodsb
    test al,al
    jz done
    out 0xe9,al
    jmp report
done:
    mov ax,0x4c00
    int 0x21
passed db 'H50EXEC PASS',13,10,0
failed db 'H50EXEC FAIL',13,10,0
relative_dir db 'C:\RELBASE',0
tsr db 'H50TSR.COM',0
toobig db 'C:\TOOBIG.COM',0
refs dw 0
missing db 'C:\H50NONE.EXE',0
badrel db 'C:\BADREL.EXE',0
goodrel db 'C:\GOODREL.EXE',0
params dw 0,tail,0,0x5c,0,0x6c,0
tail db 0,13
overlay dw 0,0x40
allocated dw 0
dta times 128 db 0
image_end:
