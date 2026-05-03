bits 16
org 0x0100

start:
    cld
    push cs
    pop ds
    push cs
    pop es

    mov dx, msg_begin
    call print_line

    call try_smartdrv
    call try_devload

    cmp byte [devload_ok], 1
    jne .skip_mscdex
    call try_mscdex
    jmp .done

.skip_mscdex:
    mov dx, msg_skip_mscdex
    call print_line

.done:
    mov dx, msg_done
    call print_line

    mov ax, 0x4C00
    int 0x21

try_smartdrv:
    mov dx, msg_skip_smartdrv
    call print_line
    ret

try_devload:
    mov byte [devload_ok], 0

    mov dx, msg_try_devload
    call print_line
    mov dx, msg_skip_devload
    call print_line
    ret

try_mscdex:
    mov dx, msg_try_mscdex
    call print_line

    mov si, path_mscdex
    mov bx, cmdtail_mscdex
    call run_program
    jc .fail

    mov dx, msg_ok_mscdex
    call print_line
    ret

.fail:
    mov dx, msg_fail_mscdex
    call print_line
    ret

; IN: SI -> ASCIIZ program path, BX -> command tail buffer (or 0 for empty)
; OUT: CF clear on child exit code 0 and normal termination, set otherwise.
run_program:
    push ax
    push dx
    push ds
    push es

    cmp bx, 0
    jne .have_tail
    mov word [exec_param_block + 2], cmdtail_empty
    mov ax, ds
    mov word [exec_param_block + 4], ax
    jmp .tail_ready

.have_tail:
    mov [exec_param_block + 2], bx
    mov ax, ds
    mov word [exec_param_block + 4], ax

.tail_ready:
    xor ax, ax
    mov word [exec_param_block + 0], ax
    mov ax, ds
    mov word [exec_param_block + 8], ax
    mov word [exec_param_block + 12], ax

    push ds
    pop es
    mov dx, si
    mov bx, exec_param_block
    mov ax, 0x4B00
    int 0x21
    jc .launch_fail

    mov ax, 0x4D00
    int 0x21
    mov [last_exit_code], al
    mov [last_term_type], ah

    cmp ah, 0
    jne .child_fail
    cmp al, 0
    jne .child_fail

    clc
    jmp .finish

.launch_fail:
    mov byte [last_exit_code], 0xFF
    mov byte [last_term_type], 0xFF
    stc
    jmp .finish

.child_fail:
    stc

.finish:
    pop es
    pop ds
    pop dx
    pop ax
    ret

print_line:
    mov ah, 0x09
    int 0x21
    ret

devload_ok db 0
last_exit_code db 0
last_term_type db 0

exec_param_block:
    dw 0
    dw cmdtail_empty
    dw 0
    dw 0x005C
    dw 0
    dw 0x006C
    dw 0

cmdtail_empty db 0, 13
cmdtail_devload db 40, ' \SYSTEM\DRIVERS\OAKCDROM.SYS /D:MSCD001', 13
cmdtail_mscdex db 11, ' /D:MSCD001', 13

path_smartdrv db '\SYSTEM\DRIVERS\SMARTDRV.EXE', 0
path_devload db '\SYSTEM\DRIVERS\DEVLOAD.COM', 0
path_mscdex db '\SYSTEM\DRIVERS\MSCDEX.EXE', 0

msg_begin db '[DRVLOAD] BEGIN', 13, 10, '$'
msg_done db '[DRVLOAD] DONE', 13, 10, '$'
msg_skip_smartdrv db '[DRVLOAD] SKIP SMARTDRV optional', 13, 10, '$'
msg_try_smartdrv db '[DRVLOAD] TRY SMARTDRV.EXE', 13, 10, '$'
msg_ok_smartdrv db '[DRVLOAD] OK SMARTDRV.EXE', 13, 10, '$'
msg_fail_smartdrv db '[DRVLOAD] FAIL SMARTDRV.EXE (optional, continue)', 13, 10, '$'
msg_try_devload db '[DRVLOAD] TRY DEVLOAD.COM \\SYSTEM\\DRIVERS\\OAKCDROM.SYS /D:MSCD001', 13, 10, '$'
msg_ok_devload db '[DRVLOAD] OK DEVLOAD.COM', 13, 10, '$'
msg_fail_devload db '[DRVLOAD] FAIL DEVLOAD.COM (MSCDEX skipped)', 13, 10, '$'
msg_skip_devload db '[DRVLOAD] SKIP DEVLOAD.COM (INT21 API gap, deferred)', 13, 10, '$'
msg_try_mscdex db '[DRVLOAD] TRY MSCDEX.EXE /D:MSCD001', 13, 10, '$'
msg_ok_mscdex db '[DRVLOAD] OK MSCDEX.EXE', 13, 10, '$'
msg_fail_mscdex db '[DRVLOAD] FAIL MSCDEX.EXE (continue)', 13, 10, '$'
msg_skip_mscdex db '[DRVLOAD] SKIP MSCDEX.EXE (DEVLOAD not ok)', 13, 10, '$'
