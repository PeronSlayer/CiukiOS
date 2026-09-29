; VMMTEST.COM - VM manager gate, system-VM side (QEMU harness probe).
; Initialises the CVSESSION VM manager, creates a second DOS VM running
; \VMMCHILD.COM (through \VM\VMFORK.COM) and keeps printing "[VM0] n" on COM1 without ever yielding.
; Interleaved "[VM1] n" lines prove pre-emptive switching between VMs.
; After six lines it waits for a key through DOS (INT 21h AH=08h, inside
; DOS): the child keeps running meanwhile. Ends with "[VMMTEST] PASS" once the
; child VM reported exit code 42.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"

start:
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    ; CVSESSION entry.
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    mov ax,es
    or ax,di
    mov si,msg_nosession
    jz fail
    ; Kernel segment and InDOS offset (DOSMGR table), first arena MCB.
    mov ax,1607h
    mov bx,15h
    xor cx,cx
    int 2Fh
    mov [kseg],es
    mov ax,[es:bx+6]
    mov [indos],ax
    mov ah,52h
    int 21h
    mov ax,[es:bx-2]
    mov [first_mcb],ax
    push cs
    pop ds
    push cs
    pop es
    movzx ebx,word [kseg]
    movzx ecx,word [indos]
    movzx edx,word [first_mcb]
    mov ax,VM_OP_VMM_INIT
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    mov si,msg_init
    jc fail_code
    ; A new VM through the fork launcher; it returns here at once.
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
    mov si,msg_create
    jc fail_code
    mov ah,4Dh
    int 21h
    mov si,msg_create
    test al,al
    jnz fail_code
    mov si,msg_created
    call serial_text
    ; Busy loop: never yields, never calls DOS.
    xor bp,bp
.loop:
    mov si,msg_vm0
    call serial_text
    mov ax,bp
    call serial_hex
    mov si,msg_crlf
    call serial_text
    call wait_tick
    cmp bp,6
    jne .no_key
    mov si,msg_key
    call serial_text
    mov ah,8
    int 21h
    mov si,msg_got_key
    call serial_text
.no_key:
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    cmp bx,1                            ; the child VM is gone
    je .ended
    inc bp
    cmp bp,400
    jb .loop
    mov si,msg_timeout
    jmp fail
.ended:
    mov [switches],edx
    mov [gated],esi
    mov [last],di
    mov si,msg_state
    call serial_text
    mov eax,[switches]
    shr eax,16
    call serial_hex
    mov ax,[switches]
    call serial_hex
    mov si,msg_gated
    call serial_text
    mov ax,[gated]
    call serial_hex
    mov si,msg_exit
    call serial_text
    mov ax,[last]
    call serial_hex
    mov si,msg_crlf
    call serial_text
    cmp word [last],42
    mov si,msg_badexit
    jne fail
    mov si,msg_pass
    call serial_text
    mov ax,4C00h
    int 21h
fail_code:
    push ax
    call serial_text
    pop ax
    call serial_hex
    mov si,msg_crlf
fail:
    call serial_text
    mov si,msg_fail
    call serial_text
    mov ax,4C01h
    int 21h

; Busy wait (no HLT, no DOS) until the BIOS tick count moves on.
wait_tick:
    push es
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
.wait:
    in al,dx
    test al,20h
    jz .wait
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

msg_nosession db '[VMMTEST] no CVSESSION',13,10,0
msg_init db '[VMMTEST] VMM_INIT failed ',0
msg_create db '[VMMTEST] VMM_CREATE failed ',0
msg_created db '[VMMTEST] VM 1 created',13,10,0
msg_vm0 db '[VM0] ',0
msg_crlf db 13,10,0
msg_key db '[VMMTEST] waiting for a key',13,10,0
msg_got_key db '[VMMTEST] key read',13,10,0
msg_timeout db '[VMMTEST] child VM did not end',13,10,0
msg_state db '[VMMTEST] switches ',0
msg_gated db ' gated ',0
msg_exit db ' exit ',0
msg_badexit db '[VMMTEST] unexpected exit code',13,10,0
msg_pass db '[VMMTEST] PASS',13,10,0
msg_fail db '[VMMTEST] FAIL',13,10,0
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMMCHILD.COM'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
kseg dw 0
indos dw 0
first_mcb dw 0
switches dd 0
gated dd 0
last dw 0
    align 2
    times 256 db 0
stack_top:
image_end:
