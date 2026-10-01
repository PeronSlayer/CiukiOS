; HANG.COM - test fixture for the LOADDRV watchdog (qemu_test_driver_pack.py).
; Hooks IRQ 3 (INT 0Bh) and INT 66h, unmasks IRQ 3, then never returns (with
; interrupts enabled), like a driver waiting for hardware that is not there.
bits 16
org 0x100
    mov dx,msg
    mov ah,9
    int 0x21
    mov ax,0x250B
    mov dx,handler
    int 0x21
    mov ax,0x2566
    int 0x21
    in al,0x21
    and al,0xF7
    out 0x21,al
    sti
.forever:
    jmp .forever
handler:
    iret
msg db '[FIXTURE] HANG hooked INT 0Bh/66h, waiting forever',13,10,'$'
