; Private DFVM v1 protocol. No native addresses, no guest polling.
; See docs/validation/f3-50-ciukidos-vmm-hook.md for packed fields.
dos_hook_initialize:
    pushf
    pusha
    push ds
    push es
    push cs
    pop ds
    mov ax, 0xC148
    mov bx, 0x4644
    mov cx, 1
    mov dx, 0x564D
    int 0x2F
    test ax, ax
    jnz .done
    cmp cx, 1
    jne .done
    cmp di, 0xFFEC
    ja .done
    cmp dword [es:di], 'DFVM'
    jne .done
    cmp word [es:di+4], 1
    jne .done
    cmp di, 0xFFEC
    ja .done
    cmp word [es:di+6], 20
    jb .done
    test dword [es:di+8], 1
    jz .done
    ; Synthetic ROM role markers reject swapped/overlapping/occupied gates
    ; before BIND can publish ownership. No native address is accepted.
    cmp word [es:di+14], 0xF000
    jne .done
    mov bx, [es:di+12]
    cmp bx, 0x1400
    jb .done
    cmp bx, 0x1FFC
    ja .done
    push es
    mov ax, 0xF000
    mov es, ax
    cmp dword [es:bx], 0xD1CBFECD
    pop es
    jne .done
    mov eax, [es:di+12]
    mov [dos_hook_dispatch], eax
    cmp word [es:di+18], 0xF000
    jne .done
    mov bx, [es:di+16]
    cmp bx, 0x1400
    jb .done
    cmp bx, 0x1FFC
    ja .done
    push es
    mov ax, 0xF000
    mov es, ax
    cmp dword [es:bx], 0xE1CBFECD
    pop es
    jne .done
    mov eax, [es:di+16]
    mov [dos_hook_event_gate], eax
    mov ax, cs
    mov si, dos_hook_bind_segments
    mov cx, (dos_hook_bind_segments_end-dos_hook_bind_segments)/2
.patch:
    lodsw
    mov bx, ax
    mov ax, cs
    mov [bx], ax
    loop .patch
    ; A reserved root PSP in the personality, outside all client allocations.
    mov ax, cs
    add ax, (dos_hook_root_psp-$$)/16
    mov bx, ax
    mov cx, ax
    mov es, ax
    mov word [es:0], 0x20CD
    add ax, 16
    mov [es:2], ax
    mov word [es:0x32], 20
    mov word [es:0x34], 0x18
    mov [es:0x36], bx
    mov si, dos_hook_binding
    xor ax, ax
    call far [dos_hook_event_gate]
    jc .done
    mov byte [dos_hook_active], 1
    mov byte [fat_cache_valid], 0
    mov byte [fat_cache_dirty], 0
    mov word [fat_cache_sector], 0xFFFF
.done:
    pop es
    pop ds
    popa
    popf
    ret

; File adapter for EXEC. The synthetic interrupt frame is private to this
; internal request; the outer EXEC INT21 frame is never reused or replayed.
dos_hook_file_io:
    pushf
    push cs
    call .entry
    ret
.entry:
    call far [cs:dos_hook_dispatch]
    iret

; AX=event, BX=PSP, CX=parent/owner. Preserve registers, return event CF.
dos_hook_notify:
    pushf
    cmp byte [cs:dos_hook_active], 1
    je .active
    popf
    clc
    ret
.active:
    popf
    pusha
    push ds
    push cs
    pop ds
    mov si, dos_hook_arguments
    call far [cs:dos_hook_event_gate]
    mov [cs:dos_hook_event_error], ax
    pop ds
    popa
    ret

dos_hook_begin:
    cmp byte [cs:dos_hook_active], 1
    jne .disabled
    pusha
    mov bx, es
    mov cx, [cs:dos_sda_owning_psp]
    mov ax, 3
    call dos_hook_notify
    jc .done
    mov si, [cs:dos_exec_state_depth]
    and si, 0xFF
    dec si
    shl si, 1
    mov [cs:dos_hook_children+si], bx
.done:
    popa
    jnc .return
    mov ax, [cs:dos_hook_event_error]
.return:
    ret
.disabled:
    clc
    ret

dos_hook_commit:
    pusha
    mov bx, [cs:dos_exec_identity_psp]
    mov ax, 4
    xor cx, cx
    call dos_hook_notify
    popa
    jnc .return
    mov ax, [cs:dos_hook_event_error]
.return:
    ret

dos_hook_unwind:
    pushf
    pusha
    mov si, [cs:dos_exec_state_depth]
    and si, 0xFF
    dec si
    shl si, 1
    mov bx, [cs:dos_hook_children+si]
    test bx, bx
    jz .done
    mov word [cs:dos_hook_children+si], 0
    mov ax, 6
    test word [cs:tmp_exec_return_flags], 1
    jz .normal
    mov ax, 5
.normal:
    test word [cs:tmp_exec_return_flags], 1
    jnz .send
    cmp byte [cs:last_term_type], 3
    jne .send
    mov ax, 7
.send:
    xor cx, cx
    call dos_hook_notify
.done:
    popa
    popf
    ret

; Resolve relative EXEC against the VMM's shared cwd and default drive.
; This also preserves DOS's fully qualified executable name in the environment.
dos_hook_exec_capture_path:
    pusha
    push ds
    push es
    mov si, dx
    push cs
    pop es
    mov di, dos_child_exec_path_buf
    mov ax, 0x6000
    call dos_hook_file_io
    mov [cs:dos_hook_event_error], ax
    pop es
    pop ds
    popa
    jnc .done
    mov ax, [cs:dos_hook_event_error]
.done:
    ret

; Populate EXEC's existing size fields without invoking the private FAT owner.
dos_hook_exec_lookup:
    push bx
    push cx
    push dx
    push ds
    push cs
    pop ds
    mov dx, dos_child_exec_path_buf
    mov ax, 0x3D00
    call dos_hook_file_io
    jc .done
    mov bx, ax
    xor cx, cx
    xor dx, dx
    mov ax, 0x4202
    call dos_hook_file_io
    pushf
    mov [cs:search_found_size_lo], ax
    mov [cs:search_found_size_hi], dx
    mov ah, 0x3E
    call dos_hook_file_io
    popf
.done:
    pop ds
    pop dx
    pop cx
    pop bx
    ret

dos_hook_exec_load:
    push bx
    push cx
    push dx
    push di
    push ds
    push es
    mov [cs:tmp_user_ds], es
    mov ax, [cs:search_found_size_hi]
    mov [cs:tmp_exec_total], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:tmp_exec_limit], ax
    jcxz .size_ok
    cmp word [cs:tmp_exec_total], 0
    jne .too_large
    cmp ax, cx
    ja .too_large
.size_ok:
    push cs
    pop ds
    mov dx, dos_child_exec_path_buf
    mov ax, 0x3D00
    call dos_hook_file_io
    jc .done
    mov bx, ax
.loop:
    mov ax, [cs:tmp_exec_total]
    or ax, [cs:tmp_exec_limit]
    jz .close
    mov ax, [cs:tmp_user_ds]
    cmp ax, [cs:tmp_exec_mz_copy_limit]
    jae .read_fail
    mov ds, ax
    mov dx, di
    mov cx, 0x8000
    test dx, dx
    jz .chunk
    mov ax, dx
    neg ax
    cmp cx, ax
    jbe .chunk
    mov cx, ax
.chunk:
    cmp word [cs:tmp_exec_total], 0
    jne .read
    cmp cx, [cs:tmp_exec_limit]
    jbe .read
    mov cx, [cs:tmp_exec_limit]
.read:
    mov ah, 0x3F
    call dos_hook_file_io
    jc .close_error
    test ax, ax
    jz .read_fail
    sub [cs:tmp_exec_limit], ax
    sbb word [cs:tmp_exec_total], 0
    add di, ax
    jnc .loop
    add word [cs:tmp_user_ds], 0x1000
    jmp .loop
.close:
    mov ah, 0x3E
    call dos_hook_file_io
    jmp .done
.read_fail:
    mov ax, 5
    stc
.close_error:
    push ax
    pushf
    mov ah, 0x3E
    call dos_hook_file_io
    popf
    pop ax
    jmp .done
.too_large:
    mov ax, 8
    stc
.done:
    pop es
    pop ds
    pop di
    pop dx
    pop cx
    pop bx
    ret

align 2
dos_hook_dispatch dd 0
dos_hook_event_gate dd 0
dos_hook_active db 0
dos_hook_result db 0
dos_hook_event_error dw 0
dos_hook_arguments dw 1,8,0,0
dos_hook_children times DOS_EXEC_STATE_FRAME_MAX dw 0
; Packed v1 binding (68 bytes); each far pointer is offset, segment.
dos_hook_binding:
    dw 1,68
    dw dos_hook_legacy_int21,0
    dw dos_sda,0
    dw dos_sda_end-dos_sda
    dw DOS_SYSVARS_OFF,DOS_SYSVARS_SEG
    dw DOS_SYSVARS_SFT_OFF,DOS_SYSVARS_SEG
    dw DOS_SYSVARS_SFT_SIZE,DOS_SYSVARS_SFT_COUNT
    dw current_psp_seg,0
    dw dta_off,0
    dw dos_hook_result,0
    dw dos_indos_flag,0
    dw dos_hook_media,0
    dw dos_default_drive,0
    dw dos_hook_devices,0
    dw DOS_SYSVARS_DPB_OFF,DOS_SYSVARS_SEG
    dw dos_hook_expanded,0
    dw kernel_int21_stacks,0,kernel_int21_stacks_end-kernel_int21_stacks
dos_hook_binding_end:
%if dos_hook_binding_end-dos_hook_binding != 68
    %error "DFVM v1 binding size changed"
%endif
dos_hook_bind_segments:
    dw dos_hook_binding+6,dos_hook_binding+10,dos_hook_binding+28
    dw dos_hook_binding+32,dos_hook_binding+36,dos_hook_binding+40
    dw dos_hook_binding+44,dos_hook_binding+48,dos_hook_binding+52
    dw dos_hook_binding+60,dos_hook_binding+64
    dw dos_hook_devices+2,dos_hook_devices+6,dos_hook_devices+10,dos_hook_devices+14
dos_hook_bind_segments_end:
dos_hook_devices:
    dw dos_hook_con,0,dos_hook_aux,0,dos_hook_prn,0,dos_hook_nul,0
dos_hook_con: dd 0xFFFFFFFF
    dw 0x8013,dos_hook_driver_return,dos_hook_driver_return
    db 'CON     '
dos_hook_aux: dd 0xFFFFFFFF
    dw 0x8000,dos_hook_driver_return,dos_hook_driver_return
    db 'AUX     '
dos_hook_prn: dd 0xFFFFFFFF
    dw 0x8000,dos_hook_driver_return,dos_hook_driver_return
    db 'PRN     '
dos_hook_nul: dd 0xFFFFFFFF
    dw 0x8004,dos_hook_driver_return,dos_hook_driver_return
    db 'NUL     '
dos_hook_driver_return: retf
dos_hook_media times 26 db 0
dos_hook_expanded times 16*232 db 0
align 16
dos_hook_root_psp times 256 db 0

dos_hook_create_psp26:
    pusha
    push ds
    push es
    mov ds, [cs:int21_trace_call_cs]
    mov es, dx
    xor si, si
    xor di, di
    mov cx, 128
    cld
    rep movsw
    xor ax, ax
    mov ds, ax
    mov si, 0x22*4
    mov di, 0x0A
    mov cx, 6
    rep movsw
    mov word [es:0x16], 0
    mov bx, dx
    mov cx, [cs:current_psp_seg]
    mov word [cs:dos_hook_arguments+4], 0x26
    mov ax, 1
    call dos_hook_notify
    mov word [cs:dos_hook_arguments+4], 0
    pop es
    pop ds
    popa
    ret
