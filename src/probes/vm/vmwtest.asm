; VMWTEST.COM - VM manager gate: a DOS window session in a second VM while
; the system VM keeps the real screen and its own keyboard (roadmap phase 2,
; M3). Runs \VMWCHILD.COM in VM 1 through \VM\VMFORK.COM, then:
;   1. sees VM 1's session begin and reads "VM1 SCREEN" from its virtual
;      text screen (READBACK) while this VM stays on the physical display;
;   2. gives VM 1 the keyboard ("[VMWTEST] focus VM1": the harness types
;      "abc") and waits for the three keys on VM 1's screen;
;   3. gets the keyboard back when the harness presses Ctrl+Esc ("[VMWTEST]
;      press the hotkey"), which VM 1 must not receive; then ("[VMWTEST]
;      focus VM0") the harness types "x", read here and not in VM 1;
;   4. sends Esc to VM 1 through the device model (DEV_KEY from this VM),
;      waits for VM 1 to end and prints "[VMWTEST] PASS".
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
    ; Session operations below address VM 1's session.
    mov bx,1
    mov ax,VM_OP_VMM_TARGET
    call far [entry]
    push cs
    pop ds
    mov si,msg_target
    jc fail
    ; 1. VM 1's session and its screen.
    mov cx,400
.wait_session:
    push cx
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [entry]
    push cs
    pop ds
    pop cx
    cmp edx,1
    je .session
    call wait_tick
    loop .wait_session
    mov si,msg_nobegin
    jmp fail
.session:
    mov si,msg_began
    call serial_text
    mov cx,400
.wait_screen:
    push cx
    xor bx,bx
    call read_row
    pop cx
    jc .screen_retry
    mov si,screen
    mov di,expect_screen
    push cx
    mov cx,expect_screen_end-expect_screen
    repe cmpsb
    pop cx
    je .screen_ok
.screen_retry:
    call wait_tick
    loop .wait_screen
    mov si,msg_noscreen
    jmp fail
.screen_ok:
    mov si,msg_screen_ok
    call serial_text
    ; 2. The keyboard to VM 1.
    mov bx,1
    call set_focus
    mov si,msg_focus1
    jc fail
    call serial_text
    mov cx,700
.wait_keys:
    push cx
    mov bx,160
    call read_row
    pop cx
    jc .keys_retry
    cmp word [screen],0761h
    jne .keys_retry
    cmp word [screen+2],0762h
    jne .keys_retry
    cmp word [screen+4],0763h
    je .keys_ok
.keys_retry:
    call wait_tick
    loop .wait_keys
    mov si,msg_nokeys
    jmp fail
.keys_ok:
    mov si,msg_keys_ok
    call serial_text
    ; 3. The keyboard back here through Ctrl+Esc, which VM 1 never sees.
    mov si,msg_hotkey
    call serial_text
.wait_hotkey:
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    shr ecx,16                          ; the VM with the keyboard
    jz .focus_back
    call wait_tick
    dec word [hotkey_wait]
    jnz .wait_hotkey
    mov si,msg_nohotkey
    jmp fail
.focus_back:
    mov si,msg_focus0
    call serial_text
    xor ah,ah
    int 16h
    push cs
    pop ds
    cmp al,'x'
    mov si,msg_wrongkey
    jne fail
    mov si,msg_key0
    call serial_text
    mov cx,10                           ; and VM 1 did not get it
.settle:
    call wait_tick
    loop .settle
    mov bx,160
    call read_row
    mov si,msg_leak
    jc fail
    cmp word [screen+6],0
    je .no_leak
    cmp word [screen+6],0720h
    jne fail
.no_leak:
    ; 4. Esc to VM 1 from here, through its device model.
    mov bx,1
    call set_focus
    mov si,msg_focus1
    jc fail
    mov bl,1                            ; Esc make, then break
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
    mov cx,600
.wait_end:
    push cx
    mov ax,VM_OP_VMM_STATE
    call far [entry]
    push cs
    pop ds
    mov [last_exit],di
    pop cx
    cmp bx,1
    je .ended
    call wait_tick
    loop .wait_end
    mov si,msg_noend
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

; BX = offset in the text screen: 20 bytes of VM 1's screen into [screen].
; CF=1 when READBACK failed.
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

; BX = VM for the keyboard.
set_focus:
    mov ax,VM_OP_VMM_FOCUS
    call far [entry]
    push cs
    pop ds
    push cs
    pop es
    ret

; Busy wait (no HLT, no DOS) until the BIOS tick count moves on.
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

msg_nosession db '[VMWTEST] no CVSESSION',13,10,0
msg_target db '[VMWTEST] VMM_TARGET failed',13,10,0
msg_fork db '[VMWTEST] VMFORK failed',13,10,0
msg_nobegin db '[VMWTEST] VM 1 did not begin its session',13,10,0
msg_began db '[VMWTEST] VM 1 session active',13,10,0
msg_noscreen db '[VMWTEST] VM 1 screen not seen',13,10,0
msg_screen_ok db '[VMWTEST] VM 1 screen seen',13,10,0
msg_focus1 db '[VMWTEST] focus VM1',13,10,0
msg_nokeys db '[VMWTEST] VM 1 did not get the keys',13,10,0
msg_keys_ok db '[VMWTEST] VM 1 got the keys',13,10,0
msg_focus0 db '[VMWTEST] focus VM0',13,10,0
msg_hotkey db '[VMWTEST] press the hotkey',13,10,0
msg_nohotkey db '[VMWTEST] Ctrl+Esc did not give the keyboard back',13,10,0
hotkey_wait dw 400
msg_wrongkey db '[VMWTEST] VM 0 read the wrong key',13,10,0
msg_key0 db '[VMWTEST] VM 0 key x',13,10,0
msg_leak db '[VMWTEST] VM 1 got the key of VM 0',13,10,0
msg_devkey db '[VMWTEST] DEV_KEY failed',13,10,0
msg_noend db '[VMWTEST] VM 1 did not end',13,10,0
msg_badexit db '[VMWTEST] VM 1 exit code not 0',13,10,0
msg_pass db '[VMWTEST] PASS',13,10,0
msg_fail db '[VMWTEST] FAIL',13,10,0
expect_screen db 'V',7,'M',7,'1',7,' ',7,'S',7,'C',7,'R',7,'E',7,'E',7,'N',7
expect_screen_end:
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMWCHILD.COM'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
last_exit dw 0
screen times 20 db 0
    align 2
    times 512 db 0
stack_top:
image_end:
