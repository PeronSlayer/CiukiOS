; AUXSTART.COM - install the video BIOS auxiliary stack quietly.
;
; The desktop runs it at startup, while the boot splash is still on screen.
; It runs \SYSTEM\VIDEO\AUXSTACK.COM (vbesvga.drv's unmodified resident
; helper) and routes AUXSTACK's banner to COM1 on a graphics screen, so
; nothing is drawn over the splash. On a text screen the banner is shown.
; ERRORLEVEL is AUXSTACK's, or 1 when it could not be run.
bits 16
cpu 386
org 100h

start:
    cld
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    call quiet_detect
    call silence
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov [saved_sp],sp
    mov dx,auxstack_path
    mov bx,params
    mov ax,4B00h
    int 21h
    cli
    mov bx,cs
    mov ss,bx
    mov sp,[cs:saved_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    cld
    pushf
    call speak
    popf
    mov al,1
    jc .exit
    mov ah,4Dh
    int 21h
.exit:
    mov ah,4Ch
    int 21h

%include "src/com/quiet_console.inc"

auxstack_path db '\SYSTEM\VIDEO\AUXSTACK.COM',0
tail db 0,13
params dw 0,tail,0,5Ch,0,6Ch,0
saved_sp dw 0
    align 2
    times 256 db 0
stack_top:
image_end:
