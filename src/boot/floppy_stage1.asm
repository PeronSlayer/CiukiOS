bits 16
org 0x0000

%define CMD_BUF_LEN 64
%define COM_LOAD_SEG 0x2000
%define FAT_SPT 18
%define FAT_HEADS 2
%define FAT_RESERVED_SECTORS 7
%define FAT_SECTORS_PER_FAT 9
%define FAT_COUNT 2
%define FAT_ROOT_DIR_SECTORS 14
%define FAT_ROOT_START_LBA (FAT_RESERVED_SECTORS + (FAT_COUNT * FAT_SECTORS_PER_FAT))
%define FAT_DATA_START_LBA (FAT_ROOT_START_LBA + FAT_ROOT_DIR_SECTORS)

stage1_start:
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE
    sti

    mov [boot_drive], dl

    call serial_init

    mov si, msg_stage1
    call print_string_dual
    mov si, msg_stage1_serial
    call print_string_serial

    call run_bios_diagnostics
    call install_int21_vector
    call run_stage1_selftest

main_loop:
    mov si, msg_prompt
    call print_string_dual

    call read_command_line
    call dispatch_command
    jmp main_loop

run_bios_diagnostics:
    mov si, msg_diag_begin
    call print_string_dual

    mov si, msg_diag_int10
    call print_string_dual

    mov ah, 0x00
    mov dl, [boot_drive]
    int 0x13
    jc .int13_fail
    mov si, msg_diag_int13_ok
    call print_string_dual
    jmp .int13_done
.int13_fail:
    mov si, msg_diag_int13_fail
    call print_string_dual
.int13_done:

    mov ah, 0x01
    int 0x16
    mov si, msg_diag_int16_ok
    call print_string_dual

    mov ah, 0x00
    int 0x1A
    mov si, msg_diag_int1a
    call print_string_dual
    mov ax, cx
    call print_hex16_dual
    mov ax, dx
    call print_hex16_dual
    call print_newline_dual

    ret

install_int21_vector:
    push ax
    push bx
    push es

    xor ax, ax
    mov es, ax
    mov bx, 0x21 * 4

    mov ax, [es:bx]
    mov [old_int21_off], ax
    mov ax, [es:bx + 2]
    mov [old_int21_seg], ax

    mov word [es:bx], int21_handler
    mov ax, cs
    mov [es:bx + 2], ax

    mov byte [int21_installed], 1
    mov byte [last_exit_code], 0
    mov byte [last_term_type], 0

    pop es
    pop bx
    pop ax

    mov si, msg_int21_installed
    call print_string_dual
    ret

int21_handler:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    cmp ah, 0x02
    je .fn_02
    cmp ah, 0x09
    je .fn_09
    cmp ah, 0x4C
    je .fn_4c
    cmp ah, 0x4D
    je .fn_4d
    jmp .unsupported

.fn_02:
    mov al, dl
    call bios_putc
    call serial_putc
    jmp .done

.fn_09:
    mov si, dx
.fn_09_loop:
    lodsb
    cmp al, '$'
    je .done
    call bios_putc
    call serial_putc
    jmp .fn_09_loop

.fn_4c:
    mov [cs:last_exit_code], al
    mov byte [cs:last_term_type], 0
    jmp .done

.fn_4d:
    mov al, [cs:last_exit_code]
    mov ah, [cs:last_term_type]
    jmp .done

.unsupported:
    mov ax, 0x0001

.done:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    iret

int21_smoke_test:
    push ds

    mov si, msg_dos21_begin
    call print_string_dual

    mov ax, cs
    mov ds, ax

    mov dx, msg_dos21_ah09
    mov ah, 0x09
    int 0x21

    mov dl, '*'
    mov ah, 0x02
    int 0x21
    call print_newline_dual

    mov ax, 0x4C2A
    int 0x21

    mov ah, 0x4D
    int 0x21

    mov si, msg_dos21_status
    call print_string_dual
    call print_hex8_dual
    mov al, ' '
    call putc_dual
    mov al, ah
    call print_hex8_dual
    call print_newline_dual

    mov si, msg_dos21_serial_pass
    call print_string_serial

    pop ds
    ret

run_stage1_selftest:
    mov si, msg_stage1_selftest_begin
    call print_string_dual
    mov si, msg_stage1_selftest_serial_begin
    call print_string_serial
    mov si, cmd_selftest_dos21
    call load_cmd_buffer
    call dispatch_command
    mov si, cmd_selftest_comdemo
    call load_cmd_buffer
    call dispatch_command
    mov si, msg_stage1_selftest_done
    call print_string_dual
    mov si, msg_stage1_selftest_serial_done
    call print_string_serial
    ret

load_cmd_buffer:
    push di
    mov di, cmd_buffer
.copy:
    lodsb
    stosb
    test al, al
    jnz .copy
    pop di
    ret

run_com_demo:
    mov si, msg_com_begin
    call print_string_dual

    call load_com_demo_from_disk
    jc .load_fail

    mov [saved_ss], ss
    mov [saved_sp], sp
    mov [saved_ds], ds
    mov [saved_es], es

    cli
    mov ax, COM_LOAD_SEG
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE
    sti

    call far [cs:com_entry_off]

    cli
    mov ax, cs
    mov ds, ax
    mov ax, [saved_ss]
    mov ss, ax
    mov sp, [saved_sp]
    sti

    mov ax, [saved_ds]
    mov ds, ax
    mov ax, [saved_es]
    mov es, ax

    mov ah, 0x4D
    int 0x21
    mov bl, al

    mov al, [last_exit_code]
    mov si, msg_com_done
    call print_string_dual
    call print_hex8_dual
    mov al, ' '
    call putc_dual
    mov al, bl
    call print_hex8_dual
    call print_newline_dual

    cmp byte [last_exit_code], 0x37
    jne .serial_fail
    mov si, msg_com_serial_pass
    call print_string_serial
    ret
.load_fail:
    mov si, msg_com_load_fail
    call print_string_dual
    mov si, msg_com_serial_fail
    call print_string_serial
    ret
.serial_fail:
    mov si, msg_com_serial_fail
    call print_string_serial
    ret

load_com_demo_from_disk:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov ax, COM_LOAD_SEG
    mov es, ax

    xor ax, ax
    xor di, di
    mov cx, 128
    rep stosw

    mov word [es:0x0000], 0x20CD
    mov byte [es:0x0080], 0

    mov dx, FAT_ROOT_START_LBA

.scan_next_sector:
    cmp dx, FAT_ROOT_START_LBA + FAT_ROOT_DIR_SECTORS
    jae .read_fail

    mov ax, dx
    mov bx, 0x0200
    call read_sector_lba
    jc .read_fail

    mov di, 0x0200
    mov cx, 16

.scan_entries:
    mov al, [es:di]
    cmp al, 0x00
    je .read_fail
    cmp al, 0xE5
    je .next_entry

    mov al, [es:di + 11]
    cmp al, 0x0F
    je .next_entry
    test al, 0x08
    jnz .next_entry

    push cx
    push dx
    call fat_entry_matches_comdemo
    pop dx
    pop cx
    jc .found_entry

.next_entry:
    add di, 32
    loop .scan_entries

    inc dx
    jmp .scan_next_sector

.found_entry:
    mov ax, [es:di + 26]
    cmp ax, 2
    jb .read_fail

    sub ax, 2
    add ax, FAT_DATA_START_LBA
    mov bx, 0x0100
    call read_sector_lba
    jc .read_fail

    mov word [com_entry_off], 0x0100
    mov word [com_entry_seg], COM_LOAD_SEG

    clc
    jmp .done

.read_fail:
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

fat_entry_matches_comdemo:
    push ax
    push bx
    push cx

    mov bx, 0
    mov cx, 11

.cmp_loop:
    mov al, [fat_comdemo_name + bx]
    cmp al, [es:di + bx]
    jne .not_match
    inc bx
    loop .cmp_loop

    stc
    jmp .done

.not_match:
    clc

.done:
    pop cx
    pop bx
    pop ax
    ret

read_sector_lba:
    push bx
    push cx
    push dx
    push si

    mov si, bx

    xor dx, dx
    mov cx, FAT_SPT
    div cx

    mov cl, dl
    inc cl

    xor dx, dx
    mov bx, FAT_HEADS
    div bx

    mov ch, al
    mov dh, dl
    mov bx, si
    mov dl, [boot_drive]

    mov ah, 0x02
    mov al, 0x01
    int 0x13

    pop si
    pop dx
    pop cx
    pop bx
    ret

dispatch_command:
    mov si, cmd_buffer
    call skip_spaces
    mov di, si
    mov bx, di

    cmp byte [di], 0
    je .done

    mov di, bx
    mov si, str_help
    call str_eq
    jc .cmd_help
    mov di, bx
    mov si, str_cls
    call str_eq
    jc .cmd_cls
    mov di, bx
    mov si, str_ticks
    call str_eq
    jc .cmd_ticks
    mov di, bx
    mov si, str_drive
    call str_eq
    jc .cmd_drive
    mov di, bx
    mov si, str_dos21
    call str_eq
    jc .cmd_dos21
    mov di, bx
    mov si, str_comdemo
    call str_eq
    jc .cmd_comdemo
    mov di, bx
    mov si, str_reboot
    call str_eq
    jc .cmd_reboot
    mov di, bx
    mov si, str_halt
    call str_eq
    jc .cmd_halt

    mov si, msg_unknown
    call print_string_dual
    jmp .done

.cmd_help:
    mov si, msg_help
    call print_string_dual
    jmp .done

.cmd_cls:
    mov ax, 0x0003
    int 0x10
    mov si, msg_cleared
    call print_string_dual
    jmp .done

.cmd_ticks:
    mov ah, 0x00
    int 0x1A
    mov si, msg_ticks
    call print_string_dual
    mov ax, cx
    call print_hex16_dual
    mov ax, dx
    call print_hex16_dual
    call print_newline_dual
    jmp .done

.cmd_drive:
    mov si, msg_drive
    call print_string_dual
    xor ah, ah
    mov al, [boot_drive]
    call print_hex8_dual
    call print_newline_dual
    jmp .done

.cmd_dos21:
    mov al, [int21_installed]
    cmp al, 1
    jne .cmd_dos21_missing
    call int21_smoke_test
    jmp .done
.cmd_dos21_missing:
    mov si, msg_int21_missing
    call print_string_dual
    jmp .done

.cmd_comdemo:
    call run_com_demo
    jmp .done

.cmd_reboot:
    mov si, msg_rebooting
    call print_string_dual
    int 0x19
    jmp .done

.cmd_halt:
    mov si, msg_halting
    call print_string_dual
.halt_forever:
    cli
    hlt
    jmp .halt_forever

.done:
    ret

read_command_line:
    mov di, cmd_buffer
    mov cx, CMD_BUF_LEN - 1
.read_key:
    xor ah, ah
    int 0x16

    cmp al, 0x0D
    je .finish

    cmp al, 0x08
    je .backspace

    cmp al, 0
    je .read_key

    cmp cx, 0
    je .read_key

    stosb
    dec cx
    call putc_dual
    jmp .read_key

.backspace:
    cmp di, cmd_buffer
    je .read_key
    dec di
    inc cx
    mov al, 0x08
    call putc_dual
    mov al, ' '
    call putc_dual
    mov al, 0x08
    call putc_dual
    jmp .read_key

.finish:
    mov al, 0
    stosb
    call print_newline_dual
    ret

skip_spaces:
.skip:
    cmp byte [si], ' '
    jne .done
    inc si
    jmp .skip
.done:
    ret

; Compare DI (input command) to SI (constant command string).
; Carry set if equal and fully terminated.
str_eq:
.next:
    mov al, [di]
    mov ah, [si]
    cmp ah, 0
    je .expect_end
    cmp al, ah
    jne .not_equal
    inc di
    inc si
    jmp .next
.expect_end:
    cmp al, 0
    je .equal
    cmp al, ' '
    je .equal
.not_equal:
    clc
    ret
.equal:
    stc
    ret

print_string_dual:
    lodsb
    test al, al
    jz .done
    call putc_dual
    jmp print_string_dual
.done:
    ret

print_string_serial:
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp print_string_serial
.done:
    ret

print_newline_dual:
    mov al, 13
    call putc_dual
    mov al, 10
    call putc_dual
    ret

print_hex16_dual:
    push ax
    mov al, ah
    call print_hex8_dual
    pop ax
    call print_hex8_dual
    ret

print_hex8_dual:
    push ax
    mov ah, al
    shr al, 4
    call print_hex_nibble_dual
    mov al, ah
    and al, 0x0F
    call print_hex_nibble_dual
    pop ax
    ret

print_hex_nibble_dual:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    call putc_dual
    ret

putc_dual:
    push ax
    call bios_putc
    pop ax
    call serial_putc
    ret

bios_putc:
    push ax
    push bx
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    pop bx
    pop ax
    ret

serial_init:
    mov dx, 0x03F8 + 1
    mov al, 0x00
    out dx, al
    mov dx, 0x03F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x03F8 + 0
    mov al, 0x03
    out dx, al
    mov dx, 0x03F8 + 1
    mov al, 0x00
    out dx, al
    mov dx, 0x03F8 + 3
    mov al, 0x03
    out dx, al
    mov dx, 0x03F8 + 2
    mov al, 0xC7
    out dx, al
    mov dx, 0x03F8 + 4
    mov al, 0x0B
    out dx, al
    ret

serial_putc:
    push ax
    push dx
    mov ah, al
.wait:
    mov dx, 0x03F8 + 5
    in al, dx
    test al, 0x20
    jz .wait
    mov dx, 0x03F8
    mov al, ah
    out dx, al
    pop dx
    pop ax
    ret

boot_drive db 0
int21_installed db 0
last_exit_code db 0
last_term_type db 0
old_int21_off dw 0
old_int21_seg dw 0
saved_ss dw 0
saved_sp dw 0
saved_ds dw 0
saved_es dw 0
com_entry_off dw 0
com_entry_seg dw 0
cmd_buffer times CMD_BUF_LEN db 0

msg_stage1        db "[STAGE1] CiukiOS stage1 running", 13, 10, 0
msg_stage1_serial db "[STAGE1-SERIAL] READY", 13, 10, 0
msg_diag_begin    db "[STAGE1] BIOS diagnostics", 13, 10, 0
msg_diag_int10    db "[STAGE1] INT10h OK", 13, 10, 0
msg_diag_int13_ok db "[STAGE1] INT13h OK", 13, 10, 0
msg_diag_int13_fail db "[STAGE1] INT13h FAIL", 13, 10, 0
msg_diag_int16_ok db "[STAGE1] INT16h OK", 13, 10, 0
msg_diag_int1a    db "[STAGE1] INT1Ah ticks=0x", 0
msg_int21_installed db "[STAGE1] INT21h vector installed", 13, 10, 0
msg_int21_missing db "[STAGE1] INT21h vector not installed", 13, 10, 0
msg_stage1_selftest_begin db "[STAGE1] selftest begin", 13, 10, 0
msg_stage1_selftest_done db "[STAGE1] selftest done", 13, 10, 0
msg_stage1_selftest_serial_begin db "[STAGE1-SELFTEST] BEGIN", 13, 10, 0
msg_stage1_selftest_serial_done db "[STAGE1-SELFTEST] DONE", 13, 10, 0

msg_prompt    db "ciukios> ", 0
msg_unknown   db "unknown command. type 'help'", 13, 10, 0
msg_help      db "commands: help cls ticks drive dos21 comdemo reboot halt", 13, 10, 0
msg_cleared   db "screen cleared", 13, 10, 0
msg_ticks     db "ticks=0x", 0
msg_drive     db "boot drive=0x", 0
msg_dos21_begin db "[STAGE1] INT21h smoke", 13, 10, 0
msg_dos21_ah09 db "[INT21/AH=09h] console path active", 13, 10, '$'
msg_dos21_status db "[INT21/AH=4Dh] code/type=0x", 0
msg_dos21_serial_pass db "[DOS21-SERIAL] PASS", 13, 10, 0
msg_com_begin db "[STAGE1] COM demo load/exec", 13, 10, 0
msg_com_load_fail db "[STAGE1] COM demo disk read FAIL", 13, 10, 0
msg_com_done  db "[STAGE1] COM demo code/query=0x", 0
msg_com_serial_pass db "[COMDEMO-SERIAL] PASS", 13, 10, 0
msg_com_serial_fail db "[COMDEMO-SERIAL] FAIL", 13, 10, 0
msg_rebooting db "rebooting...", 13, 10, 0
msg_halting   db "halting...", 13, 10, 0

str_help   db "help", 0
str_cls    db "cls", 0
str_ticks  db "ticks", 0
str_drive  db "drive", 0
str_dos21  db "dos21", 0
str_comdemo db "comdemo", 0
str_reboot db "reboot", 0
str_halt   db "halt", 0

cmd_selftest_dos21 db "dos21", 0
cmd_selftest_comdemo db "comdemo", 0

fat_comdemo_name db "COMDEMO COM"
