; Report the XMS 3.x largest and total free sizes in KiB, in eight hex digits.
bits 16
org 0x100
start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    mov bx,(image_end - $$ + 0x100 + 15) >> 4
    mov ah,0x4a
    int 0x21
    jc .dos_failed
    mov ax,0x4300
    int 0x2f
    cmp al,0x80
    jne .missing
    mov ax,0x4310
    int 0x2f
    mov [xms_call],bx
    mov [xms_call+2],es
    mov ah,0x88
    call far [xms_call]
    cmp bl,0
    jne .missing
    push eax
    push edx
    mov si,msg_largest
    call puts
    pop edx
    pop eax
    push edx
    call hex32
    mov si,msg_free
    call puts
    pop eax
    push eax
    call hex32
    mov si,msg_end
    call puts
    pop eax
    cmp eax,2048
    jb .alloc_failed
    mov edx,eax
    sub edx,1024
    mov ah,0x89
    call far [xms_call]
    cmp ax,1
    jne .alloc_failed
    mov ah,0x0a
    call far [xms_call]
    cmp ax,1
    jne .alloc_failed
    mov si,msg_alloc_pass
    call puts
    mov ah,0x48
    mov bx,0xffff
    int 0x21
    jnc .dos_failed
    cmp ax,8
    jne .dos_failed
    movzx eax,bx
    shr eax,6
    push eax
    mov si,msg_dos_largest
    call puts
    pop eax
    call hex32
    mov si,msg_dos_suffix
    call puts
    mov ax,0x4c00
    int 0x21
.dos_failed:
    mov si,msg_dos_failed
    call puts
    mov ax,0x4c01
    int 0x21
.alloc_failed:
    mov si,msg_alloc_failed
    call puts
    mov ax,0x4c01
    int 0x21
.missing:
    mov si,msg_missing
    call puts
    mov ax,0x4c01
    int 0x21

puts:
    lodsb
    test al,al
    jz .done
    mov dl,al
    mov ah,2
    int 0x21
    jmp puts
.done:
    ret

hex32:
    push cx
    mov cx,8
.digit:
    rol eax,4
    push eax
    and al,15
    cmp al,10
    jb .number
    add al,'A'-10
    jmp .emit
.number:
    add al,'0'
.emit:
    mov dl,al
    mov ah,2
    int 0x21
    pop eax
    loop .digit
    pop cx
    ret


xms_call dd 0
msg_largest db '[MEMCAP] XMS88 largest=',0
msg_free db ' free=',0
msg_end db 13,10,0
msg_alloc_pass db '[MEMCAP] XMS89 allocate+free PASS',13,10,0
msg_alloc_failed db '[MEMCAP] XMS89 allocate+free FAIL',13,10,0
msg_dos_largest db '[MEMCAP] DOS largest=',0
msg_dos_suffix db ' KiB',13,10,0
msg_dos_failed db '[MEMCAP] DOS largest query FAIL',13,10,0
msg_missing db '[MEMCAP] XMS88 unavailable',13,10,0
times 2048 db 0
stack_top:
image_end:
