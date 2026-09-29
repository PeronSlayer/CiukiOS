; VMOTEST.COM - VM manager gate: the mouse follows the focus between two DOS
; window sessions (roadmap phase 2, M3). Runs \VMOCHILD.COM 1 in VM 1 and
; \VMOCHILD.COM 2 in VM 2 through \VM\VMFORK.COM; each installs a BIOS
; pointing-device handler on its own device model and keeps its packet
; totals on row 1 of its virtual screen, read here through VMM_TARGET:
;   1. both screens are seen ("VM1 MOUSE", "VM2 MOUSE");
;   2. "[VMOTEST] focus VM1, move right": the harness moves the physical
;      mouse right; VM 1 must get packets with X > 0, VM 2 none;
;   3. "[VMOTEST] focus VM2, move down": the physical mouse moves down;
;      VM 2 must get packets with Y < 0, VM 1 no more than it had;
;   4. focus VM 1 and DEV_MOUSE (X +5, Y +7, left button) into its session:
;      VM 1 must get exactly that, VM 2 nothing more;
;   5. Esc through each VM's device model ends both.
; "[VMOTEST] PASS" on COM1.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_devices_abi.inc"

TEXT_OFFSET equ 18000h                  ; B8000h in the 128 KB aperture model

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
    mov byte [fork_vm],'1'
    call fork
    mov si,msg_fork
    jc fail
    mov byte [fork_vm],'2'
    call fork
    mov si,msg_fork
    jc fail
    ; 1. Both sessions, each screen its own.
    mov byte [vm],1
    call wait_screen
    mov byte [vm],2
    call wait_screen
    mov si,msg_screens
    call serial_text
    ; 2. The physical mouse to VM 1.
    mov bx,1
    call focus
    mov si,msg_focus1
    call serial_text
    mov byte [vm],1
    mov cx,700
.right:
    push cx
    call totals
    pop cx
    cmp word [packets],0
    je .right_wait
    cmp word [sum_x],0
    jg .right_seen
.right_wait:
    call wait_tick
    loop .right
    mov si,msg_nophysical
    jmp fail
.right_seen:
    mov byte [vm],2
    call totals
    cmp word [packets],0
    mov si,msg_leak
    jne fail
    mov si,msg_right_ok
    call serial_text
    ; 3. The physical mouse to VM 2.
    mov byte [vm],1
    call totals
    mov ax,[packets]
    mov [vm1_packets],ax
    mov bx,2
    call focus
    mov si,msg_focus2
    call serial_text
    mov byte [vm],2
    mov cx,700
.down:
    push cx
    call totals
    pop cx
    cmp word [packets],0
    je .down_wait
    cmp word [sum_y],0
    jl .down_seen
.down_wait:
    call wait_tick
    loop .down
    mov si,msg_nophysical
    jmp fail
.down_seen:
    mov byte [vm],1
    call totals
    mov ax,[packets]
    cmp ax,[vm1_packets]
    mov si,msg_leak
    jne fail
    mov si,msg_down_ok
    call serial_text
    ; 4. A host-originated movement into VM 1's session.
    mov cx,18                           ; late physical packets settle first
.settle:
    call wait_tick
    loop .settle
    mov byte [vm],2
    call totals
    mov ax,[packets]
    mov [vm2_packets],ax
    mov bx,1
    call focus
    mov byte [vm],1
    call totals
    mov si,packets
    mov di,before
    mov cx,4
    rep movsw
    call target
    mov bx,5
    mov cx,7
    mov dl,1
    mov ax,VM_OP_DEV_MOUSE
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    mov si,msg_devmouse
    jc fail
    mov cx,700
.injected:
    push cx
    call totals
    pop cx
    mov ax,[packets]
    cmp ax,[before]
    jne .injected_seen
    call wait_tick
    loop .injected
    mov si,msg_noinjected
    jmp fail
.injected_seen:
    mov si,msg_wrong_packet
    mov ax,[packets]
    sub ax,[before]
    cmp ax,1
    jne fail
    mov ax,[sum_x]
    sub ax,[before+2]
    cmp ax,5
    jne fail
    mov ax,[sum_y]
    sub ax,[before+4]
    cmp ax,7
    jne fail
    cmp word [buttons],1
    jne fail
    mov byte [vm],2
    call totals
    mov ax,[packets]
    cmp ax,[vm2_packets]
    mov si,msg_leak
    jne fail
    mov si,msg_injected_ok
    call serial_text
    ; 5. Esc to each VM through its own device model; both end.
    mov byte [vm],1
    call send_esc
    mov byte [vm],2
    call send_esc
    mov bx,0
    call focus
    mov cx,600
.wait_end:
    push cx
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    pop cx
    cmp bx,1
    je .ended
    call wait_tick
    loop .wait_end
    mov si,msg_noend
    jmp fail
.ended:
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

; VMFORK \VMOCHILD.COM [fork_vm]. CF=1 on failure.
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

; BX = VM for the keyboard and the mouse.
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

; VM [vm]'s session active and its row 0 "VMn MOUSE".
wait_screen:
    call target
    mov al,[vm]
    add al,'0'
    mov [expect_screen+4],al
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
    jne .retry
    push cx
    xor bx,bx
    call read_row
    pop cx
    jc .retry
    push cx
    mov si,screen
    mov di,expect_screen
    mov cx,expect_screen_end-expect_screen
    repe cmpsb
    pop cx
    je .seen
.retry:
    call wait_tick
    loop .wait
    mov si,msg_noscreen
    jmp fail
.seen:
    ret

; VM [vm]'s packet totals (row 1 of its screen) into [packets]...
totals:
    call target
    mov bx,160
    call read_row
    mov si,msg_readback
    jc fail
    mov si,screen
    mov di,packets
    mov cx,4
    rep movsw
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

; BX = offset in the text screen: 20 bytes of the target's screen into
; [screen]. CF=1 when READBACK failed.
read_row:
    movzx edx,bx
    add edx,TEXT_OFFSET
    mov di,screen
    mov cx,20
    mov ax,VM_OP_READBACK
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
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

msg_nosession db '[VMOTEST] no CVSESSION',13,10,0
msg_fork db '[VMOTEST] VMFORK failed',13,10,0
msg_target db '[VMOTEST] VMM_TARGET failed',13,10,0
msg_focus_failed db '[VMOTEST] VMM_FOCUS failed',13,10,0
msg_noscreen db '[VMOTEST] a VM',27h,'s screen was not seen',13,10,0
msg_screens db '[VMOTEST] VM 1 and VM 2 screens seen',13,10,0
msg_focus1 db '[VMOTEST] focus VM1, move right',13,10,0
msg_focus2 db '[VMOTEST] focus VM2, move down',13,10,0
msg_nophysical db '[VMOTEST] the focused VM did not get the physical mouse',13,10,0
msg_leak db '[VMOTEST] a VM got another VM',27h,'s mouse',13,10,0
msg_readback db '[VMOTEST] READBACK failed',13,10,0
msg_right_ok db '[VMOTEST] right reached VM 1 only',13,10,0
msg_down_ok db '[VMOTEST] down reached VM 2 only',13,10,0
msg_devmouse db '[VMOTEST] DEV_MOUSE failed',13,10,0
msg_noinjected db '[VMOTEST] VM 1 did not get the DEV_MOUSE packet',13,10,0
msg_wrong_packet db '[VMOTEST] VM 1 got a different packet than DEV_MOUSE sent',13,10,0
msg_injected_ok db '[VMOTEST] DEV_MOUSE reached VM 1 only',13,10,0
msg_devkey db '[VMOTEST] DEV_KEY failed',13,10,0
msg_noend db '[VMOTEST] the VMs did not end',13,10,0
msg_pass db '[VMOTEST] PASS',13,10,0
msg_fail db '[VMOTEST] FAIL',13,10,0
expect_screen db 'V',7,'M',7,'1',7,' ',7,'M',7,'O',7,'U',7,'S',7,'E',7
expect_screen_end:
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMOCHILD.COM '
fork_vm db '1'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
vm db 0
vm1_packets dw 0
vm2_packets dw 0
packets dw 0
sum_x dw 0
sum_y dw 0
buttons dw 0
before dw 0,0,0,0
screen times 20 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
