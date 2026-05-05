bits 16
org 0x0100

start:
    cld
    push cs
    pop ds
    push cs
    pop es

    call parse_args

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

    cmp byte [evidence_mode], 1
    jne .skip

    call load_sys_driver
    pushf
    call print_devload_result
    popf
    jc .fail

    mov byte [devload_ok], 1
    mov dx, msg_ok_devload
    call print_line
    ret

.fail:
    mov dx, msg_fail_devload
    call print_line
    ret

.skip:
    mov dx, msg_skip_devload
    call print_line
    ret

try_mscdex:
    mov dx, msg_try_mscdex
    call print_line

    mov si, path_mscdex
    mov bx, cmdtail_mscdex
    call run_program
    pushf
    call print_mscdex_result
    popf
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

load_sys_driver:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    mov byte [last_exit_code], 0x01
    mov byte [last_term_type], 0x00
    mov word [sys_driver_seg], 0
    mov word [sys_handle], 0
    mov ax, 0x3D00
    mov dx, path_qcdrom
    int 0x21
    jc .fail
    mov [sys_handle], ax
    mov bx, ax
    mov ax, 0x4202
    xor cx, cx
    xor dx, dx
    int 0x21
    jc .fail_close
    cmp dx, 0
    jne .fail_close
    cmp ax, 0
    je .fail_close
    mov [sys_file_size], ax
    mov bx, 0x1000
    mov ah, 0x48
    int 0x21
    jc .fail_close
    mov [sys_driver_seg], ax

    mov bx, [sys_handle]
    mov ax, 0x4200
    xor cx, cx
    xor dx, dx
    int 0x21
    jc .fail_free
    mov cx, [sys_file_size]
    mov bx, [sys_handle]
    push ds
    mov ax, [sys_driver_seg]
    mov ds, ax
    xor dx, dx
    mov ah, 0x3F
    int 0x21
    pop ds
    jc .fail_free
    cmp ax, [sys_file_size]
    jne .fail_free
    mov bx, [sys_handle]
    mov ah, 0x3E
    int 0x21
    mov word [sys_handle], 0
    push es
    mov ax, [sys_driver_seg]
    mov es, ax
    mov word [es:0x0000], 0xFFFF
    mov word [es:0x0002], 0xFFFF
    mov ax, [es:0x0006]
    mov [sys_strategy_ptr], ax
    mov ax, [sys_driver_seg]
    mov [sys_strategy_ptr + 2], ax
    mov ax, [es:0x0008]
    mov [sys_interrupt_ptr], ax
    mov ax, [sys_driver_seg]
    mov [sys_interrupt_ptr + 2], ax
    pop es
    call prepare_sys_init_request

    push ds
    mov ax, [sys_driver_seg]
    mov ds, ax
    push cs
    pop es
    mov bx, sys_request_packet
    call far [cs:sys_strategy_ptr]
    push cs
    pop es
    mov bx, sys_request_packet
    call far [cs:sys_interrupt_ptr]
    pop ds
    mov ax, [sys_request_packet + 3]
    test ax, 0x8000
    jnz .driver_error
    call link_sys_device_driver
    mov byte [last_exit_code], 0x00
    mov byte [last_term_type], 0x00
    clc
    jmp .done
.driver_error:
    mov al, [sys_request_packet + 3]
    mov [last_exit_code], al
    jmp .fail_free
.fail_free:
    mov ax, [sys_driver_seg]
    cmp ax, 0
    je .fail_close
    push es
    mov es, ax
    mov ah, 0x49
    int 0x21
    pop es
    mov word [sys_driver_seg], 0
.fail_close:
    mov bx, [sys_handle]
    cmp bx, 0
    je .fail
    mov ah, 0x3E
    int 0x21
    mov word [sys_handle], 0
.fail:
    stc
.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

link_sys_device_driver:
    push ax
    push bx
    push dx
    push ds
    push es
    mov ah, 0x52
    int 0x21
    jc .done
    mov ax, [es:bx + 0x22]
    mov dx, [es:bx + 0x24]
    cmp ax, 0
    jne .old_ready
    cmp dx, 0
    jne .old_ready
    mov ax, 0xFFFF
    mov dx, 0xFFFF
.old_ready:
    push es
    mov es, [cs:sys_driver_seg]
    mov [es:0x0000], ax
    mov [es:0x0002], dx
    pop es
    mov word [es:bx + 0x10], 512
    mov byte [es:bx + 0x20], 0x02
    mov byte [es:bx + 0x21], 0x1A
    mov word [es:bx + 0x22], 0x0000
    mov ax, [cs:sys_driver_seg]
    mov [es:bx + 0x24], ax
.done:
    clc
    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

prepare_sys_init_request:
    push ax
    push cx
    push di
    push es
    push cs
    pop es
    mov di, sys_request_packet
    mov cx, sys_request_packet_len
    xor ax, ax
    rep stosb
    mov byte [sys_request_packet + 0x00], sys_request_packet_len
    mov byte [sys_request_packet + 0x01], 0x00
    mov byte [sys_request_packet + 0x02], 0x00
    mov word [sys_request_packet + 0x03], 0x0000
    mov byte [sys_request_packet + 0x0D], 0x00
    mov ax, [sys_file_size]
    mov word [sys_request_packet + 0x0E], ax
    mov ax, [sys_driver_seg]
    mov word [sys_request_packet + 0x10], ax
    mov word [sys_request_packet + 0x12], 0x0000
    mov word [sys_request_packet + 0x14], 0x0000
    mov byte [sys_request_packet + 0x16], 0x02
    mov word [sys_request_packet + 0x17], sys_driver_cmdline
    mov ax, cs
    mov word [sys_request_packet + 0x19], ax
    pop es
    pop di
    pop cx
    pop ax
    ret

parse_args:
    mov byte [evidence_mode], 0
    xor ch, ch
    mov cl, [0x80]
    mov si, 0x81

.scan:
    cmp cl, 8
    jb .done
    lodsb
    dec cl
    cmp al, 0x2F
    jne .scan

    push si
    push cx
    mov di, arg_devload
    mov cx, 7
    repe cmpsb
    pop cx
    pop si
    jne .scan

    mov byte [evidence_mode], 1

.done:
    ret

print_devload_result:
    mov dx, msg_result_devload_prefix
    call print_line
    mov al, [last_term_type]
    call print_hex_byte
    mov dx, msg_result_exit_prefix
    call print_line
    mov al, [last_exit_code]
    call print_hex_byte
    mov dx, msg_crlf
    call print_line
    ret

print_mscdex_result:
    mov dx, msg_result_mscdex_prefix
    call print_line
    mov al, [last_term_type]
    call print_hex_byte
    mov dx, msg_result_exit_prefix
    call print_line
    mov al, [last_exit_code]
    call print_hex_byte
    mov dx, msg_crlf
    call print_line
    ret

print_hex_byte:
    push ax
    push bx
    mov bl, al
    shr al, 4
    call print_hex_nibble
    mov al, bl
    and al, 0x0F
    call print_hex_nibble
    pop bx
    pop ax
    ret

print_hex_nibble:
    cmp al, 10
    jb .digit
    add al, 0x37
    jmp .emit

.digit:
    add al, 0x30

.emit:
    mov dl, al
    mov ah, 0x02
    int 0x21
    ret

print_line:
    mov ah, 0x09
    int 0x21
    ret

sys_handle dw 0
sys_driver_seg dw 0
sys_file_size dw 0
sys_strategy_ptr dw 0, 0
sys_interrupt_ptr dw 0, 0
sys_request_packet_len equ 0x1B
sys_request_packet times sys_request_packet_len db 0
sys_driver_cmdline db 92,83,89,83,84,69,77,92,68,82,73,86,69,82,83,92,81,67,68,82,79,77,46,83,89,83,32,47,68,58,81,67,68,82,79,77,49,13,0
path_qcdrom db 92,83,89,83,84,69,77,92,68,82,73,86,69,82,83,92,81,67,68,82,79,77,46,83,89,83,0

devload_ok db 0
evidence_mode db 0
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
arg_devload db 'DEVLOAD'
cmdtail_devload db 38, ' \SYSTEM\DRIVERS\QCDROM.SYS /D:QCDROM1', 13
cmdtail_mscdex db 11, ' /D:QCDROM1', 13

path_smartdrv db '\SYSTEM\DRIVERS\SMARTDRV.EXE', 0
path_devload db '\SYSTEM\DRIVERS\DEVLOAD.COM', 0
path_mscdex db '\SYSTEM\DRIVERS\MSCDEX.EXE', 0

msg_begin db '[DRVLOAD] BEGIN', 13, 10, '$'
msg_done db '[DRVLOAD] DONE', 13, 10, '$'
msg_skip_smartdrv db '[DRVLOAD] SKIP SMARTDRV optional', 13, 10, '$'
msg_try_smartdrv db '[DRVLOAD] TRY SMARTDRV.EXE', 13, 10, '$'
msg_ok_smartdrv db '[DRVLOAD] OK SMARTDRV.EXE', 13, 10, '$'
msg_fail_smartdrv db '[DRVLOAD] FAIL SMARTDRV.EXE (optional, continue)', 13, 10, '$'
msg_try_devload db '[DRVLOAD] TRY SYSLOADER \SYSTEM\DRIVERS\QCDROM.SYS /D:QCDROM1', 13, 10, '$'
msg_ok_devload db '[DRVLOAD] OK SYSLOADER', 13, 10, '$'
msg_fail_devload db '[DRVLOAD] FAIL SYSLOADER (MSCDEX skipped)', 13, 10, '$'
msg_skip_devload db '[DRVLOAD] SKIP SYSLOADER (pass /DEVLOAD to enable)', 13, 10, '$'
msg_try_mscdex db '[DRVLOAD] TRY MSCDEX.EXE /D:QCDROM1', 13, 10, '$'
msg_ok_mscdex db '[DRVLOAD] OK MSCDEX.EXE', 13, 10, '$'
msg_fail_mscdex db '[DRVLOAD] FAIL MSCDEX.EXE (continue)', 13, 10, '$'
msg_skip_mscdex db "[DRVLOAD] SKIP MSCDEX.EXE (DEVLOAD not ok)", 13, 10, "$"
msg_result_devload_prefix db "[DRVLOAD] RESULT DEVLOAD term=0x", "$"
msg_result_mscdex_prefix db "[DRVLOAD] RESULT MSCDEX term=0x", "$"
msg_result_exit_prefix db " exit=0x", "$"
msg_crlf db 13, 10, "$"
