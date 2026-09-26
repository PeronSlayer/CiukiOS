bits 16
org 0x0100

; Run once, before the shell opens graphics. The kernel relinquishes its
; unsuccessful native mouse handler before reaching this helper. A working
; native or already installed driver is left in place. No device is invented
; when the BIOS does not provide PS/2 services.
start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    cld
    xor ax, ax
    int 0x33
    cmp ax, 0xFFFF
    je .ready

    push cs
    pop ds
    push cs
    pop es
    mov bx, (image_end - $$ + 0x0100 + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc .memory_failed
    mov dx, msg_try
    call print
    mov ax, cs
    mov [exec_tail_seg], ax
    mov [exec_fcb1_seg], ax
    mov [exec_fcb2_seg], ax
    mov dx, driver_path
    mov bx, exec_block
    mov ax, 0x4B00
    int 0x21
    pushf
    push cs
    pop ds
    push cs
    pop es
    popf
    jc .exec_failed
    ; Consume the child termination status. A TSR may terminate with AH=3;
    ; validate the installed interface itself instead of treating that as
    ; an ordinary process failure or trusting a banner/exit code alone.
    mov ah, 0x4D
    int 0x21
    xor ax, ax
    int 0x33
    cmp ax, 0xFFFF
    jne .no_mouse
    push cs
    pop ds
    mov dx, msg_ready
    call print
.ready:
    mov ax, 0x4C00
    int 0x21
.no_mouse:
    push cs
    pop ds
    mov dx, msg_absent
    call print
    mov ax, 0x4C01
    int 0x21
.exec_failed:
    mov dx, msg_exec
    call print
    mov ax, 0x4C02
    int 0x21
.memory_failed:
    mov ax, 0x4C03
    int 0x21
print:
    mov ah, 0x09
    int 0x21
    ret

driver_path db '\DRIVERS\MOUSE\CTMOUSE.EXE', 0
; /P: PS/2 only (no serial probing); /W: no UMB relocation; /B: refuse
; stacking if another driver appeared. No wheel negotiation is requested.
driver_tail db 9, ' /P /W /B', 13
exec_block:
    dw 0
    dw driver_tail
exec_tail_seg dw 0
    dw exec_fcb1
exec_fcb1_seg dw 0
    dw exec_fcb2
exec_fcb2_seg dw 0
exec_fcb1 db 0, '           ', 0, 0, 0, 0
exec_fcb2 db 0, '           ', 0, 0, 0, 0
msg_try db '[INPUT] Trying BIOS PS/2 mouse driver.', 13, 10, '$'
msg_ready db '[INPUT] BIOS PS/2 mouse ready.', 13, 10, '$'
msg_absent db '[INPUT] BIOS mouse unavailable; keyboard remains enabled.', 13, 10, '$'
msg_exec db '[INPUT] Cannot load \DRIVERS\MOUSE\CTMOUSE.EXE.', 13, 10, '$'
align 2
    times 512 db 0
stack_top:
image_end:
