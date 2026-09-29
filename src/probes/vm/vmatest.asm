; VMATEST.COM - VM manager gate: the one AC'97 stream follows the focus
; between two DOS window sessions that play at once (roadmap phase 2, M3).
; Measures the TSC rate, runs \VMSCHILD.COM <kHz> 1 in VM 1 (220 Hz) and
; \VMSCHILD.COM <kHz> 2 in VM 2 (551 Hz) through \VM\VMFORK.COM, then holds
; each state for 3 s after announcing it on COM1:
;   "[VMATEST] owner VM1 (focus VM1)": VM 1 plays;
;   "[VMATEST] owner VM2 (focus VM2)": VM 2 plays;
;   "[VMATEST] owner VM1 (focus VM0)": the system VM has no sound: the
;                          first session that plays (VM 1);
;   "[VMATEST] owner VM2 (VM1 ended)": Esc ends VM 1: VM 2 plays;
; then Esc ends VM 2. "[VMATEST] PASS" once both exited with 0; the harness
; judges the recorded sound of each state and the blocks each VM played
; (muted sessions keep playing in real time).
bits 16
cpu 586
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_devices_abi.inc"

HOLD_TICKS equ 55                       ; 3 s

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
    mov byte [fork_vm],'1'
    call fork
    mov si,msg_fork
    jc fail
    mov byte [fork_vm],'2'
    call fork
    mov si,msg_fork
    jc fail
    ; Both sessions active, then both playing for a while.
    mov byte [vm],1
    call wait_active
    mov byte [vm],2
    call wait_active
    mov cx,36
    call hold
    mov si,msg_playing
    call serial_text
    ; Each state for 3 s.
    mov bx,1
    call focus
    mov si,msg_owner1
    call serial_text
    call hold_state
    mov bx,2
    call focus
    mov si,msg_owner2
    call serial_text
    call hold_state
    xor bx,bx
    call focus
    mov si,msg_owner0
    call serial_text
    call hold_state
    mov byte [vm],1
    call send_esc
    mov cx,600
.wait_vm1:
    push cx
    call vm_count
    pop cx
    cmp bx,2
    je .vm1_ended
    call wait_tick
    loop .wait_vm1
    mov si,msg_noend
    jmp fail
.vm1_ended:
    cmp word [last_exit],0
    mov si,msg_badexit
    jne fail
    mov si,msg_owner_last
    call serial_text
    call hold_state
    mov byte [vm],2
    call send_esc
    mov cx,600
.wait_vm2:
    push cx
    call vm_count
    pop cx
    cmp bx,1
    je .vm2_ended
    call wait_tick
    loop .wait_vm2
    mov si,msg_noend
    jmp fail
.vm2_ended:
    cmp word [last_exit],0
    mov si,msg_badexit
    jne fail
    xor bx,bx
    call focus
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

; VMFORK \VMSCHILD.COM <kHz> [fork_vm]. CF=1 on failure.
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
vm_count:
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    mov [last_exit],di
    ret

; Session operations address VM [vm].
target:
    movzx bx,byte [vm]
    mov ax,VM_OP_VMM_TARGET
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    mov si,msg_target
    jc fail
    ret

; BX = VM for the keyboard (and the sound).
focus:
    mov ax,VM_OP_VMM_FOCUS
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    mov si,msg_focus_failed
    jc fail
    ret

; VM [vm]'s session active (bounded).
wait_active:
    call target
    mov cx,600
.wait:
    push cx
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [entry]
    push cs
    pop ds
    pop cx
    cmp edx,1
    je .active
    call wait_tick
    loop .wait
    mov si,msg_noactive
    jmp fail
.active:
    ret

; CX BIOS ticks of this VM's clock (owed ticks arrive in bursts after
; the other VMs' slices: count the ticks, not their changes).
hold_state:
    mov cx,HOLD_TICKS
hold:
    push es
    push 40h
    pop es
    mov ax,[es:6Ch]
    add cx,ax
.wait:
    call wait_tick
    mov ax,[es:6Ch]
    sub ax,cx
    js .wait
    pop es
    ret

; Esc make and break to VM [vm] through its device model (it needs the
; focus to take keys).
send_esc:
    movzx bx,byte [vm]
    call focus
    call target
    mov bl,1
    mov cl,1
    mov ax,VM_OP_DEV_KEY
    call far [entry]
    push cs
    pop ds
    mov si,msg_devkey
    jc fail
    mov bl,1
    mov cl,0
    mov ax,VM_OP_DEV_KEY
    call far [entry]
    push cs
    pop ds
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

msg_nosession db '[VMATEST] no CVSESSION',13,10,0
msg_fork db '[VMATEST] VMFORK failed',13,10,0
msg_target db '[VMATEST] VMM_TARGET failed',13,10,0
msg_focus_failed db '[VMATEST] VMM_FOCUS failed',13,10,0
msg_noactive db '[VMATEST] a VM did not begin its session',13,10,0
msg_playing db '[VMATEST] both playing',13,10,0
msg_owner1 db '[VMATEST] owner VM1 (focus VM1)',13,10,0
msg_owner2 db '[VMATEST] owner VM2 (focus VM2)',13,10,0
msg_owner0 db '[VMATEST] owner VM1 (focus VM0)',13,10,0
msg_owner_last db '[VMATEST] owner VM2 (VM1 ended)',13,10,0
msg_devkey db '[VMATEST] DEV_KEY failed',13,10,0
msg_noend db '[VMATEST] a VM did not end',13,10,0
msg_badexit db '[VMATEST] a VM exit code not 0',13,10,0
msg_pass db '[VMATEST] PASS',13,10,0
msg_fail db '[VMATEST] FAIL',13,10,0
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMSCHILD.COM '
tail_hex db '00000000 '
fork_vm db '1'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
tsc0 dd 0,0
vm db 0
last_exit dw 0
    align 2
    times 512 db 0
stack_top:
image_end:
