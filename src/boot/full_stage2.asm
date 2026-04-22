bits 16
org 0x0000

stage2_entry:
    push cs
    pop ds

    ; First try GEM.BAT, then fall back to GEM.EXE.
    mov dx, path_gem_bat
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jnc .wait_child

    mov dx, path_gem_exe
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jc .done

.wait_child:
    mov ah, 0x4D
    int 0x21

.done:
    retf

path_gem_bat db "GEM.BAT", 0
path_gem_exe db "GEM.EXE", 0
