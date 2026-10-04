; NATPAGE.COM - exercise Jemm-owned page blocks without exposing pointers to DOS.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"

start:
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    push cs
    pop es
    mov ax,[entry]
    or ax,[entry+2]
    jz fail
    mov cx,8
.repeat:
    push cx
    mov ax,VM_OP_NATIVE_PAGE_PROBE
    call far [entry]
    push cs
    pop ds
    pop cx
    jc fail
    test ax,ax
    jnz fail
    loop .repeat
    mov si,msg_pass
    call serial_text
    mov ax,4C00h
    int 21h
fail:
    mov si,msg_fail
    call serial_text
    mov ax,4C01h
    int 21h

serial_text:
    lodsb
    test al,al
    jz .done
    push ax
    push dx
    mov dx,3FDh
.ready:
    in al,dx
    test al,20h
    jz .ready
    pop dx
    pop ax
    mov dx,3F8h
    out dx,al
    jmp serial_text
.done:
    ret

entry dd 0
msg_pass db '[NATIVE-PAGES] PASS 8 owner/zero/release probes',13,10,0
msg_fail db '[NATIVE-PAGES] FAIL',13,10,0
image_end:
