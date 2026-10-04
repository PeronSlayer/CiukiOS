; VMSTART.COM - load the CiukiOS VM session stack for the DOS window.
;
;   VMSTART [Jemm options]
;
; The desktop runs it at the start of every normal session. Runs
; \VM\JEMMEX.EXE LOAD <options> (default: the options the DOS window
; profile is validated with, see below) and then \VM\JLOAD.EXE \VM\CVSESS.DLL,
; unless the session module is already resident. Messages are English; a
; failed step returns ERRORLEVEL 1. On a graphics screen (the boot splash at
; startup) nothing is drawn: its messages and the console output of Jemm
; and JLOAD go to COM1 only.
;
; Default options: no EMS, UMBs only in the qualified C000-EFFF region, and
; no VME. JemmEx owns the BIOS-discovered RAM and allocates it on demand.
bits 16
cpu 386
org 100h

%include "src/vm/session_abi.inc"

start:
    cld
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    mov si,no_memory
    jc fail
    call quiet_detect

    ; Already resident (the desktop starts it at boot): nothing to do.
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    push cs
    pop es
    jz .load
    mov ax,4C00h
    int 21h
.load:
    ; Jemm386 tail: " LOAD " + options (default or the command tail).
    mov di,jemm_tail+1
    mov si,load_word
    call copy_z
    mov si,81h
.skip:
    lodsb
    cmp al,' '
    je .skip
    cmp al,9
    je .skip
    dec si
    cmp al,13
    jne .copy_tail
    mov si,default_options
    call copy_z
    jmp .tail_done
.copy_tail:
    lodsb
    cmp al,13
    je .tail_done
    stosb
    cmp di,jemm_tail+126
    jb .copy_tail
.tail_done:
    mov byte [di],13
    mov ax,di
    sub ax,jemm_tail+1
    mov [jemm_tail],al

    mov si,msg_jemm
    call print
    call silence
    mov word [params+2],jemm_tail
    mov dx,jemm_path
    call exec
    mov si,jemm_failed
    jc fail
    mov ah,4Dh
    int 21h
    test al,al
    jnz fail

    mov si,msg_jload
    call print
    mov word [params+2],jload_tail
    mov dx,jload_path
    call exec
    mov si,jload_failed
    jc fail
    mov ah,4Dh
    int 21h
    test al,al
    jnz fail

    call speak
    mov si,msg_ready
    call print
    mov ax,4C00h
    int 21h

fail:
    push si
    call speak
    pop si
    call print
    mov ax,4C01h
    int 21h


; DS:SI zero-terminated -> ES:DI (DI advanced).
copy_z:
    lodsb
    test al,al
    jz .done
    stosb
    jmp copy_z
.done:
    ret

; DS:DX program, params -> tail. CF from EXEC.
exec:
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov bx,params
    mov ax,4B00h
    mov [saved_sp],sp
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
    ret

print:
    lodsb
    test al,al
    jz .done
    cmp byte [quiet],0
    jne .serial
    mov dl,al
    mov ah,2
    int 21h
    jmp print
.serial:
    call com1
    jmp print
.done:
    ret

%include "src/com/quiet_console.inc"

load_word db ' LOAD ',0
%ifdef VMSTART_JEMMEX
default_options db 'NOEMS X=A000-CCFF I=CD00-EBFF X=EC00-FFFF NOVME',0
jemm_path db '\VM\JEMMEX.EXE',0
%else
default_options db 'NOEMS NOHI X=A000-CCFF I=CD00-EBFF X=EC00-FFFF NODYN MAX=32M MIN=32M NOVME',0
jemm_path db '\VM\JEMM386.EXE',0
%endif
jload_path db '\VM\JLOAD.EXE',0
jload_tail db jload_tail_end-jload_tail-1
    db ' \VM\CVSESS.DLL'
jload_tail_end:
    db 13
msg_jemm db 'VMSTART: loading Jemm memory manager and V86 monitor...',13,10,0
msg_jload db 'VMSTART: loading the CiukiOS session module (CVSESS.DLL)...',13,10,0
msg_ready db 'VMSTART: ready.',13,10,0
no_memory db 'VMSTART: cannot resize its memory block.',13,10,0
jemm_failed db 'VMSTART: Jemm memory manager failed (see its message).',13,10,0
jload_failed db 'VMSTART: \VM\JLOAD.EXE failed (see its message).',13,10,0

params dw 0,0,0,5Ch,0,6Ch,0
saved_sp dw 0
jemm_tail times 128 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
