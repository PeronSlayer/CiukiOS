; VMITEST.COM - VM manager gate: interrupt vectors of a forked VM.
; Hooks INT 1Ch and INT 09h with handlers in its own block (as the desktop's
; DOS-window code does), then runs \VMICHILD.COM <PSP> <size> (this block)
; in VM 1 through \VM\VMFORK.COM. In VM 1 that block is freed: VMFORK must
; give those vectors their earlier value, or VM 1 would run freed memory.
; This VM's own hooks must keep working meanwhile. "[VMITEST] PASS" when
; VM 1 exited with 0 and this VM's INT 1Ch hook counted the ticks.
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
    ; Hooks in this block.
    push es
    xor ax,ax
    mov es,ax
    cli
    mov eax,[es:1Ch*4]
    mov [old1c],eax
    mov eax,[es:09h*4]
    mov [old09],eax
    mov word [es:1Ch*4],hook1c
    mov [es:1Ch*4+2],cs
    mov word [es:09h*4],hook09
    mov [es:09h*4+2],cs
    sti
    pop es
    ; VM 1 gets this PSP and this block's size in its tail.
    mov ax,cs
    mov di,tail_hex
    call hex4_store
    inc di
    mov ax,(image_end-$$+100h+15)/16
    call hex4_store
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
    jc fail_unhook
    mov ah,4Dh
    int 21h
    test al,al
    jnz fail_unhook
    mov word [ticks],0
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
    call wait_tick
    loop .wait
    mov si,msg_noend
    jmp fail_unhook
.ended:
    cmp word [last_exit],0
    mov si,msg_badexit
    jne fail_unhook
    cmp word [ticks],10
    mov si,msg_noticks
    jb fail_unhook
    call unhook
    mov si,msg_pass
    call serial_text
    mov ax,4C00h
    int 21h
fail_unhook:
    push si
    call unhook
    pop si
fail:
    call serial_text
    mov si,msg_fail
    call serial_text
    mov ax,4C01h
    int 21h

; AX -> 4 hex digits at ES:DI.
hex4_store:
    mov cx,4
.hex:
    rol ax,4
    push ax
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe .store
    add al,7
.store:
    stosb
    pop ax
    loop .hex
    ret

unhook:
    push es
    xor ax,ax
    mov es,ax
    cli
    mov eax,[old1c]
    mov [es:1Ch*4],eax
    mov eax,[old09]
    mov [es:09h*4],eax
    sti
    pop es
    ret

hook1c:
    inc word [cs:ticks]
    jmp far [cs:old1c]
hook09:
    inc word [cs:keys]
    jmp far [cs:old09]

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

msg_nosession db '[VMITEST] no CVSESSION',13,10,0
msg_fork db '[VMITEST] VMFORK failed',13,10,0
msg_noend db '[VMITEST] VM 1 did not end',13,10,0
msg_badexit db '[VMITEST] VM 1 exit code not 0',13,10,0
msg_noticks db '[VMITEST] this VM',27h,'s INT 1Ch hook stopped counting',13,10,0
msg_pass db '[VMITEST] PASS',13,10,0
msg_fail db '[VMITEST] FAIL',13,10,0
fork_path db '\VM\VMFORK.COM',0
fork_tail db fork_tail_end-fork_tail-1,' \VMICHILD.COM '
tail_hex db '0000 0000'
fork_tail_end: db 13
params dw 0,fork_tail,0,5Ch,0,6Ch,0
entry dd 0
old1c dd 0
old09 dd 0
ticks dw 0
keys dw 0
last_exit dw 0
    align 16
    times 16384 db 0                    ; a large block, reused in VM 1
    times 512 db 0
stack_top:
image_end:
