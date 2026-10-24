; SI=selector, CX=exact ASCII byte count. CF=invalid. Source provenance is
; separate from request text; only fw_cfg may request platform=e500.
; Every suffix is an exact NUL-terminated token matched by token_match.
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
    mov ax, si
    add ax, cx
    mov [selector_end], ax
    mov bx, chain_probe_names
    cmp dword [si], 'all:'
    jne .phase
    cmp byte [selector_fw], 2
    jne .bad
    inc si
    jmp .prefix
.phase:
    cmp byte [si], 'f'
    jne .bad
    movzx bx, byte [si+1]
    sub bx, '0'
    cmp bx, 6
    ja .bad
    add bx, bx
    mov bx, [probe_tables+bx]
.prefix:
    cmp byte [si+2], ':'
    jne .bad
    add si, 3
.probe:
    cmp byte [bx], 0
    je .bad
    mov di, bx
    call token_match
    jc .next_probe
    ; Only a whole name followed by a space selects a probe, so a shorter
    ; name never swallows the prefix of a longer one (boot/bootinfo).
    mov di, ax
    cmp di, [selector_end]
    jae .next_probe
    cmp byte [di], ' '
    je .probe_end
.next_probe:
    inc bx
    cmp byte [bx-1], 0
    jne .next_probe
    jmp .probe
.probe_end:
    ; BX names the exact table entry; 'sweep' is the cfg-only sweep entry.
    mov si, di
    cmp dword [bx], 'swee'
    jne .run
    cmp word [bx+4], 'p'
    jne .run
    cmp byte [selector_fw], 2 ; sweeps enter only through BOOT.CFG
    jne .bad
    mov byte [selector_sweep], 1
.run:
    mov di, run_suffix ; " run=" and exactly eight hex digits.
    mov cx, 8
    call token_hex
    jc .bad
    cmp bx, dos_conformance_name
    je .dos_suffix
    cmp bx, f6_browser_name
    je .browser_suffix
    cmp bx, f3_game_name
    je .game_suffix
    cmp si, [selector_end]
    jne .suffix
    cmp bx, f3_title_name
    je .bad
    jmp .good
.suffix:
    cmp byte [selector_sweep], 1
    je .cursor
    cmp bx, f3_title_name
    je .title_suffix
    ; Optional fw_cfg-only suffixes: platform, safe, F2 crash server.
    cmp byte [selector_fw], 1
    jne .bad
    mov di, platform_suffix
    call token_match
    jc .try_safe
    mov si, ax
    mov byte [selector_force], 1
.try_safe:
    cmp si, [selector_end]
    je .good
    ; F3 takes only the platform override, fw_cfg only (lead, 2026-10-11: the
    ; qemu-e500 profile appends it to every selector); safe mode stays refused.
    mov di, [selector_start]
    cmp byte [di+1], '3'
    je .bad
    mov di, safe_suffix
    call token_match
    jc .try_server
    mov si, ax
    mov byte [selector_safe], 1
.try_server:
    cmp si, [selector_end]
    je .good
    ; BX still identifies the exact matched probe, not a text prefix.
    cmp bx, f2_crash_name
    jne .bad
    mov di, desktop_suffix
    call token_match
    jnc .exact_end
    mov di, standin_suffix
    call token_match
    jc .bad
.exact_end: ; AX=end of the last suffix, which must be the selector end
    cmp ax, [selector_end]
    jne .bad
.good:
    mov si, [selector_start]
    mov cx, [selector_len]
    clc
    ret
.cursor:
    mov di, step_suffix
    call token_match
    jc .bad
    mov si, ax
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
    mov di, state_suffix ; " state=" plus eight hex digits
    mov cx, 8
    call token_hex
    jc .bad
    cmp edx, 0x7ffff
    ja .bad
    cmp si, [selector_end]
    je .good
.bad:
    stc
    ret
.dos_suffix:
    ; f3-55 grammar for every source: exactly ' guest=freedos',
    ; ' guest=ciukidos' or ' guest=ciukidos kernel=unified'; no platform or
    ; safe suffix and no unified FreeDOS kernel.
    mov di, dos_freedos_suffix
    call token_match
    jnc .exact_end
    mov di, dos_ciukidos_suffix
    call token_match
    jc .bad
    cmp ax, [selector_end]
    je .good
    mov si, ax
    mov di, dos_kernel_suffix
    call token_match
    jc .bad
    jmp .exact_end
.game_suffix:
    ; f3-61 grammar, as selector.c and the model: an optional
    ; ' kernel=freedos-1.3' or ' kernel=unified', then the F3 suffixes.
    cmp si, [selector_end]
    je .good
    mov di, game_freedos_suffix
    call token_match
    jnc .game_kernel
    mov di, dos_kernel_suffix
    call token_match
    jc .suffix
.game_kernel:
    mov si, ax
    cmp si, [selector_end]
    je .good
    jmp .suffix
.browser_suffix:
    ; f6-18 grammar for every source, as selector.c browser_url and the
    ; model: ' url=' http[s]://host[:port]/path, host [A-Za-z0-9.-]+, port
    ; 1..5 digits, path [A-Za-z0-9_./-]*, then nothing or exactly ' now=1'.
    ; The 64-byte cap leaves at most 36 URL bytes after the 28-byte
    ; 'f6:browser run=<8hex> url=', so PROBE_URL_MAX (40) always holds.
    mov di, url_suffix
    call token_match
    jc .bad
    mov si, ax
    mov di, https_prefix
    call token_match
    jnc .scheme
    mov di, http_prefix
    call token_match
    jc .bad
.scheme:
    mov si, ax
    mov cx, si ; host start
.host:
    cmp si, [selector_end]
    jae .bad ; the path is mandatory
    lodsb
    call url_host_char
    jnc .host
    dec si ; SI=first byte after the host
    cmp si, cx
    je .bad
    cmp al, ':'
    jne .slash
    inc si
    mov cx, si ; port digits start
.port:
    cmp si, [selector_end]
    jae .bad
    lodsb
    sub al, '0'
    cmp al, 9
    jbe .port
    dec si
    mov ax, si
    sub ax, cx
    dec ax
    cmp ax, 4 ; 1..5 digits
    ja .bad
    mov al, [si]
.slash:
    cmp al, '/'
    jne .bad
.path:
    inc si
    cmp si, [selector_end]
    jae .url_end
    mov al, [si]
    cmp al, '_'
    je .path
    cmp al, '/'
    je .path
    call url_host_char
    jnc .path
.url_end:
    cmp si, [selector_end]
    je .good
    mov di, now_suffix
    call token_match
    jc .bad
    jmp .exact_end
.title_suffix:
    mov di, title_suffix
    call token_match
    jc .bad
    mov si, ax
    mov ax, [selector_end]
    sub ax, si
    dec ax
    cmp ax, 22 ; 1..23 title characters
    ja .bad
    mov al, [si]
    cmp al, 'a'
    jb .bad
    cmp al, 'z'
    ja .bad
.title_char:
    lodsb
    cmp al, '-'
    je .title_next
    cmp al, '0'
    jb .bad
    cmp al, '9'
    jbe .title_next
    cmp al, 'a'
    jb .bad
    cmp al, 'z'
    ja .bad
.title_next:
    cmp si, [selector_end]
    jb .title_char
    jmp .good

; SI=input (preserved), DI=NUL-terminated token. AX=input end of the match,
; CF=mismatch. [selector_end] bounds every input read; config_read sets it
; to the end of BOOT.CFG before matching its options.
token_match:
    push si
.next:
    mov al, [di]
    inc di
    test al, al ; clears CF
    jz .end
    cmp si, [selector_end]
    jae .miss
    cmp [si], al
    jne .miss
    inc si
    jmp .next
.miss:
    stc
.end:
    mov ax, si
    pop si
    ret

; DI=NUL-terminated prefix, CX=exact hex digit count at SI. SI advances past
; the digits, EDX=value; CF=invalid. Digits 0-9, a-f and A-F only.
token_hex:
    call token_match
    jc .bad
    mov si, ax
    add ax, cx
    cmp ax, [selector_end]
    ja .bad
    xor edx, edx
.digit:
    lodsb
    sub al, '0'
    cmp al, 9
    jbe .value
    or al, 0x20 ; 'A'-'F' and 'a'-'f' both become 0x31..0x36 here
    sub al, 'a'-'0'
    cmp al, 5
    ja .bad
    add al, 10
.value:
    shl edx, 4
    or dl, al ; clears CF
    loop .digit
    ret
.bad:
    stc
    ret
; CF clear for a URL host byte [A-Za-z0-9.-] (selector.c url_host_char).
; Only bytes above '9' are case-folded in AL, so ':' and '/' stay intact.
url_host_char:
    cmp al, '-'
    je .done ; equal leaves CF clear
    cmp al, '.'
    je .done
    cmp al, '0'
    jb .done
    cmp al, '9'+1
    jb .digit
    or al, 0x20 ; only 'A'-'Z' and 'a'-'z' reach 'a'..'z' here
    cmp al, 'a'
    jb .done
    cmp al, 'z'
    jbe .digit
    stc
.done:
    ret
.digit:
    clc
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
; Phase digit -> NUL-separated probe list; an empty name ends each list.
probe_tables dw probe_names, f1_probe_names, f2_probe_names, f3_probe_names
    dw f4_probe_names, f5_probe_names, f6_probe_names
probe_names:
    db 'sweep',0,'all',0,'core',0,'boot',0,'bootinfo',0,'allocator',0,'protection',0
    db 'isolation',0,'preempt',0,'localfault',0,'syslife',0,'panic',0
    db 'fpu',0,'runner',0,0
f1_probe_names:
    db 'sweep',0,'all',0,'core',0
    db 'registry',0,'input',0,'input-fault',0,'framebuffer',0,'ata',0,'ata-fault',0
    db 'partition',0,'fat-read',0,'fat-write',0,'cache',0,'mount-crash',0,'safe',0,'bootlog',0,0
f2_probe_names:
    db 'sweep',0,'elf-load',0,'spawn-wait',0,'fd-table',0,'mmap',0,'signals-fault',0
    db 'threads-wait',0
f2_crash_name db 'crash-isolation',0,'libc-smoke',0,'app-gate',0,0
f3_probe_names:
    db 'sweep',0,'vm-boot',0,'vm-memory',0,'vm-devices',0,'vm-video',0,'vm-window',0,'vm-audio-state',0,'dpmi',0
              db 'dos4gw',0,'doom',0,'doom-sound',0,'wolf3d',0,'vm-starve',0,'quake',0,'vm-fullscreen',0
f3_game_name db 'vm-game',0
dos_conformance_name db 'dos-conformance',0
f3_title_name db 'title',0,0
; NUL-terminated suffix tokens for token_match/token_hex.
run_suffix db ' run=',0
step_suffix db ' step=',0
state_suffix db ' state=',0
dos_freedos_suffix db ' guest=freedos',0
dos_ciukidos_suffix db ' guest=ciukidos',0
dos_kernel_suffix db ' kernel=unified',0
game_freedos_suffix db ' kernel=freedos-1.3',0
url_suffix db ' url=',0
https_prefix db 'https://',0
http_prefix db 'http://',0
now_suffix db ' now=1',0
title_suffix db ' title=',0
platform_suffix db ' platform=e500',0
safe_suffix db ' safe=1',0
desktop_suffix db ' server=desktop',0
standin_suffix db ' server=standin',0
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
    ; Each option is an exact token ended by space, CR, LF or the file end.
    mov di, si
    call .boundary
    jnc .space
    mov [selector_end], bp
    mov bx, option_tokens
.option:
    mov di, bx
    call token_match
    jc .next_option
    mov di, ax
    call .boundary
    jnc .apply
.next_option:
    inc bx
    cmp byte [bx-1], 0
    jne .next_option
    cmp byte [bx], 0
    jne .option
    mov di, mode_prefix
    mov cx, 4
    call token_hex
    jc .invalid
    cmp edx, 0x3fff
    ja .invalid
    mov di, si
    call .boundary
    jc .invalid
    mov [preferred_mode], dx
    jmp .next
.apply:
    ; safe=0, serial=1, accel=0, usbscan=0, ecpoll=0/1 and setup=1 change nothing
    ; here: a normal option never clears a positive safe-mode source;
    ; B(options) keeps accel=0 (the kernel declines acceleration before
    ; marker I/O), usbscan=0 and ecpoll=0/1 (f2-96: the ThinkPad EC volume
    ; poll runs every 2 s by default, ecpoll=0 turns it off, ecpoll=1 selects
    ; 1 Hz), policies the kernel reads from the copied options.
    cmp bx, safe_option
    jne .serial
    or dword [B(flags)], CBI_F_SAFE_MODE
.serial:
    cmp bx, serial_option
    jne .startup
    and dword [B(flags)], ~CBI_F_SERIAL_LOG
.startup:
    ; G14: menu=1 shows the Startup menu at every start (System
    ; Configuration's "Enable Startup menu"); autoscan=0/2 follow Msdos.sys
    ; AutoScan (no ScanDisk message / ScanDisk without it).
    cmp bx, menu_option
    jne .autoscan
    mov byte [startup_auto], 1
.autoscan:
    cmp bx, autoscan0_token
    jne .autoscan_quiet
    mov byte [autoscan_mode], 0
.autoscan_quiet:
    cmp bx, autoscan2_token
    jne .next
    mov byte [autoscan_mode], 2
.next:
    mov si, di
    jmp .token
.space:
    inc si
    jmp .token
; DI=token end (DI <= BP). CF clear at space, CR, LF or the end of the file.
.boundary:
    cmp di, bp
    je .bounded
    mov al, [di]
    cmp al, ' '
    je .bounded
    cmp al, 13
    je .bounded
    cmp al, 10
    je .bounded
    stc
.bounded:
    ret
.warning:
.invalid:
    mov si, selection_error
    jmp fatal
.done:
    ret
; Exact BOOT.CFG options; an empty name ends the list.
option_tokens db 'safe=0',0,'serial=1',0,'accel=0',0,'usbscan=0',0,'ecpoll=0',0,'ecpoll=1',0,'setup=1',0
safe_option db 'safe=1',0
serial_option db 'serial=0',0
menu_option db 'menu=1',0
autoscan0_token db 'autoscan=0',0
autoscan2_token db 'autoscan=2',0,0
mode_prefix db 'mode=0x',0
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
    test dword [B(flags)], CBI_F_TEST_REQUEST | CBI_F_SAFE_MODE
    jnz .announce
    ; Normal boot (docs/boot.md): a cleared screen with one discreet line;
    ; the serial log keeps SELECT_READY and the full menu text unchanged.
    mov ax, 3
    int 0x10
    ; INT 10h AH=13h write string (IBM PC/AT BIOS; SeaBIOS vgasrc/vgabios.c
    ; handle_1013): ES:BP text, CX length, DX row/col, BL attribute. AL=0
    ; leaves the cursor at the top, where S/L/P output then starts.
    mov ax, 0x1300
    mov bx, 0x0007
    mov cx, menu_hint_end-menu_hint
    mov dx, 0x1800
    push ds
    pop es
    mov bp, menu_hint
    int 0x10
    mov byte [screen_quiet], 1
.announce:
    mov si, menu_text
    call print
    ; G14: a failed start, menu=1 or a Diagnostic startup request opens the
    ; Startup menu without a key; a BOOT.CFG test request keeps its sweep.
    test dword [B(flags)], CBI_F_TEST_REQUEST
    jnz .restart
    cmp byte [startup_auto], 0
    jne .startup
.restart:
    mov bp, 55 ; At least three seconds at the BIOS 18.2 Hz rate.
    call deadline_start
.poll:
    call progress_check
    jc .done
    call startup_hotkey ; CF: Ctrl held (Windows 98 Startup menu key)
    jc .startup
    call menu_getc
    jc .deadline
    test al, al
    jz .extended
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
    cmp al, 'h'
    je .hardware
    cmp al, 'H'
    je .hardware
.deadline:
    call deadline_expired
    jnc .poll
.done:
    mov si, source_prefix
    call print
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
.extended:
    ; AH=scan code: F8 opens the Startup menu; F5, Shift+F5 and Shift+F8
    ; choose Safe mode, Safe mode command prompt only and Step-by-step.
    call startup_scan
    jc .deadline
    call .cancel_cfg
    test al, al
    jz .startup_menu
    call startup_apply
    jmp .done
.startup:
    call .cancel_cfg
.startup_menu:
    call startup_menu
    jmp .done
.normal:
    ; Normal declines to add safe mode; positive config/fw_cfg sources persist.
    call .cancel_cfg
    jmp .done
.safe:
    call .cancel_cfg
    or dword [B(flags)], CBI_F_SAFE_MODE
    mov byte [screen_quiet], 0
    mov si, safe_confirmation
    call print
    mov si, safe_selected
    call print
    jmp .restart ; Allow P/L after S with a fresh bounded three-second window.
.cancel_cfg:
    and dword [B(flags)], ~CBI_F_TEST_REQUEST
    mov word [B(test_request_len)], 0
    mov byte [selector_source], 0
    ret
.log:
    ; L shows the screen log and enables one-boot runtime cost diagnostics.
    xor dword [B(flags)], CBI_F_SERIAL_LOG
    or byte [B(flags)+1], CBI_F_VERBOSE >> 8
    or byte [B(flags)+1], CBI_F_DIAG_VERBOSE >> 8
    mov byte [screen_quiet], 0
    jmp .deadline
.hardware:
    call .cancel_cfg
    or dword [B(flags)], CBI_F_HW_CHECK | CBI_F_VERBOSE
    mov byte [screen_quiet], 0
    jmp .done
.probe:
    call .cancel_cfg
    mov byte [screen_quiet], 0
    mov si, probe_prompt
    call print
    mov bp, 548 ; Thirty-second command-entry deadline.
    call deadline_start
    xor di, di
.line:
    call progress_check
    jc .invalid
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
source_prefix db 'L:SELECT_SOURCE=',0
source_menu db 'menu',13,10,0
source_fw db 'fw_' ; 'fw_cfg',13,10,0: source_cfg and newline share its tail
source_cfg db 'cfg'
newline db 13,10,0
safe_confirmation db 'Safe mode; P for a probe, or wait',13,10,0
safe_selected db 'L:SAFE=1',13,10,0
menu_text db 'SELECT_READY',13,10
    db 'CiukiOS ', CIUKI_VERSION, ': N Normal / S Safe / L Log / H Hardware / P Probe (3 s)',13,10,0
probe_prompt db 'fN:<probe> run=<8hex>: ',0
menu_hint db 'Ctrl or F8: Startup menu, S for safe mode, L for log, H for Hardware check'
menu_hint_end:

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
    mov ah, 0xff ; no scan code: serial bytes never read as F-keys
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

f4_probe_names db 'optical',0,'audio-native',0,'ntfs-ro',0,'vm-audio',0,'media-remove',0,'vm-audio-jitter',0,0
f5_probe_names db 'f5-core',0,'w32-exceptions',0,'w32-memory',0,'w32-objects',0,'w32-dialogs',0,'w32-upper-dlls',0,'w32-media',0,'w32-fixtures',0,'w32-kernel32',0,'w32-fullscreen',0,'w32-registry',0,'w32-audio',0,'w32-gdi32',0,'w32-crash',0,'w32-user32',0,'w32-codecs',0,'w32-ddraw',0,'w32-input',0,'w32-restart',0,'w32-installer-nsis',0,'w32-gl',0,'win32-diablo-shareware',0,'win32-sc',0,'win32-aoe-trial',0,'win32-half-life-uplink',0,'win32-ut-demo-348',0,'win32-wa',0,'win32-heroes3-demo',0,'win32-rct-demo',0,'win32-jazz2-shareware',0,'win32-ta-demo',0,0
f6_probe_names db 'net-arp',0
f6_browser_name db 'browser',0,0
