; VMSTEST.COM - VM manager gate: Sound Blaster DMA of a VM that does not run
; all the time. Measures the TSC rate (while it is the only VM), runs
; \VMSCHILD.COM <kHz> in VM 1 through \VM\VMFORK.COM and keeps this VM busy
; ("[VMS0] n" on COM1, never yielding) until VM 1 has played its square wave
; and ended. "[VMSTEST] PASS" when VM 1 exited with 0; the harness judges
; the recorded sound.
bits 16
cpu 586
org 100h
%include "src/vm/session_abi.inc"

start:
    mov sp,stack_top
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
    mov si,msg_nosession
    jz fail
    ; TSC kHz over 10 BIOS ticks (549.25 ms).
    call wait_tick
    rdtsc
    mov [tsc0],eax
    mov [tsc0+4],edx
    mov cx,10
.measure:
    call wait_tick
    loop .measure
    rdtsc
    sub eax,[tsc0]
    sbb edx,[tsc0+4]
    mov ecx,549
    div ecx
    mov edx,eax
    mov di,tail_hex
    mov cx,8
.hex:
    rol edx,4
    mov al,dl
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe .store
    add al,7
.store:
    stosb
    loop .hex
    mov [params+4],cs
    mov [params+8],cs
    mov [params+12],cs
    mov dx,fork_path
    mov bx,params
    mov ax,4B00h
    int 21h
    push cs
    pop ds
    push cs
    pop es
    mov si,msg_fork
    jc fail
    mov ah,4Dh
    int 21h
    test al,al
    jnz fail
    mov si,msg_forked
    call serial_text
    xor bp,bp
.busy:
    mov si,msg_vm0
    call serial_text
    mov ax,bp
    call serial_hex
    mov si,msg_crlf
    call serial_text
    call wait_tick
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    mov [last_exit],di
    cmp bx,1
    je .ended
    inc bp
    cmp bp,2000
    jb .busy
    mov si,msg_timeout
    jmp fail
.ended:
    cmp word [last_exit],0
    mov si,msg_badexit
    jne fail
    mov si,msg_pass
    call serial_text
    mov ax,4C00h
    int 21h
fail:
    call serial_text
    mov si,msg_fail
    call serial_text
    mov ax,4C01h
    int 21h

wait_tick:
    push es
    push ecx
    push eax
    push 40h
    pop es
    mov eax,[es:6Ch]
    mov ecx,400000000
.w:
    cmp eax,[es:6Ch]
    jne .d
    dec ecx
    jnz .w
.d:
    pop eax
    pop ecx
    pop es
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

msg_nosession db '[VMSTEST] no CVSESSION',13,10,0
msg_fork db '[VMSTEST] VMFORK failed',13,10,0
msg_forked db '[VMSTEST] VM 1 created',13,10,0
msg_timeout db '[VMSTEST] VM 1 did not end',13,10,0
msg_badexit db '[VMSTEST] VM 1 exit code not 0',13,10,0
msg_pass db '[VMSTEST] PASS',13,10,0
msg_fail db '[VMSTEST] FAIL',13,10,0
msg_vm0 db '[VMS0] ',0
msg_crlf db 13,10,0
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMSCHILD.COM '
tail_hex db '00000000'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
tsc0 dd 0,0
last_exit dw 0
    align 2
    times 512 db 0
stack_top:
image_end:
