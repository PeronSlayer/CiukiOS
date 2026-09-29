; VMCCHILD.COM - VM manager gate, VM 1 side of VMCTEST: counts BIOS timer
; ticks over 10 s of real time while the system VM does the same, both busy.
; "[VMC1] ticks xxxx" on COM1; exit code 0.
bits 16
cpu 386
org 100h

start:
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    call measure_ticks
    push ax
    mov si,msg_ticks
    call serial_text
    pop ax
    call serial_hex
    mov si,msg_crlf
    call serial_text
    mov ax,4C00h
    int 21h

%include "src/probes/vm/vmclock.inc"

serial_hex:
    mov cx,4
.digit:
    rol ax,4
    push ax
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe .out
    add al,7
.out:
    call serial_char
    pop ax
    loop .digit
    ret

serial_text:
    lodsb
    test al,al
    jz .done
    call serial_char
    jmp serial_text
.done:
    ret

serial_char:
    push dx
    push ax
    mov dx,3FDh
.w:
    in al,dx
    test al,20h
    jz .w
    pop ax
    mov dx,3F8h
    out dx,al
    pop dx
    ret

msg_ticks db '[VMC1] ticks ',0
msg_crlf db 13,10,0
    align 2
    times 512 db 0
stack_top:
image_end:
