; Cooperative progress deadlines. No BIOS interrupt vector is replaced.
; BDA clock: SeaBIOS src/clock.c, same daily wrap as menu.inc. A firmware
; call that never returns cannot be interrupted by this watchdog.
progress_init:
    mov dword [progress_trail+8], 0
    mov dword [progress_trail+12], 0
    mov dword [progress_trail+16], 0
    mov dword [progress_trail+20], 0
    mov byte [progress_active], 0
    ret

; AL=stage (0 menu, 1 video, 2 ELF, 3 handoff). Preserve caller registers.
progress_begin:
    pushad
    movzx ebx, al
    mov [progress_stage], bx
    bts dword [progress_trail+8], ebx
    shl bx, 1
    mov ax, [progress_limits+bx]
    mov [progress_limit], ax
    mov si, [progress_names+bx]
    mov [progress_name], si
    mov si, progress_token
    call progress_print
    call progress_clock
    mov [progress_tick], eax
    mov dword [progress_polls], 0x2000000
    mov byte [progress_active], 1
    popad
    ret

; EAX=current BDA tick. Preserve FS (VBE list/unreal copy callers use it).
progress_clock:
    push fs
    xor ax, ax
    mov fs, ax
    mov eax, [fs:0x46c]
    pop fs
    ret

; CF=expired; does not consume any caller registers or restart the deadline.
progress_check:
    pushad
    cmp byte [progress_active], 0
    je .pending
    movzx ebx, word [progress_stage]
    bt dword [progress_trail+16], ebx
    jc .expired
    dec dword [progress_polls]
    jz .stall
    call progress_clock
    sub eax, [progress_tick]
    jnc .elapsed
    add eax, 0x1800b0
.elapsed:
    movzx edx, word [progress_limit]
    cmp eax, edx
    jb .pending
.stall:
    bts dword [progress_trail+16], ebx
    mov si, progress_stall
    call progress_print
.expired:
    popad
    stc
    ret
.pending:
    popad
    clc
    ret

; Kernel bytes are essential: never jump into a partially loaded ELF.
progress_require:
    call progress_check
    jnc .ok
    mov si, progress_abort
    jmp fatal
.ok:
    ret

progress_end:
    call progress_check
    pushad
    movzx ebx, word [progress_stage]
    bts dword [progress_trail+12], ebx
    mov byte [progress_active], 0
    popad
    ret

; Fatal's original error code is printed separately. Retain its stage too.
progress_failure:
    pushad
    cmp byte [progress_active], 0
    je .done
    movzx ebx, word [progress_stage]
    bts dword [progress_trail+20], ebx
    mov si, progress_fail
    call progress_print
.done:
    popad
    ret

; Short tokens are visible even on a quiet normal boot with no UART.
progress_print:
    push ax
    mov al, [screen_quiet]
    push ax
    mov byte [screen_quiet], 0
    call print
    mov si, [progress_name]
    call print
    mov si, newline
    call print
    pop ax
    mov [screen_quiet], al
    pop ax
    ret

progress_active db 0
progress_stage dw 0
progress_name dw 0
progress_limit dw 0
progress_tick dd 0
progress_polls dd 0
progress_limits dw 5461, 364, 2184, 55 ; 5 min menu (G14 Startup menu), 20 s video, 120 s ELF, 3 s handoff
progress_names dw progress_menu, progress_video, progress_elf, progress_handoff
progress_menu db 'menu',0
progress_video db 'video',0
progress_elf db 'elf',0
progress_handoff db 'handoff',0
progress_token db 'L:STAGE ',0
progress_stall db 'L:STALL ',0
progress_fail db 'L:FAIL ',0
progress_abort db 'L:ABORT deadline',0
