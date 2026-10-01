; Start of the LFN test probes (OpenWatcom C, .COM): stack, main, exit code.
cpu 386
segment _TEXT class=CODE public align=16 use16
segment CONST class=DATA public align=2 use16
segment CONST2 class=DATA public align=2 use16
segment _DATA class=DATA public align=2 use16
segment _BSS class=BSS public align=2 use16
segment STACK class=STACK public align=16 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS STACK
extern probe_main_
global start
global intr_
global __U4M
global __I4M
global __U4D
global __I4D
segment _TEXT
start:
    mov sp,stack_top
    mov bx,image_end
    add bx,15
    shr bx,4
    mov ah,4Ah
    int 21h
    ; BSS is not in the file: zero it.
    mov di,bss_start
    mov cx,stack_top
    sub cx,di
    xor al,al
    cld
    rep stosb
    call probe_main_
    mov ah,4Ch
    int 21h
; int intr(int n, struct regs *r) - AX BX CX DX SI DI DS ES in and out,
; FLAGS at +16. Returns the carry.
intr_:
    push bx
    push cx
    push si
    push di
    push bp
    push es
    push ds
    mov [cs:intno],al
    mov bp,dx
    push bp
    push word [bp+12]
    mov es,[bp+14]
    mov ax,[bp+0]
    mov bx,[bp+2]
    mov cx,[bp+4]
    mov dx,[bp+6]
    mov si,[bp+8]
    mov di,[bp+10]
    pop ds
    db 0CDh
intno:
    db 21h
    pushf
    push ds
    push ax
    mov ax,cs
    mov ds,ax
    mov bp,sp
    mov bp,[bp+6]
    pop word [bp+0]
    pop word [bp+12]
    pop word [bp+16]
    mov [bp+2],bx
    mov [bp+4],cx
    mov [bp+6],dx
    mov [bp+8],si
    mov [bp+10],di
    mov [bp+14],es
    mov ax,[bp+16]
    and ax,1
    add sp,2
    pop ds
    pop es
    pop bp
    pop di
    pop si
    pop cx
    pop bx
    ret
__U4M:
__I4M:
    push si
    push di
    mov si,ax
    mov di,dx
    mul bx
    push dx
    push ax
    mov ax,di
    mul bx
    mov di,ax
    mov ax,si
    mul cx
    add di,ax
    pop ax
    pop dx
    add dx,di
    pop di
    pop si
    ret
__U4D:
__I4D:
    push bp
    push dx
    push ax
    push cx
    push bx
    mov bp,sp
    mov eax,[bp+4]
    mov ebx,[bp+0]
    xor edx,edx
    test ebx,ebx
    jz .z
    div ebx
.z: mov ebx,edx
    mov edx,eax
    shr edx,16
    mov ecx,ebx
    shr ecx,16
    add sp,8
    pop bp
    ret
segment _BSS
bss_start:
segment STACK
    times 4096 db 0
stack_top:
image_end:
