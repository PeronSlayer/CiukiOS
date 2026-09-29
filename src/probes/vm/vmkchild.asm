; VMKCHILD.COM - VM manager gate: how a VM with a DOS window session ends.
; Runs in VM 1 (VMFORK from VMKTEST). Begins the CVSESSION session and the
; device-model keyboard (the keyboard focus stays with the system VM), then,
; by the command tail:
;   E  exits at once (exit code 3) without DEV_END or END: the VM manager
;      must end the session when the VM exits;
;   D  ends devices and session properly, then exits with 3;
;   H  hangs with virtual interrupts off (CLI, JMP $): only VMM_KILL from the
;      system VM ends it.
; Exit codes 1/2: BEGIN/DEV_BEGIN failed.
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
.blank:
    lodsb
    cmp al,' '
    je .blank
    and al,0DFh
    mov [mode],al
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
    mov al,1
    jc .exit
    mov ebx,CVDEV_CAP_KEYBOARD
    xor ecx,ecx
    mov edx,1000000
    mov ax,VM_OP_DEV_BEGIN
    call far [entry]
    push cs
    pop ds
    mov al,2
    jc .exit
    mov si,msg_open
    call serial_text
    cmp byte [mode],'H'
    je .hang
    cmp byte [mode],'D'
    jne .leave
    mov ax,VM_OP_DEV_END
    call far [entry]
    mov ax,VM_OP_END
    call far [entry]
.leave:
    mov al,3
.exit:
    mov ah,4Ch
    int 21h
.hang:
    cli
    jmp $

serial_text:
    lodsb
    test al,al
    jz .done
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
    jmp serial_text
.done:
    ret

msg_open db '[VMK1] session open',13,10,0
entry dd 0
mode db 0
    align 2
    times 256 db 0
stack_top:
image_end:
