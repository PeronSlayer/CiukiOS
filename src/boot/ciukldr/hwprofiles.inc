; G30B: Windows 98 hardware-profile selection. Same atomic 8.3 file read by
; Control Panel/kernel. No boot_info change: private CLDRPRG1 trailer v1,
; bytes 24..31 = 'HP01', selected byte, count byte, u16 configuration length.
; The exact file bytes live at loader offset 0x18000 (GS=0x3000, offset 0x8000) and remain in loader_end.
; BIOS INT 16h keys, INT 1Ah/BDA deadline helpers from menu.inc.
hwprofiles_boot:
    mov byte [hw_count], 0
    mov byte [hw_default], 0
    mov word [hw_wait], 30
    mov eax, [system_cluster]
    mov si, hw_filename
    call directory_find
    jc .done
    test byte [found_attr], 0x10
    jnz .done
    cmp dword [found_size], 24575
    ja .done
    mov dx, [found_size]
    mov [hw_size], dx
    mov word [hw_raw_at], 0
    call file_open
.line:
    mov di, hw_line
    xor cx, cx
.read:
    call file_getc
    jc .last
    mov bx, [hw_raw_at]
    ; file_getc may consult the converter index through GS; preserve it.
    push gs
    push ax
    mov ax, 0x3000
    mov gs, ax
    pop ax
    mov [gs:hw_raw+bx], al
    pop gs
    inc word [hw_raw_at]
    cmp al, 13
    je .read
    cmp al, 10
    je .parse
    cmp cx, 159
    jae .read
    stosb
    inc cx
    jmp .read
.parse:
    mov byte [di], 0
    call hw_parse_line
    jmp .line
.last:
    mov byte [di], 0
    call hw_parse_line
    cmp byte [hw_count], 0
    je .done
    mov al, [hw_default]
    cmp al, [hw_count]
    jb .default
    xor al, al
.default:
    mov [hw_selected], al
    cmp byte [hw_count], 1
    je .chosen
    cmp word [hw_wait], 0
    je .chosen
    ; Waiting for input is deliberate, not a stalled loader stage.
    mov byte [progress_active], 0
    mov byte [screen_quiet], 0
    mov ax, 3
    int 0x10
    mov si, hw_title
    call print
    xor bx, bx
.names:
    mov al, bl
    add al, '1'
    call menu_echo
    mov al, ' '
    call menu_echo
    mov si, bx
    shl si, 5
    add si, hw_names
    push bx
    call print
    mov si, newline
    call print
    pop bx
    inc bl
    cmp bl, [hw_count]
    jb .names
    mov si, hw_hint
    call print
    call hw_selection
.second:
    mov bp, 18
    call deadline_start
.wait:
    call menu_getc
    jnc .key
    cmp word [hw_wait], -1
    je .wait
    call deadline_expired
    jnc .wait
    dec word [hw_wait]
    jnz .second
    jmp .chosen
.key:
    cmp al, 13
    je .chosen
    test al, al
    jz .arrow
    sub al, '1'
    cmp al, [hw_count]
    jae .wait
    mov [hw_selected], al
    jmp .changed
.arrow:
    mov al, [hw_selected]
    cmp ah, 0x48
    jne .down
    test al, al
    jz .wait
    dec al
    jmp .set
.down:
    cmp ah, 0x50
    jne .wait
    inc al
    cmp al, [hw_count]
    jae .wait
.set:
    mov [hw_selected], al
.changed:
    mov word [hw_wait], -1
    call hw_selection
    jmp .wait
.chosen:
    mov dword [progress_trail+24], 'HP01'
    mov al, [hw_selected]
    mov [progress_trail+28], al
    mov al, [hw_count]
    mov [progress_trail+29], al
    mov ax, [hw_size]
    mov [progress_trail+30], ax
    mov al, 0
    call progress_begin
.done:
    ret
hw_selection:
    mov si, hw_selection_text
    call print
    mov al, [hw_selected]
    add al, '1'
    call menu_echo
    mov si, newline
    call print
    ret
hw_parse_line:
    mov si, hw_line
    cmp dword [si], 'prof'
    jne .wait
    cmp dword [si+4], 'ile='
    jne .done
    cmp byte [hw_count], 8
    jae .done
    movzx di, byte [hw_count]
    shl di, 5
    add di, hw_names
    add si, 8
    xor cx, cx
.name:
    lodsb
    cmp al, '|'
    je .end_name
    cmp al, 32
    jb .done
    cmp al, 126
    ja .done
    cmp cx, 31
    jae .done
    stosb
    inc cx
    jmp .name
.end_name:
    test cx, cx
    jz .done
    mov byte [di], 0
    inc byte [hw_count]
    ret
.wait:
    cmp dword [si], 'wait'
    jne .default
    cmp byte [si+4], '='
    jne .done
    add si, 5
    cmp word [si], '-1'
    jne .number_wait
    cmp byte [si+2], 0
    jne .done
    mov word [hw_wait], -1
    ret
.number_wait:
    call hw_decimal
    jc .done
    mov [hw_wait], ax
    ret
.default:
    cmp dword [si], 'defa'
    jne .done
    cmp dword [si+4], 'ult='
    jne .done
    add si, 8
    call hw_decimal
    jc .done
    cmp ax, 8
    jae .done
    mov [hw_default], al
.done:
    ret
hw_decimal:
    xor ax, ax
    xor bx, bx
.digit:
    mov bl, [si]
    test bl, bl
    jz .end
    sub bl, '0'
    cmp bl, 9
    ja .bad
    imul ax, ax, 10
    add ax, bx
    cmp ax, 999
    ja .bad
    inc si
    jmp .digit
.end:
    clc
    ret
.bad:
    stc
    ret
hw_filename db 'HWPROF  CFG'
hw_title db 'Hardware Profile/Configuration Recovery',13,10,13,10,0
hw_selection_text db 'Selected profile: ',0
hw_hint db 'Choose 1-8 or Up/Down, then Enter, or wait for the default profile.',13,10,0
hw_count db 0
hw_default db 0
hw_selected db 0
hw_wait dw 30
hw_size dw 0
hw_raw_at dw 0
