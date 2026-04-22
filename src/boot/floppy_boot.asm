bits 16
org 0x7C00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    call serial_init

    mov si, msg_boot
    call print_bios_string

    mov si, msg_boot
    call print_serial_string

halt:
    cli
    hlt
    jmp halt

serial_init:
    mov dx, 0x03F8 + 1
    mov al, 0x00
    out dx, al

    mov dx, 0x03F8 + 3
    mov al, 0x80
    out dx, al

    mov dx, 0x03F8 + 0
    mov al, 0x03
    out dx, al

    mov dx, 0x03F8 + 1
    mov al, 0x00
    out dx, al

    mov dx, 0x03F8 + 3
    mov al, 0x03
    out dx, al

    mov dx, 0x03F8 + 2
    mov al, 0xC7
    out dx, al

    mov dx, 0x03F8 + 4
    mov al, 0x0B
    out dx, al
    ret

print_bios_string:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print_bios_string
.done:
    ret

print_serial_string:
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp print_serial_string
.done:
    ret

serial_putc:
    push ax
    push dx
    mov ah, al
.wait:
    mov dx, 0x03F8 + 5
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x03F8
    mov al, ah
    out dx, al
    pop dx
    pop ax
    ret

msg_boot db "[BOOT] CiukiOS pre-Alpha v0.5.0", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
