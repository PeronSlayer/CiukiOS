bits 16
org 0x0000

runtime_start:
    jmp short runtime_entry

runtime_signature db 'CIUKRT01'
runtime_version db 'CiukiOS runtime split service table v0.1', 0
runtime_stage2_ready_marker db '[S2] ready', 13, 10, 0

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
    dw 0x0004
    dw 0x0008
    dw 0x0001
    dw 0x0001
    dw runtime_identity_service
    dw 0x0000
    dw 0x0002
    dw 0x0001
    dw runtime_version_service
    dw 0x0000
    dw 0x0003
    dw 0x0001
    dw runtime_stage2_ready_service
    dw 0x0000
    dw 0x0004
    dw 0x0001
    dw runtime_dos_version_service
    dw 0x0000

runtime_identity_service:
    mov ax, 0x5254
    clc
    retf

runtime_version_service:
    push cs
    pop ds
    mov si, runtime_version
    clc
    retf

runtime_stage2_ready_service:
    push cs
    pop ds
    mov si, runtime_stage2_ready_marker
    clc
    retf

runtime_dos_version_service:
    mov ax, 0x0005
    xor bx, bx
    xor cx, cx
    clc
    retf
