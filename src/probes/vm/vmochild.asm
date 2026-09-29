; VMOCHILD.COM - VM manager gate, a VM with its own DOS window session and
; device-model mouse (VMOTEST). Begins the CVSESSION session and the model's
; keyboard and mouse, writes "VMn MOUSE" on row 0 of its virtual screen and
; installs a PS/2 pointing-device handler through the BIOS (INT 15h C2xxh,
; which programs the model's controller). Each packet it gets adds to its
; totals, kept as words at row 1 of its screen (packets, X sum, Y sum,
; buttons: read back by VMOTEST) and printed on COM1 ("[VMO1] mouse
; nnnn xxxx yyyy bbbb"). Esc ends the session; exit code 0, or 1-4 on a
; failed step. A digit in the command tail names the VM (default 1).
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
    mov [msg_bios+4],al
    mov [msg_end+4],al
    mov [msg_ready+4],al
    mov [msg_mouse+4],al
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
    mov ebx,CVDEV_CAP_KEYBOARD|CVDEV_CAP_MOUSE
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
    call show_totals
    ; The BIOS pointing-device interface, on the model's controller.
    mov ax,0C205h                       ; initialise, 3-byte packets
    mov bh,3
    int 15h
    jc .bios_failed
    push cs
    pop es
    mov bx,packet_handler
    mov ax,0C207h
    int 15h
    jc .bios_failed
    mov ax,0C200h                       ; enable
    mov bh,1
    int 15h
    jnc .bios_ready
.bios_failed:
    push cs
    pop ds
    mov si,msg_bios
    mov bl,4
    jmp fail_devices
.bios_ready:
    push cs
    pop ds
    push cs
    pop es
    mov si,msg_ready
    call serial_text
.loop:
    mov ax,[packets]
    cmp ax,[shown]
    je .key
    mov [shown],ax
    call show_totals
    mov si,msg_mouse
    call serial_text
    mov si,totals
    mov cx,4
.value:
    lodsw
    call serial_hex
    mov al,' '
    call serial_char
    loop .value
    mov si,msg_crlf
    call serial_text
.key:
    mov ah,1
    int 16h
    jz .loop
    xor ah,ah
    int 16h
    cmp al,27
    jne .loop
    mov ax,0C200h                       ; disable, no handler
    mov bh,0
    int 15h
    xor bx,bx
    mov es,bx
    mov ax,0C207h
    int 15h
    push cs
    pop es
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
fail_devices:
    push bx
    mov ax,VM_OP_DEV_END
    call far [entry]
    push cs
    pop ds
    pop bx
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

; The totals as words at row 1 of the virtual screen.
show_totals:
    push es
    push 0B800h
    pop es
    mov di,160
    mov si,totals
    mov cx,4
    cli
    rep movsw
    sti
    pop es
    ret

; BIOS pointing-device handler (far): status, X, Y, 0 on the stack.
packet_handler:
    push bp
    mov bp,sp
    push ax
    push ds
    push cs
    pop ds
    movzx ax,byte [bp+10]               ; X, sign in status bit 4
    test byte [bp+12],10h
    jz .x
    sub ax,100h
.x:
    add [sum_x],ax
    movzx ax,byte [bp+8]                ; Y, sign in status bit 5
    test byte [bp+12],20h
    jz .y
    sub ax,100h
.y:
    add [sum_y],ax
    movzx ax,byte [bp+12]
    and ax,7
    mov [buttons],ax
    inc word [packets]
    pop ds
    pop ax
    pop bp
    retf

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

msg_begin db '[VMO1] BEGIN failed',13,10,0
msg_dev db '[VMO1] DEV_BEGIN failed',13,10,0
msg_bios db '[VMO1] INT 15h pointing device failed',13,10,0
msg_end db '[VMO1] END failed',13,10,0
msg_ready db '[VMO1] ready',13,10,0
msg_mouse db '[VMO1] mouse ',0
msg_bye db '[VMO1] session ended',13,10,0
msg_crlf db 13,10,0
msg_screen db 'VM1 MOUSE',0
entry dd 0
shown dw 0
totals:
packets dw 0
sum_x dw 0
sum_y dw 0
buttons dw 0
    align 2
    times 512 db 0
stack_top:
image_end:
