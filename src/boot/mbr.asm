; Ciuki F0 MBR. SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x600
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    mov si, 0x7c00
    mov di, 0x600
    mov cx, 256
    rep movsw
    jmp 0:relocated
relocated:
    sti
    mov [drive], dl
    mov bx, 0x55aa
    mov ah, 0x41
    int 0x13
    jc .chs
    cmp bx, 0xaa55
    jne .chs
    test cl, 1
    jz .chs
    inc byte [edd]
    jmp .load
.chs:
    mov ah, 8
    mov dl, [drive]
    int 0x13
    jc disk_error
    and cx, 63
    jz disk_error
    mov [spt], cx
    movzx ax, dh
    inc ax
    mov [heads], ax
.load:
    call read
    mov ax, 0x2000
    mov es, ax
    cmp dword [es:0], 0x52444c43 ; "CLDR"
    jne header_error
    cmp word [es:4], 1
    jne header_error
    cmp word [es:14], 16
    jne header_error
    mov bp, [es:6]
    dec bp
    cmp bp, 1022
    ja header_error
    inc bp
    ; Refuse a loader that would overwrite EBDA/conventional firmware memory.
    int 0x12
    shl ax, 1
    sub ax, 256
    cmp bp, ax
    ja header_error
    mov ax, [es:12]
    cmp ax, 16
    jb header_error
    push ax
    shr ax, 9
    cmp ax, bp
    jae header_error
    pop ax
    mov [entry], ax
    mov edx, -1
    mov bx, 16
    mov cx, 496
.crc:
    mov al, [es:bx]
    xor dl, al
    mov si, 8
.bit:
    shr edx, 1
    jnc .next
    xor edx, 0xedb88320
.next:
    dec si
    jnz .bit
    inc bx
    loop .crc
    dec bp
    jz .verify
    add word [dap+6], 32
    inc word [dap+8]
    call read
    mov es, [dap+6]
    xor bx, bx
    mov cx, 512
    jmp .crc
.verify:
    not edx
    mov ax, 0x2000
    mov fs, ax
    cmp edx, [fs:8]
    jne crc_error
    mov dl, [drive]
    jmp far [entry]
read:
    pushad
    mov dl, [drive]
    cmp byte [edd], 0
    je .chs
    mov si, dap
    mov ah, 0x42
    int 0x13
    jmp .done
.chs:
    mov ax, [dap+8]
    xor dx, dx
    div word [spt]
    mov cl, dl
    inc cl
    xor dx, dx
    div word [heads]
    ; Validated loader LBAs are <=1023, so cylinder is always <1024.
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah
    mov dl, [drive]
    mov es, [dap+6]
    xor bx, bx
    mov ax, 0x0201
    int 0x13
.done:
    jc disk_error
    popad
    ret
header_error:
    mov al, 'H'
    jmp error
crc_error:
    mov al, 'C'
    jmp error
disk_error:
    mov al, 'D'
error:
    ; One-letter MBR error, both sinks; COM1 divisor 3, 38400 8N1.
    mov bl, al
    xor bh, bh
    mov ah, 0x0e
    int 0x10
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 3
    out dx, al
    inc dx
    xor al, al
    out dx, al
    inc dx
    inc dx
    mov al, 3
    out dx, al
    mov dx, 0x3fd
    mov cx, 0x1000
.poll:
    in al, dx
    test al, 0x20
    jnz .send
    loop .poll
    jmp .wait
.send:
    mov dx, 0x3f8
    mov al, bl
    out dx, al
.wait:
    xor ax, ax
    int 0x16
.halt:
    hlt
    jmp .halt
drive db 0
edd db 0
spt dw 0
heads dw 0
entry dw 0, 0x2000
align 4, db 0
dap db 16, 0
    dw 1, 0, 0x2000
    dq 1
times 440-($-$$) db 0
dd 0 ; Disk signature, filled independently of boot code.
dw 0
times 64 db 0 ; Filled by the image builder.
dw 0xaa55
