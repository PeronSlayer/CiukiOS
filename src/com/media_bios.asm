bits 16
%ifndef MEDIA_DRIVER
section .start
global _start
extern media_main, _bss_start, _bss_end, _stack_top
_start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, _stack_top
    sti
    cld
    mov di, _bss_start
    mov cx, _bss_end
    sub cx, di
    xor ax, ax
    rep stosb
    mov bx, _stack_top
    add bx, 15
    shr bx, 4
    mov ah, 0x4a
    int 0x21
    call media_main
    mov ah, 0x4c
    int 0x21
%endif
section .text
extern bi_ax, bi_bx, bi_cx, bi_dx, bi_si, bi_di, bi_flags
global bios13, dos21, key16, own_segment, timer_ticks, port_in, port_out, port_inw, port_outw
; BIOS firmware has historically damaged upper halves and segment registers.
; All firmware access uses private globals and preserves the complete caller
; state. In particular AH=08 returns ES:DI; these never escape to C code.
%macro interrupt_wrapper 2
%1:
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    push cs
    pop ds
    push cs
    pop es
    mov ax, [bi_ax]
    mov bx, [bi_bx]
    mov cx, [bi_cx]
    mov dx, [bi_dx]
    mov si, [bi_si]
    mov di, [bi_di]
    sti
    int %2
    mov [cs:bi_ax], ax
    mov [cs:bi_bx], bx
    mov [cs:bi_cx], cx
    mov [cs:bi_dx], dx
    mov [cs:bi_si], si
    mov [cs:bi_di], di
    pushf
    pop word [cs:bi_flags]
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    cld
    ret
%endmacro
interrupt_wrapper bios13, 0x13
interrupt_wrapper dos21, 0x21
interrupt_wrapper key16, 0x16
own_segment:
    mov ax, cs
    ret
timer_ticks:
    push es
    mov ax, 0x40
    mov es, ax
    mov ax, [es:0x6c]
    pop es
    ret
port_in:
    push bp
    mov bp, sp
    mov dx, [bp+4]
    xor ax, ax
    in al, dx
    pop bp
    ret
port_inw:
    push bp
    mov bp, sp
    mov dx, [bp+4]
    in ax, dx
    pop bp
    ret
port_out:
    push bp
    mov bp, sp
    mov dx, [bp+4]
    mov ax, [bp+6]
    out dx, al
    pop bp
    ret
port_outw:
    push bp
    mov bp, sp
    mov dx, [bp+4]
    mov ax, [bp+6]
    out dx, ax
    pop bp
    ret
