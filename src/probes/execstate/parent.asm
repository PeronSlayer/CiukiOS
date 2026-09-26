; Real DOS EXEC/InDOS/metadata test; binary records go to QEMU debug port E9.
bits 16
org 0x100
start:
    cli
    push cs
    pop ss
    mov sp, stack_top
    sti
    push cs
    pop ds
    push cs
    pop es
    mov bx, (stack_top-$$+0x100+15)/16
    mov ah, 0x4a
    int 0x21
    jc fatal
    mov ah,0x34
    int 0x21
    mov [flag_off],bx
    mov [kernel_seg],es
    mov ax,0x1607
    mov bx,0x15
    xor cx,cx
    int 0x2f
    mov ax,[es:bx+2]
    mov [caller_ds_off],ax
    mov ax,[es:bx+4]
    mov [caller_bx_off],ax
    mov ax,cs
    mov [params+4],ax
    mov [params+8],ax
    mov [params+12],ax
    mov ax,0x3513
    int 0x21
    mov [old13],bx
    mov [old13+2],es
    mov dx,hook13
    mov ax,0x2513
    int 0x21
    mov word [kind],0
    call record
.next:
    mov bx,[case_id]
    shl bx,1
    mov dx,[paths+bx]
    mov ax,cs
    mov es,ax
    mov bx,params
    mov ax,0x4b00
    cmp word [case_id],10
    jne .not_overlay
    mov bx,overlay_params
    mov ax,cs
    add ax,(overlay_space-$$+0x100)/16
    mov [bx],ax
    mov [bx+2],ax
    mov ax,0x4b03
.not_overlay:
    cmp word [case_id],11
    jne .valid_al
    mov al,0x7f
.valid_al:
    mov cx,0x1234
    mov si,0x5678
    mov di,0x789a
    mov bp,0x2345
    ; ZF=1 must survive even a child whose last DOS call alters ZF.
    push ax
    xor ax,ax
    pop ax
    mov word [cs:kind],1
    call record
    int 0x21
    mov word [cs:kind],2
    call record
    cmp word [case_id],10
    jne .no_overlay_check
    mov si,overlay_space
    mov di,overlay_expected
    mov cx,overlay_expected_end-overlay_expected
    push ds
    pop es
    repe cmpsb
    je .no_overlay_check
    mov al,'!'
    out 0xe9,al
.no_overlay_check:
    mov ah,0x4d
    int 0x21
    mov word [cs:kind],3
    call record
    inc word [case_id]
    cmp word [case_id],14
    jb .next
    mov word [kind],5
    call record
    push ds
    mov dx,[old13]
    mov ds,[old13+2]
    mov ax,0x2513
    int 0x21
    pop ds
    mov ax,0x4c00
    int 0x21
fatal:
    mov word [cs:kind],0xff
    call record
    mov ax,0x4c01
    int 0x21

; Capture before any DOS call, preserving every register and FLAGS.
hook13:
    pushf
    pusha
    push es
    mov es,[cs:kernel_seg]
    mov bx,[cs:flag_off]
    mov al,'@'
    out 0xe9,al
    mov al,'I'
    out 0xe9,al
    mov al,[es:bx]
    add al,'0'
    out 0xe9,al
    mov al,10
    out 0xe9,al
    pop es
    popa
    popf
    jmp far [cs:old13]
record:
    pushf
    pusha
    push ds
    push es
    mov bp,sp
    push cs
    pop ds
    mov si,marker
    call puts
    mov ax,[kind]
    call hex
    mov ax,[case_id]
    call hex
    mov es,[kernel_seg]
    mov bx,[flag_off]
    xor ax,ax
    mov al,[es:bx]
    call hex
    ; PUSHA snapshot: ES DS DI SI BP SP BX DX CX AX FLAGS.
    mov si,bp
    mov cx,11
.reg:
    mov ax,[ss:si]
    call hex
    add si,2
    loop .reg
    mov bx,[caller_ds_off]
    mov ax,[es:bx]
    call hex
    mov bx,[caller_bx_off]
    mov ax,[es:bx]
    call hex
    mov al,10
    out 0xe9,al
    pop es
    pop ds
    popa
    popf
    ret
puts:
    lodsb
    test al,al
    jz .done
    out 0xe9,al
    jmp puts
.done: ret
hex:
    push ax
    push bx
    push cx
    mov bx,ax
    mov cx,4
.loop:
    rol bx,4
    mov al,bl
    and al,15
    add al,'0'
    cmp al,'9'
    jbe .out
    add al,7
.out:
    out 0xe9,al
    loop .loop
    mov al,' '
    out 0xe9,al
    pop cx
    pop bx
    pop ax
    ret
marker db '@EXSTATE ',0
kind dw 0
case_id dw 0
flag_off dw 0
kernel_seg dw 0
caller_ds_off dw 0
caller_bx_off dw 0
old13 dw 0,0
params dw 0,tail,0,0x5c,0,0x6c,0
tail db 0,13
paths dw p0,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10,p0,p12,p13
p0 db 'C4C.COM',0
p1 db 'C20.COM',0
p2 db 'CRET.COM',0
p3 db 'C00.COM',0
p4 db 'M4C.EXE',0
p5 db 'M20.EXE',0
p6 db 'M00.EXE',0
p7 db 'NEST.COM',0
p8 db 'ABSENT.EXE',0
p9 db 'BROKEN.EXE',0
p10 db 'OVERLAY.EXE',0
p12 db 'NOMEM.EXE',0
p13 db 'C31.COM',0
overlay_expected db 'OVERLAY-EXEC-IN-DOS-TEST',0
overlay_expected_end:
overlay_params dw 0,0
align 16,db 0
overlay_space times 512 db 0
stack_bottom times 1024 db 0
stack_top:
