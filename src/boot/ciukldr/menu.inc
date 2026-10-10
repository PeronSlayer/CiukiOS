; SI=selector, CX=exact ASCII byte count. CF=invalid. Source provenance is
; separate from request text; only fw_cfg may request platform=e500.
selector_validate:
    mov byte [selector_force], 0
    mov byte [selector_safe], 0
    mov byte [selector_sweep], 0
    cmp cx, 64
    ja .bad
    cmp cx, 16
    jb .bad
    mov [selector_len], cx
    mov [selector_start], si
    mov di, si
    add di, cx
    mov [selector_end], di
    cmp dword [si], 'all:'
    jne .phase
    cmp byte [selector_fw], 2
    jne .bad
    mov bx, chain_probe_names
    inc si
    jmp .prefix
.phase:
    cmp byte [si], 'f'
    jne .bad
    mov bx, probe_names
    cmp byte [si+1], '0'
    je .prefix
    cmp byte [si+1], '1'
    jne .try_f2
    mov bx, f1_probe_names
    jmp .prefix
.try_f2:
    cmp byte [si+1], '2'
    jne .bad
    mov bx, f2_probe_names
.prefix:
    cmp byte [si+2], ':'
    jne .bad
    add si, 3
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
    cmp bx, chain_probe_names
    je .sweep
    cmp bx, f0_sweep_name
    je .sweep
    cmp bx, f1_sweep_name
    je .sweep
    cmp bx, f2_sweep_name
    jne .run
.sweep:
    cmp byte [selector_fw], 2 ; sweeps enter only through BOOT.CFG
    jne .bad
    mov byte [selector_sweep], 1
.run:
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
    cmp byte [selector_sweep], 1
    je .cursor
    ; Optional fw_cfg-only suffixes: platform, safe, F2 crash server.
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
    jb .try_server
    push si
    mov di, safe_suffix
    mov cx, 7
    repe cmpsb
    pop di
    jne .try_server_from_di
    mov byte [selector_safe], 1
    cmp si, [selector_end]
    je .good
    jmp .try_server
.try_server_from_di:
    mov si, di
.try_server:
    mov ax, [selector_end]
    sub ax, si
    cmp ax, 15
    jne .bad
    ; BX still identifies the exact matched probe, not a text prefix.
    cmp bx, f2_crash_name
    jne .bad
    push si
    mov di, desktop_suffix
    mov cx, 15
    repe cmpsb
    pop di
    je .good
    mov si, di
    mov di, standin_suffix
    mov cx, 15
    repe cmpsb
    jne .bad
    jmp .good
.cursor:
    mov ax, [selector_end]
    sub ax, si
    cmp ax, 7
    jb .bad
    cmp dword [si], ' ste'
    jne .bad
    cmp word [si+4], 'p='
    jne .bad
    add si, 6
    xor dx, dx
    xor cx, cx
.decimal_step:
    cmp si, [selector_end]
    jae .step_end
    mov al, [si]
    cmp al, ' '
    je .step_end
    sub al, '0'
    cmp al, 9
    ja .bad
    inc cx
    cmp cx, 2
    ja .bad
    imul dx, 10
    movzx ax, al
    add dx, ax
    inc si
    jmp .decimal_step
.step_end:
    test cx, cx
    jz .bad
    cmp dx, 63
    ja .bad
    cmp si, [selector_end]
    je .good
    mov ax, [selector_end]
    sub ax, si
    cmp ax, 15 ; " state=" plus eight hex digits
    jne .bad
    cmp dword [si], ' sta'
    jne .bad
    cmp word [si+4], 'te'
    jne .bad
    cmp byte [si+6], '='
    jne .bad
    add si, 7
    mov cx, 8
    xor edx, edx
.state_hex:
    lodsb
    call hex_digit
    jc .bad
    movzx eax, al
    shl edx, 4
    or edx, eax
    loop .state_hex
    cmp edx, 0x7ffff
    ja .bad
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
chain_probe_names db 'sweep',0,0
probe_names:
f0_sweep_name db 'sweep',0,'all',0,'core',0,'boot',0,'bootinfo',0,'allocator',0,'protection',0
    db 'isolation',0,'preempt',0,'localfault',0,'syslife',0,'panic',0
    db 'fpu',0,'runner',0,0
f1_probe_names:
f1_sweep_name db 'sweep',0,'all',0,'core',0
    db 'registry',0,'input',0,'input-fault',0,'framebuffer',0,'ata',0,'ata-fault',0
    db 'partition',0,'fat-read',0,'fat-write',0,'cache',0,'mount-crash',0,'safe',0,'bootlog',0,0
f2_probe_names:
f2_sweep_name db 'sweep',0,'elf-load',0,'spawn-wait',0,'fd-table',0,'mmap',0,'signals-fault',0
    db 'threads-wait',0
f2_crash_name db 'crash-isolation',0,'libc-smoke',0,'app-gate',0,0
platform_suffix db ' platform=e500'
safe_suffix db ' safe=1'
desktop_suffix db ' server=desktop'
standin_suffix db ' server=standin'
selector_fw db 0
selector_force db 0
selector_safe db 0
selector_sweep db 0
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
    ; Parse exact tokens within the file extent, never match an option prefix.
    mov si, B(options)
    mov bp, si
    add bp, [file_size]
.token:
    cmp si, bp
    jae .done
    mov ax, bp
    sub ax, si
    cmp ax, 6
    jb .ordinary
    cmp dword [si], 'prob'
    jne .ordinary
    cmp word [si+4], 'e='
    jne .ordinary
    mov bx, si
.probe_boundary:
    cmp bx, B(options)
    je .probe_start
    dec bx
    cmp byte [bx], ' '
    je .probe_boundary
    cmp byte [bx], 13
    je .probe_start
    cmp byte [bx], 10
    jne .invalid
.probe_start:
    cmp byte [cfg_seen], 0
    jne .invalid
    mov byte [cfg_seen], 1
    add si, 6
    mov di, si
.probe_line:
    cmp di, bp
    jae .probe_validate
    cmp byte [di], 13
    je .probe_validate
    cmp byte [di], 10
    je .probe_validate
    inc di
    jmp .probe_line
.probe_validate:
    mov [cfg_next], di
    mov cx, di
    sub cx, si
    ; A validated fw_cfg selector has priority; still reject malformed cfg.
    push bp
    mov byte [selector_fw], 2
    call selector_validate
    pop bp
    jc .invalid
    test dword [B(flags)], CBI_F_TEST_REQUEST
    jnz .cfg_skipped
    call selector_accept
    mov byte [selector_source], 2
.cfg_skipped:
    mov si, [cfg_next]
    jmp .token
.ordinary:
    mov al, [si]
    cmp al, ' '
    je .space
    cmp al, 13
    je .space
    cmp al, 10
    je .space
    mov di, si
.scan:
    cmp di, bp
    jae .option
    mov al, [di]
    cmp al, ' '
    je .option
    cmp al, 13
    je .option
    cmp al, 10
    je .option
    cmp al, 33
    jb .invalid
    cmp al, 126
    ja .invalid
    inc di
    jmp .scan
.option:
    mov dx, di
    sub dx, si
    cmp dx, 6
    jne .serial
    cmp dword [si], 'safe'
    jne .invalid
    cmp word [si+4], '=0'
    je .next ; A normal option never clears a positive safe-mode source.
    cmp word [si+4], '=1'
    jne .invalid
    or dword [B(flags)], CBI_F_SAFE_MODE
    jmp .next
.serial:
    cmp dx, 8
    jne .mode
    cmp dword [si], 'seri'
    jne .invalid
    cmp dword [si+4], 'al=1'
    je .next
    cmp dword [si+4], 'al=0'
    jne .invalid
    and dword [B(flags)], ~CBI_F_SERIAL_LOG
    jmp .next
.mode:
    cmp dx, 11
    jne .invalid
    cmp dword [si], 'mode'
    jne .invalid
    cmp word [si+4], '=0'
    jne .invalid
    cmp byte [si+6], 'x'
    jne .invalid
    add si, 7
    xor bx, bx
    mov cx, 4
.digit:
    lodsb
    call hex_digit
    jc .invalid
    shl bx, 4
    movzx ax, al
    or bx, ax
    loop .digit
    cmp bx, 0x3fff
    ja .invalid
    mov [preferred_mode], bx
.next:
    mov si, di
    jmp .token
.space:
    inc si
    jmp .token
.warning:
.invalid:
    mov si, selection_error
    jmp fatal
.done:
    ret
preferred_mode dw 0xffff
cfg_seen db 0
cfg_next dw 0
selector_source db 0

boot_menu:
    test dword [B(flags)], CBI_F_TEST_REQUEST
    jz .menu
    cmp byte [selector_source], 2
    je .menu
    mov byte [selector_source], 1 ; validated fw_cfg, no second port read
    jmp .done
.menu:
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
    mov si, source_menu
    cmp byte [selector_source], 1
    jne .cfg_source
    mov si, source_fw
    jmp print
.cfg_source:
    cmp byte [selector_source], 2
    jne .print_source
    mov si, source_cfg
.print_source:
    jmp print
.normal:
    ; Normal declines to add safe mode; positive config/fw_cfg sources persist.
    call .cancel_cfg
    jmp .done
.safe:
    call .cancel_cfg
    or dword [B(flags)], CBI_F_SAFE_MODE
    jmp .done
.cancel_cfg:
    and dword [B(flags)], ~CBI_F_TEST_REQUEST
    mov word [B(test_request_len)], 0
    mov byte [selector_source], 0
    ret
.log:
    xor dword [B(flags)], CBI_F_SERIAL_LOG
    jmp .deadline
.probe:
    call .cancel_cfg
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
    cmp al, 32
    jb .line_deadline
    cmp al, 126
    ja .line_deadline
    cmp di, 64
    jae .invalid
    mov [selector_buffer+di], al
    inc di
    call menu_echo
.line_deadline:
    call deadline_expired
    jnc .line
    jmp .invalid
.backspace:
    test di, di
    jz .line
    dec di
    mov byte [selector_buffer+di], 0
    mov al, 8
    call menu_echo
    mov al, ' '
    call menu_echo
    mov al, 8
    call menu_echo
    jmp .line
.validate:
    mov cx, di
    mov si, selector_buffer
    mov byte [selector_fw], 0
    call selector_validate
    jc .invalid
    call selector_accept
    jmp .done
.invalid:
    mov si, selection_error
    call print
    jmp .done
source_menu db 'L:SELECT_SOURCE=menu',13,10,0
source_fw db 'L:SELECT_SOURCE=fw_cfg',13,10,0
source_cfg db 'L:SELECT_SOURCE=cfg',13,10,0
menu_text db 'SELECT_READY',13,10
    db 'CiukiOS: N Normal / S Safe mode / L Serial log / P Probe (3 s)',13,10,0
probe_prompt db 'Enter f0:<probe|all> run=<8hex>: ',0

; INT 10h teletype AL character, BH page, BL colour (SeaBIOS vgabios.c).
; https://github.com/coreboot/seabios/blob/master/vgasrc/vgabios.c
; Preserve the command buffer index and BP deadline across both sinks.
menu_echo:
    pushad
    push ds
    push es
    call uart_putc
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    pop es
    pop ds
    popad
    ret

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
