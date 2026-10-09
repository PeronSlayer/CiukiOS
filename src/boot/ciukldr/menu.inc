; SI=selector, CX=exact ASCII byte count. CF=invalid. Source provenance is
; separate from request text; only fw_cfg may request platform=e500.
selector_validate:
    mov byte [selector_force], 0
    mov byte [selector_safe], 0
    cmp cx, 64
    ja .bad
    cmp cx, 16
    jb .bad
    mov [selector_len], cx
    mov [selector_start], si
    mov di, si
    add di, cx
    mov [selector_end], di
    cmp word [si], 'f0'
    jne .bad
    cmp byte [si+2], ':'
    jne .bad
    add si, 3
    mov bx, probe_names
.probe:
    cmp byte [bx], 0
    je .bad
    mov di, si
    mov bp, bx
.match:
    mov al, [ds:bp]
    test al, al
    jz .probe_end
    cmp di, [selector_end]
    jae .bad
    cmp [di], al
    jne .next_probe
    inc di
    inc bp
    jmp .match
.next_probe:
    cmp byte [bx], 0
    je .advance
    inc bx
    jmp .next_probe
.advance:
    inc bx
    jmp .probe
.probe_end:
    mov ax, di
    add ax, 13 ; " run=" and exactly eight hex digits.
    cmp ax, [selector_end]
    ja .bad
    cmp byte [di], ' '
    jne .next_probe
    mov si, di
    cmp dword [si+1], 'run='
    jne .bad
    add si, 5
    mov cx, 8
.hex:
    lodsb
    call hex_digit
    jc .bad
    loop .hex
    cmp si, [selector_end]
    je .good
    ; Optional fw_cfg-only suffixes, in order: " platform=e500", " safe=1".
    cmp byte [selector_fw], 1
    jne .bad
    mov ax, [selector_end]
    sub ax, si
    cmp ax, 14
    jb .try_safe
    push si
    mov di, platform_suffix
    mov cx, 14
    repe cmpsb
    pop di
    jne .try_safe_from_di
    mov byte [selector_force], 1
    cmp si, [selector_end]
    je .good
    jmp .try_safe
.try_safe_from_di:
    mov si, di
.try_safe:
    mov ax, [selector_end]
    sub ax, si
    cmp ax, 7
    jne .bad
    mov di, safe_suffix
    mov cx, 7
    repe cmpsb
    jne .bad
    mov byte [selector_safe], 1
.good:
    mov si, [selector_start]
    mov cx, [selector_len]
    clc
    ret
.bad:
    stc
    ret
hex_digit:
    cmp al, '0'
    jb .bad
    cmp al, '9'
    jbe .decimal
    or al, 0x20
    cmp al, 'a'
    jb .bad
    cmp al, 'f'
    ja .bad
    sub al, 'a'-10
    clc
    ret
.decimal:
    sub al, '0'
    clc
    ret
.bad:
    stc
    ret
selector_accept:
    mov [B(test_request_len)], cx
    mov di, B(test_request)
    rep movsb
    or dword [B(flags)], CBI_F_TEST_REQUEST
    cmp byte [selector_safe], 1
    jne .not_safe
    or dword [B(flags)], CBI_F_SAFE_MODE
.not_safe:
    cmp byte [selector_force], 1
    jne .done
    mov byte [B(input_policy)], 1
    or dword [B(flags)], CBI_F_INPUT_FORCED
.done:
    ret
probe_names db 'all',0,'core',0,'boot',0,'bootinfo',0,'allocator',0,'protection',0
    db 'isolation',0,'preempt',0,'localfault',0,'syslife',0,'panic',0
    db 'fpu',0,'runner',0,0
platform_suffix db ' platform=e500'
safe_suffix db ' safe=1'
selector_fw db 0
selector_force db 0
selector_safe db 0
selector_len dw 0
selector_start dw 0
selector_end dw 0

config_read:
    mov eax, [system_cluster]
    mov si, config_name
    call directory_find
    jc .done ; Optional file.
    test byte [found_attr], 0x10
    jnz .warning
    cmp dword [found_size], 127
    ja .warning
    cmp dword [found_size], 0
    je .done
    call file_open
    mov cx, [file_size]
    mov di, B(options)
    call file_read
    ; Options are bounded and NUL-terminated by the zeroed handoff.
    mov si, B(options)
.token:
    cmp byte [si], 0
    je .done
    cmp dword [si], 'safe'
    jne .serial
    cmp word [si+4], '=1'
    jne .serial
    or dword [B(flags)], CBI_F_SAFE_MODE
.serial:
    cmp dword [si], 'seri'
    jne .mode
    cmp dword [si+4], 'al=0'
    jne .mode
    and dword [B(flags)], ~CBI_F_SERIAL_LOG
.mode:
    cmp dword [si], 'mode'
    jne .skip
    ; Accept mode=0x followed by four hex digits (including any first digit).
    cmp word [si+4], '=0'
    jne .skip
    cmp byte [si+6], 'x'
    jne .skip
    push si
    add si, 7
    xor bx, bx
    mov cx, 4
.digit:
    lodsb
    call hex_digit
    jc .bad_mode
    shl bx, 4
    movzx ax, al
    or bx, ax
    loop .digit
    cmp bx, 0x3fff
    ja .bad_mode
    mov [preferred_mode], bx
.bad_mode:
    pop si
.skip:
    ; Move to the next whitespace-delimited option, never inspect past 128.
    cmp byte [si], 0
    je .done
    cmp byte [si], ' '
    je .space
    cmp byte [si], 10
    je .space
    inc si
    jmp .skip
.space:
    inc si
    jmp .token
.warning:
    mov si, cfg_warning
    call print
.done:
    ret
preferred_mode dw 0xffff
cfg_warning db 'L:CFG_IGNORED',13,10,0

boot_menu:
    test dword [B(flags)], CBI_F_TEST_REQUEST
    jnz .done ; Already validated in platform discovery, no second port read.
    mov si, menu_text
    call print
    mov bp, 55 ; At least three seconds at the BIOS 18.2 Hz rate.
    call deadline_start
.poll:
    call menu_getc
    jc .deadline
    cmp al, 'n'
    je .normal
    cmp al, 'N'
    je .normal
    cmp al, 's'
    je .safe
    cmp al, 'S'
    je .safe
    cmp al, 'l'
    je .log
    cmp al, 'L'
    je .log
    cmp al, 'p'
    je .probe
    cmp al, 'P'
    je .probe
.deadline:
    call deadline_expired
    jnc .poll
.done:
    ret
.normal:
    and dword [B(flags)], ~CBI_F_SAFE_MODE
    ret
.safe:
    or dword [B(flags)], CBI_F_SAFE_MODE
    ret
.log:
    xor dword [B(flags)], CBI_F_SERIAL_LOG
    jmp .deadline
.probe:
    mov si, probe_prompt
    call print
    mov bp, 548 ; Thirty-second command-entry deadline.
    call deadline_start
    xor di, di
.line:
    call menu_getc
    jc .line_deadline
    cmp al, 13
    je .validate
    cmp al, 10
    je .validate
    cmp al, 8
    je .backspace
    cmp di, 64
    jae .invalid
    mov [selector_buffer+di], al
    inc di
    call uart_putc
.line_deadline:
    call deadline_expired
    jnc .line
    jmp .invalid
.backspace:
    test di, di
    jz .line
    dec di
    jmp .line
.validate:
    mov cx, di
    mov si, selector_buffer
    mov byte [selector_fw], 0
    call selector_validate
    jc .invalid
    call selector_accept
    ret
.invalid:
    mov si, selection_error
    call print
    ret
menu_text db 'SELECT_READY',13,10
    db 'CiukiOS: N Normal / S Safe mode / L Serial log / P Probe (3 s)',13,10,0
probe_prompt db 'Enter f0:<probe|all> run=<8hex>: ',0

menu_getc:
    mov ah, 1
    int 0x16
    jz .serial
    xor ah, ah
    int 0x16
    clc
    ret
.serial:
    cmp dword [B(uart_base)], 0
    je .empty
    mov dx, 0x3fd
    in al, dx
    test al, 1
    jz .empty
    mov dx, 0x3f8
    in al, dx
    clc
    ret
.empty:
    stc
    ret

deadline_start:
    push es
    xor ax, ax
    mov es, ax
    mov eax, [es:0x46c]
    mov [menu_tick], eax
    pop es
    mov dword [menu_polls], 0x2000000 ; Also bounded if firmware tick stops.
    ret
deadline_expired:
    dec dword [menu_polls]
    jz .expired
    push es
    xor ax, ax
    mov es, ax
    mov eax, [es:0x46c]
    pop es
    sub eax, [menu_tick]
    jnc .compare
    add eax, 0x1800b0 ; BIOS daily wrap.
.compare:
    movzx edx, bp
    cmp eax, edx
    jb .pending
.expired:
    stc
    ret
.pending:
    clc
    ret
menu_tick dd 0
menu_polls dd 0
