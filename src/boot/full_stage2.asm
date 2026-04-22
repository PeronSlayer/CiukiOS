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
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21

.done:
    retf

msg_begin db "[OPENGEM] launch", 13, 10, '$'
msg_fail db "[OPENGEM] launch failed", 13, 10, '$'
msg_return db "[OPENGEM] returned", 13, 10, '$'
path_ctmouse_exe db "CTMOUSE.EXE", 0
path_gem_bat db "GEM.BAT", 0
path_gem_exe db "GEM.EXE", 0
