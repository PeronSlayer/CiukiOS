; VMCTEST.COM - VM manager gate: each VM's BIOS clock keeps real time.
; Runs \VMCCHILD.COM in VM 1 through \VM\VMFORK.COM; both VMs then count
; BIOS timer ticks over 10 s of real time (CMOS clock), both busy, so each
; runs about half of the time. The ticks a VM misses while the other runs
; must still reach it, and both read the CMOS clock through the one shared
; index register at once. "[VMC0] ticks xxxx" here, "[VMC1] ticks xxxx" from
; VM 1 (18.2 per second: about 00B6h), then "[VMCTEST] done" once VM 1
; exited with 0; the harness judges the counts.
bits 16
cpu 386
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
    call measure_ticks
    push ax
    mov si,msg_ticks
    call serial_text
    pop ax
    call serial_hex
    mov si,msg_crlf
    call serial_text
    mov cx,1200
.wait:
    push cx
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    mov [last_exit],di
    pop cx
    cmp bx,1
    je .ended
    call rtc_seconds
    mov bl,al
.tick:
    call rtc_seconds                    ; about a second per round
    cmp al,bl
    je .tick
    loop .wait
    mov si,msg_noend
    jmp fail
.ended:
    cmp word [last_exit],0
    mov si,msg_badexit
    jne fail
    mov si,msg_done
    call serial_text
    mov ax,4C00h
    int 21h
fail:
    call serial_text
    mov si,msg_fail
    call serial_text
    mov ax,4C01h
    int 21h

%include "src/probes/vm/vmclock.inc"

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

msg_nosession db '[VMCTEST] no CVSESSION',13,10,0
msg_fork db '[VMCTEST] VMFORK failed',13,10,0
msg_noend db '[VMCTEST] VM 1 did not end',13,10,0
msg_badexit db '[VMCTEST] VM 1 exit code not 0',13,10,0
msg_done db '[VMCTEST] done',13,10,0
msg_fail db '[VMCTEST] FAIL',13,10,0
msg_ticks db '[VMC0] ticks ',0
msg_crlf db 13,10,0
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMCCHILD.COM'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
last_exit dw 0
    align 2
    times 512 db 0
stack_top:
image_end:
