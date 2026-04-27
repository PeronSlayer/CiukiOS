bits 16
org 0x0000

%ifndef OPENGEM_TRY_EXEC
%define OPENGEM_TRY_EXEC 1
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

    mov dx, path_gemsys_dir
    mov ah, 0x3B
    int 0x21
    jnc .cwd_gemsys_ready

    ; Fallback: if GEMSYS chdir fails, keep previous root-based flow.
    mov dx, path_root_dir
    mov ah, 0x3B
    int 0x21
    jnc .cwd_root_ready
    mov [last_fail_ax], ax
    jmp .print_fail

.cwd_gemsys_ready:

    mov dx, msg_try_ctmouse
    mov ah, 0x09
    int 0x21
    mov dx, path_ctmouse_local
    call exec_try_wait
    jc .ctmouse_done

.ctmouse_done:

    ; Pre-query VGA capability (INT10h AH=12h BL=00h)
    mov ax, 0x1200
    int 0x10
    
    mov dx, msg_try_vdi
    mov ah, 0x09
    int 0x21
    mov dx, path_gemvdi_local
    call exec_try_wait
    jnc .wait_done

    ; If GEMVDI launch fails, retry with GEM.EXE fallback
    mov dx, path_root_dir
    mov ah, 0x3B
    int 0x21
    jc .print_fail

.cwd_root_ready:

    mov dx, msg_try_vdi
    mov ah, 0x09
    int 0x21
    mov dx, path_gemvdi_rel_from_root
    call exec_try_wait
    jnc .wait_done

    mov dx, path_gem_exe_rel_from_root
    call exec_try_wait
    jnc .wait_done

    mov dx, msg_try_bat
    mov ah, 0x09
    int 0x21
    mov dx, path_gemsys_dir
    mov ah, 0x3B
    int 0x21
    jc .bat_fail
    mov dx, path_gem_bat_local
    call exec_try_wait
    jnc .wait_done

.bat_fail:
    mov [last_fail_ax], ax

.print_fail:
    mov dx, msg_fail
    mov ah, 0x09
    int 0x21
    mov ax, [last_fail_ax]
    call print_ax_hex
    mov dl, 13
    mov ah, 0x02
    int 0x21
    mov dl, 10
    mov ah, 0x02
    int 0x21
    jmp .done

.wait_done:
    mov dx, msg_return
    mov ah, 0x09
    int 0x21

.done:
    retf

exec_try_wait:
    push bx
    ; Keep DS stable across EXEC/TSR handoffs: child programs may return
    ; with registers altered, but Stage2 strings/paths are CS-relative.
    push cs
    pop ds
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jc .fail
    mov ah, 0x4D
    int 0x21
    xor ax, ax
    clc
    jmp .done
.fail:
    stc
.done:
    ; Restore DS for subsequent DOS path/message calls in Stage2 flow.
    push cs
    pop ds
    pop bx
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

msg_begin    db "[OPENGEM] launch", 13, 10, '$'
msg_blocked  db "[OPENGEM] launch skipped", 13, 10, '$'
msg_try_ctmouse db "[OPENGEM] try CTMOUSE", 13, 10, '$'
msg_try_vdi  db "[OPENGEM] try GEMVDI", 13, 10, '$'
msg_try_bat  db "[OPENGEM] try GEM.BAT", 13, 10, '$'
msg_fail     db "[OPENGEM] launch failed AX=", '$'
msg_return   db 13, 10, "[OPENGEM] returned", 13, 10, '$'
path_gemsys_dir db "\GEMAPPS\GEMSYS", 0
path_root_dir db "\", 0
path_ctmouse_local db "CTMOUSE.EXE", 0
path_gem_bat_local db "GEM.BAT", 0
path_gemvdi_local db "GEMVDI.EXE", 0
path_gemvdi_rel_from_root  db "GEMAPPS\GEMSYS\GEMVDI.EXE", 0
path_gem_exe_rel_from_root db "GEMAPPS\GEMSYS\GEM.EXE", 0
last_fail_ax dw 0
