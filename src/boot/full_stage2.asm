bits 16
org 0x0000

%ifndef OPENGEM_TRY_EXEC
%define OPENGEM_TRY_EXEC 0
%endif

stage2_entry:
    push cs
    pop ds

    mov dx, msg_begin
    mov ah, 0x09
    int 0x21

%if OPENGEM_TRY_EXEC == 0
    mov dx, msg_blocked
    mov ah, 0x09
    int 0x21
    jmp .done
%endif

    ; Keep startup coherent with OpenGEM layout expectations.
    mov dx, path_gemsys_dir
    mov ah, 0x3B
    int 0x21

    mov dx, path_sd_pattern
    xor cx, cx
    mov ah, 0x4E
    int 0x21
    jc .sd_fail
    mov dx, msg_sd_ok
    mov ah, 0x09
    int 0x21
    jmp .sd_done

.sd_fail:
    mov dx, msg_sd_fail
    mov ah, 0x09
    int 0x21

.sd_done:

    ; Preload GEMVDI first, then start GEM.EXE.
    mov dx, msg_try_vdi
    mov ah, 0x09
    int 0x21
    mov dx, path_gemvdi_root
    call exec_try_wait
    jnc .wait_done
    mov [last_fail_ax], ax

    mov dx, msg_try_gem
    mov ah, 0x09
    int 0x21
    mov dx, path_gem_exe_root
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

exec_try_wait_gemarg:
    push es
    push ds
    pop es
    mov ax, ds
    mov [exec_gemvdi_params + 4], ax
    mov bx, exec_gemvdi_params
    mov ax, 0x4B00
    int 0x21
    pop es
    jc .fail
    mov ah, 0x4D
    int 0x21
    xor ax, ax
    clc
    ret
.fail:
    stc
    ret

print_ax_hex:
    push ax
    push bx

    mov bx, ax

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
msg_blocked db "[OPENGEM] runtime not ready, launch skipped", 13, 10, '$'
msg_try_vdi db "[OPENGEM] try GEMVDI", 13, 10, '$'
msg_try_gem db "[OPENGEM] try GEM", 13, 10, '$'
msg_sd_ok db "[OPENGEM] SD ok", 13, 10, '$'
msg_sd_fail db "[OPENGEM] SD miss", 13, 10, '$'
msg_fail db "[OPENGEM] launch failed AX=", '$'
msg_return db "[OPENGEM] returned", 13, 10, '$'
path_gemsys_dir db "\GEMAPPS\GEMSYS", 0
path_sd_pattern db "SD*.*", 0
path_gemvdi_root db "GEMVDI.EXE", 0
path_gem_exe_root db "GEM.EXE", 0
path_gem_bat_root db "GEM.BAT", 0
exec_gemvdi_tail db 8, ' GEM.EXE', 13
exec_gemvdi_params:
    dw 0
    dw exec_gemvdi_tail
    dw 0
    dw 0
    dw 0
    dw 0
    dw 0
last_fail_ax dw 0
