; VMDTEST.COM - VM manager gate: two DOS window sessions at once, each with
; its own virtual VGA and device-model keyboard (roadmap phase 2, M3).
; Runs \VMWCHILD.COM 1 in VM 1 and \VMWCHILD.COM 2 in VM 2 through
; \VM\VMFORK.COM, then, addressing each session with VMM_TARGET:
;   1. both sessions are active and each screen shows its own text
;      ("VM1 SCREEN", "VM2 SCREEN") while this VM keeps the physical one;
;   2. "[VMDTEST] focus VM1": the harness types a, which only VM 1 gets;
;   3. "[VMDTEST] focus VM2": the harness types b, which only VM 2 gets;
;   4. "[VMDTEST] press the hotkey": Ctrl+Esc in VM 2 gives the keyboard
;      back here;
;   5. "[VMDTEST] focus VM0": the harness types x, read here;
;   6. Esc goes to each VM through its own device model; both end.
; "[VMDTEST] PASS" on COM1.
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
    ; Two VMs, each running its own copy of VMWCHILD.
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
    ; 2. The keyboard to VM 1: a.
    mov bx,1
    call focus
    mov si,msg_focus1
    call serial_text
    mov byte [vm],1
    mov ax,0761h
    call wait_key_cell
    mov byte [vm],2                     ; VM 2 did not get it
    call row1_empty
    mov si,msg_a_ok
    call serial_text
    ; 3. The keyboard to VM 2: b.
    mov bx,2
    call focus
    mov si,msg_focus2
    call serial_text
    mov byte [vm],2
    mov ax,0762h
    call wait_key_cell
    mov byte [vm],1                     ; VM 1 still has only a
    call row1_cells
    cmp word [screen+2],0762h
    mov si,msg_leak
    je fail
    mov si,msg_b_ok
    call serial_text
    ; 4. Ctrl+Esc in VM 2 gives the keyboard back here.
    mov si,msg_hotkey
    call serial_text
    mov cx,600
.hotkey:
    push cx
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    pop cx
    shr ecx,16
    jz .back
    call wait_tick
    dec word [hotkey_wait]
    jnz .hotkey
    mov si,msg_nohotkey
    jmp fail
.back:
    ; 5. x read here.
    mov si,msg_focus0
    call serial_text
    xor ah,ah
    int 16h
    push cs
    pop ds
    cmp al,'x'
    mov si,msg_wrongkey
    jne fail
    ; 6. Esc to each VM through its own device model; both end.
    mov byte [vm],1
    call send_esc
    mov byte [vm],2
    call send_esc
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

; VMFORK \VMWCHILD.COM [fork_vm]. CF=1 on failure.
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

; BX = VM for the keyboard.
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

; VM [vm]'s session active and its row 0 "VMn SCREEN".
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

; AX = the cell expected first on row 1 of VM [vm] (bounded wait).
wait_key_cell:
    mov [cell],ax
    call target
    mov cx,700
.wait:
    push cx
    call row1_cells
    pop cx
    mov ax,[cell]
    cmp [screen],ax
    je .seen
    call wait_tick
    loop .wait
    mov si,msg_nokey
    jmp fail
.seen:
    ret

; Row 1 of VM [vm] still empty.
row1_empty:
    call row1_cells
    mov ax,[screen]
    test ax,ax
    jz .empty
    cmp ax,0720h
    mov si,msg_leak
    jne fail
.empty:
    ret

; The first cells of row 1 of VM [vm] into [screen].
row1_cells:
    call target
    mov bx,160
    call read_row
    mov si,msg_readback
    jc fail
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

msg_nosession db '[VMDTEST] no CVSESSION',13,10,0
msg_fork db '[VMDTEST] VMFORK failed',13,10,0
msg_target db '[VMDTEST] VMM_TARGET failed',13,10,0
msg_focus_failed db '[VMDTEST] VMM_FOCUS failed',13,10,0
msg_noscreen db '[VMDTEST] a VM',27h,'s screen was not seen',13,10,0
msg_screens db '[VMDTEST] VM 1 and VM 2 screens seen',13,10,0
msg_focus1 db '[VMDTEST] focus VM1',13,10,0
msg_focus2 db '[VMDTEST] focus VM2',13,10,0
msg_nokey db '[VMDTEST] the focused VM did not get its key',13,10,0
msg_leak db '[VMDTEST] a VM got another VM',27h,'s key',13,10,0
msg_readback db '[VMDTEST] READBACK failed',13,10,0
msg_a_ok db '[VMDTEST] a reached VM 1 only',13,10,0
msg_b_ok db '[VMDTEST] b reached VM 2 only',13,10,0
msg_hotkey db '[VMDTEST] press the hotkey',13,10,0
msg_nohotkey db '[VMDTEST] Ctrl+Esc did not give the keyboard back',13,10,0
msg_focus0 db '[VMDTEST] focus VM0',13,10,0
msg_wrongkey db '[VMDTEST] VM 0 read the wrong key',13,10,0
msg_devkey db '[VMDTEST] DEV_KEY failed',13,10,0
msg_noend db '[VMDTEST] the VMs did not end',13,10,0
msg_pass db '[VMDTEST] PASS',13,10,0
msg_fail db '[VMDTEST] FAIL',13,10,0
expect_screen db 'V',7,'M',7,'1',7,' ',7,'S',7,'C',7,'R',7,'E',7,'E',7,'N',7
expect_screen_end:
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMWCHILD.COM '
fork_vm db '1'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
vm db 0
cell dw 0
hotkey_wait dw 600
screen times 20 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
