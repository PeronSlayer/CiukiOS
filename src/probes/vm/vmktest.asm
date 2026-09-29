; VMKTEST.COM - VM manager gate: ending a VM that holds the DOS window
; session, and the keyboard afterwards. For each case, through
; \VM\VMFORK.COM \VMKCHILD.COM <case>:
;   E  VM 1 exits without ending its session: once VM 1 is gone (exit code
;      3) the session must be inactive;
;   D  VM 1 ends its devices and session itself, exit code 3;
;   H  VM 1 hangs with CLI while this VM keeps running; VMM_KILL ends it
;      (exit code FFFEh) and the session must be inactive.
; After each case this VM begins and ends a session itself (nothing is left
; behind) and prints "[VMKTEST] press z <case>": the harness types z, which
; must arrive here through the BIOS exactly as z (2C7Ah). "[VMKTEST] PASS".
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
    mov si,cases
.next_case:
    lodsb
    test al,al
    jz .all_done
    push si
    mov [fork_case],al
    mov [press_case],al
    call run_case
    pop si
    jmp .next_case
.all_done:
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

; One case ([fork_case]); jumps to fail on any error.
run_case:
    call fork
    mov si,msg_fork
    jc fail
    mov bx,1                            ; QUERY addresses VM 1's session
    mov ax,VM_OP_VMM_TARGET
    call far [entry]
    push cs
    pop ds
    cmp byte [fork_case],'H'
    je .hung
    mov cx,600
.wait_exit:
    call vm_state
    cmp bx,1
    je .exited
    call wait_tick
    loop .wait_exit
    mov si,msg_noexit
    jmp fail
.exited:
    cmp word [last_exit],3
    mov si,msg_exitcode
    jne fail
    jmp .ended
.hung:
    mov cx,600
.wait_open:
    call session_active
    jnz .opened
    call wait_tick
    loop .wait_open
    mov si,msg_noopen
    jmp fail
.opened:
    mov cx,20                           ; VM 1 spins with CLI meanwhile
.run:
    call wait_tick
    loop .run
    mov bx,1
    mov ax,VM_OP_VMM_KILL
    call far [entry]
    push cs
    pop ds
    mov si,msg_kill
    jc fail
    call vm_state
    cmp bx,1
    mov si,msg_kill_count
    jne fail
    cmp word [last_exit],0FFFEh
    jne fail
.ended:
    call session_active
    mov si,msg_still_open
    jnz fail
    ; The session is free for this VM.
    mov ax,VM_OP_BEGIN
    call far [entry]
    push cs
    pop ds
    mov si,msg_begin
    jc fail
    mov ax,VM_OP_END
    call far [entry]
    push cs
    pop ds
    mov si,msg_end
    jc fail
    ; Nothing pending, then the next physical key arrives unchanged.
    mov ah,1
    int 16h
    push cs
    pop ds
    mov si,msg_pending
    jnz fail
    mov si,msg_press
    call serial_text
    xor ah,ah
    int 16h
    push cs
    pop ds
    cmp ax,2C7Ah
    je .key_ok
    mov [bad_key],ax
    mov si,msg_wrongkey
    call serial_text
    mov ax,[bad_key]
    call serial_hex
    mov si,msg_fail
    jmp fail
.key_ok:
    mov si,msg_key
    call serial_text
    ret

; VMFORK \VMKCHILD.COM [fork_case]. CF=1 on failure.
fork:
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
    jc .done
    mov ah,4Dh
    int 21h
    cmp al,1
    cmc
.done:
    ret

; BX = VMs, [last_exit].
vm_state:
    push cx
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    mov [last_exit],di
    pop cx
    ret

; ZF=0 while the DOS window session is active.
session_active:
    push cx
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [entry]
    push cs
    pop ds
    pop cx
    test edx,edx
    ret

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

msg_nosession db '[VMKTEST] no CVSESSION',13,10,0
msg_fork db '[VMKTEST] VMFORK failed',13,10,0
msg_noexit db '[VMKTEST] VM 1 did not exit',13,10,0
msg_exitcode db '[VMKTEST] VM 1 exit code not 3',13,10,0
msg_still_open db '[VMKTEST] the session is still active',13,10,0
msg_noopen db '[VMKTEST] VM 1 did not begin its session',13,10,0
msg_kill db '[VMKTEST] VMM_KILL failed',13,10,0
msg_kill_count db '[VMKTEST] VM 1 not gone after VMM_KILL',13,10,0
msg_begin db '[VMKTEST] BEGIN failed afterwards',13,10,0
msg_end db '[VMKTEST] END failed afterwards',13,10,0
msg_pending db '[VMKTEST] a key was already pending',13,10,0
msg_press db '[VMKTEST] press z '
press_case db 'E',13,10,0
msg_wrongkey db '[VMKTEST] wrong key ',0
msg_key db '[VMKTEST] key z',13,10,0
msg_pass db '[VMKTEST] PASS',13,10,0
msg_fail db 13,10,'[VMKTEST] FAIL',13,10,0
cases db 'EDH',0
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMKCHILD.COM '
fork_case db 'E'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
last_exit dw 0
bad_key dw 0
    align 2
    times 512 db 0
stack_top:
image_end:
