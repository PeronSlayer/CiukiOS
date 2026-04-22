bits 16
org 0x0000

stage2_entry:
    push cs
    pop ds

    mov dx, msg_begin
    mov ah, 0x09
    int 0x21

    ; Keep startup coherent with OpenGEM layout expectations.
    mov dx, path_gemsys_dir
    mov ah, 0x3B
    int 0x21

    ; Best effort mouse init.
    mov dx, path_ctmouse_sys
    call exec_try_ignore
    mov dx, path_ctmouse_root
    call exec_try_ignore

    ; Primary startup path: GEMVDI.EXE.
    mov dx, path_gemvdi_sys
    call exec_try_wait
    jnc .wait_done
    mov [last_fail_ax], ax

    mov dx, path_gemvdi_root
    call exec_try_wait
    jnc .wait_done
    mov [last_fail_ax], ax

    ; Fallbacks.
    mov dx, path_gem_exe_sys
    call exec_try_wait
    jnc .wait_done
    mov [last_fail_ax], ax

    mov dx, path_gem_exe_root
    call exec_try_wait
    jnc .wait_done
    mov [last_fail_ax], ax

    mov dx, path_gem_bat_sys
    call exec_try_wait
    jnc .wait_done
    mov [last_fail_ax], ax

    mov dx, path_gem_bat_root
    call exec_try_wait
    jnc .wait_done
    mov [last_fail_ax], ax

    jmp .launch_fail

.wait_done:
    mov dx, msg_return
    mov ah, 0x09
    int 0x21
    jmp .done

.launch_fail:
    mov ax, [last_fail_ax]
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

exec_try_wait:
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jc .fail
    mov ah, 0x4D
    int 0x21
    xor ax, ax
    clc
    ret
.fail:
    stc
    ret

exec_try_ignore:
    push ax
    push bx
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    pop bx
    pop ax
    ret

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
path_gemsys_dir db "\GEMAPPS\GEMSYS", 0
path_ctmouse_sys db "GEMAPPS\GEMSYS\CTMOUSE.EXE", 0
path_ctmouse_root db "CTMOUSE.EXE", 0
path_gemvdi_sys db "GEMAPPS\GEMSYS\GEMVDI.EXE", 0
path_gemvdi_root db "GEMVDI.EXE", 0
path_gem_exe_sys db "GEMAPPS\GEMSYS\GEM.EXE", 0
path_gem_exe_root db "GEM.EXE", 0
path_gem_bat_sys db "GEMAPPS\GEMSYS\GEM.BAT", 0
path_gem_bat_root db "GEM.BAT", 0
last_fail_ax dw 0
