bits 16
org 0x0000

runtime_start:
    jmp short runtime_entry

runtime_signature db 'CIUKRT01'
runtime_version db 'CiukiOS runtime split service v0.1', 0

runtime_entry:
    push ax
    push di
    cld
    mov ax, 0x0001
    stosw
    mov ax, 0x0001
    stosw
    mov ax, 0x0001
    stosw
    pop di
    pop ax
    retf
