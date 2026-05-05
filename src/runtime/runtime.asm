bits 16
org 0x0000

runtime_start:
    jmp short runtime_entry

runtime_signature db 'CIUKRT01'
runtime_version db 'CiukiOS runtime split service table v0.1', 0

runtime_entry:
    push ax
    push di
    cld
    mov ax, runtime_service_table
    stosw
    push cs
    pop ax
    stosw
    mov ax, 0x0001
    stosw
    pop di
    pop ax
    retf

runtime_service_table:
    db 'R', 'T', 'S', 'V'
    dw 0x0001
    dw 0x0001
    dw 0x0008
    dw 0x0001
    dw 0x0001
    dw runtime_identity_service
    dw 0x0000

runtime_identity_service:
    mov ax, 0x5254
    clc
    retf
