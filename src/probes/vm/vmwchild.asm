; VMWCHILD.COM - VM manager gate, second VM with its own DOS window session.
; Runs in a VM created by \VM\VMFORK.COM (from VMWTEST). Begins the CVSESSION
; session (virtual VGA) and the device model's keyboard, sets text mode 03h
; through the virtual VGA BIOS, writes "VM1 SCREEN" on row 0 and then echoes
; every key it reads through INT 16h on row 1 and on COM1 ("[VMW1] key
; xxxx"). Esc ends the session; exit code 0, or 1-3 on a failed step. A
; digit in the command tail names the VM in its texts ("VM2 SCREEN",
; "[VMW2] ..."; default 1).
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_devices_abi.inc"

start:
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    mov si,81h
.tail:
    lodsb
    cmp al,' '
    je .tail
    cmp al,'1'
    jb .named
    cmp al,'9'
    ja .named
    mov [msg_screen+2],al
    mov [msg_begin+4],al
    mov [msg_dev+4],al
    mov [msg_end+4],al
    mov [msg_ready+4],al
    mov [msg_key+4],al
    mov [msg_bye+4],al
.named:
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    push cs
    pop es
    mov ax,VM_OP_BEGIN
    call far [entry]
    push cs
    pop ds
    mov si,msg_begin
    mov bl,1
    jc fail
    mov ebx,CVDEV_CAP_KEYBOARD
    xor ecx,ecx
    mov edx,1000000                     ; TSC kHz: no timed device is used
    mov ax,VM_OP_DEV_BEGIN
    call far [entry]
    push cs
    pop ds
    mov si,msg_dev
    mov bl,2
    jc fail_session
    mov ax,0003h                        ; virtual VGA BIOS
    int 10h
    push 0B800h
    pop es
    xor di,di
    mov si,msg_screen
    mov ah,07h
.screen:
    lodsb
    test al,al
    jz .screen_done
    stosw
    jmp .screen
.screen_done:
    push cs
    pop es
    mov si,msg_ready
    call serial_text
    mov di,160                          ; row 1
.key:
    xor ah,ah
    int 16h
    push ax
    mov si,msg_key
    call serial_text
    pop ax
    push ax
    call serial_hex
    mov si,msg_crlf
    call serial_text
    pop ax
    cmp al,27
    je .done
    push es
    push 0B800h
    pop es
    mov ah,07h
    stosw
    pop es
    cmp di,160+2*79
    jb .key
    mov di,160
    jmp .key
.done:
    mov ax,VM_OP_DEV_END
    call far [entry]
    push cs
    pop ds
    mov ax,VM_OP_END
    call far [entry]
    push cs
    pop ds
    mov si,msg_end
    mov bl,3
    jc fail
    mov si,msg_bye
    call serial_text
    mov ax,4C00h
    int 21h
fail_session:
    push bx
    mov ax,VM_OP_END
    call far [entry]
    push cs
    pop ds
    pop bx
fail:
    push bx
    call serial_text
    pop ax
    mov ah,4Ch
    int 21h

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
serial_hex:
    push cx
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
    pop cx
    ret

msg_begin db '[VMW1] BEGIN failed',13,10,0
msg_dev db '[VMW1] DEV_BEGIN failed',13,10,0
msg_end db '[VMW1] END failed',13,10,0
msg_ready db '[VMW1] ready',13,10,0
msg_key db '[VMW1] key ',0
msg_bye db '[VMW1] session ended',13,10,0
msg_crlf db 13,10,0
msg_screen db 'VM1 SCREEN',0
entry dd 0
    align 2
    times 512 db 0
stack_top:
image_end:
