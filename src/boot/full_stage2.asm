bits 16
org 0x0000

stage2_entry:
    push cs
    pop ds

    mov dx, msg_begin
    mov ah, 0x09
    int 0x21
    mov dx, msg_return
    mov ah, 0x09
    int 0x21

.done:
    retf
msg_begin  db "[STAGE2] shell-only launcher", 13, 10, '$'
msg_return db "[STAGE2] return to shell", 13, 10, '$'
