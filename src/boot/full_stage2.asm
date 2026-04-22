bits 16
org 0x0000

stage2_entry:
    push cs
    pop ds

    mov dx, msg_begin
    mov ah, 0x09
    int 0x21

    ; Load mouse driver if present, then launch GEM.EXE.
    mov dx, path_ctmouse_exe
    xor bx, bx
    mov ax, 0x4B00
    int 0x21

    mov dx, path_gem_exe
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jnc .wait_child

    ; Fallback for alternate payload layouts.
    mov dx, path_gem_bat
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jc .launch_fail

.wait_child:
    mov ah, 0x4D
    int 0x21
    mov dx, msg_return
    mov ah, 0x09
    int 0x21
    jmp .done

.launch_fail:
    push ax
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    pop ax
    call print_ax_hex
    mov dl, 13
    mov ah, 0x02
    int 0x21
    mov dl, 10
    mov ah, 0x02
    int 0x21

.done:
    retf

print_ax_hex:
    push ax
    push bx

    mov bx, ax

    mov dx, msg_ax
    mov ah, 0x09
    int 0x21

    mov al, bh
    shr al, 4
    call print_hex_nibble
    mov al, bh
    and al, 0x0F
    call print_hex_nibble
    mov al, bl
    shr al, 4
    call print_hex_nibble
    mov al, bl
    and al, 0x0F
    call print_hex_nibble

    pop bx
    pop ax
    ret

print_hex_nibble:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

msg_begin db "[OPENGEM] launch", 13, 10, '$'
msg_fail db "[OPENGEM] launch failed", 13, 10, '$'
msg_ax db " AX=", '$'
msg_return db "[OPENGEM] returned", 13, 10, '$'
path_ctmouse_exe db "CTMOUSE.EXE", 0
path_gem_bat db "GEM.BAT", 0
path_gem_exe db "GEM.EXE", 0
