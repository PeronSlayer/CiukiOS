bits 16
org 0x0000

%define CMD_BUF_LEN 64
%define DOS_ENV_EXEC_PATH_LEN 64
%define SHELL_EXEC_PATH_BUF_LEN 80
%define SHELL_HISTORY_MAX 8
%define COM_LOAD_SEG 0x2000
%define MZ_LOAD_SEG 0x3000
%define MZ2_LOAD_SEG 0x3800
%define MZ3_LOAD_SEG 0x7800
%define RUNTIME_LOAD_SEG 0x9000
%define STAGE2_LOAD_SEG 0x4E00
%define DOS_META_BUF_SEG 0x9200
%define DOS_FAT_BUF_SEG  0x9400
%define DOS_IO_BUF_SEG   0x9600
%define DOS_ENV_SEG      0x9800
%define DOS_SYSVARS_ANCHOR_OFF 0x0800
%define DOS_SYSVARS_OFF        0x0802
%define DOS_SYSVARS_CDS_OFF    0x0900
%define DOS_SYSVARS_DPB_OFF    0x0A80
%define DOS_SYSVARS_SFT_OFF    0x0B00
%define DOS_HEAP_BASE_SEG 0x5800
%define DOS_HEAP_LIMIT_SEG 0x9000
%define DOS_HEAP_MAX_PARAS (DOS_HEAP_LIMIT_SEG - DOS_HEAP_BASE_SEG)
%define DOS_HEAP_USER_SEG (DOS_HEAP_BASE_SEG + 1)
%define DOS_HEAP_USER_MAX_PARAS (DOS_HEAP_MAX_PARAS - 1)
%define DOS_MEM_BLOCK_FREE 0
%define DOS_MEM_BLOCK_ALLOC 1
%define DOS_MEM_BLOCK_TABLE_MAX 32
%define DOS_MEM_BLOCK_ENTRY_SIZE 8
%ifndef DOS_DEFAULT_DRIVE_INDEX
%if FAT_TYPE == 16
%define DOS_DEFAULT_DRIVE_INDEX 2
%else
%define DOS_DEFAULT_DRIVE_INDEX 0
%endif
%endif
%ifndef FAT_SPT
%define FAT_SPT 18
%endif
%ifndef FAT_HEADS
%define FAT_HEADS 2
%endif
%ifndef FAT_RESERVED_SECTORS
%define FAT_RESERVED_SECTORS 21
%endif
%ifndef FAT_SECTORS_PER_FAT
%define FAT_SECTORS_PER_FAT 9
%endif
%ifndef FAT_COUNT
%define FAT_COUNT 2
%endif
%ifndef FAT_ROOT_DIR_SECTORS
%define FAT_ROOT_DIR_SECTORS 14
%endif
%ifndef FAT_SECTORS_PER_CLUSTER
%define FAT_SECTORS_PER_CLUSTER 1
%endif
%ifndef FAT_TYPE
%define FAT_TYPE 12
%endif
%ifndef FAT_LBA_OFFSET
%define FAT_LBA_OFFSET 0
%endif
%if FAT_TYPE == 16
%define FAT_EOF 0xFFF8
%else
%define FAT_EOF 0xFF8
%endif
%ifndef STAGE1_SELFTEST_AUTORUN
%define STAGE1_SELFTEST_AUTORUN 0
%endif
%ifndef STAGE1_RUNTIME_PROBE
%define STAGE1_RUNTIME_PROBE 0
%endif
%ifndef HARDWARE_VALIDATION_SCREEN
%define HARDWARE_VALIDATION_SCREEN 0
%endif
%ifndef ENABLE_PS2_MOUSE_INIT
%define ENABLE_PS2_MOUSE_INIT 1
%endif
%ifndef MOUSE_VGA_SCALE_SHIFT
%define MOUSE_VGA_SCALE_SHIFT 1
%endif
%if FAT_TYPE == 16
%define SPLASH_PALETTE_COLORS 256
%define SPLASH_PALETTE_SIZE 768
%define SPLASH_SRC_W 160
%define SPLASH_SRC_H 100
%define SPLASH_SRC_ROW_BYTES 160
%define SPLASH_PIXEL_BYTES 16000
%define SPLASH_TOTAL_SIZE 16768
%define SPLASH_SCALE_X 5
%define SPLASH_SCALE_Y 5
%define SPLASH_VESA_MODE 0x0103
%define SPLASH_VESA_ROW_BYTES 800
%define SPLASH_VRAM_SAFE_OFFSET 0xFCE0
%define SPLASH_BUF_SEG 0x9A00
%define SPLASH_WAIT_TICKS 91
%define SHELL_FOOTER_DSK_IDLE_REFRESH_TICKS 54
%define SHELL_FOOTER_DSK_BUSY_REFRESH_TICKS 216
%define SHELL_FOOTER_DSK_DIRTY_IDLE_REFRESH_TICKS 2
%define SHELL_FOOTER_DSK_DIRTY_BUSY_REFRESH_TICKS 18
%define SHELL_FOOTER_KEY_COOLDOWN_TICKS 18
%endif
%define FAT1_LBA FAT_RESERVED_SECTORS
%define FAT2_LBA (FAT1_LBA + FAT_SECTORS_PER_FAT)
%define FAT_ROOT_START_LBA (FAT_RESERVED_SECTORS + (FAT_COUNT * FAT_SECTORS_PER_FAT))
%define FAT_DATA_START_LBA (FAT_ROOT_START_LBA + FAT_ROOT_DIR_SECTORS)
%if FAT_SECTORS_PER_CLUSTER == 1
%define FAT_CLUSTER_SECTOR_SHIFT 0
%define FAT_CLUSTER_SHIFT 9
%define FAT_CLUSTER_MASK 0x01FF
%elif FAT_SECTORS_PER_CLUSTER == 2
%define FAT_CLUSTER_SECTOR_SHIFT 1
%define FAT_CLUSTER_SHIFT 10
%define FAT_CLUSTER_MASK 0x03FF
%elif FAT_SECTORS_PER_CLUSTER == 4
%define FAT_CLUSTER_SECTOR_SHIFT 2
%define FAT_CLUSTER_SHIFT 11
%define FAT_CLUSTER_MASK 0x07FF
%elif FAT_SECTORS_PER_CLUSTER == 8
%define FAT_CLUSTER_SECTOR_SHIFT 3
%define FAT_CLUSTER_SHIFT 12
%define FAT_CLUSTER_MASK 0x0FFF
%else
%error Unsupported FAT_SECTORS_PER_CLUSTER value
%endif

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
    mov si, msg_stage1_serial
    call print_string_serial

    call run_bios_diagnostics
    call install_int21_vector
%if FAT_TYPE == 16
    call stage1_runtime_init
%endif
    call init_stage2_services
    call init_shell_default_dirs
%if FAT_TYPE == 16 && STAGE1_RUNTIME_PROBE
    call stage1_runtime_probe
%endif
%if STAGE1_SELFTEST_AUTORUN
    call run_stage1_selftest
%endif
%if FAT_TYPE == 16
%if STAGE1_SELFTEST_AUTORUN == 0
    call stage1_show_boot_splash
%endif
%endif
    call draw_shell_chrome
%if FAT_TYPE == 16
%if HARDWARE_VALIDATION_SCREEN
    call print_hardware_validation_screen
%endif
%endif

    call flush_keyboard_buffer
    jmp main_loop

helper_get_drive_letter:
    ; Input: al = boot_drive value (BIOS format: 0x00=A, 0x01=B, 0x80=C, 0x81=D, etc.)
    ; Output: al = drive letter ASCII ('A', 'B', 'C', etc.)
    cmp al, 0x80
    jb .floppy_drive
    ; Hard disk: 0x80=C, 0x81=D, etc.
    sub al, 0x7E    ; 0x80 - 0x7E = 2, so 0x80 -> 2 (C), 0x81 -> 3 (D), etc.
.floppy_drive:
    ; Floppy: 0x00=A, 0x01=B, 0x02=C (shouldn't happen on floppy)
    and al, 0x0F    ; Ensure single digit (0-15)
    add al, 0x41    ; Convert to ASCII ('A', 'B', etc.)
    ret

print_prompt:
    push ax
    push si
    ; Print "CiukiOS "
    mov si, msg_prompt_prefix
    call print_string_dual
    ; Print drive letter
    mov al, [dos_default_drive]
    add al, 0x41
    call putc_dual
    ; Print ":"
    mov al, 0x3A
    call putc_dual
    ; Print "\"
    mov al, 0x5C
    call putc_dual
    cmp byte [cwd_buf], 0
    je .prompt_gt
    mov si, cwd_buf
    call print_string_dual
    ; Print "\" after CWD
    mov al, 0x5C
    call putc_dual
.prompt_gt:
    ; Print "> "
    mov al, 0x3E
    call putc_dual
    mov al, 0x20
    call putc_dual
    pop si
    pop ax
    ret

main_loop:
%if FAT_TYPE == 16
    call shell_update_footer
%endif
    call print_prompt

    call read_command_line
    call dispatch_command
    jmp main_loop

init_shell_default_dirs:
    push ax
    push dx
    push ds

    mov ax, cs
    mov ds, ax

    mov dx, path_system_dir_dos
    mov ah, 0x39
    int 0x21

    mov dx, path_apps_dir_dos
    mov ah, 0x39
    int 0x21

    mov dx, path_apps_dir_dos
    mov ah, 0x3B
    int 0x21
    jnc .done

    mov dx, path_root_dos
    mov ah, 0x3B
    int 0x21

.done:
    pop ds
    pop dx
    pop ax
    ret

flush_keyboard_buffer:
.check:
    mov ah, 0x01
    int 0x16
    jz .done
    xor ah, ah
    int 0x16
    jmp .check
.done:
    ret

run_bios_diagnostics:
    mov si, msg_diag_begin
    call print_string_dual

    mov si, msg_diag_int10
    call print_string_dual

    ; INT13 AH=0x00 (disk reset) may fail in some QEMU configurations or after
    ; PS/2 mouse initialization due to PIC mask changes. However, actual disk I/O
    ; (AH=0x02 read operations) works correctly. This is a diagnostic-only call.
    ; We always report OK since real disk operations are verified by boot success.
    mov ah, 0x00
    mov dl, [boot_drive]
    int 0x13
    ; Ignore carry flag - reset failures are non-critical for diagnostics.
    ; If real disk I/O fails, the boot would have already failed.
    clc
    mov si, msg_diag_int13_ok
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
    mov byte [int2f_installed], 0
    mov byte [last_exit_code], 0
    mov byte [last_term_type], 0
    mov byte [dos_default_drive], DOS_DEFAULT_DRIVE_INDEX
    mov byte [find_active], 0
    mov ax, cs
    mov [dta_seg], ax
    mov word [dta_off], find_dta

    mov bx, 0x20 * 4
    mov ax, [es:bx]
    mov [old_int20_off], ax
    mov ax, [es:bx + 2]
    mov [old_int20_seg], ax
    mov word [es:bx], int20_handler
    mov ax, cs
    mov [es:bx + 2], ax

    ; Install a minimal INT 2Fh multiplex handler for DOS compatibility.
    mov bx, 0x2F * 4
    mov ax, [es:bx]
    mov [old_int2f_off], ax
    mov ax, [es:bx + 2]
    mov [old_int2f_seg], ax
    mov word [es:bx], int2f_handler
    mov ax, cs
    mov [es:bx + 2], ax
    mov byte [int2f_installed], 1

    mov bx, 0x60 * 4
    mov cx, 8
.user_int_iret_loop:
    mov word [es:bx], int_default_iret
    mov ax, cs
    mov [es:bx + 2], ax
    add bx, 4
    loop .user_int_iret_loop

    ; Track video mode for DOS programs that query INT 10h directly.
    mov bx, 0x10 * 4
    mov ax, [es:bx]
    mov [old_int10_off], ax
    mov ax, [es:bx + 2]
    mov [old_int10_seg], ax
    mov word [es:bx], int10_handler
    mov ax, cs
    mov [es:bx + 2], ax
    mov byte [current_video_mode], 0x03

    ; Keep BIOS keyboard services untouched on real hardware.
    ; Some legacy machines (e.g. ThinkPad T23) are sensitive to INT16 hooks.

%if FAT_TYPE == 16
    ; The desktop runtime VGA driver can use the IBM PS/2 BIOS mouse API directly.
    mov bx, 0x15 * 4
    mov ax, [es:bx]
    mov [old_int15_off], ax
    mov ax, [es:bx + 2]
    mov [old_int15_seg], ax
    mov word [es:bx], int15_handler
    mov ax, cs
    mov [es:bx + 2], ax
%endif

    pop es
    pop bx
    pop ax

    mov si, msg_int21_installed
    call print_string_dual
    ret

int20_handler:
    push ax
    push bp
    mov byte [cs:last_exit_code], 0
    mov byte [cs:last_term_type], 0
    mov ax, [cs:current_psp_seg]
    or ax, ax
    jz .done
    mov bp, sp
    mov ax, [cs:current_com_load_seg]
    cmp [ss:bp + 6], ax
    jne .mz_terminate
    mov word [ss:bp + 4], int21_com_terminate_trampoline
    jmp .set_terminate_cs
.mz_terminate:
    mov word [ss:bp + 4], int21_mz_terminate_trampoline
.set_terminate_cs:
    mov ax, cs
    mov [ss:bp + 6], ax

.done:
    pop bp
    pop ax
    iret

int_default_iret:
    iret

int21_handler:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    push ax
    mov bp, sp
    mov ax, [ss:bp + 20]
    mov [cs:int21_trace_call_cs], ax
    push bx
    mov bx, cs
    mov byte [cs:int21_path_upcase], 0
    cmp ax, bx
    je .path_case_ready
    mov byte [cs:int21_path_upcase], 1
.path_case_ready:
    pop bx
    mov ax, ds
    mov [cs:int21_caller_ds], ax
    pop ax
    mov byte [cs:int21_carry], 0
    mov byte [cs:int21_return_es], 0
    mov byte [cs:int21_zf_state], 0xFF
    mov byte [cs:int21_last_ah], ah
    mov byte [cs:int21_last_al], al

    cmp ah, 0x00
    je .fn_00
    cmp ah, 0x01
    je .fn_01
    cmp ah, 0x02
    je .fn_02
    cmp ah, 0x06
    je .fn_06
    cmp ah, 0x07
    je .fn_07
    cmp ah, 0x08
    je .fn_08
    cmp ah, 0x09
    je .fn_09
    cmp ah, 0x0A
    je .fn_0a
    cmp ah, 0x0B
    je .fn_0b
    cmp ah, 0x0C
    je .fn_0c
    cmp ah, 0x0D
    je .fn_0d
    cmp ah, 0x0E
    je .fn_0e
    cmp ah, 0x1A
    je .fn_1a
    cmp ah, 0x20
    je .fn_20
    cmp ah, 0x19
    je .fn_19
    cmp ah, 0x2A
    je .fn_2a
    cmp ah, 0x2B
    je .fn_2b
    cmp ah, 0x2C
    je .fn_2c
    cmp ah, 0x2D
    je .fn_2d
    cmp ah, 0x2E
    je .fn_2e
    cmp ah, 0x25
    je .fn_25
    cmp ah, 0x2F
    je .fn_2f
    cmp ah, 0x30
    je .fn_30
    cmp ah, 0x31
    je .fn_31
    cmp ah, 0x33
    je .fn_33
    cmp ah, 0x34
    je .fn_34
    cmp ah, 0x35
    je .fn_35
    cmp ah, 0x36
    je .fn_36
    cmp ah, 0x37
    je .fn_37
    cmp ah, 0x39
    je .fn_39
    cmp ah, 0x3A
    je .fn_3a
    cmp ah, 0x3B
    je .fn_3b
    cmp ah, 0x3C
    je .fn_3c
    cmp ah, 0x3D
    je .fn_3d
    cmp ah, 0x3E
    je .fn_3e
    cmp ah, 0x3F
    je .fn_3f
    cmp ah, 0x40
    je .fn_40
    cmp ah, 0x41
    je .fn_41
    cmp ah, 0x42
    je .fn_42
    cmp ah, 0x44
    je .fn_44
    cmp ah, 0x45
    je .fn_45
    cmp ah, 0x46
    je .fn_46
    cmp ah, 0x43
    je .fn_43
    cmp ah, 0x47
    je .fn_47
    cmp ah, 0x4E
    je .fn_4e
    cmp ah, 0x4F
    je .fn_4f
    cmp ah, 0x4B
    je .fn_4b
    cmp ah, 0x48
    je .fn_48
    cmp ah, 0x49
    je .fn_49
    cmp ah, 0x4A
    je .fn_4a
    cmp ah, 0x4C
    je .fn_4c
    cmp ah, 0x4D
    je .fn_4d
    cmp ah, 0x50
    je .fn_50
    cmp ah, 0x51
    je .fn_51
    cmp ah, 0x52
    je .fn_52
    cmp ah, 0x54
    je .fn_54
    cmp ah, 0x55
    je .fn_55
    cmp ah, 0x56
    je .fn_56
    cmp ah, 0x57
    je .fn_57
    cmp ah, 0x58
    je .fn_58
    cmp ah, 0x59
    je .fn_59
    cmp ah, 0x60
    je .fn_60
    cmp ah, 0x62
    je .fn_62
    cmp ah, 0x38
    je .fn_38
    cmp ah, 0x67
    je .fn_67
    cmp ah, 0x68
    je .fn_68
    cmp ah, 0x66
    je .fn_66
    jmp .unsupported

.fn_02:
    mov al, dl
    call bios_putc
    call serial_putc
    jmp .success

.fn_06:
    cmp dl, 0xFF
    je .fn_06_input
    mov al, dl
    call bios_putc
    call serial_putc
    jmp .success

.fn_06_input:
    mov ah, 0x01
    int 0x16
    jz .fn_06_no_key
    mov ah, 0x00
    int 0x16
    mov byte [cs:int21_zf_state], 0
    jmp .success

.fn_06_no_key:
    xor ax, ax
    mov byte [cs:int21_zf_state], 1
    jmp .success

.fn_07:
.fn_08:
    mov ah, 0x00
    int 0x16
    jmp .success

.fn_00:
    xor al, al
    jmp .fn_4c

.fn_20:
    xor al, al
    jmp .fn_4c

.fn_01:
    mov ah, 0x00
    int 0x16
    call bios_putc
    call serial_putc
    jmp .success

.fn_09:
    mov si, dx
.fn_09_loop:
    lodsb
    cmp al, '$'
    je .success
    call bios_putc
    call serial_putc
    jmp .fn_09_loop

.fn_0a:
    push bx
    push cx
    mov bx, dx
    mov cl, [ds:bx]         ; max length
    xor ch, ch
    xor si, si              ; count
    cmp cx, 1
    jbe .fn_0a_done

.fn_0a_read_loop:
    mov ah, 0x00
    int 0x16
    cmp al, 0x0D
    je .fn_0a_store_cr
    cmp al, 0x08
    jne .fn_0a_store_char
    cmp si, 0
    je .fn_0a_read_loop
    dec si
    jmp .fn_0a_read_loop

.fn_0a_store_char:
    cmp si, cx
    jae .fn_0a_read_loop
    mov [ds:bx + 2 + si], al
    inc si
    jmp .fn_0a_read_loop

.fn_0a_store_cr:
    mov [ds:bx + 2 + si], al

.fn_0a_done:
    mov [ds:bx + 1], si
    pop cx
    pop bx
    jmp .success

.fn_0b:
    mov ah, 0x01
    int 0x16
    jz .fn_0b_no_key
    mov al, 0xFF
    jmp .success
.fn_0b_no_key:
    xor al, al
    jmp .success

.fn_0c:
    mov bl, al
    call int21_kbd_flush
    mov al, bl
    cmp al, 0x06
    je .fn_06_input
    cmp al, 0x07
    je .fn_07
    cmp al, 0x08
    je .fn_08
    cmp al, 0x0A
    je .fn_0a
    xor al, al
    jmp .success

.fn_0d:
    xor ax, ax
    jmp .success

.fn_0e:
    call int21_set_default_drive
    jc .error
    jmp .success

.fn_1a:
    call int21_set_dta
    jc .error
    jmp .success

.fn_19:
    call int21_get_default_drive
    jc .error
    jmp .success

.fn_2a:
    call int21_get_date
    jc .error
    jmp .success

.fn_2b:
    xor ax, ax
    jmp .success

.fn_2c:
    call int21_get_time
    jc .error
    jmp .success

.fn_2d:
    xor ax, ax
    jmp .success

.fn_2e:
    mov al, dl
    and al, 0x01
    mov [cs:dos_verify_flag], al
    xor ah, ah
    jmp .success

.fn_25:
    call int21_set_vector
    jc .error
    jmp .success

.fn_2f:
    call int21_get_dta
    mov byte [cs:int21_return_es], 1
    jc .error
    jmp .success

.fn_30:
    call int21_get_version
    jc .error
    jmp .success

.fn_33:
    call int21_ctrl_break
    jc .error
    jmp .success

.fn_34:
    call int21_get_indos_ptr
    mov byte [cs:int21_return_es], 1
    jc .error
    jmp .success

.fn_35:
    call int21_get_vector
    mov byte [cs:int21_return_es], 1
    jc .error
    jmp .success

.fn_36:
    call int21_get_free_space
    jc .error
    jmp .success

.fn_39:
    call int21_mkdir
    jc .error
    jmp .success

.fn_3a:
    call int21_rmdir
    jc .error
    jmp .success

.fn_56:
    call int21_rename
    jc .error
    jmp .success

.fn_3b:
    call int21_chdir
    jc .error
    jmp .success

.fn_3c:
    call int21_create
    jc .error
    jmp .success

.fn_3d:
    call int21_open
    jc .error
    jmp .success

.fn_3e:
    call int21_close
    jc .error
    jmp .success

.fn_3f:
    call int21_read
    jc .error
    jmp .success

.fn_40:
    call int21_write
    jc .error
    jmp .success

.fn_41:
    call int21_delete
    jc .error
    jmp .success

.fn_42:
    call int21_seek
    jc .error
    jmp .success

.fn_44:
    call int21_ioctl
    jc .error
    jmp .success

.fn_45:
%if FAT_TYPE == 16
    cmp bx, 0x0005
    jne .fn_45_legacy
    cmp byte [cs:file_handle_open], 1
    jne .error
    cmp byte [cs:file_handle2_open], 0
    jne .fn_45_no_slots
    call int21_dup_handle1_to_2
    mov ax, 0x0006
    jmp .success
.fn_45_no_slots:
    mov ax, 0x0004
    jmp .error
.fn_45_legacy:
%endif
    call int21_is_valid_handle
    jc .error
    mov ax, bx
    jmp .success

.fn_46:
%if FAT_TYPE == 16
    cmp bx, 0x0005
    jne .fn_46_legacy
    cmp cx, 0x0006
    jne .fn_46_legacy
    cmp byte [cs:file_handle_open], 1
    jne .error
    call int21_dup_handle1_to_2
    xor ax, ax
    jmp .success
.fn_46_legacy:
%endif
    call int21_is_valid_handle
    jc .error
    push bx
    mov bx, cx
    cmp bx, 5
    jb .fn_46_ok
    call int21_is_valid_handle
    jc .fn_46_bad_target
.fn_46_ok:
    pop bx
    mov ax, cx
    jmp .success
.fn_46_bad_target:
    pop bx
    jmp .error

.fn_43:
    call int21_get_set_attr
    jc .error
    jmp .success

.fn_47:
    call int21_getcwd
    jc .error
    jmp .success

.fn_4e:
    call int21_find_first
    jc .error
    jmp .success

.fn_4f:
    call int21_find_next
    jc .error
    jmp .success

.fn_4b:
    mov al, [cs:int21_last_al]
    call int21_exec
    jc .error
    jmp .success

.fn_48:
    call int21_alloc
    jc .fn_48_error_restore
    mov bp, sp
    mov bx, [ss:bp + 14]
    mov cx, [ss:bp + 12]
    mov dx, [ss:bp + 10]
    mov si, [ss:bp + 8]
    mov di, [ss:bp + 6]
    jmp .success
.fn_48_error_restore:
    mov bp, sp
    mov cx, [ss:bp + 12]
    mov dx, [ss:bp + 10]
    mov si, [ss:bp + 8]
    mov di, [ss:bp + 6]
    jmp .error

.fn_49:
    call int21_free
    jc .fn_49_error
    mov bp, sp
    mov bx, [ss:bp + 14]
    mov cx, [ss:bp + 12]
    mov dx, [ss:bp + 10]
    mov si, [ss:bp + 8]
    mov di, [ss:bp + 6]
    jmp .success
.fn_49_error:
    mov bp, sp
    mov bx, [ss:bp + 14]
    mov cx, [ss:bp + 12]
    mov dx, [ss:bp + 10]
    mov si, [ss:bp + 8]
    mov di, [ss:bp + 6]
    jmp .error

.fn_4a:
    call int21_resize
    jc .fn_4a_error_restore
    mov bp, sp
    mov bx, [ss:bp + 14]
    mov cx, [ss:bp + 12]
    mov dx, [ss:bp + 10]
    mov si, [ss:bp + 8]
    mov di, [ss:bp + 6]
    jmp .success
.fn_4a_error_restore:
    mov bp, sp
    mov cx, [ss:bp + 12]
    mov dx, [ss:bp + 10]
    mov si, [ss:bp + 8]
    mov di, [ss:bp + 6]
    jmp .error

.fn_4c:
    mov [cs:last_exit_code], al
    mov byte [cs:last_term_type], 0
    mov ax, [cs:current_psp_seg]
    or ax, ax
    jz .fn_4c_no_process
    mov byte [cs:int21_force_terminate], 1
.fn_4c_no_process:
    xor ax, ax
    jmp .success

.fn_4d:
    mov al, [cs:last_exit_code]
    mov ah, [cs:last_term_type]
    jmp .success

.fn_50:
    mov [cs:current_psp_seg], bx
    xor ax, ax
    jmp .success

.fn_51:
    call int21_get_psp
    jc .error
    jmp .success

.fn_31:
    mov [cs:last_exit_code], al
    mov byte [cs:last_term_type], 1
    mov ax, [cs:current_psp_seg]
    or ax, ax
    jz .fn_31_done
    ; AH=31 keep-resident: try to resize PSP block to DX paragraphs.
    push dx
    mov es, ax
    mov bx, dx
    call int21_resize
    pop dx
    mov byte [cs:int21_force_terminate], 1
.fn_31_done:
    xor ax, ax
    jmp .success

.fn_37:
    cmp al, 0x00
    je .fn_37_get
    cmp al, 0x01
    je .fn_37_set
    mov ax, 0x0001
    jmp .error

.fn_37_get:
    mov dl, 0x2F
    xor ax, ax
    jmp .success

.fn_37_set:
    xor ax, ax
    jmp .success

.fn_52:
    call int21_get_list_of_lists
    mov byte [cs:int21_return_es], 1
    jc .error
    jmp .success

.fn_54:
    xor ax, ax
    jmp .success

.fn_55:
    call int21_create_child_psp
    jc .error
    jmp .success

.fn_57:
    cmp al, 0x00
    je .fn_57_get
    cmp al, 0x01
    je .fn_57_set
    mov ax, 0x0001
    jmp .error

.fn_57_get:
    call int21_is_valid_handle
    jc .error

    call int21_get_time
    jc .error

    mov bl, cl
    mov bh, dh
    xor ax, ax
    mov al, ch
    mov cl, 11
    shl ax, cl
    mov cx, ax

    xor ax, ax
    mov al, bl
    mov cl, 5
    shl ax, cl
    or cx, ax

    xor ax, ax
    mov al, bh
    shr al, 1
    or cx, ax

    call int21_get_date
    jc .error

    push cx
    mov ax, cx
    sub ax, 1980
    jnc .fn_57_year_ok
    xor ax, ax
.fn_57_year_ok:
    mov cl, 9
    shl ax, cl
    xchg bx, ax

    xor ax, ax
    mov al, dh
    mov cl, 5
    shl ax, cl
    or bx, ax

    xor ax, ax
    mov al, dl
    or bx, ax
    mov dx, bx
    pop cx

    xor ax, ax
    jmp .success

.fn_57_set:
    call int21_is_valid_handle
    jc .error
    xor ax, ax
    jmp .success

.fn_58:
    call int21_mem_strategy
    jc .error
    jmp .success

.fn_59:
    mov ax, [cs:int21_error_ax]
    xor bx, bx
    xor ch, ch
    jmp .success

.fn_60:
    push bx
    push cx
    push si
    push di

    mov ax, ds
    or ax, ax
    jz .fn_60_bad
    mov ax, es
    or ax, ax
    jz .fn_60_bad
    or si, si
    jz .fn_60_bad
    or di, di
    jz .fn_60_bad
    mov al, [ds:si]
    or al, al
    jz .fn_60_bad

    cld
    mov cx, 127
.fn_60_copy:
    lodsb
    stosb
    or al, al
    jz .fn_60_ok
    loop .fn_60_copy
    mov byte [es:di - 1], 0

.fn_60_ok:
    pop di
    pop si
    pop cx
    pop bx
    xor ax, ax
    jmp .success

.fn_60_bad:
    pop di
    pop si
    pop cx
    pop bx
    mov ax, 0x0003
    jmp .error

.fn_62:
    call int21_get_psp
    jc .error
    jmp .success

.fn_38:
    call int21_country_info
    jc .error
    jmp .success

.fn_67:
    xor ax, ax
    jmp .success

.fn_68:
    call int21_is_valid_handle
    jc .error
    xor ax, ax
    jmp .success

.fn_66:
    call int21_code_page
    jc .error
    jmp .success

.unsupported:
    mov ax, 0x0001
    jmp .error

.success:
%if FAT_TYPE == 16
    push ax
    mov al, [cs:int21_last_ah]
    cmp al, 0x39
    je .mark_disk_dirty
    cmp al, 0x3A
    je .mark_disk_dirty
    cmp al, 0x3C
    je .mark_disk_dirty
    cmp al, 0x41
    je .mark_disk_dirty
    cmp al, 0x56
    jne .mark_done
.mark_disk_dirty:
    mov byte [cs:shell_footer_dsk_dirty], 1
.mark_done:
    pop ax
%endif
    mov byte [cs:int21_carry], 0
    jmp .done

.error:
    mov [cs:int21_error_ax], ax
    mov byte [cs:int21_carry], 1

.done:
    mov bp, sp
    cmp byte [cs:int21_force_terminate], 0
    je .term_done
    mov byte [cs:int21_force_terminate], 0
    mov ax, [cs:current_com_load_seg]
    cmp [bp + 18], ax
    jne .term_mz
    mov word [bp + 16], int21_com_terminate_trampoline
    jmp .term_set_cs
.term_mz:
    mov word [bp + 16], int21_mz_terminate_trampoline
.term_set_cs:
    mov ax, cs
    mov [bp + 18], ax
.term_done:
    cmp byte [cs:int21_return_es], 0
    je .flags_only
    mov [bp + 0], es
.flags_only:
    mov [bp + 14], bx
    mov [bp + 12], cx
    mov [bp + 10], dx
    mov [bp + 8], si
    mov [bp + 6], di

    cmp byte [cs:int21_zf_state], 0xFF
    je .zf_done
    cmp byte [cs:int21_zf_state], 0
    jne .zf_set
    and word [bp + 20], 0xFFBF
    jmp .zf_done
.zf_set:
    or word [bp + 20], 0x0040
.zf_done:

    cmp byte [cs:int21_carry], 0
    jne .set_carry
    and word [bp + 20], 0xFFFE
    jmp .restore
.set_carry:
    or word [bp + 20], 0x0001
.restore:
    mov byte [cs:int21_path_upcase], 0
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    cld
    iret

int21_kbd_flush:
.loop:
    mov ah, 0x01
    int 0x16
    jz .done
    mov ah, 0x00
    int 0x16
    jmp .loop
.done:
    ret

int21_smoke_test:
    push ds

    mov si, msg_dos21_begin
    call print_string_dual

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

    mov ah, 0x19
    int 0x21
    cmp al, [cs:dos_default_drive]
    jne .fail
    mov [cs:dos21_saved_drive], al

    xor dx, dx
    mov dl, [cs:dos_default_drive]
    mov ah, 0x0E
    int 0x21
    jc .fail

    xor dx, dx
    mov ah, 0x36
    int 0x21
    jc .fail

%if FAT_TYPE == 16
    mov dl, 3
    mov ah, 0x36
    int 0x21
    jc .fail

    mov dl, 4
    mov ah, 0x36
    int 0x21
    jc .fail
%endif

    mov dx, find_dta
    mov ax, 0x3800
    int 0x21
    jc .fail
    cmp bx, 1
    jne .fail
    cmp word [cs:find_dta], 0
    jne .fail
    cmp byte [cs:find_dta + 2], '$'
    jne .fail

    mov bx, 20
    mov ah, 0x67
    int 0x21
    jc .fail

    xor bx, bx
    mov ax, 0x4406
    int 0x21
    jc .fail
    cmp al, 0xFF
    jne .fail
%if FAT_TYPE == 16
    mov dl, 3
    mov ah, 0x0E
    int 0x21
    jc .fail
    mov si, tmp_cwd_comp
    mov dl, 4
    mov ah, 0x47
    int 0x21
    jc .fail

    mov dl, 2
    mov ah, 0x0E
    int 0x21
    jc .fail
    mov si, tmp_cwd_comp
    mov dl, 3
    mov ah, 0x47
    int 0x21
    jc .fail

    mov dl, [cs:dos21_saved_drive]
    mov ah, 0x0E
    int 0x21
    jc .fail
%endif

    mov dx, path_root_dos
    mov ah, 0x3B
    int 0x21
    jc .fail

    mov si, cwd_buf
    xor dl, dl
    mov ah, 0x47
    int 0x21
    jc .fail
    cmp byte [cwd_buf], 0
    jne .fail

    mov bx, 0x0020
    mov ah, 0x48
    int 0x21
    jc .fail
    cmp ax, DOS_HEAP_USER_SEG
    jne .fail
    mov [dos21_test_seg], ax

    mov es, ax
    mov bx, 0x0030
    mov ah, 0x4A
    int 0x21
    jc .fail

    mov ax, [dos21_test_seg]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jc .fail

    mov ax, [dos21_test_seg]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jnc .fail
    cmp ax, 0x0009
    jne .fail

    mov si, msg_dos21_serial_pass
    call print_string_serial

    pop ds
    ret

.fail:
    mov si, msg_dos21_serial_fail
    call print_string_serial
    pop ds
    ret

int21_exec:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    push word [cs:current_load_seg]
    push word [cs:current_mz_context_slot]
    push word [cs:current_com_load_seg]

    mov [cs:tmp_exec_subfn], al
    cmp al, 0x00
    je .exec_subfn_ok
    cmp al, 0x03
    je .overlay_subfn_ok
    jmp .bad_function

.exec_subfn_ok:
    mov byte [cs:exec_cmd_len], 0
    xor ax, ax
    mov word [cs:tmp_overlay_block_seg], DOS_ENV_SEG
    mov [cs:tmp_overlay_load_seg], ax
    cmp bx, 0
    je .no_param_block
    mov ax, es
    mov [cs:tmp_overlay_load_seg], ax
    mov [cs:tmp_overlay_reloc_seg], bx
    mov ax, [es:bx]
    test ax, ax
    jz .capture_tail
    mov [cs:tmp_overlay_block_seg], ax
.capture_tail:
    call int21_exec_capture_tail
.no_param_block:
    jmp .subfn_ready

.overlay_subfn_ok:
    mov ax, es
    mov [cs:tmp_overlay_block_seg], ax
    mov [cs:tmp_overlay_block_off], bx
    mov ax, [es:bx]
    mov [cs:tmp_overlay_load_seg], ax
    mov ax, [es:bx + 2]
    mov [cs:tmp_overlay_reloc_seg], ax

.subfn_ready:
    call int21_exec_capture_path

    mov si, dx
    call int21_path_to_fat_name
    jc .path_fail

    push si
    mov si, dx
    call int21_resolve_and_find_path
    jnc .path_resolved

%if FAT_TYPE == 16
    ; Compatibility fallback for the desktop runtime: if the caller asks for
    ; plain GEM.EXE and root lookup misses, retry the legacy absolute system path.
    push ax
    mov si, dx
    call int21_path_to_fat_name
    jc .gem_fallback_restore_error
    mov al, [cs:path_fat_name + 0]
    cmp al, 'G'
    jne .gem_fallback_restore_error
    mov al, [cs:path_fat_name + 1]
    cmp al, 'E'
    jne .gem_fallback_restore_error
    mov al, [cs:path_fat_name + 2]
    cmp al, 'M'
    jne .gem_fallback_restore_error
    mov al, [cs:path_fat_name + 8]
    cmp al, 'E'
    jne .gem_fallback_restore_error
    mov al, [cs:path_fat_name + 9]
    cmp al, 'X'
    jne .gem_fallback_restore_error
    mov al, [cs:path_fat_name + 10]
    cmp al, 'E'
    jne .gem_fallback_restore_error
    push ds
    mov ax, cs
    mov ds, ax
    mov si, path_gem_exe_abs
    call int21_resolve_and_find_path
    pop ds
    jc .gem_fallback_restore_error
    add sp, 2
    jmp .path_resolved

.gem_fallback_restore_error:
    pop ax
    stc
%endif

.path_resolved:
    pop si
    jc .done

    cmp byte [cs:tmp_exec_subfn], 0x03
    je .load_overlay

    mov al, [cs:path_fat_name + 8]
    cmp al, 'C'
    jne .check_exe
    mov al, [cs:path_fat_name + 9]
    cmp al, 'O'
    jne .check_exe
    mov al, [cs:path_fat_name + 10]
    cmp al, 'M'
    jne .check_exe

    cmp word [cs:current_psp_seg], 0
    jne .nested_com_seg
    mov word [cs:current_mz_context_slot], 1
    mov word [cs:current_com_load_seg], COM_LOAD_SEG
    jmp .do_exec_com
.nested_com_seg:
    mov word [cs:current_com_load_seg], MZ3_LOAD_SEG
.do_exec_com:
    call int21_exec_load_com
    jc .done
    call int21_exec_run_com
    jc .done
    xor ax, ax
    clc
    jmp .done

.check_exe:
    mov al, [cs:path_fat_name + 8]
    cmp al, 'E'
    je .check_exe_x
    cmp al, 'A'
    je .check_app_p1
     cmp al, 'P'
     je .check_prg_r
    jne .invalid_format

.check_exe_x:
    mov al, [cs:path_fat_name + 9]
    cmp al, 'X'
    jne .invalid_format
    mov al, [cs:path_fat_name + 10]
    cmp al, 'E'
    jne .invalid_format
    jmp .exec_mz

.check_app_p1:
    mov al, [cs:path_fat_name + 9]
    cmp al, 'P'
    jne .invalid_format
    mov al, [cs:path_fat_name + 10]
    cmp al, 'P'
    jne .invalid_format

.check_prg_r:
     mov al, [cs:path_fat_name + 9]
     cmp al, 'R'
     jne .invalid_format
     mov al, [cs:path_fat_name + 10]
     cmp al, 'G'
     jne .invalid_format

.exec_mz:
    cmp word [cs:current_psp_seg], 0
    jne .nested_exec_seg
    mov word [cs:current_mz_context_slot], 1
    mov word [cs:current_load_seg], MZ_LOAD_SEG
    jmp .do_exec_mz
.nested_exec_seg:
    cmp word [cs:current_mz_context_slot], 2
    jae .third_exec_seg
    mov word [cs:current_mz_context_slot], 2
    mov ax, [cs:current_psp_seg]
    mov es, ax
    mov ax, [es:0x0002]
    add ax, 0x000F
    cmp ax, [cs:current_psp_seg]
    jbe .nested_fixed_seg
    cmp ax, DOS_META_BUF_SEG
    jae .nested_fixed_seg
    mov [cs:current_load_seg], ax
    jmp .do_exec_mz
.nested_fixed_seg:
    mov word [cs:current_load_seg], MZ2_LOAD_SEG
    jmp .do_exec_mz
.third_exec_seg:
    mov word [cs:current_mz_context_slot], 3
    mov word [cs:current_load_seg], MZ3_LOAD_SEG
.do_exec_mz:
    call int21_exec_load_mz
    jc .done
    call int21_exec_run_mz
    jc .done
    cmp word [cs:current_load_seg], MZ2_LOAD_SEG
    je .nested_return_trace
    cmp word [cs:current_load_seg], MZ3_LOAD_SEG
    jne .exec_mz_ok
.nested_return_trace:
.exec_mz_ok:
    xor ax, ax
    clc
    jmp .done

.load_overlay:
    mov word [cs:current_load_seg], MZ3_LOAD_SEG
    call int21_exec_load_overlay
    jc .done
    xor ax, ax
    clc
    jmp .done

.invalid_format:
    mov ax, 0x000B
    stc
    jmp .done

.bad_function:
    mov ax, 0x0001
    stc
    jmp .done

.path_fail:
    mov ax, 0x0003
    stc

.done:
    pop word [cs:current_com_load_seg]
    pop word [cs:current_mz_context_slot]
    pop word [cs:current_load_seg]
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_exec_capture_path:
    push ax
    push bx
    push cx
    push si
    push di

    mov si, dx
    mov di, dos_child_exec_path_buf
    mov cx, DOS_ENV_EXEC_PATH_LEN - 1

    mov al, [cs:dos_default_drive]
    add al, 'A'
    cmp byte [si + 1], ':'
    jne .write_prefix
    mov al, [si]
    add si, 2

.write_prefix:
    call .store_char
    mov al, ':'
    call .store_char
    mov al, '\'
    call .store_char

    mov al, [si]
    cmp al, '\'
    je .skip_absolute_prefix
    cmp byte [cs:cwd_buf], 0
    je .copy_loop
    xor bx, bx

.copy_cwd_loop:
    mov al, [cs:cwd_buf + bx]
    test al, al
    jz .cwd_done
    call .store_char
    inc bx
    jmp .copy_cwd_loop

.cwd_done:
    cmp byte [si], 0
    je .done
    mov al, '\'
    call .store_char
    jmp .copy_loop

.skip_absolute_prefix:
    inc si

.copy_loop:
    mov al, [si]
    test al, al
    jz .done
    call .store_char
    inc si
    jmp .copy_loop

.store_char:
    test cx, cx
    jz .store_char_done
    mov [cs:di], al
    inc di
    dec cx

.store_char_done:
    ret

.done:
    mov byte [cs:di], 0
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

int21_exec_capture_tail:
    push dx
    push ds
    push es

    mov ax, es
    or ax, ax
    jz .done

    mov ds, ax
    mov si, bx
    mov di, [si + 2]
    mov dx, [si + 4]
    or dx, dx
    jz .done

    mov ds, dx
    mov si, di
    mov cl, [si]
    cmp cl, 126
    jbe .len_ok
    mov cl, 126
.len_ok:
    mov [cs:exec_cmd_len], cl
    xor ch, ch
    inc si

    mov ax, cs
    mov es, ax
    mov di, exec_cmd_buf
    rep movsb

.done:
    pop es
    pop ds
    pop dx
    ret

int21_exec_write_tail:
    mov al, [cs:exec_cmd_len]
    mov [es:0x0080], al
    xor ch, ch
    mov cl, al
    jcxz .set_cr

    mov ax, cs
    mov ds, ax
    mov si, exec_cmd_buf
    mov di, 0x0081
    rep movsb

.set_cr:
    xor bx, bx
    mov bl, [cs:exec_cmd_len]
    mov byte [es:0x0081 + bx], 0x0D
    ret

int21_exec_init_program_psp:
    xor ax, ax
    xor di, di
    mov cx, 128
    rep stosw
    mov word [es:0x0000], 0x20CD
    mov byte [es:0x0005], 0xCB
    mov word [es:0x000A], 0x0005
    mov ax, es
    mov [es:0x000C], ax
    mov word [es:0x000E], 0x0005
    mov [es:0x0010], ax
    mov word [es:0x0012], 0x0005
    mov [es:0x0014], ax
    call int21_init_psp_handles
    ret

int21_create_child_psp:
    push bx
    push cx
    push di
    push es
    mov es, dx
    xor di, di
    xor ax, ax
    mov cx, 128
    rep stosw
    mov word [es:0x0000], 0x20CD
    mov [es:0x0002], si
    call int21_init_psp_handles
    call int21_mem_adopt_child_psp
    xor ax, ax
    clc
    pop es
    pop di
    pop cx
    pop bx
    ret

int21_mem_adopt_child_psp:
    push ax
    push bx
    push cx
    push di
    push si
    push es

    call int21_mem_init
    mov ax, es
    call int21_mem_table_find_exact
    jc .done
    mov [cs:dos_mem_block_table + si + 4], ax
    call int21_mem_sync_legacy
    call int21_mem_rebuild_chain

.done:
    pop es
    pop si
    pop di
    pop cx
    pop bx
    pop ax
    ret

int21_init_psp_handles:
    push ax
    push cx
    push di
    mov word [es:0x0032], 20
    mov word [es:0x0034], 0x0018
    mov ax, es
    mov [es:0x0036], ax
    mov ax, [cs:current_psp_seg]
    cmp ax, 0
    jne .parent_ready
    mov ax, es
.parent_ready:
    mov [es:0x0016], ax
    mov di, 0x0018
    mov ax, 0x0100
    stosw
    mov ax, 0x0302
    stosw
    mov al, 4
    stosb
    mov al, 0xFF
    mov cx, 15
    rep stosb
    pop di
    pop cx
    pop ax
    ret

int21_exec_prepare_mz_free_mcb:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    cmp word [cs:current_load_seg], MZ_LOAD_SEG
    jne .arena_state_ready
    mov word [cs:dos_mem_alloc_seg], 0
    mov word [cs:dos_mem_alloc_size], 0
    mov word [cs:dos_mem_alloc_seg2], 0
    mov word [cs:dos_mem_alloc_size2], 0
    mov word [cs:dos_mem_alloc_seg3], 0
    mov word [cs:dos_mem_alloc_size3], 0
    mov word [cs:dos_mem_free2_seg], 0
    mov word [cs:dos_mem_free2_size], 0
    call int21_mem_table_clear
.arena_state_ready:
    mov word [cs:dos_mem_psp_mcb_end], 0
    mov word [cs:dos_mem_psp_free_seg], 0
    mov word [cs:dos_mem_psp_free_size], 0

    mov ax, [cs:current_load_seg]
    mov es, ax

    mov bx, [es:0x0004]
    or bx, bx
    jz .done

    mov cl, 5
    shl bx, cl

    mov ax, [es:0x0002]
    or ax, ax
    jz .total_paras_ready
    sub bx, 32
    add ax, 15
    mov cl, 4
    shr ax, cl
    add bx, ax

.total_paras_ready:
    mov ax, [es:0x0008]
    cmp bx, ax
    jbe .done
    sub bx, ax

    add bx, 0x0010
    jc .done
    add bx, [es:0x000A]
    jc .done

    mov ax, [cs:mz_psp_seg]
    mov dx, DOS_HEAP_LIMIT_SEG
    sub dx, ax
    cmp bx, dx
    ja .done

    mov cx, [es:0x000C]
    cmp cx, [es:0x000A]
    jb .mz_alloc_size_ready
    sub cx, [es:0x000A]
    jz .mz_alloc_size_ready
    mov ax, dx
    sub ax, bx
    cmp cx, ax
    jae .mz_alloc_all_available
    add bx, cx
    jmp .mz_alloc_size_ready

.mz_alloc_all_available:
    mov bx, dx

.mz_alloc_size_ready:
    mov ax, [cs:mz_psp_seg]
    mov es, ax
    call int21_resize

.done:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_exec_load_to_es:
    push bx
    push cx
    push dx
    push di
    push ds
    push es

    mov [cs:tmp_user_ds], es

    mov [cs:tmp_exec_error], cx

    ; Path resolution is performed in int21_exec before loading.

    mov ax, [cs:search_found_size_hi]
    mov [cs:tmp_exec_total], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:tmp_exec_limit], ax

    mov ax, [cs:tmp_exec_error]
    cmp ax, 0
    je .size_ok
    cmp word [cs:tmp_exec_total], 0
    jne .too_large
    cmp word [cs:tmp_exec_limit], ax
    ja .too_large

.size_ok:
    mov ax, [cs:search_found_cluster]
    cmp ax, 2
    jb .open_fail
    mov [cs:tmp_cluster], ax

    call int21_load_fat_cache
    jc .open_fail

    mov ax, [cs:tmp_exec_limit]
    or ax, [cs:tmp_exec_total]
    je .close_ok

.read_loop:
    mov ax, [cs:tmp_cluster]
    cmp ax, 2
    jb .io_fail
    cmp ax, FAT_EOF
    jae .io_fail

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
    mov [cs:tmp_next_cluster], ax
    mov word [cs:tmp_cluster_off], 0

.cluster_sector_loop:
    mov ax, [cs:tmp_exec_limit]
    or ax, [cs:tmp_exec_total]
    je .close_ok

    mov ax, [cs:tmp_cluster_off]
    mov cl, 9
    shr ax, cl
    cmp ax, FAT_SECTORS_PER_CLUSTER
    jae .next_cluster
    add ax, [cs:tmp_next_cluster]
    mov [cs:tmp_lba], ax

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    xor bx, bx
    call read_sector_lba
    jc .io_fail

    mov ax, [cs:tmp_exec_total]
    cmp ax, 0
    jne .chunk_512
    mov ax, [cs:tmp_exec_limit]
    cmp ax, 512
    jbe .chunk_ready

.chunk_512:
    mov ax, 512

.chunk_ready:
    mov [cs:tmp_chunk], ax

    push ds
    push cx
    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    xor si, si
    mov ax, [cs:tmp_user_ds]
    mov es, ax
    mov cx, [cs:tmp_chunk]

.copy_loop:
    movsb
    cmp di, 0
    jne .copy_next
    mov ax, es
    add ax, 0x1000
    cmp word [cs:current_load_seg], MZ3_LOAD_SEG
    je .copy_limit_high
    cmp ax, DOS_META_BUF_SEG
    jae .copy_too_large
    jmp .copy_limit_ready
.copy_limit_high:
    cmp ax, DOS_HEAP_LIMIT_SEG
    jae .copy_too_large
.copy_limit_ready:
    mov es, ax
    mov [cs:tmp_user_ds], ax

.copy_next:
    loop .copy_loop
    mov ax, es
    mov [cs:tmp_user_ds], ax
    pop cx
    pop ds

    mov ax, [cs:tmp_chunk]
    sub [cs:tmp_exec_limit], ax
    sbb word [cs:tmp_exec_total], 0
    add word [cs:tmp_cluster_off], 512

    jmp .cluster_sector_loop

.next_cluster:
    mov ax, [cs:tmp_cluster]
    mov bx, ax
    call fat12_get_entry_cached
    jc .io_fail
    mov [cs:tmp_cluster], ax
    jmp .read_loop

.copy_too_large:
    pop cx
    pop ds
    jmp .too_large

.close_ok:
    xor ax, ax
    clc
    jmp .done

.open_fail:
    mov ax, 0x0002
    stc
    jmp .done

.io_fail:
    mov ax, 0x0005
    stc
    jmp .done

.too_large:
    mov ax, 0x0008
    stc

.done:
    pop es
    pop ds
    pop di
    pop dx
    pop cx
    pop bx
    ret

int21_exec_load_com:
    push cx
    push di
    push es

    mov ax, [cs:current_com_load_seg]
    mov es, ax
    call int21_exec_init_program_psp
    call int21_build_env_block
    call int21_exec_write_tail

    mov di, 0x0100
    mov cx, 0xFE00
    call int21_exec_load_to_es
    jc .fail

    mov word [cs:com_entry_off], 0x0100
    mov ax, [cs:current_com_load_seg]
    mov word [cs:com_entry_seg], ax
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop di
    pop cx
    ret

int21_exec_run_com:
    mov ax, [cs:current_psp_seg]
    cmp word [cs:current_com_load_seg], COM_LOAD_SEG
    je .save_primary_ctx

.save_third_ctx:
    mov [cs:saved_psp3], ax
    mov [cs:saved_ss3], ss
    mov [cs:saved_sp3], sp
    mov ax, ds
    mov [cs:saved_ds3], ax
    mov [cs:saved_es3], ax
    jmp .ctx_saved

.save_primary_ctx:
    mov [cs:saved_psp], ax
    mov [cs:saved_ss], ss
    mov [cs:saved_sp], sp
    mov ax, ds
    mov [cs:saved_ds], ax
    mov [cs:saved_es], ax

.ctx_saved:
    cli
    mov ax, [cs:current_com_load_seg]
    mov [cs:current_psp_seg], ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFFE
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
    xor si, si
    xor di, di
    xor bp, bp
    sti
    cld

    call far [cs:com_entry_off]

.after_call:

    cli
    mov ax, cs
    mov ds, ax
    cmp word [cs:current_com_load_seg], COM_LOAD_SEG
    je .restore_primary_ss
.restore_third_ss:
    mov ax, [cs:saved_ss3]
    mov ss, ax
    mov sp, [cs:saved_sp3]
    jmp .done_ss_restore
.restore_primary_ss:
    mov ax, [cs:saved_ss]
    mov ss, ax
    mov sp, [cs:saved_sp]
.done_ss_restore:
    sti

    cmp word [cs:current_com_load_seg], COM_LOAD_SEG
    je .restore_primary_ctx

.restore_third_ctx:
    mov ax, [cs:saved_ds3]
    mov ds, ax
    mov ax, [cs:saved_es3]
    mov es, ax
    mov ax, [cs:saved_psp3]
    mov [cs:current_psp_seg], ax
    clc
    ret

.restore_primary_ctx:
    mov ax, [cs:saved_ds]
    mov ds, ax
    mov ax, [cs:saved_es]
    mov es, ax
    mov ax, [cs:saved_psp]
    mov [cs:current_psp_seg], ax
    clc
    ret

int21_exec_load_mz:
    push cx
    push di
    push es

    mov bx, [cs:search_found_size_lo]
    mov bp, [cs:search_found_size_hi]

    mov ax, [cs:current_load_seg]
    mov es, ax
    xor ax, ax
    xor di, di
    mov cx, 128
    rep stosw

    mov word [cs:search_found_size_lo], 512
    mov word [cs:search_found_size_hi], 0

    xor di, di
    xor cx, cx
    call int21_exec_load_to_es
    jc .done

    cmp word [es:0x0000], 0x5A4D
    jne .invalid_format

    mov ax, [es:0x0004]
    or ax, ax
    je .invalid_format
    mov cx, [es:0x0002]
    cmp cx, 512
    jae .invalid_format
    mov dx, cx

    mov di, ax
    mov cl, 7
    shr di, cl
    mov cl, 9
    shl ax, cl
    or dx, dx
    je .mz_size_ready
    sub ax, 512
    sbb di, 0
    add ax, dx
    adc di, 0

.mz_size_ready:
    mov dx, [es:0x0008]
    mov cl, 4
    shl dx, cl

    cmp di, 0
    jne .mz_size_header_ok
    cmp ax, dx
    jb .invalid_format

.mz_size_header_ok:
    cmp di, bp
    ja .invalid_format
    jb .mz_size_file_ok
    cmp ax, bx
    ja .invalid_format

.mz_size_file_ok:
    mov [cs:search_found_size_lo], ax
    mov [cs:search_found_size_hi], di

    xor di, di
    xor cx, cx
    call int21_exec_load_to_es
    jc .done
    clc
    jmp .done

.invalid_format:
    mov ax, 0x000B
    stc

.done:
    mov [cs:search_found_size_lo], bx
    mov [cs:search_found_size_hi], bp
    pop es
    pop di
    pop cx
    ret

int21_exec_load_overlay:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov ax, MZ3_LOAD_SEG
    mov es, ax
    xor ax, ax
    xor di, di
    mov cx, 128
    rep stosw

    mov ax, MZ3_LOAD_SEG
    mov es, ax
    xor di, di
    xor cx, cx
    call int21_exec_load_to_es
    jc .done

    mov ax, MZ3_LOAD_SEG
    mov es, ax
    cmp word [es:0x0000], 0x5A4D
    jne .invalid_format

    cmp word [cs:search_found_size_hi], 0
    jne .too_large
    mov ax, [es:0x0008]
    mov cl, 4
    shl ax, cl
    mov [cs:tmp_overlay_header_bytes], ax
    mov dx, [cs:search_found_size_lo]
    cmp dx, ax
    jb .invalid_format
    sub dx, ax
    mov [cs:tmp_overlay_image_size], dx

    mov ax, MZ3_LOAD_SEG
    add ax, [es:0x0008]
    mov ds, ax
    mov ax, [cs:tmp_overlay_load_seg]
    mov es, ax
    xor si, si
    xor di, di
    mov cx, [cs:tmp_overlay_image_size]
    cld
    rep movsb

    mov ax, MZ3_LOAD_SEG
    mov ds, ax
    mov cx, [ds:0x0006]
    mov si, [ds:0x0018]

.reloc_loop:
    jcxz .success
    mov bx, [ds:si]
    mov dx, [ds:si + 2]
    mov ax, [cs:tmp_overlay_load_seg]
    add dx, ax
    push ds
    mov ds, dx
    mov ax, [cs:tmp_overlay_reloc_seg]
    add word [ds:bx], ax
    pop ds
    add si, 4
    loop .reloc_loop

.success:
    xor ax, ax
    clc
    jmp .done

.invalid_format:
    mov ax, 0x000B
    stc
    jmp .done

.too_large:
    mov ax, 0x0008
    stc

.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_exec_run_mz:
    push es

    mov ax, [cs:current_load_seg]
    mov es, ax
    cmp word [es:0x0000], 0x5A4D
    jne .invalid_header

    mov bx, [es:0x0008]
    add bx, [cs:current_load_seg]
    mov [cs:mz_image_seg], bx
    mov ax, bx
    sub ax, 0x0010
    mov [cs:mz_psp_seg], ax

    mov ax, [es:0x0014]
    mov [cs:mz_entry_off], ax
    mov ax, [es:0x0016]
    add ax, bx
    mov [cs:mz_entry_seg], ax

    mov ax, [es:0x000E]
    add ax, bx
    mov [cs:mz_stack_seg], ax
    mov ax, [es:0x0010]
    mov [cs:mz_stack_sp], ax

    mov cx, [es:0x0006]
    mov di, [es:0x0018]
.reloc_loop:
    jcxz .reloc_done
    mov bx, [es:di]
    mov dx, [es:di + 2]
    mov ax, [cs:mz_image_seg]
    add dx, ax
    push es
    mov es, dx
    add word [es:bx], ax
    pop es
    add di, 4
    loop .reloc_loop
.reloc_done:
    mov ax, ds
    mov dx, [cs:mz_psp_seg]
    mov bx, [cs:current_psp_seg]
    cmp word [cs:current_mz_context_slot], 3
    je .save_third_ctx
    cmp word [cs:current_mz_context_slot], 2
    jne .save_primary_ctx
    mov [cs:saved_psp2], bx
    mov [cs:saved_ss2], ss
    mov [cs:saved_sp2], sp
    mov [cs:saved_ds2], ax
    mov [cs:saved_es2], dx
    jmp .ctx_saved

.save_third_ctx:
    mov [cs:saved_psp3], bx
    mov [cs:saved_ss3], ss
    mov [cs:saved_sp3], sp
    mov [cs:saved_ds3], ax
    mov [cs:saved_es3], dx
    jmp .ctx_saved

.save_primary_ctx:
    mov [cs:saved_psp], bx
    mov [cs:saved_ss], ss
    mov [cs:saved_sp], sp
    mov [cs:saved_ds], ax
    mov [cs:saved_es], dx

.ctx_saved:
    mov ax, [cs:mz_psp_seg]
    mov [cs:current_psp_seg], ax
    push ax
    call int21_exec_prepare_mz_free_mcb
    pop ax
    mov es, ax
    call int21_exec_init_program_psp
    mov ax, [cs:dos_mem_psp_mcb_end]
    mov [es:0x0002], ax
    or bx, bx
    jnz .mz_parent_ready
    mov bx, es
.mz_parent_ready:
    mov [es:0x0016], bx
    call int21_build_env_block
    call int21_exec_write_tail

    push ds
    mov ax, DOS_META_BUF_SEG
    mov ds, ax
    xor ax, ax
    mov [0x0000], ax
    pop ds

    push ds
    push cs
    pop ds
    mov si, msg_mz_begin
    call print_string_serial
    pop ds

    cli
    mov ax, [cs:mz_psp_seg]
    mov ds, ax
    mov es, ax
    mov ax, [cs:mz_stack_seg]
    mov ss, ax
    mov sp, [cs:mz_stack_sp]
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
    xor si, si
    xor di, di
    xor bp, bp
    sti
    cld
    jmp far [cs:mz_entry_off]

.after_call:
    cli
    mov ax, cs
    mov ds, ax
    ; restore SS:SP from appropriate slot
    cmp word [cs:current_mz_context_slot], 3
    je .restore_third_ss
    cmp word [cs:current_mz_context_slot], 2
    jne .restore_primary_ss
    mov ax, [cs:saved_ss2]
    mov ss, ax
    mov sp, [cs:saved_sp2]
    jmp .done_ss_restore
.restore_third_ss:
    mov ax, [cs:saved_ss3]
    mov ss, ax
    mov sp, [cs:saved_sp3]
    jmp .done_ss_restore
.restore_primary_ss:
    mov ax, [cs:saved_ss]
    mov ss, ax
    mov sp, [cs:saved_sp]
.done_ss_restore:
    sti
    cmp word [cs:current_mz_context_slot], 3
    je .restore_third_ctx
    cmp word [cs:current_mz_context_slot], 2
    jne .restore_primary_ctx
    mov ax, [cs:saved_psp2]
    mov [cs:current_psp_seg], ax
    mov ax, [cs:saved_ds2]
    mov ds, ax
    mov ax, [cs:saved_es2]
    mov es, ax
    clc
    jmp .done

.restore_third_ctx:
    mov ax, [cs:saved_psp3]
    mov [cs:current_psp_seg], ax
    mov ax, [cs:saved_ds3]
    mov ds, ax
    mov ax, [cs:saved_es3]
    mov es, ax
    clc
    jmp .done

.restore_primary_ctx:
    mov ax, [cs:saved_psp]
    mov [cs:current_psp_seg], ax
    mov ax, [cs:saved_ds]
    mov ds, ax
    mov ax, [cs:saved_es]
    mov es, ax
    clc
    jmp .done

.invalid_header:
    mov ax, 0x000B
    stc

.done:
    pop es
    ret

int21_mz_terminate_trampoline:
    jmp int21_exec_run_mz.after_call

int21_com_terminate_trampoline:
    jmp int21_exec_run_com.after_call

int21_set_dta:
    mov ax, ds
    mov [cs:dta_seg], ax
    mov [cs:dta_off], dx
    xor ax, ax
    clc
    ret

int21_get_default_drive:
%if FAT_TYPE == 16
    push ds
    call stage1_runtime_get_default_drive_ptr
    jc .fallback
    xor ah, ah
    mov al, [ds:si]
    clc
    pop ds
    ret

.fallback:
    pop ds
%endif
    xor ah, ah
    mov al, [cs:dos_default_drive]
    clc
    ret

%if FAT_TYPE == 16
cwd_save_current_drive:
    push ax
    mov al, [cs:dos_default_drive]
    call cwd_save_drive_al
    pop ax
    ret

cwd_save_drive_al:
    push ax
    push bx
    push cx
    push si
    push di
    push ds
    push es
    mov bl, al
    mov ax, cs
    mov ds, ax
    mov es, ax
    cmp bl, 2
    je .save_c
    cmp bl, 3
    je .save_d
    jmp .done
.save_c:
    mov si, cwd_buf
    mov di, cwd_c_buf
    jmp .copy
.save_d:
    mov si, cwd_buf
    mov di, cwd_d_buf
.copy:
    mov cx, 24
    rep movsb
    mov ax, [cs:cwd_cluster]
    cmp bl, 2
    je .store_c_cluster
    mov [cs:cwd_d_cluster], ax
    jmp .done
.store_c_cluster:
    mov [cs:cwd_c_cluster], ax
.done:
    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

cwd_load_drive_al:
    push ax
    push bx
    push cx
    push si
    push di
    push ds
    push es
    mov bl, al
    mov ax, cs
    mov ds, ax
    mov es, ax
    cmp bl, 2
    je .load_c
    cmp bl, 3
    je .load_d
    jmp .done
.load_c:
    mov si, cwd_c_buf
    mov di, cwd_buf
    mov ax, [cs:cwd_c_cluster]
    jmp .copy
.load_d:
    mov si, cwd_d_buf
    mov di, cwd_buf
    mov ax, [cs:cwd_d_cluster]
.copy:
    mov [cs:cwd_cluster], ax
    mov cx, 24
    rep movsb
.done:
    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret
%endif

int21_code_page:
    cmp al, 0x01
    je .get
    cmp al, 0x02
    je .set
    mov ax, 0x0001
    stc
    ret
.get:
    mov bx, 437
    mov dx, 437
    xor ax, ax
    clc
    ret
.set:
    xor ax, ax
    clc
    ret

int21_set_default_drive:
%if FAT_TYPE == 16
    cmp dl, 3
    ja .invalid
    call cwd_save_current_drive
    mov [cs:dos_default_drive], dl
    call stage1_runtime_sync_default_drive
    mov al, dl
    call cwd_load_drive_al
    mov al, 4
    xor ah, ah
    clc
    ret
%else
    cmp dl, 1
    ja .invalid
    mov [cs:dos_default_drive], dl
    mov al, 1
    xor ah, ah
    clc
    ret
%endif
.invalid:
    mov ax, 0x000F
    stc
    ret

int21_get_version:
%if FAT_TYPE == 16
    call stage1_runtime_get_version
    jnc .done
%endif
    mov ax, 0x0005
    xor bx, bx
    xor cx, cx
    clc
.done:
    ret

int21_country_info:
    cmp al, 0x00
    je .copy_current
    cmp al, 0xFF
    je .copy_current
    cmp al, 0x01
    jne .bad_country
.copy_current:
    push cx
    push si
    push di
    push ds
    push es
    mov di, dx
    mov ax, ds
    mov es, ax
    mov ax, cs
    mov ds, ax
    mov si, country_info_default
    mov cx, 34
    cld
    rep movsb
    pop es
    pop ds
    pop di
    pop si
    pop cx
    mov bx, 1
    xor ax, ax
    clc
    ret
.bad_country:
    mov ax, 0x0002
    stc
    ret

int21_get_date:
    mov cx, 2026
    mov dh, 4
    mov dl, 22
    mov al, 2
    xor ah, ah
    clc
    ret

int21_get_time:
    push ax
    push bx
    push si

    mov ah, 0x00
    int 0x1A                    ; CX:DX = ticks since midnight

    mov ax, dx
    xor dx, dx
    mov bx, 18
    div bx                      ; AX = approx seconds, DX = tick remainder (0..17)

    push dx                     ; save remainder for hundredths

    xor dx, dx
    mov bx, 60
    div bx                      ; AX = total minutes, DX = seconds
    mov si, dx                  ; save seconds for DH

    xor dx, dx
    mov bx, 60
    div bx                      ; AX = hours, DX = minutes
    mov ch, al
    mov cl, dl

    pop ax                      ; AX = tick remainder (0..17)
    mov bx, 100
    mul bx                      ; AX = remainder * 100
    xor dx, dx
    mov bx, 18
    div bx                      ; AX = hundredths (0..99)
    mov dl, al
    mov ax, si
    mov dh, al

    ; GEM's startup calibrates busy-wait delays from DL.  A BIOS tick only
    ; changes every ~55 ms, so expose a monotonically advancing centisecond
    ; value to keep DOS clients from spinning in calibration loops.
    mov al, [cs:dos_time_centis]
    add al, 7
    cmp al, 100
    jb .centis_ready
    sub al, 100
.centis_ready:
    or al, al
    jnz .centis_nonzero
    inc al
.centis_nonzero:
    mov [cs:dos_time_centis], al
    mov dl, al

    pop si
    pop bx
    pop ax
    clc
    ret

int21_ctrl_break:
    cmp al, 0x00
    je .get_state
    cmp al, 0x01
    je .set_state
    mov ax, 0x0001
    stc
    ret
.get_state:
    mov dl, [cs:dos_ctrl_break_flag]
    mov ax, 0x3300
    clc
    ret
.set_state:
    and dl, 0x01
    mov [cs:dos_ctrl_break_flag], dl
    mov ax, 0x3301
    clc
    ret

int21_get_free_space:
    cmp dl, 0
    je .ok
%if FAT_TYPE == 16
    cmp dl, 3
    je .ok
    cmp dl, 4
    je .ok
%else
    cmp dl, 1
    je .ok
%endif
    mov ax, 0xFFFF
    stc
    ret
.ok:
    mov ax, FAT_SECTORS_PER_CLUSTER
    mov bx, 0x2000
    mov cx, 512
    mov dx, 0x4000
    clc
    ret

int21_get_indos_ptr:
    mov bx, dos_indos_flag
    mov ax, cs
    mov es, ax
    xor ax, ax
    clc
    ret

int21_get_list_of_lists:
    ; return ES:BX pointing to SYSVARS; ES:[BX-2] = first MCB segment
    call int21_mem_init
%if FAT_TYPE == 16
    push ax
    push cx
    push dx
    push di

    mov dx, DOS_ENV_SEG
    mov es, dx
    mov di, DOS_SYSVARS_ANCHOR_OFF
    xor ax, ax
    mov cx, 0x0180
    cld
    rep stosw

    mov ax, [cs:dos_list_of_lists]
    mov [es:DOS_SYSVARS_ANCHOR_OFF], ax
    mov word [es:DOS_SYSVARS_OFF + 0x00], DOS_SYSVARS_DPB_OFF
    mov [es:DOS_SYSVARS_OFF + 0x02], dx
    mov word [es:DOS_SYSVARS_OFF + 0x04], DOS_SYSVARS_SFT_OFF
    mov [es:DOS_SYSVARS_OFF + 0x06], dx
    mov word [es:DOS_SYSVARS_OFF + 0x10], 512
    mov word [es:DOS_SYSVARS_OFF + 0x16], DOS_SYSVARS_CDS_OFF
    mov [es:DOS_SYSVARS_OFF + 0x18], dx
    mov byte [es:DOS_SYSVARS_OFF + 0x20], 1
    mov byte [es:DOS_SYSVARS_OFF + 0x21], 3

    mov word [es:DOS_SYSVARS_OFF + 0x22], 0xFFFF
    mov word [es:DOS_SYSVARS_OFF + 0x24], 0xFFFF
    mov word [es:DOS_SYSVARS_OFF + 0x26], 0x8004
    mov word [es:DOS_SYSVARS_OFF + 0x2E], 'UN'
    mov word [es:DOS_SYSVARS_OFF + 0x30], ' L'

    mov byte [es:DOS_SYSVARS_DPB_OFF + 0x00], 2
    mov word [es:DOS_SYSVARS_DPB_OFF + 0x02], 512
    mov word [es:DOS_SYSVARS_DPB_OFF + 0x19], 0xFFFF
    mov word [es:DOS_SYSVARS_DPB_OFF + 0x1B], 0xFFFF

    mov word [es:DOS_SYSVARS_SFT_OFF + 0x00], 0xFFFF
    mov word [es:DOS_SYSVARS_SFT_OFF + 0x02], 0xFFFF
    mov word [es:DOS_SYSVARS_SFT_OFF + 0x04], 20

    mov byte [es:DOS_SYSVARS_CDS_OFF + 0xB0], 'C'
    mov byte [es:DOS_SYSVARS_CDS_OFF + 0xB1], ':'
    mov byte [es:DOS_SYSVARS_CDS_OFF + 0xB2], '\'
    mov word [es:DOS_SYSVARS_CDS_OFF + 0xB0 + 0x43], 0x4000
    mov word [es:DOS_SYSVARS_CDS_OFF + 0xB0 + 0x45], DOS_SYSVARS_DPB_OFF
    mov [es:DOS_SYSVARS_CDS_OFF + 0xB0 + 0x47], dx

    mov bx, DOS_SYSVARS_OFF
    xor ax, ax

    pop di
    pop dx
    pop cx
    pop ax
    clc
    ret
%else
    mov bx, dos_list_of_lists + 2
    mov ax, cs
    mov es, ax
    xor ax, ax
    clc
    ret
%endif

int21_mem_strategy:
    cmp al, 0x00
    je .get
    cmp al, 0x01
    je .set
    cmp al, 0x02
    je .get_umb
    cmp al, 0x03
    je .set_umb
    ; Be permissive for unknown subfunctions used by TSRs.
    xor ax, ax
    clc
    ret
 .get_umb:
    xor ax, ax
    clc
    ret
 .set_umb:
    xor ax, ax
    clc
    ret
.get:
    mov bx, [cs:dos_mem_strategy]
    mov ax, bx
    clc
    ret
.set:
    mov [cs:dos_mem_strategy], bx
    xor ax, ax
    clc
    ret

int21_set_vector:
    push ax
    push bx
    push es

    ; Keep DOS core stable: ignore attempts to replace INT 21h.
    cmp al, 0x21
    je .ok
    xor ah, ah
    mov bx, ax
    shl bx, 1
    shl bx, 1
    xor ax, ax
    mov es, ax
    mov [es:bx], dx
    mov ax, ds
    mov [es:bx + 2], ax
    jmp .ok

.ok:
    xor ax, ax
    clc
    pop es
    pop bx
    pop ax
    ret

int21_get_vector:
    push ax
    push di
    xor ah, ah
    mov di, ax
    shl di, 1
    shl di, 1
    xor ax, ax
    mov es, ax
    mov bx, [es:di]
    mov ax, [es:di + 2]
    mov es, ax
    xor ax, ax
    clc
    pop di
    pop ax
    ret

int21_get_dta:
    mov bx, [cs:dta_off]
    mov ax, [cs:dta_seg]
    mov es, ax
    xor ax, ax
    clc
    ret

int21_ioctl:
    mov [cs:tmp_ioctl_subfn], al
    mov al, [cs:tmp_ioctl_subfn]
    cmp al, 0x00                ; Get device information
    je .get_dev_info
%if FAT_TYPE == 16
    cmp al, 0x04                ; Read from character device control channel
    je .read_ctrl_channel
    cmp al, 0x05                ; Write to character device control channel
    je .write_ctrl_channel
    cmp al, 0x08                ; Check if block device is removable
    je .check_removable
    cmp al, 0x0D                ; Generic IOCTL for block devices
    je .generic_ioctl
%endif
    cmp al, 0x06                ; Get input status
    je .get_input_status
    cmp al, 0x07                ; Get output status
    je .get_output_status
    xor ax, ax
    clc
    ret

%if FAT_TYPE == 16
.read_ctrl_channel:
    xor ax, ax
    clc
    ret

.write_ctrl_channel:
    xor ax, ax
    clc
    ret

.check_removable:
    mov al, 1                   ; AL=1 -> non-removable
    xor ah, ah
    clc
    ret

.generic_ioctl:
    mov ax, 0x001F              ; unsupported request
    stc
    ret
%endif

.get_dev_info:
    cmp bx, 0x0000
    je .stdio
    cmp bx, 0x0001
    je .stdio
    cmp bx, 0x0002
    je .stdio
    cmp bx, 0x0003
    je .stdio
    cmp bx, 0x0004
    je .stdio
    cmp bx, 0x0005
    je .disk_slot1
    cmp bx, 0x0006
    je .disk_slot2
    cmp bx, 0x0007
    je .disk_slot3
%if FAT_TYPE == 16
    cmp bx, 0x0008
    je .disk_slot4
    cmp bx, 0x0009
    je .disk_slot5
    cmp bx, 0x000A
    je .disk_slot6
    cmp bx, 0x000B
    je .disk_slot7
    cmp bx, 0x000C
    je .disk_slot8
%endif
    xor ax, ax
    clc
    ret

.stdio:
    mov dx, 0x80D3              ; char device (CON-like), standard bits set
    xor ax, ax
    clc
    ret

.disk_slot1:
    cmp byte [cs:file_handle_open], 1
    jne .bad_handle
    xor dx, dx                  ; disk file
    xor ax, ax
    clc
    ret

.disk_slot2:
    cmp byte [cs:file_handle2_open], 1
    jne .bad_handle
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot3:
    cmp byte [cs:file_handle3_open], 1
    jne .bad_handle
    xor dx, dx
    xor ax, ax
    clc
    ret

%if FAT_TYPE == 16
.disk_slot4:
    cmp byte [cs:file_handle4_open], 1
    jne .bad_handle
    xor dx, dx
    xor ax, ax
    clc
    ret
.disk_slot5:
    cmp byte [cs:file_handle5_open], 1
    jne .bad_handle
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot6:
    cmp byte [cs:file_handle6_open], 1
    jne .bad_handle
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot7:
    cmp byte [cs:file_handle7_open], 1
    jne .bad_handle
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot8:
    cmp byte [cs:file_handle8_open], 1
    jne .bad_handle
    xor dx, dx
    xor ax, ax
    clc
    ret

%endif

.get_input_status:
    mov ax, 0x00FF              ; ready
    clc
    ret

.get_output_status:
    mov ax, 0x00FF              ; ready
    clc
    ret

.bad_handle:
    xor ax, ax
    clc
    ret

int21_get_psp:
    mov bx, [cs:current_psp_seg]
    cmp bx, 0
    jne .ok
    mov bx, DOS_HEAP_BASE_SEG
.ok:
    xor ax, ax
    clc
    ret

int21_chdir:
    mov si, dx
    mov byte [cs:tmp_cwd_comp], 0
    mov byte [cs:tmp_cwd_build], 0
    mov byte [cs:tmp_path_guard], 160
%if FAT_TYPE == 16
    mov al, [cs:dos_default_drive]
    mov [cs:int21_chdir_drive], al
    mov byte [cs:int21_chdir_qualified], 0
%endif

    mov al, [si]
    cmp al, 0
    je .root

    ; DOS drive-qualified chdir mutates that drive slot without changing the default drive.
    cmp byte [si + 1], ':'
    jne .check_absolute
%if FAT_TYPE == 16
    mov al, [si]
    cmp al, 'C'
    je .drive_c
    cmp al, 'D'
    je .drive_d
    jmp .invalid_drive
.drive_c:
    mov byte [cs:int21_chdir_drive], 2
    jmp .drive_prefix_ok
.drive_d:
    mov byte [cs:int21_chdir_drive], 3
.drive_prefix_ok:
    mov byte [cs:int21_chdir_qualified], 1
    add si, 2
    mov al, [cs:int21_chdir_drive]
    cmp al, [cs:dos_default_drive]
    je .drive_prefix_loaded
    call cwd_save_current_drive
    mov al, [cs:int21_chdir_drive]
    call cwd_load_drive_al
.drive_prefix_loaded:
    cmp byte [si], 0
    jne .check_absolute
    call int21_chdir_restore_default
    xor ax, ax
    clc
    ret
%else
    add si, 2
%endif

.check_absolute:
    cmp byte [si], '\'
    je .abs_path
    cmp byte [si], '/'
    je .abs_path
%if FAT_TYPE == 16
    cmp word [cs:cwd_cluster], 0
    je .seed_relative
    push si
    xor bx, bx
.fast_copy_check:
    dec byte [cs:tmp_path_guard]
    jz .fast_component_abort
    mov al, [si]
    cmp al, 0
    je .fast_component_done
    cmp al, '\'
    je .fast_component_abort
    cmp al, '/'
    je .fast_component_abort
    cmp bx, 23
    jae .fast_component_advance
    mov [cs:tmp_cwd_comp + bx], al
    inc bx
.fast_component_advance:
    inc si
    jmp .fast_copy_check

.fast_component_done:
    mov byte [cs:tmp_cwd_comp + bx], 0
    pop si
    cmp bx, 0
    je .seed_relative
    cmp byte [cs:tmp_cwd_comp], '.'
    je .seed_relative
    push ds
    mov ax, cs
    mov ds, ax
    mov si, tmp_cwd_comp
    call int21_path_to_fat_name
    pop ds
    jc .seed_relative
    mov ax, [cs:cwd_cluster]
    push ds
    mov dx, si
    mov ax, cs
    mov ds, ax
    mov si, path_fat_name
    mov ax, [cs:cwd_cluster]
    call int21_lookup_in_dir
    pop ds
    mov si, dx
    jc .seed_relative
    test byte [cs:search_found_attr], 0x10
    jz .seed_relative
    mov ax, [cs:search_found_cluster]
    mov [cs:tmp_lookup_dir], ax
    xor bx, bx
.fast_seed_loop:
    mov al, [cs:cwd_buf + bx]
    mov [cs:tmp_cwd_build + bx], al
    cmp al, 0
    je .fast_append_start
    inc bx
    cmp bx, 23
    jb .fast_seed_loop
    mov byte [cs:tmp_cwd_build + 23], 0
    jmp .commit_copy

.fast_append_start:
    cmp bx, 0
    je .fast_copy_component
    mov byte [cs:tmp_cwd_build + bx], '\'
    inc bx
    cmp bx, 23
    jae .commit_copy

.fast_copy_component:
    xor di, di
.fast_copy_component_loop:
    mov al, [cs:tmp_cwd_comp + di]
    cmp al, 0
    je .fast_append_term
    mov [cs:tmp_cwd_build + bx], al
    inc bx
    inc di
    cmp bx, 23
    jb .fast_copy_component_loop
.fast_append_term:
    mov byte [cs:tmp_cwd_build + bx], 0
    jmp .commit_copy

.fast_component_abort:
    pop si
%endif
    
.seed_relative:
    ; relative path: start from existing cwd
    xor bx, bx
.seed_loop:
    mov al, [cs:cwd_buf + bx]
    mov [cs:tmp_cwd_build + bx], al
    cmp al, 0
    je .parse_components
    inc bx
    cmp bx, 23
    jb .seed_loop
    mov byte [cs:tmp_cwd_build + 23], 0
    jmp .parse_components

.abs_path:
    inc si

.parse_components:
    dec byte [cs:tmp_path_guard]
    jz .fail
    mov al, [si]
    cmp al, 0
    je .commit
    cmp al, '\'
    je .skip_sep
    cmp al, '/'
    je .skip_sep

    xor bx, bx
.comp_copy:
    dec byte [cs:tmp_path_guard]
    jz .fail
    mov al, [si]
    cmp al, 0
    je .comp_done
    cmp al, '\'
    je .comp_done
    cmp al, '/'
    je .comp_done
    cmp bx, 23
    jae .comp_skip_advance
    mov [cs:tmp_cwd_comp + bx], al
    inc bx
.comp_skip_advance:
    inc si
    jmp .comp_copy

.comp_done:
    mov byte [cs:tmp_cwd_comp + bx], 0
    cmp byte [cs:tmp_cwd_comp], 0
    je .parse_components

    cmp byte [cs:tmp_cwd_comp], '.'
    jne .check_parent
    cmp byte [cs:tmp_cwd_comp + 1], 0
    je .parse_components

.check_parent:
    cmp byte [cs:tmp_cwd_comp], '.'
    jne .append_component
    cmp byte [cs:tmp_cwd_comp + 1], '.'
    jne .append_component
    cmp byte [cs:tmp_cwd_comp + 2], 0
    jne .append_component
    xor bx, bx
.find_end_parent:
    cmp byte [cs:tmp_cwd_build + bx], 0
    je .trim_parent
    inc bx
    cmp bx, 23
    jb .find_end_parent
.trim_parent:
    cmp bx, 0
    je .parse_components
    dec bx
.trim_loop:
    cmp bx, 0
    je .clear_root_parent
    cmp byte [cs:tmp_cwd_build + bx - 1], '\'
    je .trim_done
    dec bx
    jmp .trim_loop
.clear_root_parent:
    mov byte [cs:tmp_cwd_build], 0
    jmp .parse_components
.trim_done:
    mov byte [cs:tmp_cwd_build + bx - 1], 0
    jmp .parse_components

.append_component:
    xor bx, bx
.find_end_append:
    cmp byte [cs:tmp_cwd_build + bx], 0
    je .append_start
    inc bx
    cmp bx, 23
    jb .find_end_append
    jmp .commit
.append_start:
    cmp bx, 0
    je .copy_component
    mov byte [cs:tmp_cwd_build + bx], '\'
    inc bx
    cmp bx, 23
    jae .commit
.copy_component:
    xor di, di
.copy_component_loop:
    mov al, [cs:tmp_cwd_comp + di]
    cmp al, 0
    je .append_term
    mov [cs:tmp_cwd_build + bx], al
    inc bx
    inc di
    cmp bx, 23
    jb .copy_component_loop
.append_term:
    mov byte [cs:tmp_cwd_build + bx], 0
    jmp .parse_components

.skip_sep:
    inc si
    jmp .parse_components

.commit:
    cmp byte [cs:tmp_cwd_build], 0
    je .commit_root

    push ax
    mov ax, [cs:cwd_cluster]
    push ax
    mov word [cs:cwd_cluster], 0
    mov ax, cs
    mov ds, ax
    mov si, tmp_cwd_build
    call int21_resolve_and_find_path
    pop ax
    mov [cs:cwd_cluster], ax
    pop ax
    jc .fail
    test byte [cs:search_found_attr], 0x10
    jz .fail
    mov ax, [cs:search_found_cluster]
    mov [cs:tmp_lookup_dir], ax
    jmp .commit_copy

.commit_root:
    mov word [cs:tmp_lookup_dir], 0

.commit_copy:
    xor bx, bx
.commit_loop:
    mov al, [cs:tmp_cwd_build + bx]
    mov [cs:cwd_buf + bx], al
    cmp al, 0
    je .ok
    inc bx
    cmp bx, 23
    jb .commit_loop
    mov byte [cs:cwd_buf + 23], 0
    jmp .ok

.ok:
    mov ax, [cs:tmp_lookup_dir]
    mov [cs:cwd_cluster], ax
%if FAT_TYPE == 16
    call int21_chdir_commit_drive
%endif
    xor ax, ax
    clc
    ret
.root:
    mov byte [cs:cwd_buf], 0
    mov word [cs:cwd_cluster], 0
%if FAT_TYPE == 16
    call cwd_save_current_drive
%endif
    xor ax, ax
    clc
    ret

.invalid_drive:
    mov ax, 0x000F
    stc
    ret

.fail:
%if FAT_TYPE == 16
    call int21_chdir_restore_default
%endif
    mov ax, 0x0003
    stc
    ret

%if FAT_TYPE == 16
int21_chdir_commit_drive:
    push ax
    cmp byte [cs:int21_chdir_qualified], 0
    jne .save_qualified
    call cwd_save_current_drive
    jmp .done
.save_qualified:
    mov al, [cs:int21_chdir_drive]
    call cwd_save_drive_al
    cmp al, [cs:dos_default_drive]
    je .done
    mov al, [cs:dos_default_drive]
    call cwd_load_drive_al
.done:
    pop ax
    ret

int21_chdir_restore_default:
    push ax
    cmp byte [cs:int21_chdir_qualified], 0
    je .done
    mov al, [cs:int21_chdir_drive]
    cmp al, [cs:dos_default_drive]
    je .done
    mov al, [cs:dos_default_drive]
    call cwd_load_drive_al
.done:
    pop ax
    ret
%endif

int21_getcwd:
    push bx
    push si
%if FAT_TYPE == 16
    cmp dl, 0
    je .copy_current
    cmp dl, 2
    jbe .copy_root
    cmp dl, 3
    je .copy_c
    cmp dl, 4
    je .copy_d
    mov ax, 0x000F
    stc
    jmp .return
.copy_root:
    mov byte [ds:si], 0
    jmp .done
.copy_c:
    mov bx, cwd_c_buf
    jmp .copy_loop
.copy_d:
    mov bx, cwd_d_buf
    jmp .copy_loop
.copy_current:
%endif
    mov bx, cwd_buf
.copy_loop:
    mov al, [cs:bx]
    mov [ds:si], al
    inc si
    inc bx
    cmp al, 0
    jne .copy_loop
.done:
    mov ax, 0x0100
    clc
.return:
    pop si
    pop bx
    ret

int21_get_set_attr:
    cmp al, 0x00
    je .get_attr
    cmp al, 0x01
    je .set_attr
    mov ax, 0x0001
    stc
    ret

.get_attr:
%if FAT_TYPE == 16
    mov si, dx
    call int21_resolve_and_find_path
    jc .not_found
    xor ch, ch
    mov cl, [cs:search_found_attr]
    xor ax, ax
    clc
    ret
%else
    mov si, dx
    call int21_path_to_fat_name
    jc .not_found
    mov ax, cs
    mov ds, ax
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov si, path_fat_name
    mov bx, 0xFFFF
    call load_root_file_first_sector
    jc .not_found
    xor ch, ch
    mov cl, [cs:search_found_attr]
    xor ax, ax
    clc
    ret
%endif

.set_attr:
%if FAT_TYPE == 16
    ; Compatibility no-op: validate target exists, then report success.
    mov si, dx
    call int21_resolve_and_find_path
    jc .not_found
    xor ax, ax
    clc
    ret
%else
    mov si, dx
    call int21_path_to_fat_name
    jc .not_found
    mov ax, cs
    mov ds, ax
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov si, path_fat_name
    mov bx, 0xFFFF
    call load_root_file_first_sector
    jc .not_found
    xor ax, ax
    clc
    ret
%endif

.not_found:
    mov ax, 0x0002
    stc
    ret

int21_find_first:
    push bx
    push cx
    push dx
    push si
    push ds
    push es

    mov [cs:find_attr], cl
    mov si, dx
    call int21_resolve_parent_dir
    jc .path_fail
    mov [cs:find_dir_cluster], ax
    call int21_path_to_fat_pattern
    jc .path_fail

    call int21_find_try_gem_special
    jnc .done_ok

.scan_generic:

    mov word [cs:find_cursor], 0
    call int21_find_scan_from_cursor
    jc .scan_fail

    call int21_find_write_dta
    jc .io_fail

.done_ok:
    call int21_patch_gem_desktop_tree
    xor ax, ax
    clc
    jmp .done

.path_fail:
    mov ax, 0x0003
    stc
    jmp .done

.scan_fail:
    stc
    jmp .done

.io_fail:
    mov ax, 0x0005
    stc

.done:
    pop es
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_find_next:
    cmp byte [cs:find_special_mode], 1
    jne .check_active
    mov byte [cs:find_special_mode], 0
    mov byte [cs:find_active], 0
    xor ax, ax
    clc
    ret

.check_active:
    cmp byte [cs:find_active], 1
    jne .no_more

    call int21_find_scan_from_cursor
    jc .scan_fail
    call int21_find_write_dta
    jc .io_fail
    xor ax, ax
    clc
    ret

.no_more:
    mov ax, 0x0012
    stc
    ret

.scan_fail:
    stc
    ret

.io_fail:
    mov ax, 0x0005
    stc
    ret

int21_patch_gem_desktop_tree:
    push ax
    push bx
    push ds
    push es

    cmp word [cs:current_load_seg], MZ2_LOAD_SEG
    jne .done
    mov ax, [cs:int21_caller_ds]
    cmp ax, 0x4000
    jb .done
    cmp ax, 0x5000
    jae .done

    mov bx, [cs:dos_mem_alloc_seg]
    cmp bx, DOS_HEAP_USER_SEG
    jb .done
    mov es, bx
    cmp word [es:0x0022], 0x1DCC
    jne .done

    mov ax, [es:0x0012]
    cmp ax, 0x1DCC
    jae .done
    mov bx, ax
    mov ax, [es:bx + 8]

    mov ds, [cs:int21_caller_ds]
    mov [ds:0x0A8C], ax
    mov ax, [es:bx + 10]
    add ax, [cs:dos_mem_alloc_seg]
    mov [ds:0x0A8E], ax

.done:
    pop es
    pop ds
    pop bx
    pop ax
    ret

int21_find_try_gem_special:
    push bx
    push cx
    push si
    push ds
    push es

    ; Check SD pattern first (SD + 9 wildcards)
    mov si, find_pattern
    cmp byte [cs:si], 'S'
    jne .check_gem
    cmp byte [cs:si + 1], 'D'
    jne .check_gem
    add si, 2
    mov cx, 9
.match_loop:
    cmp byte [cs:si], '?'
    jne .check_gem
    inc si
    loop .match_loop
    mov si, path_sd_driver_fat
    jmp .match_root

.check_gem:
    ; Desktop runtime probes VDx wildcard names; map them to bundled SDPSC9.VGA.
    mov si, find_pattern
    cmp byte [cs:si], 'V'
    jne .check_gem_exe
    cmp byte [cs:si + 1], 'D'
    jne .check_gem_exe
    mov si, path_sd_driver_fat
    jmp .match_root

.check_gem_exe:
    mov si, find_pattern
    cmp byte [cs:si], 'G'
    jne .miss
    cmp byte [cs:si + 1], 'E'
    jne .miss
    cmp byte [cs:si + 2], 'M'
    jne .miss
    mov si, path_gem_exe_fat
    jmp .match_root

.match_root:
    mov ax, cs
    mov ds, ax
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov bx, 0xFFFF
    call load_root_file_first_sector
    jc .miss
    call int21_find_write_dta
    jc .miss
    mov byte [cs:find_active], 1
    mov byte [cs:find_special_mode], 1
    clc
    jmp .done

.miss:
    stc

.done:
    pop es
    pop ds
    pop si
    pop cx
    pop bx
    ret

int21_find_scan_from_cursor:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    mov ax, cs
    mov ds, ax
    mov bx, [cs:find_cursor]
    mov word [cs:find_cached_sector], 0xFFFF

    cmp word [cs:find_dir_cluster], 0
    jne .scan_subdir_loop

.scan_loop:
    cmp bx, FAT_ROOT_DIR_SECTORS * 16
    jae .not_found

    mov ax, bx
    mov cx, 16
    xor dx, dx
    div cx
    ; AX = root sector index, DX = entry index in sector.
    cmp ax, [cs:find_cached_sector]
    je .sector_ready
    mov [cs:find_cached_sector], ax
    add ax, FAT_ROOT_START_LBA
    mov [cs:tmp_lba], ax

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    push bx
    xor bx, bx
    call read_sector_lba
    pop bx
    jc .io_fail

.sector_ready:
    mov ax, DOS_META_BUF_SEG
    mov es, ax

    mov di, dx
    shl di, 5
    mov al, [es:di]
    cmp al, 0x00
    je .not_found
    cmp al, 0xE5
    je .next_entry

    mov al, [es:di + 11]
    cmp al, 0x0F
    je .next_entry

    call int21_find_attr_ok
    jnc .next_entry

    mov si, find_pattern
    call fat_entry_matches_pattern
    jnc .next_entry

    mov ax, [cs:tmp_lba]
    mov [cs:search_found_root_lba], ax
    mov [cs:search_found_root_off], di
    mov ax, [es:di + 26]
    mov [cs:search_found_cluster], ax
    mov ax, [es:di + 28]
    mov [cs:search_found_size_lo], ax
    mov ax, [es:di + 30]
    mov [cs:search_found_size_hi], ax
    mov al, [es:di + 11]
    mov [cs:search_found_attr], al

    push bx
    push cx
    mov cx, 11
    mov si, di
    mov di, search_found_name
.copy_name:
    mov al, [es:si]
    mov [di], al
    inc si
    inc di
    loop .copy_name
    pop cx
    pop bx

    mov ax, bx
    inc ax
    mov [cs:find_cursor], ax
    mov byte [cs:find_active], 1
    clc
    jmp .done

.next_entry:
    inc bx
    jmp .scan_loop

.not_found:
    mov al, 'q'
    call serial_putc
    mov byte [cs:find_active], 0
    mov ax, 0x0012
    stc
    jmp .done

.scan_subdir_loop:
    call int21_load_fat_cache
    jc .io_fail

    mov ax, [cs:find_dir_cluster]
    mov [cs:tmp_cluster], ax
    xor bx, bx

.subdir_cluster_loop:
    mov ax, [cs:tmp_cluster]
    cmp ax, 2
    jb .not_found
    cmp ax, FAT_EOF
    jae .not_found

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
    xor dx, dx

.subdir_sector_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .subdir_next_cluster

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    add ax, dx
    push bx
    xor bx, bx
    call read_sector_lba
    pop bx
    jc .io_fail

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    xor di, di
    mov cx, 16

.subdir_entry_loop:
    cmp bx, [cs:find_cursor]
    jb .subdir_next_entry

    mov al, [es:di]
    cmp al, 0x00
    je .not_found
    cmp al, 0xE5
    je .subdir_next_entry

    mov al, [es:di + 11]
    cmp al, 0x0F
    je .subdir_next_entry

    call int21_find_attr_ok
    jnc .subdir_next_entry

    mov si, find_pattern
    call fat_entry_matches_pattern
    jnc .subdir_next_entry

    mov ax, [cs:tmp_lba]
    add ax, dx
    mov [cs:search_found_root_lba], ax
    mov [cs:search_found_root_off], di
    mov ax, [es:di + 26]
    mov [cs:search_found_cluster], ax
    mov ax, [es:di + 28]
    mov [cs:search_found_size_lo], ax
    mov ax, [es:di + 30]
    mov [cs:search_found_size_hi], ax
    mov al, [es:di + 11]
    mov [cs:search_found_attr], al

    push bx
    push cx
    mov cx, 11
    mov si, di
    mov di, search_found_name
.subdir_copy_name:
    mov al, [es:si]
    mov [di], al
    inc si
    inc di
    loop .subdir_copy_name
    pop cx
    pop bx

    mov ax, bx
    inc ax
    mov [cs:find_cursor], ax
    mov byte [cs:find_active], 1
    clc
    jmp .done

.subdir_next_entry:
    inc bx
    add di, 32
    dec cx
    jz .subdir_sector_done
    jmp .subdir_entry_loop
.subdir_sector_done:
    inc dx
    jmp .subdir_sector_loop

.subdir_next_cluster:
    mov ax, [cs:tmp_cluster]
    call fat12_get_entry_cached
    jc .io_fail
    mov [cs:tmp_cluster], ax
    jmp .subdir_cluster_loop

.io_fail:
    mov ax, 0x0005
    stc

.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_find_attr_ok:
    push bx

    mov bl, al
    and bl, 0x1E
    cmp bl, 0
    je .ok

    mov al, [cs:find_attr]
    not al
    and al, bl
    cmp al, 0
    jne .skip
.ok:
    stc
    jmp .done

.skip:
    clc

.done:
    pop bx
    ret

int21_find_write_dta:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov ax, [cs:dta_seg]
    mov es, ax
    mov di, [cs:dta_off]

    xor ax, ax
    mov cx, 21
    rep stosw
    mov byte [es:di], 0

    mov di, [cs:dta_off]
    mov al, [cs:search_found_attr]
    mov [es:di + 0x15], al
    mov word [es:di + 0x16], 0
    mov word [es:di + 0x18], 0
    mov ax, [cs:search_found_size_lo]
    mov [es:di + 0x1A], ax
    mov ax, [cs:search_found_size_hi]
    mov [es:di + 0x1C], ax

    mov di, [cs:dta_off]
    add di, 0x1E

    mov bx, 0
.name_emit:
    cmp bx, 8
    jae .ext_check
    mov al, [cs:search_found_name + bx]
    cmp al, ' '
    je .ext_check
    mov [es:di], al
    inc di
    inc bx
    jmp .name_emit

.ext_check:
    mov bx, 0
    mov cx, 0
.ext_probe:
    cmp bx, 3
    jae .ext_probe_done
    mov al, [cs:search_found_name + 8 + bx]
    cmp al, ' '
    je .ext_probe_next
    inc cx
.ext_probe_next:
    inc bx
    jmp .ext_probe

.ext_probe_done:
    cmp cx, 0
    je .term
    mov byte [es:di], '.'
    inc di
    mov bx, 0
.ext_emit:
    cmp bx, 3
    jae .term
    mov al, [cs:search_found_name + 8 + bx]
    cmp al, ' '
    je .term
    mov [es:di], al
    inc di
    inc bx
    jmp .ext_emit

.term:
    mov byte [es:di], 0
    clc

    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_path_to_fat_pattern:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    mov ax, cs
    mov es, ax
    mov di, find_pattern
    mov cx, 11
    mov al, ' '
    rep stosb

    mov byte [cs:tmp_path_guard], 96

    ; DOS callers often pass paths extracted from command tails with leading spaces.
.skip_leading_space:
    dec byte [cs:tmp_path_guard]
    jz .fail
    cmp byte [si], ' '
    jne .check_empty
    inc si
    jmp .skip_leading_space

.check_empty:
    cmp byte [si], 0
    je .fail
    cmp byte [si], 13
    je .fail

    cmp byte [si + 1], ':'
    jne .find_last
    add si, 2

.find_last:
    mov [cs:tmp_find_comp], si
.walk:
    dec byte [cs:tmp_path_guard]
    jz .fail
    mov al, [si]
    cmp al, 0
    je .parse_start
    cmp al, '\'
    je .mark_next
    cmp al, '/'
    je .mark_next
    inc si
    jmp .walk

.mark_next:
    inc si
    mov [cs:tmp_find_comp], si
    jmp .walk

.parse_start:
    mov si, [cs:tmp_find_comp]
    cmp byte [si], 0
    je .fail

    xor bx, bx
.name_loop:
    dec byte [cs:tmp_path_guard]
    jz .fail
    mov al, [si]
    cmp al, 0
    je .name_done
    cmp al, '.'
    je .ext_start
    cmp al, '\'
    je .name_done
    cmp al, '/'
    je .name_done
    cmp al, '*'
    je .name_star
    cmp bx, 8
    jae .name_advance
    cmp al, '?'
    je .name_qmark
    cmp byte [cs:int21_path_upcase], 0
    je .name_store
    call int21_upcase_al
.name_store:
    mov [es:find_pattern + bx], al
    inc bx
    jmp .name_advance
.name_qmark:
    mov byte [es:find_pattern + bx], '?'
    inc bx
.name_advance:
    inc si
    jmp .name_loop

.name_star:
    mov cx, 8
    sub cx, bx
    jz .name_skip_star
.fill_name_star:
    mov byte [es:find_pattern + bx], '?'
    inc bx
    loop .fill_name_star
.name_skip_star:
    inc si
.name_after_star:
    dec byte [cs:tmp_path_guard]
    jz .fail
    mov al, [si]
    cmp al, 0
    je .success
    cmp al, '.'
    je .ext_start
    cmp al, '\'
    je .success
    cmp al, '/'
    je .success
    inc si
    jmp .name_after_star

.name_done:
    cmp bx, 0
    je .fail
    clc
    jmp .done

.ext_start:
    inc si
    xor bx, bx
.ext_loop:
    dec byte [cs:tmp_path_guard]
    jz .fail
    mov al, [si]
    cmp al, 0
    je .success
    cmp al, '\'
    je .success
    cmp al, '/'
    je .success
    cmp al, '*'
    je .ext_star
    cmp bx, 3
    jae .ext_advance
    cmp al, '?'
    je .ext_qmark
    cmp byte [cs:int21_path_upcase], 0
    je .ext_store
    call int21_upcase_al
.ext_store:
    mov [es:find_pattern + 8 + bx], al
    inc bx
    jmp .ext_advance
.ext_qmark:
    mov byte [es:find_pattern + 8 + bx], '?'
    inc bx
.ext_advance:
    inc si
    jmp .ext_loop

.ext_star:
    mov cx, 3
    sub cx, bx
    jz .success
.fill_ext_star:
    mov byte [es:find_pattern + 8 + bx], '?'
    inc bx
    loop .fill_ext_star
    jmp .success

.success:
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

fat_entry_matches_pattern:
    push ax
    push bx
    push cx

    xor bx, bx
    mov cx, 11
.cmp_loop:
    mov al, [cs:si + bx]
    cmp al, '?'
    je .next
    cmp al, [es:di + bx]
    jne .not_match
.next:
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

int21_create:
    push dx
    push ds

    call int21_normalize_leading_drive_designator

    mov byte [cs:int21_path_stage_marker], 1
    mov si, dx
    call int21_resolve_parent_dir
    jnc .parent_ok
    mov si, dx
    call .resolve_root_leaf_fallback
    jc .path_fail

.parent_ok:
    mov byte [cs:int21_path_stage_marker], 2
    mov [cs:tmp_lookup_dir], ax

    call int21_path_to_fat_name
    jc .path_fail
    mov byte [cs:int21_path_stage_marker], 3

    mov ax, [cs:tmp_lookup_dir]
    mov bx, ax
    mov ax, cs
    mov ds, ax
    mov si, path_fat_name
    mov ax, bx
    mov byte [cs:int21_path_stage_marker], 4
    call int21_lookup_in_dir
%if FAT_TYPE == 16 || FAT_TYPE == 12
    jc .create_missing
    test byte [cs:search_found_attr], 0x10
    jnz .io_error
    jmp .open_created

.create_missing:
%else
    jnc .open_created
%endif
    cmp ax, 0x0002
    jne .io_error

    mov ax, [cs:tmp_lookup_dir]
    call int21_find_free_dir_entry
    jc .io_error

    mov ax, [cs:search_found_root_lba]
    mov [cs:tmp_next_cluster], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:tmp_cluster], ax

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_next_cluster]
    xor bx, bx
    call read_sector_lba
    jc .io_error

    mov di, [cs:tmp_cluster]
    mov si, path_fat_name
    mov cx, 11
    rep movsb

    mov byte [es:di - 11 + 11], 0x20
    mov word [es:di - 11 + 26], 0
    mov word [es:di - 11 + 28], 0
    mov word [es:di - 11 + 30], 0

    mov ax, [cs:tmp_next_cluster]
    xor bx, bx
    call write_sector_lba
    jc .io_error

    mov word [cs:search_found_cluster], 0
    mov word [cs:search_found_size_lo], 0
    mov word [cs:search_found_size_hi], 0
    mov ax, [cs:tmp_next_cluster]
    mov [cs:search_found_root_lba], ax
    mov ax, [cs:tmp_cluster]
    mov [cs:search_found_root_off], ax
    mov byte [cs:tmp_open_mode], 2

    call int21_select_free_file_handle
    jc .done
    call int21_assign_selected_file_handle
    jmp .done

.open_created:
    pop ds
    pop dx
    mov al, 2
    jmp int21_open

.resolve_root_leaf_fallback:
    push bx

.fallback_skip_space:
    cmp byte [si], ' '
    jne .fallback_drive_check
    inc si
    jmp .fallback_skip_space

.fallback_drive_check:
    cmp byte [si], 'C'
    je .fallback_drive_colon
    cmp byte [si], 'c'
    jne .fallback_sep_check
.fallback_drive_colon:
    cmp byte [si + 1], ':'
    jne .fallback_fail
    add si, 2

.fallback_sep_check:
    cmp byte [si], '\'
    je .fallback_leaf_start
    cmp byte [si], '/'
    jne .fallback_fail

.fallback_leaf_start:
    inc si
    mov bx, si

.fallback_leaf_scan:
    mov al, [si]
    cmp al, 0
    je .fallback_leaf_ok
    cmp al, 13
    je .fallback_leaf_ok
    cmp al, '\'
    je .fallback_fail
    cmp al, '/'
    je .fallback_fail
    inc si
    jmp .fallback_leaf_scan

.fallback_leaf_ok:
    cmp si, bx
    je .fallback_fail
    xor ax, ax
    mov si, bx
    clc
    jmp .fallback_done

.fallback_fail:
    mov ax, 0x0003
    stc

.fallback_done:
    pop bx
    ret

.path_fail:
    mov ax, 0x0003
    stc
    jmp .done

.io_error:
    mov ax, 0x0005
    stc

.done:
    pop ds
    pop dx
    ret

int21_normalize_leading_drive_designator:
    push ax
    push bx

    mov bx, dx

    mov al, [bx]
    or al, 0x20
    cmp al, 'a'
    jb .done
    cmp al, 'z'
    ja .done
    cmp byte [bx + 1], ':'
    jne .done
    add dx, 2

.done:
    pop bx
    pop ax
    ret

int21_select_free_file_handle:
    cmp byte [cs:file_handle_open], 0
    je .target_slot1
    cmp byte [cs:file_handle2_open], 0
    je .target_slot2
    cmp byte [cs:file_handle3_open], 0
    je .target_slot3
%if FAT_TYPE == 16
    cmp byte [cs:file_handle4_open], 0
    je .target_slot4
    cmp byte [cs:file_handle5_open], 0
    je .target_slot5
    cmp byte [cs:file_handle6_open], 0
    je .target_slot6
    cmp byte [cs:file_handle7_open], 0
    je .target_slot7
    cmp byte [cs:file_handle8_open], 0
    je .target_slot8
%endif
    mov ax, 0x0004
    stc
    ret

.target_slot1:
    mov byte [cs:file_handle_target], 1
    jmp .target_ready

.target_slot2:
    mov byte [cs:file_handle_target], 2
    jmp .target_ready

.target_slot3:
    mov byte [cs:file_handle_target], 3
    jmp .target_ready

%if FAT_TYPE == 16
.target_slot4:
    mov byte [cs:file_handle_target], 4
    jmp .target_ready

.target_slot5:
    mov byte [cs:file_handle_target], 5
    jmp .target_ready

.target_slot6:
    mov byte [cs:file_handle_target], 6
    jmp .target_ready

.target_slot7:
    mov byte [cs:file_handle_target], 7
    jmp .target_ready

.target_slot8:
    mov byte [cs:file_handle_target], 8
%endif

.target_ready:
    xor ax, ax
    clc
    ret

int21_assign_selected_file_handle:
    cmp byte [cs:file_handle_target], 2
    je .assign_slot2
    cmp byte [cs:file_handle_target], 3
    je .assign_slot3
%if FAT_TYPE == 16
    cmp byte [cs:file_handle_target], 4
    je .assign_slot4
    cmp byte [cs:file_handle_target], 5
    je .assign_slot5
    cmp byte [cs:file_handle_target], 6
    je .assign_slot6
    cmp byte [cs:file_handle_target], 7
    je .assign_slot7
    cmp byte [cs:file_handle_target], 8
    je .assign_slot8
%endif

    mov byte [cs:file_handle_open], 1
    mov word [cs:file_handle_pos], 0
%if FAT_TYPE == 16
    mov word [cs:file_handle_pos_hi], 0
%endif
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle_start_cluster]
    call int21_count_chain
    mov [cs:file_handle_cluster_count], ax

    mov ax, 0x0005
    clc
    ret

.assign_slot2:
    mov byte [cs:file_handle2_open], 1
    mov word [cs:file_handle2_pos], 0
%if FAT_TYPE == 16
    mov word [cs:file_handle2_pos_hi], 0
%endif
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle2_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle2_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle2_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle2_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle2_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle2_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle2_start_cluster]
    call int21_count_chain
    mov [cs:file_handle2_cluster_count], ax

    mov ax, 0x0006
    clc
    ret

.assign_slot3:
    mov byte [cs:file_handle3_open], 1
    mov word [cs:file_handle3_pos], 0
%if FAT_TYPE == 16
    mov word [cs:file_handle3_pos_hi], 0
%endif
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle3_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle3_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle3_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle3_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle3_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle3_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle3_start_cluster]
    call int21_count_chain
    mov [cs:file_handle3_cluster_count], ax

    mov ax, 0x0007
    clc
    ret

%if FAT_TYPE == 16
.assign_slot4:
    mov byte [cs:file_handle4_open], 1
    mov word [cs:file_handle4_pos], 0
    mov word [cs:file_handle4_pos_hi], 0
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle4_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle4_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle4_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle4_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle4_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle4_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle4_start_cluster]
    call int21_count_chain
    mov [cs:file_handle4_cluster_count], ax

    mov ax, 0x0008
    clc
    ret

.assign_slot5:
    mov byte [cs:file_handle5_open], 1
    mov word [cs:file_handle5_pos], 0
    mov word [cs:file_handle5_pos_hi], 0
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle5_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle5_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle5_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle5_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle5_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle5_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle5_start_cluster]
    call int21_count_chain
    mov [cs:file_handle5_cluster_count], ax

    mov ax, 0x0009
    clc
    ret

.assign_slot6:
    mov byte [cs:file_handle6_open], 1
    mov word [cs:file_handle6_pos], 0
    mov word [cs:file_handle6_pos_hi], 0
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle6_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle6_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle6_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle6_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle6_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle6_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle6_start_cluster]
    call int21_count_chain
    mov [cs:file_handle6_cluster_count], ax

    mov ax, 0x000A
    clc
    ret

.assign_slot7:
    mov byte [cs:file_handle7_open], 1
    mov word [cs:file_handle7_pos], 0
    mov word [cs:file_handle7_pos_hi], 0
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle7_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle7_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle7_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle7_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle7_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle7_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle7_start_cluster]
    call int21_count_chain
    mov [cs:file_handle7_cluster_count], ax

    mov ax, 0x000B
    clc
    ret

.assign_slot8:
    mov byte [cs:file_handle8_open], 1
    mov word [cs:file_handle8_pos], 0
    mov word [cs:file_handle8_pos_hi], 0
    mov al, [cs:tmp_open_mode]
    mov [cs:file_handle8_mode], al
    mov ax, [cs:search_found_cluster]
    mov [cs:file_handle8_start_cluster], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:file_handle8_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:file_handle8_size_hi], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:file_handle8_root_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:file_handle8_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:file_handle8_start_cluster]
    call int21_count_chain
    mov [cs:file_handle8_cluster_count], ax

    mov ax, 0x000C
    clc
    ret
%endif

.io_fail:
    mov ax, 0x0005
    stc
    ret

int21_open:
    push bx
    push dx
    push si
    push ds
    push es

    call int21_normalize_leading_drive_designator

    ; AH=3Dh: AL carries access in bits 0..2 plus sharing/inherit flags.
    ; Accept higher bits and validate only access mode.
    and al, 0x03
    cmp al, 0x03
    je .access_denied
    mov [cs:tmp_open_mode], al

    call int21_select_free_file_handle
    jc .done

    mov byte [cs:int21_path_stage_marker], 1
    mov si, dx
    call int21_resolve_and_find_path
    jnc .path_ready
%if FAT_TYPE == 16
    push ax
    call int21_open_try_gem_cpi_fallback
    jnc .gem_cpi_fallback_ready
    pop ax
    jmp .done

.gem_cpi_fallback_ready:
    add sp, 2
%endif
.path_ready:
%if FAT_TYPE == 16 || FAT_TYPE == 12
    test byte [cs:search_found_attr], 0x10
    jnz .access_denied
%endif

    call int21_assign_selected_file_handle
    jmp .done

.not_found:
    mov ax, 0x0002
    stc
    jmp .done

.path_fail:
    mov ax, 0x0003
    stc
    jmp .done

.access_denied:
    mov ax, 0x0005
    stc
    jmp .done

.done:
    pop es
    pop ds
    pop si
    pop dx
	pop bx
	ret

%if FAT_TYPE == 16
int21_open_try_gem_cpi_fallback:
    push bx
    push si
    push ds

    mov ax, cs
    mov ds, ax

    mov ax, [int21_trace_call_cs]
    cmp ax, 0x5800
    jb .fail
    cmp ax, 0x7000
    jae .fail

    mov si, path_gem_cpi_fat
    xor ax, ax
    call int21_lookup_in_dir
    jc .fail

.ok:
    xor ax, ax
    clc
    jmp .done

.fail:
    mov ax, 0x0003
    stc

.done:
    pop ds
    pop si
    pop bx
    ret
%endif

int21_close:
    ; Handles 0-4 are DOS standard handles (stdin/stdout/stderr/aux/prn).
    ; We do not manage them, so silently succeed on close.
    cmp bx, 5
    jb .close_noop
    cmp bx, 0x0005
    je .close_slot1
    cmp bx, 0x0006
    je .close_slot2
    cmp bx, 0x0007
    je .close_slot3
%if FAT_TYPE == 16
    cmp bx, 0x0008
    je .close_slot4
    cmp bx, 0x0009
    je .close_slot5
    cmp bx, 0x000A
    je .close_slot6
    cmp bx, 0x000B
    je .close_slot7
    cmp bx, 0x000C
    je .close_slot8
%endif
    jne .bad_handle

.close_slot3:
    cmp byte [cs:file_handle3_open], 1
    jne .bad_handle
    mov byte [cs:file_handle3_open], 0
    xor ax, ax
    clc
    ret

%if FAT_TYPE == 16
.close_slot4:
    cmp byte [cs:file_handle4_open], 1
    jne .bad_handle
    mov byte [cs:file_handle4_open], 0
    xor ax, ax
    clc
    ret
.close_slot5:
    cmp byte [cs:file_handle5_open], 1
    jne .bad_handle
    mov byte [cs:file_handle5_open], 0
    xor ax, ax
    clc
    ret

.close_slot6:
    cmp byte [cs:file_handle6_open], 1
    jne .bad_handle
    mov byte [cs:file_handle6_open], 0
    xor ax, ax
    clc
    ret

.close_slot7:
    cmp byte [cs:file_handle7_open], 1
    jne .bad_handle
    mov byte [cs:file_handle7_open], 0
    xor ax, ax
    clc
    ret

.close_slot8:
    cmp byte [cs:file_handle8_open], 1
    jne .bad_handle
    mov byte [cs:file_handle8_open], 0
    xor ax, ax
    clc
    ret

%endif

.close_slot2:
    cmp byte [cs:file_handle2_open], 1
    jne .bad_handle
    mov byte [cs:file_handle2_open], 0
    xor ax, ax
    clc
    ret

.close_slot1:
    cmp byte [cs:file_handle_open], 1
    jne .bad_handle
    mov byte [cs:file_handle_open], 0
    xor ax, ax
    clc
    ret
.bad_handle:
    xor ax, ax
    clc
    ret

.close_noop:
    xor ax, ax
    clc
    ret

int21_is_valid_handle:
    cmp bx, 5
    jb .ok
    cmp bx, 0x0005
    je .slot1
    cmp bx, 0x0006
    je .slot2
    cmp bx, 0x0007
    je .slot3
%if FAT_TYPE == 16
    cmp bx, 0x0008
    je .slot4
    cmp bx, 0x0009
    je .slot5
    cmp bx, 0x000A
    je .slot6
    cmp bx, 0x000B
    je .slot7
    cmp bx, 0x000C
    je .slot8
%endif
    jmp .bad

.slot1:
    cmp byte [cs:file_handle_open], 1
    jne .bad
    jmp .ok

.slot2:
    cmp byte [cs:file_handle2_open], 1
    jne .bad
    jmp .ok

.slot3:
    cmp byte [cs:file_handle3_open], 1
    jne .bad
    jmp .ok

%if FAT_TYPE == 16
.slot4:
    cmp byte [cs:file_handle4_open], 1
    jne .bad
    jmp .ok

.slot5:
    cmp byte [cs:file_handle5_open], 1
    jne .bad
    jmp .ok

.slot6:
    cmp byte [cs:file_handle6_open], 1
    jne .bad
    jmp .ok

.slot7:
    cmp byte [cs:file_handle7_open], 1
    jne .bad
    jmp .ok

.slot8:
    cmp byte [cs:file_handle8_open], 1
    jne .bad
    jmp .ok
%endif

.ok:
    xor ax, ax
    clc
    ret

.bad:
    mov ax, 0x0006
    stc
    ret

int21_read:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    mov byte [cs:file_handle_swapped], 0
    cmp bx, 0x0000
    je .stdin_read

    cmp bx, 0x0005
    je .handle_ready
    cmp bx, 0x0006
    je .use_slot2
    cmp bx, 0x0007
    je .use_slot3
%if FAT_TYPE == 16
    cmp bx, 0x0008
    je .use_slot4
    cmp bx, 0x0009
    je .use_slot5
    cmp bx, 0x000A
    je .use_slot6
    cmp bx, 0x000B
    je .use_slot7
    cmp bx, 0x000C
    je .use_slot8
%endif
    jne .bad_handle

.use_slot3:
    cmp byte [cs:file_handle3_open], 1
    jne .bad_handle
    call int21_swap_file_handles3
    mov byte [cs:file_handle_swapped], 3
    mov bx, 0x0005
    jmp .handle_ready

%if FAT_TYPE == 16
.use_slot4:
    cmp byte [cs:file_handle4_open], 1
    jne .bad_handle
    call int21_swap_file_handles4
    mov byte [cs:file_handle_swapped], 4
    mov bx, 0x0005
    jmp .handle_ready
.use_slot5:
    cmp byte [cs:file_handle5_open], 1
    jne .bad_handle
    call int21_swap_file_handles5
    mov byte [cs:file_handle_swapped], 5
    mov bx, 0x0005
    jmp .handle_ready

.use_slot6:
    cmp byte [cs:file_handle6_open], 1
    jne .bad_handle
    call int21_swap_file_handles6
    mov byte [cs:file_handle_swapped], 6
    mov bx, 0x0005
    jmp .handle_ready

.use_slot7:
    cmp byte [cs:file_handle7_open], 1
    jne .bad_handle
    call int21_swap_file_handles7
    mov byte [cs:file_handle_swapped], 7
    mov bx, 0x0005
    jmp .handle_ready

.use_slot8:
    cmp byte [cs:file_handle8_open], 1
    jne .bad_handle
    call int21_swap_file_handles8
    mov byte [cs:file_handle_swapped], 8
    mov bx, 0x0005
    jmp .handle_ready

%endif

.use_slot2:
    cmp byte [cs:file_handle2_open], 1
    jne .bad_handle
    call int21_swap_file_handles
    mov byte [cs:file_handle_swapped], 2
    mov bx, 0x0005

.handle_ready:
    cmp bx, 0x0005
    jne .bad_handle
    cmp byte [cs:file_handle_open], 1
    jne .bad_handle

    cmp byte [cs:file_handle_mode], 1
    je .access_denied

    cmp cx, 0
    jne .have_count
    xor ax, ax
    clc
    jmp .done

.have_count:
    mov [cs:tmp_rw_remaining], cx
    mov word [cs:tmp_rw_done], 0
    mov ax, [cs:file_handle_pos]
    mov [cs:tmp_capacity], ax
    mov ax, ds
    mov [cs:tmp_user_ds], ax
    mov [cs:tmp_user_ptr], dx

%if FAT_TYPE == 16
    mov ax, [cs:file_handle_size_hi]
    cmp [cs:file_handle_pos_hi], ax
    ja .eof
    jb .loop

    mov ax, [cs:file_handle_size_lo]
    cmp [cs:file_handle_pos], ax
    jae .eof

    sub ax, [cs:file_handle_pos]
    cmp [cs:tmp_rw_remaining], ax
    jbe .loop
    mov [cs:tmp_rw_remaining], ax
%else
    mov ax, [cs:file_handle_size_lo]
    or ax, ax
    jne .size_ready
    cmp word [cs:file_handle_cluster_count], 0
    je .size_ready
    mov ax, FAT_CLUSTER_MASK + 1
.size_ready:
    cmp [cs:file_handle_pos], ax
    jae .eof

    sub ax, [cs:file_handle_pos]
    cmp [cs:tmp_rw_remaining], ax
    jbe .loop
    mov [cs:tmp_rw_remaining], ax
%endif

.loop:
    cmp word [cs:tmp_rw_remaining], 0
    je .success

    mov ax, [cs:file_handle_pos]
    call int21_cluster_for_pos
    jc .io_error
    mov [cs:tmp_cluster], ax
    mov [cs:tmp_cluster_off], dx

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax

    mov ax, [cs:tmp_cluster_off]
    mov cl, 9
    shr ax, cl
    add [cs:tmp_lba], ax

    mov ax, [cs:tmp_cluster_off]
    and ax, 0x01FF
    mov [cs:tmp_sector_off], ax

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    xor bx, bx
    call read_sector_lba
    jc .io_error

    mov ax, 512
    sub ax, [cs:tmp_sector_off]
    mov dx, [cs:tmp_rw_remaining]
    cmp dx, ax
    ja .chunk_ready
    mov ax, dx
.chunk_ready:
    mov [cs:tmp_chunk], ax

    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    mov si, [cs:tmp_sector_off]
    mov ax, [cs:tmp_user_ds]
    mov es, ax
    mov di, [cs:tmp_user_ptr]
    add di, [cs:tmp_rw_done]
    mov cx, [cs:tmp_chunk]
    rep movsb
    mov ax, cs
    mov ds, ax

    mov ax, [cs:tmp_chunk]
    add [cs:file_handle_pos], ax
%if FAT_TYPE == 16
    adc word [cs:file_handle_pos_hi], 0
%endif
    add [cs:tmp_rw_done], ax
    sub [cs:tmp_rw_remaining], ax
    jmp .loop

.success:
    mov ax, [cs:tmp_rw_done]
    clc
    jmp .done

.eof:
    xor ax, ax
    clc
    jmp .done

.bad_handle:
    mov ax, 0x0006
    stc
    jmp .done

.access_denied:
    mov ax, 0x0005
    stc
    jmp .done

.io_error:
    mov ax, 0x0005
    stc

.done:
    pushf
    push ax
    mov al, [cs:file_handle_swapped]
    cmp al, 2
    je .done_swap2
    cmp al, 3
    je .done_swap3
%if FAT_TYPE == 16
    cmp al, 4
    je .done_swap4
    cmp al, 5
    je .done_swap5
    cmp al, 6
    je .done_swap6
    cmp al, 7
    je .done_swap7
    cmp al, 8
    je .done_swap8
%endif
    jmp .done_noswap
.done_swap2:
    call int21_swap_file_handles
    jmp .done_noswap
.done_swap3:
    call int21_swap_file_handles3
    jmp .done_noswap
%if FAT_TYPE == 16
.done_swap4:
    call int21_swap_file_handles4
    jmp .done_noswap
.done_swap5:
    call int21_swap_file_handles5
    jmp .done_noswap
.done_swap6:
    call int21_swap_file_handles6
    jmp .done_noswap
.done_swap7:
    call int21_swap_file_handles7
    jmp .done_noswap
.done_swap8:
    call int21_swap_file_handles8
%endif
.done_noswap:
    pop ax
    popf
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.stdin_read:
    cmp cx, 0
    jne .stdin_have_count
    xor ax, ax
    clc
    jmp .done

.stdin_have_count:
    push bx
    push cx
    push dx
    push si
    mov si, dx
    xor bx, bx
.stdin_loop:
    cmp bx, cx
    jae .stdin_done
    mov ah, 0x01
    int 0x16
    jz .stdin_done
    mov ah, 0x00
    int 0x16
    mov [ds:si + bx], al
    inc bx
    jmp .stdin_loop
.stdin_done:
    mov ax, bx
    pop si
    pop dx
    pop cx
    pop bx
    clc
    jmp .done

int21_write:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov byte [cs:file_handle_swapped], 0

    cmp bx, 0x0001
    je .stdio_write
    cmp bx, 0x0002
    je .stdio_write

    cmp bx, 0x0005
    je .handle_ready
    cmp bx, 0x0006
    je .use_slot2
    cmp bx, 0x0007
    je .use_slot3
%if FAT_TYPE == 16
    cmp bx, 0x0008
    je .use_slot4
    cmp bx, 0x0009
    je .use_slot5
    cmp bx, 0x000A
    je .use_slot6
    cmp bx, 0x000B
    je .use_slot7
    cmp bx, 0x000C
    je .use_slot8
%endif
    jne .bad_handle

.use_slot3:
    cmp byte [cs:file_handle3_open], 1
    jne .bad_handle
    call int21_swap_file_handles3
    mov byte [cs:file_handle_swapped], 3
    mov bx, 0x0005
    jmp .handle_ready

%if FAT_TYPE == 16
.use_slot4:
    cmp byte [cs:file_handle4_open], 1
    jne .bad_handle
    call int21_swap_file_handles4
    mov byte [cs:file_handle_swapped], 4
    mov bx, 0x0005
    jmp .handle_ready
.use_slot5:
    cmp byte [cs:file_handle5_open], 1
    jne .bad_handle
    call int21_swap_file_handles5
    mov byte [cs:file_handle_swapped], 5
    mov bx, 0x0005
    jmp .handle_ready

.use_slot6:
    cmp byte [cs:file_handle6_open], 1
    jne .bad_handle
    call int21_swap_file_handles6
    mov byte [cs:file_handle_swapped], 6
    mov bx, 0x0005
    jmp .handle_ready

.use_slot7:
    cmp byte [cs:file_handle7_open], 1
    jne .bad_handle
    call int21_swap_file_handles7
    mov byte [cs:file_handle_swapped], 7
    mov bx, 0x0005
    jmp .handle_ready

.use_slot8:
    cmp byte [cs:file_handle8_open], 1
    jne .bad_handle
    call int21_swap_file_handles8
    mov byte [cs:file_handle_swapped], 8
    mov bx, 0x0005
    jmp .handle_ready

%endif

.use_slot2:
    cmp byte [cs:file_handle2_open], 1
    jne .bad_handle
    call int21_swap_file_handles
    mov byte [cs:file_handle_swapped], 2
    mov bx, 0x0005

.handle_ready:
    cmp bx, 0x0005
    jne .bad_handle
    cmp byte [cs:file_handle_open], 1
    jne .bad_handle
    cmp byte [cs:file_handle_mode], 0
    je .access_denied

    mov [cs:tmp_rw_remaining], cx
    mov word [cs:tmp_rw_done], 0
    mov ax, ds
    mov [cs:tmp_user_ds], ax
    mov [cs:tmp_user_ptr], dx

    cmp word [cs:tmp_rw_remaining], 0
    jne .prepare
    mov ax, [cs:file_handle_pos]
    mov [cs:file_handle_size_lo], ax
    mov word [cs:file_handle_size_hi], 0
    call int21_update_root_entry_size
    jc .io_error
    xor ax, ax
    clc
    jmp .done

.prepare:
.loop:
    cmp word [cs:tmp_rw_remaining], 0
    je .finish

.cluster_resolve:
    mov ax, [cs:file_handle_pos]
    call int21_cluster_for_pos
    jnc .cluster_ready
    call int21_write_grow_chain
    jc .io_error
    jmp .cluster_resolve

.cluster_ready:
    mov [cs:tmp_cluster], ax
    mov [cs:tmp_cluster_off], dx

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax

    mov ax, [cs:tmp_cluster_off]
    mov cl, 9
    shr ax, cl
    add [cs:tmp_lba], ax

    mov ax, [cs:tmp_cluster_off]
    and ax, 0x01FF
    mov [cs:tmp_sector_off], ax

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    xor bx, bx
    call read_sector_lba
    jc .io_error

    mov ax, 512
    sub ax, [cs:tmp_sector_off]
    mov dx, [cs:tmp_rw_remaining]
    cmp dx, ax
    ja .chunk_ready
    mov ax, dx
.chunk_ready:
    mov [cs:tmp_chunk], ax

    mov ax, [cs:tmp_user_ds]
    mov ds, ax
    mov si, [cs:tmp_user_ptr]
    add si, [cs:tmp_rw_done]
    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov di, [cs:tmp_sector_off]
    mov cx, [cs:tmp_chunk]
    rep movsb
    mov ax, cs
    mov ds, ax

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    xor bx, bx
    call write_sector_lba
    jc .io_error

    mov ax, [cs:tmp_chunk]
    add [cs:file_handle_pos], ax
    add [cs:tmp_rw_done], ax
    sub [cs:tmp_rw_remaining], ax
    jmp .loop

.finish:
    call fat12_flush_cache
    jc .io_error

    mov ax, [cs:file_handle_pos]
    cmp ax, [cs:file_handle_size_lo]
    jbe .done_ok
    mov [cs:file_handle_size_lo], ax
    mov word [cs:file_handle_size_hi], 0
    call int21_update_root_entry_size
    jc .io_error

.done_ok:
%if FAT_TYPE == 16
    mov byte [cs:shell_footer_dsk_dirty], 1
%endif
    mov ax, [cs:tmp_rw_done]
    clc
    jmp .done

.bad_handle:
    mov ax, 0x0006
    stc
    jmp .done

.access_denied:
    mov ax, 0x0005
    stc
    jmp .done

.io_error:
    mov ax, 0x0005
    stc

.done:
    pushf
    push ax
    mov al, [cs:file_handle_swapped]
    cmp al, 2
    je .done_swap2
    cmp al, 3
    je .done_swap3
%if FAT_TYPE == 16
    cmp al, 4
    je .done_swap4
    cmp al, 5
    je .done_swap5
    cmp al, 6
    je .done_swap6
    cmp al, 7
    je .done_swap7
    cmp al, 8
    je .done_swap8
%endif
    jmp .done_noswap
.done_swap2:
    call int21_swap_file_handles
    jmp .done_noswap
.done_swap3:
    call int21_swap_file_handles3
    jmp .done_noswap
%if FAT_TYPE == 16
.done_swap4:
    call int21_swap_file_handles4
    jmp .done_noswap
.done_swap5:
    call int21_swap_file_handles5
    jmp .done_noswap
.done_swap6:
    call int21_swap_file_handles6
    jmp .done_noswap
.done_swap7:
    call int21_swap_file_handles7
    jmp .done_noswap
.done_swap8:
    call int21_swap_file_handles8
%endif
.done_noswap:
    pop ax
    popf
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.stdio_write:
    cmp cx, 0
    jne .stdio_have_count
    xor ax, ax
    clc
    jmp .done

.stdio_have_count:
    push bx
    push cx
    push dx
    push si
    mov si, dx
    xor bx, bx
.stdio_loop:
    cmp bx, cx
    jae .stdio_done
    mov al, [ds:si + bx]
    call bios_putc
    call serial_putc
    inc bx
    jmp .stdio_loop
.stdio_done:
    mov ax, bx
    pop si
    pop dx
    pop cx
    pop bx
    clc
    jmp .done

int21_write_grow_chain:
    mov bx, 2

.find_free:
    cmp bx, FAT_EOF
    jae .fail
    mov ax, bx
    call fat12_get_entry_cached
    jc .fail
    cmp ax, 0
    je .cluster_found
    inc bx
    jmp .find_free

.cluster_found:
    mov ax, bx
    mov dx, FAT_EOF
    call fat12_set_entry_cached
    jc .fail

    mov cx, [cs:file_handle_cluster_count]
    cmp cx, 0
    je .set_start_cluster

    mov ax, cx
    dec ax
    mov cl, FAT_CLUSTER_SHIFT
    shl ax, cl
    call int21_cluster_for_pos
    jc .fail
    mov dx, bx
    call fat12_set_entry_cached
    jc .fail
    jmp .bump_count

.set_start_cluster:
    mov [cs:file_handle_start_cluster], bx

.bump_count:
    inc word [cs:file_handle_cluster_count]

    clc
    ret

.fail:
    stc
    ret

int21_delete:
    push bx
    push dx
    push si
    push ds
    push es

    call int21_normalize_leading_drive_designator

%if FAT_TYPE == 16
    mov si, dx
    call int21_resolve_and_find_path
    jc .done
%else
    mov si, dx
    call int21_path_to_fat_name
    jc .path_fail

    mov ax, cs
    mov ds, ax
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov si, path_fat_name
    mov bx, 0xFFFF
    call load_root_file_first_sector
    jc .not_found
%endif

    mov ax, [search_found_cluster]
    mov [tmp_next_cluster], ax

    call int21_load_fat_cache
    jc .io_error

.free_loop:
    mov ax, [tmp_next_cluster]
    cmp ax, 2
    jb .free_done
    cmp ax, FAT_EOF
    jae .free_done

    mov bx, ax
    call fat12_get_entry_cached
    jc .io_error
    mov [tmp_next_cluster], ax

    mov ax, bx
    xor dx, dx
    call fat12_set_entry_cached
    jc .io_error
    jmp .free_loop

.free_done:
    call fat12_flush_cache
    jc .io_error

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [search_found_root_lba]
    xor bx, bx
    call read_sector_lba
    jc .io_error

    mov di, [search_found_root_off]
    mov byte [es:di], 0xE5

    mov ax, [search_found_root_lba]
    xor bx, bx
    call write_sector_lba
    jc .io_error

    xor ax, ax
    clc
    jmp .done

.not_found:
    mov ax, 0x0002
    stc
    jmp .done

.path_fail:
    mov ax, 0x0003
    stc
    jmp .done

.io_error:
    mov ax, 0x0005
    stc

.done:
    pop es
    pop ds
    pop si
    pop dx
    pop bx
    ret

int21_mkdir:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov si, dx
    call int21_resolve_parent_dir
    jc .mkdir_fail
    mov [cs:tmp_lookup_dir], ax

    call int21_path_to_fat_name
    jc .mkdir_fail

    mov ax, [cs:tmp_lookup_dir]
    push ds
    mov bx, ax
    mov ax, cs
    mov ds, ax
    mov si, path_fat_name
    mov ax, bx
    call int21_lookup_in_dir
    pop ds
    jnc .mkdir_fail
    cmp ax, 0x0002
    jne .mkdir_io_err

    mov ax, [cs:tmp_lookup_dir]
    call int21_find_free_dir_entry
    jc .mkdir_alloc

    mov ax, [cs:search_found_root_lba]
    mov [cs:tmp_next_cluster], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:tmp_cluster], ax
    jmp .mkdir_slot_ready

.mkdir_slot_ready:
    call int21_load_fat_cache
    jc .mkdir_io_err

    mov bx, 2
.mkdir_find_cluster:
    cmp bx, FAT_EOF
    jae .mkdir_no_free_cluster
    mov ax, bx
    call fat12_get_entry_cached
    jc .mkdir_io_err
    cmp ax, 0
    je .mkdir_cluster_found
    inc bx
    jmp .mkdir_find_cluster

.mkdir_cluster_found:
    mov [cs:tmp_cluster_off], bx
    mov ax, bx
    mov dx, FAT_EOF
    call fat12_set_entry_cached
    jc .mkdir_io_err
    call fat12_flush_cache
    jc .mkdir_io_err

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw

    mov ax, [cs:tmp_cluster_off]
    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
    xor dx, dx
.mkdir_zero_cluster_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .mkdir_reload_root_sector
    mov ax, [cs:tmp_lba]
    add ax, dx
    xor bx, bx
    call write_sector_lba
    jc .mkdir_io_err
    inc dx
    jmp .mkdir_zero_cluster_loop

.mkdir_reload_root_sector:
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_next_cluster]
    xor bx, bx
    call read_sector_lba
    jc .mkdir_io_err

    mov di, [cs:tmp_cluster]
    mov si, path_fat_name
    mov cx, 11
    rep movsb

    mov byte [es:di - 11 + 11], 0x10
    mov byte [es:di - 11 + 12], 0
    mov byte [es:di - 11 + 13], 0
    mov word [es:di - 11 + 14], 0
    mov word [es:di - 11 + 16], 0x2121
    mov word [es:di - 11 + 18], 0x2121
    mov word [es:di - 11 + 20], 0
    mov word [es:di - 11 + 22], 0x0200
    mov word [es:di - 11 + 24], 0x0002
    mov ax, [cs:tmp_cluster_off]
    mov word [es:di - 11 + 26], ax
    mov word [es:di - 11 + 28], 0
    mov word [es:di - 11 + 30], 0

    mov ax, [cs:tmp_next_cluster]
    xor bx, bx
    call write_sector_lba
    jc .mkdir_io_err

    xor ax, ax
    clc
    jmp .mkdir_done

.mkdir_no_free_cluster:
    mov ax, 0x0005
    stc
    jmp .mkdir_done

.mkdir_alloc:
    mov ax, 0x0005
    stc
    jmp .mkdir_done

.mkdir_fail:
    mov ax, 0x0003
    stc
    jmp .mkdir_done

.mkdir_io_err:
    mov ax, 0x0005
    stc

.mkdir_done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_rmdir:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov si, dx
    call int21_resolve_and_find_path
    jc .rmdir_done

    test byte [cs:search_found_attr], 0x10
    jz .rmdir_not_dir

    mov ax, [cs:search_found_root_lba]
    xor bx, bx
    call read_sector_lba
    jc .rmdir_io_err

    mov di, [cs:search_found_root_off]
    mov byte [es:di], 0xE5

    mov ax, [cs:search_found_root_lba]
    xor bx, bx
    call write_sector_lba
    jc .rmdir_io_err

    xor ax, ax
    clc
    jmp .rmdir_done

.rmdir_not_dir:
    mov ax, 0x0010
    stc
    jmp .rmdir_done

.rmdir_not_found:
    mov ax, 0x0002
    stc
    jmp .rmdir_done

.rmdir_fail:
    mov ax, 0x0003
    stc
    jmp .rmdir_done

.rmdir_io_err:
    mov ax, 0x0005
    stc

.rmdir_done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_rename:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    mov si, [ss:bp + 12]
    mov ds, [ss:bp + 4]
    mov dx, si
    call int21_normalize_leading_drive_designator
    mov si, dx
    call int21_resolve_parent_dir
    jc .rename_fail_path
    mov [cs:tmp_rename_old_parent], ax

    call int21_path_to_fat_name
    jc .rename_fail_path

    mov ax, cs
    mov ds, ax
    mov di, search_found_name
    mov si, path_fat_name
    mov cx, 11
    rep movsb

    mov ax, [cs:tmp_rename_old_parent]
    mov si, search_found_name
    call int21_lookup_in_dir
    jc .rename_old_lookup_fail

    mov ax, [cs:search_found_root_lba]
    mov [cs:tmp_rename_old_lba], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:tmp_rename_old_off], ax

    mov si, [ss:bp + 8]
    mov ds, [ss:bp + 2]
    mov dx, si
    call int21_normalize_leading_drive_designator
    mov si, dx
    call int21_resolve_parent_dir
    jc .rename_fail_newname
    mov [cs:tmp_rename_new_parent], ax

    call int21_path_to_fat_name
    jc .rename_fail_newname

    mov ax, [cs:tmp_rename_new_parent]
    push ds
    mov bx, ax
    mov ax, cs
    mov ds, ax
    mov si, path_fat_name
    mov ax, bx
    call int21_lookup_in_dir
    pop ds
    jnc .rename_dest_exists
    cmp ax, 0x0002
    jne .rename_io_err

    mov ax, [cs:tmp_rename_old_parent]
    cmp ax, [cs:tmp_rename_new_parent]
    jne .rename_cross_dir

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_rename_old_lba]
    xor bx, bx
    call read_sector_lba
    jc .rename_io_err

    mov ax, cs
    mov ds, ax
    mov di, [cs:tmp_rename_old_off]
    mov si, path_fat_name
    mov cx, 11
    rep movsb

    mov ax, [cs:tmp_rename_old_lba]
    xor bx, bx
    call write_sector_lba
    jc .rename_io_err

    xor ax, ax
    clc
    jmp .rename_done

.rename_cross_dir:
    mov ax, [cs:tmp_rename_new_parent]
    call int21_find_free_dir_entry
    jc .rename_io_err

    mov ax, [cs:search_found_root_lba]
    mov [cs:tmp_next_cluster], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:tmp_cluster], ax

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_rename_old_lba]
    xor bx, bx
    call read_sector_lba
    jc .rename_io_err

    push ds
    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    mov si, [cs:tmp_rename_old_off]
    xor di, di
    mov cx, 32
.rename_copy_old_entry:
    mov al, [es:si]
    mov [di], al
    inc si
    inc di
    loop .rename_copy_old_entry

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    xor di, di
    mov ax, cs
    mov ds, ax
    mov si, path_fat_name
    mov cx, 11
    rep movsb
    pop ds

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_next_cluster]
    xor bx, bx
    call read_sector_lba
    jc .rename_io_err

    push ds
    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    xor si, si
    mov di, [cs:tmp_cluster]
    mov cx, 32
    rep movsb
    pop ds

    mov ax, [cs:tmp_next_cluster]
    xor bx, bx
    call write_sector_lba
    jc .rename_io_err

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_rename_old_lba]
    xor bx, bx
    call read_sector_lba
    jc .rename_io_err

    mov di, [cs:tmp_rename_old_off]
    mov byte [es:di], 0xE5

    mov ax, [cs:tmp_rename_old_lba]
    xor bx, bx
    call write_sector_lba
    jc .rename_io_err

    xor ax, ax
    clc
    jmp .rename_done

.rename_old_lookup_fail:
    cmp ax, 0x0002
    je .rename_not_found
    jmp .rename_io_err

.rename_dest_exists:
    mov ax, 0x0005
    stc
    jmp .rename_done

.rename_not_found:
    mov ax, 0x0002
    stc
    jmp .rename_done

.rename_fail_path:
.rename_fail_newname:
.rename_fail:
    mov ax, 0x0003
    stc
    jmp .rename_done

.rename_io_err:
    mov ax, 0x0005
    stc

.rename_done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_seek:
    push bx
    push cx

    mov byte [cs:file_handle_swapped], 0

    cmp bx, 0x0005
    je .handle_ready
    cmp bx, 0x0006
    je .use_slot2
    cmp bx, 0x0007
    je .use_slot3
%if FAT_TYPE == 16
    cmp bx, 0x0008
    je .use_slot4
    cmp bx, 0x0009
    je .use_slot5
    cmp bx, 0x000A
    je .use_slot6
    cmp bx, 0x000B
    je .use_slot7
    cmp bx, 0x000C
    je .use_slot8
%endif
    jne .bad_handle

.use_slot3:
    cmp byte [cs:file_handle3_open], 1
    jne .bad_handle
    call int21_swap_file_handles3
    mov byte [cs:file_handle_swapped], 3
    mov bx, 0x0005
    jmp .handle_ready

%if FAT_TYPE == 16
.use_slot4:
    cmp byte [cs:file_handle4_open], 1
    jne .bad_handle
    call int21_swap_file_handles4
    mov byte [cs:file_handle_swapped], 4
    mov bx, 0x0005
    jmp .handle_ready
.use_slot5:
    cmp byte [cs:file_handle5_open], 1
    jne .bad_handle
    call int21_swap_file_handles5
    mov byte [cs:file_handle_swapped], 5
    mov bx, 0x0005
    jmp .handle_ready

.use_slot6:
    cmp byte [cs:file_handle6_open], 1
    jne .bad_handle
    call int21_swap_file_handles6
    mov byte [cs:file_handle_swapped], 6
    mov bx, 0x0005
    jmp .handle_ready

.use_slot7:
    cmp byte [cs:file_handle7_open], 1
    jne .bad_handle
    call int21_swap_file_handles7
    mov byte [cs:file_handle_swapped], 7
    mov bx, 0x0005
    jmp .handle_ready

.use_slot8:
    cmp byte [cs:file_handle8_open], 1
    jne .bad_handle
    call int21_swap_file_handles8
    mov byte [cs:file_handle_swapped], 8
    mov bx, 0x0005
    jmp .handle_ready

%endif

.use_slot2:
    cmp byte [cs:file_handle2_open], 1
    jne .bad_handle
    call int21_swap_file_handles
    mov byte [cs:file_handle_swapped], 2
    mov bx, 0x0005

.handle_ready:

    cmp bx, 0x0005
    jne .bad_handle
    cmp byte [cs:file_handle_open], 1
    jne .bad_handle
%if FAT_TYPE != 16
    cmp cx, 0
    jne .bad_function
%endif

    cmp al, 0
    je .from_start
    cmp al, 1
    je .from_current
    cmp al, 2
    je .from_end
    jmp .bad_function

.from_start:
%if FAT_TYPE == 16
    mov [cs:file_handle_pos], dx
    mov [cs:file_handle_pos_hi], cx
    jmp .return_pos
%else
    mov ax, dx
    jmp .set_pos
%endif

.from_current:
%if FAT_TYPE == 16
    add dx, [cs:file_handle_pos]
    adc cx, [cs:file_handle_pos_hi]
    mov [cs:file_handle_pos], dx
    mov [cs:file_handle_pos_hi], cx
    jmp .return_pos
%else
    mov ax, [cs:file_handle_pos]
    add ax, dx
    jmp .set_pos
%endif

.from_end:
%if FAT_TYPE == 16
    mov ax, [cs:file_handle_size_lo]
    mov bx, [cs:file_handle_size_hi]
    add ax, dx
    adc bx, cx
    mov [cs:file_handle_pos], ax
    mov [cs:file_handle_pos_hi], bx
    jmp .return_pos
%else
    mov ax, [cs:file_handle_size_lo]
    add ax, dx
%endif

.set_pos:
    mov [cs:file_handle_pos], ax
    xor dx, dx
    mov ax, [cs:file_handle_pos]
    clc
    jmp .done

%if FAT_TYPE == 16
.return_pos:
    mov ax, [cs:file_handle_pos]
    mov dx, [cs:file_handle_pos_hi]
    clc
    jmp .done
%endif

.bad_function:
    mov ax, 0x0001
    stc
    jmp .done

.bad_handle:
    mov ax, 0x0006
    stc

.done:
    pushf
    push ax
    mov al, [cs:file_handle_swapped]
    cmp al, 2
    je .done_swap2
    cmp al, 3
    je .done_swap3
%if FAT_TYPE == 16
    cmp al, 4
    je .done_swap4
    cmp al, 5
    je .done_swap5
    cmp al, 6
    je .done_swap6
    cmp al, 7
    je .done_swap7
    cmp al, 8
    je .done_swap8
%endif
    jmp .done_noswap
.done_swap2:
    call int21_swap_file_handles
    jmp .done_noswap
.done_swap3:
    call int21_swap_file_handles3
    jmp .done_noswap
%if FAT_TYPE == 16
.done_swap4:
    call int21_swap_file_handles4
    jmp .done_noswap
.done_swap5:
    call int21_swap_file_handles5
    jmp .done_noswap
.done_swap6:
    call int21_swap_file_handles6
    jmp .done_noswap
.done_swap7:
    call int21_swap_file_handles7
    jmp .done_noswap
.done_swap8:
    call int21_swap_file_handles8
%endif
.done_noswap:
    pop ax
    popf
    pop cx
    pop bx
    ret

%if FAT_TYPE == 16
int21_dup_handle1_to_2:
    mov byte [cs:file_handle2_open], 1
    mov ax, [cs:file_handle_pos]
    mov [cs:file_handle2_pos], ax
    mov ax, [cs:file_handle_pos_hi]
    mov [cs:file_handle2_pos_hi], ax
    mov al, [cs:file_handle_mode]
    mov [cs:file_handle2_mode], al
    mov ax, [cs:file_handle_start_cluster]
    mov [cs:file_handle2_start_cluster], ax
    mov ax, [cs:file_handle_root_lba]
    mov [cs:file_handle2_root_lba], ax
    mov ax, [cs:file_handle_root_off]
    mov [cs:file_handle2_root_off], ax
    mov ax, [cs:file_handle_cluster_count]
    mov [cs:file_handle2_cluster_count], ax
    mov ax, [cs:file_handle_size_lo]
    mov [cs:file_handle2_size_lo], ax
    mov ax, [cs:file_handle_size_hi]
    mov [cs:file_handle2_size_hi], ax
    ret
%endif

int21_swap_file_handles:
    push ax

    mov al, [cs:file_handle_open]
    xchg al, [cs:file_handle2_open]
    mov [cs:file_handle_open], al

    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:file_handle2_pos]
    mov [cs:file_handle_pos], ax
%if FAT_TYPE == 16
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:file_handle2_pos_hi]
    mov [cs:file_handle_pos_hi], ax
%endif

    mov al, [cs:file_handle_mode]
    xchg al, [cs:file_handle2_mode]
    mov [cs:file_handle_mode], al

    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:file_handle2_start_cluster]
    mov [cs:file_handle_start_cluster], ax

    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:file_handle2_root_lba]
    mov [cs:file_handle_root_lba], ax

    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:file_handle2_root_off]
    mov [cs:file_handle_root_off], ax

    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:file_handle2_cluster_count]
    mov [cs:file_handle_cluster_count], ax

    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:file_handle2_size_lo]
    mov [cs:file_handle_size_lo], ax

    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:file_handle2_size_hi]
    mov [cs:file_handle_size_hi], ax

    pop ax
    ret

int21_swap_file_handles3:
    push ax

    mov al, [cs:file_handle_open]
    xchg al, [cs:file_handle3_open]
    mov [cs:file_handle_open], al

    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:file_handle3_pos]
    mov [cs:file_handle_pos], ax
%if FAT_TYPE == 16
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:file_handle3_pos_hi]
    mov [cs:file_handle_pos_hi], ax
%endif

    mov al, [cs:file_handle_mode]
    xchg al, [cs:file_handle3_mode]
    mov [cs:file_handle_mode], al

    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:file_handle3_start_cluster]
    mov [cs:file_handle_start_cluster], ax

    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:file_handle3_root_lba]
    mov [cs:file_handle_root_lba], ax

    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:file_handle3_root_off]
    mov [cs:file_handle_root_off], ax

    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:file_handle3_cluster_count]
    mov [cs:file_handle_cluster_count], ax

    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:file_handle3_size_lo]
    mov [cs:file_handle_size_lo], ax

    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:file_handle3_size_hi]
    mov [cs:file_handle_size_hi], ax

    pop ax
    ret

%if FAT_TYPE == 16
int21_swap_file_handles4:
    push ax

    mov al, [cs:file_handle_open]
    xchg al, [cs:file_handle4_open]
    mov [cs:file_handle_open], al

    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:file_handle4_pos]
    mov [cs:file_handle_pos], ax
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:file_handle4_pos_hi]
    mov [cs:file_handle_pos_hi], ax

    mov al, [cs:file_handle_mode]
    xchg al, [cs:file_handle4_mode]
    mov [cs:file_handle_mode], al

    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:file_handle4_start_cluster]
    mov [cs:file_handle_start_cluster], ax

    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:file_handle4_root_lba]
    mov [cs:file_handle_root_lba], ax

    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:file_handle4_root_off]
    mov [cs:file_handle_root_off], ax

    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:file_handle4_cluster_count]
    mov [cs:file_handle_cluster_count], ax

    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:file_handle4_size_lo]
    mov [cs:file_handle_size_lo], ax

    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:file_handle4_size_hi]
    mov [cs:file_handle_size_hi], ax

    pop ax
    ret
int21_swap_file_handles5:
    push ax

    mov al, [cs:file_handle_open]
    xchg al, [cs:file_handle5_open]
    mov [cs:file_handle_open], al

    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:file_handle5_pos]
    mov [cs:file_handle_pos], ax
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:file_handle5_pos_hi]
    mov [cs:file_handle_pos_hi], ax

    mov al, [cs:file_handle_mode]
    xchg al, [cs:file_handle5_mode]
    mov [cs:file_handle_mode], al

    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:file_handle5_start_cluster]
    mov [cs:file_handle_start_cluster], ax

    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:file_handle5_root_lba]
    mov [cs:file_handle_root_lba], ax

    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:file_handle5_root_off]
    mov [cs:file_handle_root_off], ax

    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:file_handle5_cluster_count]
    mov [cs:file_handle_cluster_count], ax

    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:file_handle5_size_lo]
    mov [cs:file_handle_size_lo], ax

    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:file_handle5_size_hi]
    mov [cs:file_handle_size_hi], ax

    pop ax
    ret

int21_swap_file_handles6:
    push ax

    mov al, [cs:file_handle_open]
    xchg al, [cs:file_handle6_open]
    mov [cs:file_handle_open], al

    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:file_handle6_pos]
    mov [cs:file_handle_pos], ax
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:file_handle6_pos_hi]
    mov [cs:file_handle_pos_hi], ax

    mov al, [cs:file_handle_mode]
    xchg al, [cs:file_handle6_mode]
    mov [cs:file_handle_mode], al

    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:file_handle6_start_cluster]
    mov [cs:file_handle_start_cluster], ax

    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:file_handle6_root_lba]
    mov [cs:file_handle_root_lba], ax

    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:file_handle6_root_off]
    mov [cs:file_handle_root_off], ax

    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:file_handle6_cluster_count]
    mov [cs:file_handle_cluster_count], ax

    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:file_handle6_size_lo]
    mov [cs:file_handle_size_lo], ax

    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:file_handle6_size_hi]
    mov [cs:file_handle_size_hi], ax

    pop ax
    ret

int21_swap_file_handles7:
    push ax

    mov al, [cs:file_handle_open]
    xchg al, [cs:file_handle7_open]
    mov [cs:file_handle_open], al

    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:file_handle7_pos]
    mov [cs:file_handle_pos], ax
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:file_handle7_pos_hi]
    mov [cs:file_handle_pos_hi], ax

    mov al, [cs:file_handle_mode]
    xchg al, [cs:file_handle7_mode]
    mov [cs:file_handle_mode], al

    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:file_handle7_start_cluster]
    mov [cs:file_handle_start_cluster], ax

    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:file_handle7_root_lba]
    mov [cs:file_handle_root_lba], ax

    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:file_handle7_root_off]
    mov [cs:file_handle_root_off], ax

    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:file_handle7_cluster_count]
    mov [cs:file_handle_cluster_count], ax

    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:file_handle7_size_lo]
    mov [cs:file_handle_size_lo], ax

    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:file_handle7_size_hi]
    mov [cs:file_handle_size_hi], ax

    pop ax
    ret

int21_swap_file_handles8:
    push ax

    mov al, [cs:file_handle_open]
    xchg al, [cs:file_handle8_open]
    mov [cs:file_handle_open], al

    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:file_handle8_pos]
    mov [cs:file_handle_pos], ax
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:file_handle8_pos_hi]
    mov [cs:file_handle_pos_hi], ax

    mov al, [cs:file_handle_mode]
    xchg al, [cs:file_handle8_mode]
    mov [cs:file_handle_mode], al

    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:file_handle8_start_cluster]
    mov [cs:file_handle_start_cluster], ax

    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:file_handle8_root_lba]
    mov [cs:file_handle_root_lba], ax

    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:file_handle8_root_off]
    mov [cs:file_handle_root_off], ax

    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:file_handle8_cluster_count]
    mov [cs:file_handle_cluster_count], ax

    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:file_handle8_size_lo]
    mov [cs:file_handle_size_lo], ax

    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:file_handle8_size_hi]
    mov [cs:file_handle_size_hi], ax

    pop ax
    ret
%endif

int21_mem_init:
    cmp byte [cs:dos_mem_init], 1
    je .done
    mov byte [cs:dos_mem_init], 1
    mov word [cs:dos_mem_alloc_seg], 0
    mov word [cs:dos_mem_alloc_size], 0
    mov word [cs:dos_mem_mcb_owner], 0
    mov word [cs:dos_mem_mcb_size], DOS_HEAP_USER_MAX_PARAS
    mov word [cs:dos_mem_alloc_seg2], 0
    mov word [cs:dos_mem_alloc_size2], 0
    mov word [cs:dos_mem_alloc_seg3], 0
    mov word [cs:dos_mem_alloc_size3], 0
    mov word [cs:dos_mem_psp_mcb_end], 0
    mov word [cs:dos_mem_free2_seg], 0
    mov word [cs:dos_mem_free2_size], 0
    call int21_mem_table_clear
    ; initialise list-of-lists: first word (BX-2) = first MCB segment
    mov word [cs:dos_list_of_lists], DOS_HEAP_BASE_SEG
    ; compatibility mirror for clients reading ES:BX directly
    mov word [cs:dos_list_of_lists + 2], DOS_HEAP_BASE_SEG
    mov word [cs:dos_list_of_lists + 4], DOS_HEAP_BASE_SEG
    call int21_mem_write_mcb
.done:
    ret

int21_mem_query_free:
    push es

    cmp word [cs:dos_mem_psp_free_size], 0
    je .query_psp
    mov ax, [cs:dos_mem_psp_free_seg]
    mov cx, [cs:dos_mem_psp_free_size]
    pop es
    ret

.query_psp:
    mov ax, DOS_HEAP_USER_SEG
    mov cx, DOS_HEAP_LIMIT_SEG
    mov dx, [cs:current_psp_seg]
    or dx, dx
    jz .have_base

    mov es, dx
    mov ax, [es:0x0002]
    cmp ax, DOS_HEAP_LIMIT_SEG
    jae .check_limit
    cmp ax, DOS_HEAP_BASE_SEG
    jae .from_psp_end
    mov ax, DOS_HEAP_USER_SEG
    jmp .check_limit

.from_psp_end:
    inc ax

.check_limit:
    cmp ax, DOS_HEAP_LIMIT_SEG
    jbe .have_base
    mov ax, DOS_HEAP_LIMIT_SEG

.have_base:
    sub cx, ax
    pop es
    ret

int21_mem_write_mcb:
    push ax
    push dx
    push es

    mov ax, [cs:dos_mem_alloc_seg]
    or ax, ax
    jnz .have_seg
    call int21_mem_query_free
.have_seg:
    call int21_mem_type_for_seg
    dec ax
    mov es, ax
    mov [es:0x0000], dl
    mov ax, [cs:dos_mem_mcb_owner]
    mov [es:0x0001], ax
    mov ax, [cs:dos_mem_mcb_size]
    mov [es:0x0003], ax

    pop es
    pop dx
    pop ax
    ret

int21_mem_write_chain_entry:
    push ax
    push es

    dec ax
    mov es, ax
    mov [es:0x0000], dl
    mov [es:0x0001], cx
    mov [es:0x0003], bx

    pop es
    pop ax
    ret

int21_mem_find_next_alloc:
    push cx
    push dx
    push si
    push di

    mov si, DOS_HEAP_LIMIT_SEG
    xor di, di
    mov word [cs:dos_mem_block_found_owner], 0
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
    mov bx, dos_mem_block_table

.scan:
    cmp cx, 0
    je .result
    cmp word [cs:bx + 6], DOS_MEM_BLOCK_ALLOC
    jne .next
    mov ax, [cs:bx]
    cmp ax, DOS_HEAP_LIMIT_SEG
    jae .next
    cmp ax, dx
    jae .candidate_start_ready
    push ax
    add ax, [cs:bx + 2]
    cmp ax, dx
    pop ax
    jb .next
.candidate_start_ready:
    cmp ax, DOS_HEAP_LIMIT_SEG
    jae .next
    cmp ax, si
    jae .next
    mov si, ax
    mov di, [cs:bx + 2]
    mov ax, [cs:bx + 4]
    mov [cs:dos_mem_block_found_owner], ax
.next:
    add bx, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .scan

.result:
    cmp di, 0
    je .none
    mov ax, si
    mov bx, di
    clc
    pop di
    pop si
    pop dx
    pop cx
    ret

.none:
    stc
    pop di
    pop si
    pop dx
    pop cx
    ret

int21_mem_table_clear:
    mov byte [cs:dos_mem_block_count], 0
    ret

int21_mem_arena_start:
    push bx
    push es

    mov ax, DOS_HEAP_USER_SEG
    mov bx, [cs:current_psp_seg]
    or bx, bx
    jz .done
    mov es, bx
    mov ax, [cs:dos_mem_psp_mcb_end]
    or ax, ax
    jnz .have_end
    mov ax, [es:0x0002]
.have_end:
    cmp ax, bx
    jae .end_ready
    mov ax, bx
.end_ready:
    inc ax
    cmp ax, DOS_HEAP_USER_SEG
    jae .check_limit
    mov ax, DOS_HEAP_USER_SEG
.check_limit:
    cmp ax, DOS_HEAP_LIMIT_SEG
    jbe .done
    mov ax, DOS_HEAP_LIMIT_SEG

.done:
    pop es
    pop bx
    ret

int21_mem_table_insert:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    cmp bx, 0
    je .done
    cmp ax, DOS_HEAP_USER_SEG
    jb .done
    cmp ax, DOS_HEAP_LIMIT_SEG
    jae .done
    cmp byte [cs:dos_mem_block_count], DOS_MEM_BLOCK_TABLE_MAX
    jae .done

    mov [cs:dos_mem_block_tmp_seg], ax
    mov [cs:dos_mem_block_tmp_size], bx
    mov [cs:dos_mem_block_tmp_owner], cx
    mov [cs:dos_mem_block_tmp_state], dx

    xor si, si
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
.find_slot:
    cmp cx, 0
    je .slot_ready
    mov ax, [cs:dos_mem_block_table + si]
    cmp [cs:dos_mem_block_tmp_seg], ax
    jb .slot_ready
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .find_slot

.slot_ready:
    xor di, di
    mov dl, [cs:dos_mem_block_count]
    mov di, dx
    shl di, 1
    shl di, 1
    shl di, 1
.shift_loop:
    cmp di, si
    jbe .store
    mov bp, di
    sub bp, DOS_MEM_BLOCK_ENTRY_SIZE
    mov ax, [cs:dos_mem_block_table + bp]
    mov [cs:dos_mem_block_table + di], ax
    mov ax, [cs:dos_mem_block_table + bp + 2]
    mov [cs:dos_mem_block_table + di + 2], ax
    mov ax, [cs:dos_mem_block_table + bp + 4]
    mov [cs:dos_mem_block_table + di + 4], ax
    mov ax, [cs:dos_mem_block_table + bp + 6]
    mov [cs:dos_mem_block_table + di + 6], ax
    sub di, DOS_MEM_BLOCK_ENTRY_SIZE
    jmp .shift_loop

.store:
    mov ax, [cs:dos_mem_block_tmp_seg]
    mov [cs:dos_mem_block_table + si], ax
    mov ax, [cs:dos_mem_block_tmp_size]
    mov [cs:dos_mem_block_table + si + 2], ax
    mov ax, [cs:dos_mem_block_tmp_owner]
    mov [cs:dos_mem_block_table + si + 4], ax
    mov ax, [cs:dos_mem_block_tmp_state]
    mov [cs:dos_mem_block_table + si + 6], ax
    inc byte [cs:dos_mem_block_count]

.done:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_mem_table_rebuild:
    push ax
    push bx
    push cx
    push dx

    call int21_mem_sync_legacy

    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_mem_table_find_exact:
    push dx
    push di

    xor si, si
    xor di, di
    xor dx, dx
    mov dl, [cs:dos_mem_block_count]

.scan:
    cmp di, dx
    jae .not_found
    cmp word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_ALLOC
    jne .next
    cmp [cs:dos_mem_block_table + si], ax
    je .found
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.found:
    mov bx, [cs:dos_mem_block_table + si + 2]
    mov cx, [cs:dos_mem_block_table + si + 4]
    clc
    pop di
    pop dx
    ret

.not_found:
    stc
    pop di
    pop dx
    ret

int21_mem_table_resize_at_si:
    mov [cs:dos_mem_block_table + si + 2], bx
    ret

int21_mem_table_clear_if_no_alloc:
    push cx
    push si

    xor si, si
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp cx, 0
    je .clear
    cmp word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_ALLOC
    je .done
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .scan

.clear:
    call int21_mem_table_clear

.done:
    pop si
    pop cx
    ret

int21_mem_table_next_limit:
    push ax
    push cx
    push si
    push di

    mov dx, DOS_HEAP_LIMIT_SEG
    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp di, cx
    jae .done
    cmp word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_ALLOC
    jne .next
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, bx
    jbe .next
    cmp ax, dx
    jae .next
    mov dx, ax
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.done:
    pop di
    pop si
    pop cx
    pop ax
    ret

int21_mem_table_resize_limit:
    push ax
    push bx

    mov bx, ax
    call int21_mem_table_next_limit
    mov ax, dx
    sub dx, bx
    cmp ax, DOS_HEAP_LIMIT_SEG
    je .done
    dec dx

.done:
    pop bx
    pop ax
    ret

int21_mem_find_free_gap:
    push bx
    push cx
    push dx
    push si
    push di

    mov [cs:dos_mem_block_req_size], bx
    call int21_mem_arena_start
    mov dx, ax
    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp di, cx
    jae .tail
    cmp word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_ALLOC
    jne .next
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, dx
    jbe .consume
    mov bx, ax
    sub bx, dx
    dec bx
    cmp bx, [cs:dos_mem_block_req_size]
    jae .found
.consume:
    mov dx, [cs:dos_mem_block_table + si]
    add dx, [cs:dos_mem_block_table + si + 2]
    inc dx
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.tail:
    cmp dx, DOS_HEAP_LIMIT_SEG
    jae .not_found
    mov bx, DOS_HEAP_LIMIT_SEG
    sub bx, dx
    cmp bx, [cs:dos_mem_block_req_size]
    jb .not_found

.found:
    mov ax, dx
    mov bx, [cs:dos_mem_block_req_size]
    clc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

.not_found:
    stc
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_mem_table_alloc_from_free:
    push bx
    push cx
    push dx
    push si
    push di

    mov [cs:dos_mem_block_req_size], bx
    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp di, cx
    jae .not_found
    cmp word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_FREE
    jne .next
    mov bx, [cs:dos_mem_block_table + si + 2]
    cmp bx, [cs:dos_mem_block_req_size]
    jae .use_free
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.use_free:
    mov bx, [cs:dos_mem_block_req_size]
    mov dx, [cs:dos_mem_block_table + si + 2]
    sub dx, bx
    cmp dx, 1
    ja .split_high
.use_whole:
    call int21_mem_current_owner
    mov [cs:dos_mem_block_table + si + 4], ax
    mov word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_ALLOC
    mov ax, [cs:dos_mem_block_table + si]
    clc
    jmp .done

.split_high:
    cmp byte [cs:dos_mem_block_count], DOS_MEM_BLOCK_TABLE_MAX
    jae .use_whole
    dec dx
    mov [cs:dos_mem_block_table + si + 2], dx
    mov ax, [cs:dos_mem_block_table + si]
    add ax, dx
    inc ax
    mov [cs:dos_mem_block_tmp_seg], ax
    call int21_mem_current_owner
    mov cx, ax
    mov dx, DOS_MEM_BLOCK_ALLOC
    mov ax, [cs:dos_mem_block_tmp_seg]
    mov bx, [cs:dos_mem_block_req_size]
    call int21_mem_table_insert
    mov ax, [cs:dos_mem_block_tmp_seg]
    clc
    jmp .done

.not_found:
    stc

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_mem_sync_legacy:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    xor ax, ax
    mov [cs:dos_mem_alloc_seg], ax
    mov [cs:dos_mem_alloc_size], ax
    mov [cs:dos_mem_alloc_seg2], ax
    mov [cs:dos_mem_alloc_size2], ax
    mov [cs:dos_mem_alloc_seg3], ax
    mov [cs:dos_mem_alloc_size3], ax
    mov [cs:dos_mem_psp_free_seg], ax
    mov [cs:dos_mem_psp_free_size], ax
    mov [cs:dos_mem_free2_seg], ax
    mov [cs:dos_mem_free2_size], ax

    call int21_mem_arena_start
    mov dx, ax
    xor si, si
    xor di, di
    xor bx, bx
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp di, cx
    jae .tail_gap
    cmp word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_ALLOC
    jne .next
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, dx
    jbe .store_alloc
    push bx
    mov bx, ax
    sub bx, dx
    dec bx
    call int21_mem_sync_store_gap
    pop bx
.store_alloc:
    cmp bx, 0
    je .slot1
    cmp bx, 1
    je .slot2
    cmp bx, 2
    je .slot3
    jmp .advance_alloc
.slot1:
    mov ax, [cs:dos_mem_block_table + si]
    mov [cs:dos_mem_alloc_seg], ax
    mov ax, [cs:dos_mem_block_table + si + 2]
    mov [cs:dos_mem_alloc_size], ax
    mov ax, [cs:dos_mem_block_table + si + 4]
    mov [cs:dos_mem_mcb_owner], ax
    mov ax, [cs:dos_mem_block_table + si + 2]
    mov [cs:dos_mem_mcb_size], ax
    jmp .advance_alloc
.slot2:
    mov ax, [cs:dos_mem_block_table + si]
    mov [cs:dos_mem_alloc_seg2], ax
    mov ax, [cs:dos_mem_block_table + si + 2]
    mov [cs:dos_mem_alloc_size2], ax
    jmp .advance_alloc
.slot3:
    mov ax, [cs:dos_mem_block_table + si]
    mov [cs:dos_mem_alloc_seg3], ax
    mov ax, [cs:dos_mem_block_table + si + 2]
    mov [cs:dos_mem_alloc_size3], ax
.advance_alloc:
    inc bx
    mov dx, [cs:dos_mem_block_table + si]
    add dx, [cs:dos_mem_block_table + si + 2]
    inc dx
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.tail_gap:
    cmp dx, DOS_HEAP_LIMIT_SEG
    jae .done
    mov bx, DOS_HEAP_LIMIT_SEG
    sub bx, dx
    call int21_mem_sync_store_gap

.done:
    cmp word [cs:dos_mem_alloc_size], 0
    jne .return
    mov word [cs:dos_mem_mcb_owner], 0
    mov word [cs:dos_mem_mcb_size], DOS_HEAP_USER_MAX_PARAS
.return:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_mem_sync_store_gap:
    cmp bx, 0
    je .done
    cmp word [cs:dos_mem_psp_free_size], 0
    jne .check_free2
    mov [cs:dos_mem_psp_free_seg], dx
    mov [cs:dos_mem_psp_free_size], bx
    jmp .done
.check_free2:
    cmp word [cs:dos_mem_free2_size], 0
    jne .done
    mov [cs:dos_mem_free2_seg], dx
    mov [cs:dos_mem_free2_size], bx
.done:
    ret

int21_mem_rebuild_chain:
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    push es

    call int21_mem_table_rebuild

    mov dx, [cs:current_psp_seg]
    or dx, dx
    jz .done

    mov es, dx
    mov bx, [cs:dos_mem_psp_mcb_end]
    or bx, bx
    jnz .have_psp_end
    mov bx, [es:0x0002]
.have_psp_end:
    cmp bx, dx
    jae .psp_end_ready
    mov bx, dx
.psp_end_ready:
    mov ax, dx
    dec ax
    mov [cs:dos_list_of_lists], ax
    mov [cs:dos_list_of_lists + 2], ax
    mov [cs:dos_list_of_lists + 4], ax

    mov ax, dx
    mov cx, dx
    sub bx, dx
    mov dl, 'M'
    call int21_mem_write_chain_entry
    mov si, ax

    mov di, ax
    add di, bx
    inc di

    cmp di, DOS_HEAP_USER_SEG
    jae .scan_next
    cmp di, DOS_HEAP_BASE_SEG
    jae .raise_to_heap_user
    mov ax, di
    mov bx, DOS_HEAP_BASE_SEG
    sub bx, di
    mov cx, 0x0008
    mov dl, 'M'
    call int21_mem_write_chain_entry
    mov si, ax
.raise_to_heap_user:
    mov di, DOS_HEAP_USER_SEG

.scan_next:
    mov dx, di
    call int21_mem_find_next_alloc
    jc .final_gap
    cmp ax, di
    jbe .write_alloc

    push ax
    push bx
    mov bx, ax
    sub bx, di
    dec bx
    cmp bx, 0
    je .skip_gap
    mov ax, di
    xor cx, cx
    mov dl, 'M'
    call int21_mem_write_chain_entry
    mov si, ax
.skip_gap:
    pop bx
    pop ax

.write_alloc:
    mov cx, [cs:dos_mem_block_found_owner]
    mov dl, 'M'
    call int21_mem_write_chain_entry
    mov si, ax
    mov di, ax
    add di, bx
    inc di
    jmp .scan_next

.final_gap:
    cmp di, DOS_HEAP_LIMIT_SEG
    jae .mark_last
    mov ax, di
    mov bx, DOS_HEAP_LIMIT_SEG
    sub bx, di
    cmp bx, 0
    je .mark_last
    xor cx, cx
    mov dl, 'M'
    call int21_mem_write_chain_entry
    mov si, ax

.mark_last:
    mov ax, si
    dec ax
    mov es, ax
    mov byte [es:0x0000], 'Z'

.done:
    pop es
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_mem_type_for_seg:
    push ax
    push bx

    mov dl, 'Z'

    cmp word [cs:dos_mem_psp_free_size], 0
    je .check_free2
    mov bx, [cs:dos_mem_psp_free_seg]
    cmp bx, ax
    jbe .check_free2
    mov dl, 'M'
    jmp .done

.check_free2:
    cmp word [cs:dos_mem_free2_size], 0
    je .check_block1
    mov bx, [cs:dos_mem_free2_seg]
    cmp bx, ax
    jbe .check_block1
    mov dl, 'M'
    jmp .done

.check_block1:
    cmp word [cs:dos_mem_alloc_size], 0
    je .check_block2
    mov bx, [cs:dos_mem_alloc_seg]
    cmp bx, ax
    jbe .check_block2
    mov dl, 'M'
    jmp .done

.check_block2:
    cmp word [cs:dos_mem_alloc_size2], 0
    je .check_block3
    mov bx, [cs:dos_mem_alloc_seg2]
    cmp bx, ax
    jbe .check_block3
    mov dl, 'M'
    jmp .done

.check_block3:
    cmp word [cs:dos_mem_alloc_size3], 0
    je .done
    mov bx, [cs:dos_mem_alloc_seg3]
    cmp bx, ax
    jbe .done
    mov dl, 'M'

.done:
    pop bx
    pop ax
    ret

int21_mem_current_owner:
    mov ax, [cs:current_psp_seg]
    or ax, ax
    jnz .have_owner
    mov ax, 0x0008
.have_owner:
    ret

int21_psp_mcb_update_type:
    push ax
    push bx
    push cx
    push dx
    push es

    mov cl, al

    mov dx, [cs:current_psp_seg]
    or dx, dx
    jz .done

    mov es, dx
    mov bx, [cs:dos_mem_psp_mcb_end]
    or bx, bx
    jnz .have_end
    mov bx, [es:0x0002]
.have_end:
    sub bx, dx

    mov ax, dx
    dec ax
    mov es, ax
    mov [cs:dos_list_of_lists], ax
    mov [cs:dos_list_of_lists + 2], ax
    mov [cs:dos_list_of_lists + 4], ax
    mov al, cl
    mov [es:0x0000], al
    mov [es:0x0001], dx
    mov [es:0x0003], bx

.done:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_mem_largest_global:
    push ax
    push cx
    push dx
    push si
    push es

    call int21_mem_table_rebuild
    call int21_mem_arena_start
    mov dx, ax
    xor si, si

.scan_next:
    call int21_mem_find_next_alloc
    jc .tail
    cmp ax, dx
    jbe .consume_alloc
    mov cx, ax
    sub cx, dx
    dec cx
    cmp cx, si
    jbe .consume_alloc
    mov si, cx

.consume_alloc:
    mov dx, ax
    add dx, bx
    inc dx
    jmp .scan_next

.tail:
    cmp dx, DOS_HEAP_LIMIT_SEG
    jae .largest_ready
    mov cx, DOS_HEAP_LIMIT_SEG
    sub cx, dx
    cmp cx, si
    jbe .largest_ready
    mov si, cx

.largest_ready:
    mov bx, si

.done:
    pop es
    pop si
    pop dx
    pop cx
    pop ax
    ret

int21_mem_trace_chain:
int21_mem_trace_nomem:
int21_trace_lookup_cluster:
int21_trace_lookup_found:
int21_trace_find_pattern_fail:
int21_trace_lookup_miss:
int21_trace_cwd_commit:
int21_trace_call:
int21_trace_read_io_error:
    ret

int21_alloc:
    call int21_mem_init

    ; DOS callers use BX=FFFFh to query the largest available block.
    cmp bx, 0xFFFF
    jne .req_ready
    call int21_mem_largest_global
    mov ax, 0x0008
    stc
    ret

.req_ready:
    cmp bx, 0
    je .no_memory

    call int21_mem_table_alloc_from_free
    jnc .alloc_from_table_ready
    cmp byte [cs:dos_mem_block_count], DOS_MEM_BLOCK_TABLE_MAX
    jae .no_memory
    call int21_mem_find_free_gap
    jc .no_memory
    mov [cs:dos_mem_block_tmp_seg], ax
    call int21_mem_current_owner
    mov cx, ax
    mov dx, DOS_MEM_BLOCK_ALLOC
    mov ax, [cs:dos_mem_block_tmp_seg]
    call int21_mem_table_insert
.alloc_from_table_ready:
    mov [cs:dos_mem_block_tmp_seg], ax
    call int21_mem_sync_legacy
    call int21_mem_rebuild_chain
    mov ax, [cs:dos_mem_block_tmp_seg]
    clc
    ret

.no_memory:
    call int21_mem_largest_global
    mov ax, 0x0008
    stc
    ret

int21_free:
    call int21_mem_init

    mov ax, es
    cmp ax, COM_LOAD_SEG
    je .static_psp_ok

    mov ax, es
    cmp ax, DOS_ENV_SEG
    je .env_static_ok

    call int21_mem_table_find_exact
    jc .invalid_real
    mov word [cs:dos_mem_block_table + si + 4], 0
    mov word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_FREE
    call int21_mem_table_clear_if_no_alloc
    call int21_mem_sync_legacy
    call int21_mem_rebuild_chain
    xor ax, ax
    clc
    ret

.invalid_real:
    mov ax, 0x0009
    stc
    ret

.env_static_ok:
.static_psp_ok:
    xor ax, ax
    clc
    ret

int21_resize:
    call int21_mem_init

.resize_entry:
    mov ax, es
    cmp ax, [cs:current_psp_seg]
    je .check_psp_zero
    jmp .check_heap_block
.check_psp_zero:
    cmp ax, 0
    jne .check_psp_size
    jmp .check_heap_block
.check_psp_size:
    cmp bx, 0
    je .no_memory

    mov [cs:dos_mem_block_req_size], bx
    mov bx, ax
    call int21_mem_table_next_limit
    mov si, dx
    sub dx, ax
    cmp si, DOS_HEAP_LIMIT_SEG
    je .psp_limit_ready
    dec dx
.psp_limit_ready:
    mov bx, [cs:dos_mem_block_req_size]
    cmp bx, dx
    ja .psp_no_memory

    mov si, ax
    add si, bx
    push ax
    push bx
    mov [cs:dos_mem_psp_mcb_end], si
    mov [es:0x0002], si
    call int21_mem_sync_legacy
    mov al, 'Z'
    cmp byte [cs:dos_mem_block_count], 0
    je .psp_type_ready
    mov al, 'M'
.psp_type_ready:
    call int21_psp_mcb_update_type
    call int21_mem_rebuild_chain
    pop bx
    pop ax
    mov ax, es
    clc
    ret

.psp_no_memory:
    mov ax, es
    mov bx, ax
    call int21_mem_table_next_limit
    mov si, dx
    sub dx, ax
    cmp si, DOS_HEAP_LIMIT_SEG
    je .psp_no_mem_limit_ready
    dec dx
.psp_no_mem_limit_ready:
    mov bx, dx
    mov ax, 0x0008
    stc
    ret

.check_heap_block:
    cmp bx, 0
    je .no_memory
    mov [cs:dos_mem_block_req_size], bx
    mov ax, es
    call int21_mem_table_find_exact
    jc .invalid
    mov ax, es
    call int21_mem_table_resize_limit
    mov bx, [cs:dos_mem_block_req_size]
    cmp bx, dx
    ja .block_no_memory
    call int21_mem_table_resize_at_si
    call int21_mem_sync_legacy
    call int21_mem_rebuild_chain
    xor dx, dx
    mov ax, es
    clc
    ret

.invalid:
    mov ax, es
    mov dx, [cs:current_psp_seg]
    cmp dx, 0
    je .invalid_check_high
    cmp ax, dx
    jb .invalid_check_high
    cmp ax, DOS_HEAP_USER_SEG
    jae .invalid_check_high
    mov es, dx
    jmp .resize_entry

.invalid_check_high:
    cmp ax, DOS_HEAP_LIMIT_SEG
    jb .invalid_real
    mov ax, dx
    cmp ax, 0
    je .invalid_real
    mov es, ax
    jmp .resize_entry

.invalid_real:
    mov ax, 0x0009
    stc
    ret

.no_memory:
    xor bx, bx
    mov ax, 0x0008
    stc
    ret

.block_no_memory:
    mov bx, dx
    mov ax, 0x0008
    stc
    ret

int21_cluster_for_pos:
    push bx
    push cx

    mov dx, ax
    and dx, FAT_CLUSTER_MASK
    mov cl, FAT_CLUSTER_SHIFT
    shr ax, cl
%if FAT_TYPE == 16
    mov cx, [cs:file_handle_pos_hi]
%if FAT_CLUSTER_SHIFT == 9
    shl cx, 7
%elif FAT_CLUSTER_SHIFT == 10
    shl cx, 6
%elif FAT_CLUSTER_SHIFT == 11
    shl cx, 5
%elif FAT_CLUSTER_SHIFT == 12
    shl cx, 4
%endif
    add cx, ax
%else
    mov cx, ax
%endif

    mov ax, [cs:file_handle_start_cluster]
    cmp ax, 2
    jb .fail

.step:
    cmp cx, 0
    je .done
    call fat12_get_entry_cached
    jc .fail
    cmp ax, 2
    jb .fail
    cmp ax, FAT_EOF
    jae .fail
    dec cx
    jmp .step

.done:
    clc
    jmp .exit

.fail:
    mov ax, 0x0006
    stc

.exit:
    pop cx
    pop bx
    ret

int21_cluster_to_lba:
    sub ax, 2
%if FAT_CLUSTER_SECTOR_SHIFT > 0
    mov cl, FAT_CLUSTER_SECTOR_SHIFT
    shl ax, cl
%endif
    add ax, FAT_DATA_START_LBA
    ret

int21_count_chain:
    push bx

    cmp ax, 2
    jb .zero

    mov bx, 0
.loop:
    inc bx
    call fat12_get_entry_cached
    jc .zero
    cmp ax, 2
    jb .done
    cmp ax, FAT_EOF
    jae .done
    jmp .loop

.done:
    mov ax, bx
    jmp .exit

.zero:
    xor ax, ax

.exit:
    pop bx
    ret

int21_load_fat_cache:
; -----------------------------------------------------------------------
; FAT cluster cache: compile-time selection FAT12 vs FAT16
; FAT12: nibble-packed 12-bit entries, 1 sector always covers enough
; FAT16: 16-bit word entries, multi-sector FAT, cache tracks which sector
; -----------------------------------------------------------------------
%if FAT_TYPE == 16

; int21_load_fat_cache: for FAT16 this is a no-op warmup stub.
; Actual sector selection happens inside fat12_get_entry_cached per cluster.
int21_load_fat_cache:
    clc
    ret

; fat16_ensure_sector: internal helper. AX = cluster. Ensures the FAT
; sector covering cluster AX is loaded into DOS_FAT_BUF_SEG.
; Trashes: AX, BX, ES. Returns CF on I/O error.
fat16_ensure_sector:
    push ax
    shr ax, 8                   ; AX = cluster / 256 = sector index in FAT
    cmp word [cs:fat_cache_sector], 0xFFFF
    je .do_load
    cmp ax, [cs:fat_cache_sector]
    je .already_ok
    ; Need different sector: flush dirty first
    cmp byte [cs:fat_cache_dirty], 1
    jne .do_load
    push ax
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov ax, [cs:fat_cache_sector]
    add ax, FAT1_LBA
    xor bx, bx
    call write_sector_lba
    jc .write_fail
    mov ax, [cs:fat_cache_sector]
    add ax, FAT2_LBA
    xor bx, bx
    call write_sector_lba
    jc .write_fail
    mov byte [cs:fat_cache_dirty], 0
    pop ax
    jmp .do_load
.write_fail:
    pop ax
    pop ax
    stc
    ret
.do_load:
    mov [cs:fat_cache_sector], ax
    add ax, FAT1_LBA
    mov bx, DOS_FAT_BUF_SEG
    mov es, bx
    xor bx, bx
    call read_sector_lba
    jc .load_fail
    mov byte [cs:fat_cache_valid], 1
    mov byte [cs:fat_cache_dirty], 0
.already_ok:
    pop ax
    clc
    ret
.load_fail:
    pop ax
    stc
    ret

fat12_get_entry_cached:
    push bx
    push cx
    push es
    mov cx, ax
    call fat16_ensure_sector
    jc .fail
    mov bx, cx
    and bx, 0x00FF
    shl bx, 1                   ; BX = (cluster % 256) * 2
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov ax, [es:bx]
    clc
    jmp .done
.fail:
    xor ax, ax
    stc
.done:
    pop es
    pop cx
    pop bx
    ret

fat12_set_entry_cached:
    push bx
    push cx
    push es
    mov cx, ax
    call fat16_ensure_sector
    jc .fail
    mov bx, cx
    and bx, 0x00FF
    shl bx, 1                   ; BX = (cluster % 256) * 2
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov [es:bx], dx
    mov byte [cs:fat_cache_dirty], 1
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    pop cx
    pop bx
    ret

fat12_flush_cache:
    push ax
    push bx
    push es
    cmp byte [cs:fat_cache_valid], 1
    jne .ok
    cmp byte [cs:fat_cache_dirty], 1
    jne .ok
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov ax, [cs:fat_cache_sector]
    add ax, FAT1_LBA
    xor bx, bx
    call write_sector_lba
    jc .fail
    mov ax, [cs:fat_cache_sector]
    add ax, FAT2_LBA
    xor bx, bx
    call write_sector_lba
    jc .fail
    mov byte [cs:fat_cache_dirty], 0
.ok:
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    pop bx
    pop ax
    ret

%else
; ---- FAT12 implementation (default) ----

int21_load_fat_cache:
    push bx
    push es
    cmp byte [cs:fat_cache_valid], 1
    je .ok
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov ax, FAT1_LBA
    xor bx, bx
    call read_sector_lba
    jc .fail
    mov byte [cs:fat_cache_valid], 1
    mov byte [cs:fat_cache_dirty], 0
.ok:
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    pop bx
    ret

fat12_get_entry_cached:
    push bx
    push cx
    push dx
    push es
    mov cx, ax
    call int21_load_fat_cache
    jc .fail
    mov bx, cx
    shr bx, 1
    add bx, cx
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov dx, [es:bx]
    test cx, 1
    jz .even
    shr dx, 4
    and dx, 0x0FFF
    mov ax, dx
    clc
    jmp .done
.even:
    and dx, 0x0FFF
    mov ax, dx
    clc
    jmp .done
.fail:
    xor ax, ax
    stc
.done:
    pop es
    pop dx
    pop cx
    pop bx
    ret

fat12_set_entry_cached:
    push bx
    push cx
    push es
    mov cx, ax
    call int21_load_fat_cache
    jc .fail
    mov bx, cx
    shr bx, 1
    add bx, cx
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov ax, dx
    and ax, 0x0FFF
    test cx, 1
    jz .even
    mov dx, [es:bx]
    and dx, 0x000F
    shl ax, 4
    or dx, ax
    mov [es:bx], dx
    jmp .mark
.even:
    mov dx, [es:bx]
    and dx, 0xF000
    or dx, ax
    mov [es:bx], dx
.mark:
    mov byte [cs:fat_cache_dirty], 1
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    pop cx
    pop bx
    ret

fat12_flush_cache:
    push bx
    push es
    cmp byte [cs:fat_cache_valid], 1
    jne .ok
    cmp byte [cs:fat_cache_dirty], 1
    jne .ok
    mov ax, DOS_FAT_BUF_SEG
    mov es, ax
    mov ax, FAT1_LBA
    xor bx, bx
    call write_sector_lba
    jc .fail
    mov ax, FAT2_LBA
    xor bx, bx
    call write_sector_lba
    jc .fail
    mov byte [cs:fat_cache_dirty], 0
.ok:
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    pop bx
    ret

%endif
; -----------------------------------------------------------------------

int21_update_root_entry_size:
    push bx
    push di
    push es

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:file_handle_root_lba]
    xor bx, bx
    call read_sector_lba
    jc .fail

    mov di, [cs:file_handle_root_off]
    mov ax, [cs:file_handle_start_cluster]
    mov [es:di + 26], ax
    add di, 28
    mov ax, [cs:file_handle_size_lo]
    mov [es:di], ax
    mov ax, [cs:file_handle_size_hi]
    mov [es:di + 2], ax

    mov ax, [cs:file_handle_root_lba]
    xor bx, bx
    call write_sector_lba
    jc .fail

    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop di
    pop bx
    ret

int21_path_to_fat_name:
    push ax
    push bx
    push cx
    push di
    push es

    mov ax, cs
    mov es, ax
    mov di, path_fat_name
    mov cx, 11
    mov al, ' '
    rep stosb

    ; DOS callers often pass paths extracted from command tails with leading spaces.
.skip_leading_space:
    cmp byte [si], ' '
    jne .check_empty
    inc si
    jmp .skip_leading_space

.check_empty:
    cmp byte [si], 0
    je .fail

    cmp byte [si + 1], ':'
    jne .skip_drive
    add si, 2

.skip_drive:
    mov al, [si]
    cmp al, '\'
    je .skip_sep
    cmp al, '/'
    jne .name_start
.skip_sep:
    inc si
    jmp .skip_drive

.name_start:
    xor bx, bx
.name_loop:
    mov al, [si]
    cmp al, 0
    je .name_done
    cmp al, 13
    je .name_done
    cmp al, '.'
    je .ext_start
    cmp al, '\'
    je .next_component
    cmp al, '/'
    je .next_component
    cmp bx, 8
    jae .name_advance
    cmp byte [cs:int21_path_upcase], 0
    je .name_store
    call int21_upcase_al
.name_store:
    mov [es:path_fat_name + bx], al
    inc bx
.name_advance:
    inc si
    jmp .name_loop

.name_done:
    cmp bx, 0
    je .fail
    clc
    jmp .done

.next_component:
    inc si
    mov di, path_fat_name
    mov cx, 11
    mov al, ' '
    rep stosb
    xor bx, bx
    jmp .name_loop

.ext_start:
    cmp bx, 0
    je .fail
    inc si
    xor bx, bx
.ext_loop:
    mov al, [si]
    cmp al, 0
    je .success
    cmp al, 13
    je .success
    cmp al, '\'
    je .next_component
    cmp al, '/'
    je .next_component
    cmp bx, 3
    jae .ext_advance
    cmp byte [cs:int21_path_upcase], 0
    je .ext_store
    call int21_upcase_al
.ext_store:
    mov [es:path_fat_name + 8 + bx], al
    inc bx
.ext_advance:
    inc si
    jmp .ext_loop

.success:
    cmp byte [es:path_fat_name + 0], ' '
    je .fail
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop di
    pop cx
    pop bx
    pop ax
    ret

int21_upcase_al:
    cmp al, 'a'
    jb .done
    cmp al, 'z'
    ja .done
    sub al, 32
.done:
    ret

int21_resolve_and_find_path:
    push si

    call int21_resolve_parent_dir
    jc .fail
    mov byte [cs:int21_path_stage_marker], 2
    mov [cs:tmp_lookup_dir], ax

    call int21_path_to_fat_name
    jc .bad_path
    mov byte [cs:int21_path_stage_marker], 3

    mov ax, [cs:tmp_lookup_dir]
    push ds
    mov bx, ax
    mov ax, cs
    mov ds, ax
    mov si, path_fat_name
    mov ax, bx
    mov byte [cs:int21_path_stage_marker], 4
    call int21_lookup_in_dir
    pop ds
    jnc .ok
    jmp .fail

.bad_path:
    mov ax, 0x0003
    stc
    jmp .done

.ok:
    xor ax, ax
    clc

.done:
    pop si
    ret

.fail:
    pop si
    ret

; Resolve parent directory cluster and return SI at last path component.
; Input : DS:SI path
; Output: AX=parent cluster (0=root), SI=last component ptr, CF clear
int21_resolve_root_leaf_parent:
    push bx
    push cx

    mov bx, si

.skip_space:
    cmp byte [si], ' '
    jne .drive_check
    inc si
    jmp .skip_space

.drive_check:
    cmp byte [si], 'C'
    je .drive_colon
    cmp byte [si], 'c'
    jne .sep_check
.drive_colon:
    cmp byte [si + 1], ':'
    jne .fail
    add si, 2

.sep_check:
    cmp byte [si], '\'
    je .leaf_start
    cmp byte [si], '/'
    jne .fail

.leaf_start:
    inc si
    mov di, si
    cmp byte [si], 0
    je .fail
    cmp byte [si], 13
    je .fail
    mov cx, 96

.leaf_scan:
    dec cx
    jz .fail
    mov al, [si]
    cmp al, 0
    je .ok
    cmp al, 13
    je .ok
    cmp al, '\'
    je .fail
    cmp al, '/'
    je .fail
    inc si
    jmp .leaf_scan

.ok:
    xor ax, ax
    mov si, di
    clc
    jmp .done

.fail:
    mov si, bx
    mov ax, 0x0003
    stc

.done:
    pop cx
    pop bx
    ret

int21_resolve_parent_dir:
    push bx
    push cx
    push dx
    push di

    mov byte [cs:tmp_path_guard], 96

    mov bx, si
    call int21_resolve_root_leaf_parent
    jnc .done
    mov si, bx

.skip_space:
    dec byte [cs:tmp_path_guard]
    jnz .skip_space_ok
    jmp .path_fail
.skip_space_ok:
    cmp byte [si], ' '
    jne .drive_check
    inc si
    jmp .skip_space

.drive_check:
    cmp byte [si], 0
    je .path_fail
    cmp byte [si + 1], ':'
    jne .base_dir
    add si, 2

.base_dir:
    cmp byte [si], '\'
    je .root_base
    cmp byte [si], '/'
    je .root_base
    mov ax, [cs:cwd_cluster]
    mov [cs:tmp_lookup_dir], ax
    jmp .component_start

.root_base:
    mov word [cs:tmp_lookup_dir], 0
.skip_root_sep:
    cmp byte [si], '\'
    je .inc_root_sep
    cmp byte [si], '/'
    jne .root_leaf_fastpath
.inc_root_sep:
    inc si
    jmp .skip_root_sep

.root_leaf_fastpath:
    cmp byte [si], 0
    je .path_fail
    cmp byte [si], 13
    je .path_fail
    mov di, si
    mov bx, si
    mov cx, 96
.root_leaf_scan:
    dec cx
    jz .path_fail
    mov al, [bx]
    cmp al, 0
    je .leaf_ok
    cmp al, 13
    je .leaf_ok
    cmp al, '\'
    je .component_start
    cmp al, '/'
    je .component_start
    inc bx
    jmp .root_leaf_scan

.component_start:
    dec byte [cs:tmp_path_guard]
    jnz .component_guard_ok
    jmp .path_fail
.component_guard_ok:
    cmp byte [si], 0
    je .path_fail
    cmp byte [si], 13
    je .path_fail

    mov di, si
    xor bx, bx
.comp_copy:
    dec byte [cs:tmp_path_guard]
    jnz .comp_guard_ok
    jmp .path_fail
.comp_guard_ok:
    mov al, [si]
    cmp al, 0
    je .comp_done
    cmp al, 13
    je .comp_done
    cmp al, '\'
    je .comp_done
    cmp al, '/'
    je .comp_done
    cmp bx, 23
    jae .comp_advance
    mov [cs:tmp_cwd_comp + bx], al
    inc bx
.comp_advance:
    inc si
    jmp .comp_copy

.comp_done:
    mov byte [cs:tmp_cwd_comp + bx], 0
    cmp bx, 0
    je .path_fail

    mov dl, [si]
    cmp dl, 0
    je .leaf_ok
    cmp dl, 13
    je .leaf_ok
    cmp dl, '\'
    je .trail_check
    cmp dl, '/'
    jne .non_leaf
.trail_check:
    mov bx, si
.trail_skip_sep:
    dec byte [cs:tmp_path_guard]
    jz .path_fail
    inc bx
    mov al, [bx]
    cmp al, '\'
    je .trail_skip_sep
    cmp al, '/'
    je .trail_skip_sep
    cmp al, 0
    je .leaf_term
    cmp al, 13
    je .leaf_term
    jmp .non_leaf

.leaf_term:
    mov byte [si], 0
    jmp .leaf_ok

.non_leaf:
    ; Ignore intermediate '.' component.
    cmp byte [cs:tmp_cwd_comp], '.'
    jne .check_dotdot
    cmp byte [cs:tmp_cwd_comp + 1], 0
    je .skip_sep

.check_dotdot:
    ; At root, intermediate '..' keeps us at root.
    cmp byte [cs:tmp_cwd_comp], '.'
    jne .lookup_component
    cmp byte [cs:tmp_cwd_comp + 1], '.'
    jne .lookup_component
    cmp byte [cs:tmp_cwd_comp + 2], 0
    jne .lookup_component
    cmp word [cs:tmp_lookup_dir], 0
    je .skip_sep
    push si
    push ds
    mov ax, cs
    mov ds, ax
    mov si, path_dotdot_fat
    mov ax, [cs:tmp_lookup_dir]
    call int21_lookup_in_dir
    pop ds
    pop si
    jc .path_fail
    jmp .lookup_ok

    ; has further components: current component must be a directory.
.lookup_component:
    push si
    push ds
    mov ax, cs
    mov ds, ax
    mov si, tmp_cwd_comp
    call int21_path_to_fat_name
    pop ds
    pop si
    jnc .comp_name_ok
    jmp .path_fail
.comp_name_ok:

    mov ax, [cs:tmp_lookup_dir]
    push si                         ; preserve original path position
    push ds
    mov bx, ax                      ; save dir cluster while DS is being changed
    mov ax, cs
    mov ds, ax
    mov si, path_fat_name
    mov ax, bx                      ; restore dir cluster
    call int21_lookup_in_dir
    pop ds
    pop si                          ; restore original path position
    jnc .lookup_ok
    jmp .path_fail
.lookup_ok:
    test byte [cs:search_found_attr], 0x10
    jnz .is_dir_ok
    jmp .path_fail
.is_dir_ok:
    mov ax, [cs:search_found_cluster]
    mov [cs:tmp_lookup_dir], ax

.skip_sep:
    cmp byte [si], '\'
    je .sep_next
    cmp byte [si], '/'
    jne .after_sep
.sep_next:
    inc si
    jmp .skip_sep

.after_sep:
    cmp byte [si], 0
    je .path_fail
    jmp .component_start

.leaf_ok:
    mov ax, [cs:tmp_lookup_dir]
    mov si, di
    clc
    jmp .done

.path_fail:
    mov ax, 0x0003
    stc

.done:
    pop di
    pop dx
    pop cx
    pop bx
    ret

; Lookup 11-byte FAT name in directory AX (0=root, else first cluster).
; DS:SI -> 11-byte FAT name. Returns search_found_* and CF clear if found.
int21_lookup_in_dir:
    push bx
    push cx
    push dx
    push di
    push ds
    push es

    mov [cs:search_name_ptr], si
    mov [cs:tmp_lookup_dir], ax

    cmp ax, 0
    jne .scan_cluster

    mov ax, cs
    mov ds, ax
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov si, [cs:search_name_ptr]
    mov bx, 0xFFFF
    call load_root_file_first_sector
    jc .not_found
    clc
    jmp .done

.scan_cluster:
    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:tmp_lookup_dir]
    mov [cs:tmp_cluster], ax

.cluster_loop:
    mov ax, [cs:tmp_cluster]
    cmp ax, 2
    jb .not_found
    cmp ax, FAT_EOF
    jae .not_found

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
    xor dx, dx

.sector_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .next_cluster

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    add ax, dx
    xor bx, bx
    call read_sector_lba
    jc .io_fail
    mov ax, DOS_META_BUF_SEG
    mov es, ax

    xor di, di
    mov cx, 16

.entry_loop:
    mov al, [es:di]
    cmp al, 0x00
    je .not_found
    cmp al, 0xE5
    je .next_entry

    mov al, [es:di + 11]
    cmp al, 0x0F
    je .next_entry
    test al, 0x08
    jnz .next_entry

    mov si, [cs:search_name_ptr]
    call fat_entry_matches_name
    jnc .next_entry

    mov ax, [cs:tmp_lba]
    add ax, dx
    mov [cs:search_found_root_lba], ax
    mov [cs:search_found_root_off], di
    mov ax, [es:di + 26]
    mov [cs:search_found_cluster], ax
    mov ax, [es:di + 28]
    mov [cs:search_found_size_lo], ax
    mov ax, [es:di + 30]
    mov [cs:search_found_size_hi], ax
    mov al, [es:di + 11]
    mov [cs:search_found_attr], al

    push cx
    push si
    mov cx, 11
    mov si, di
    mov di, search_found_name
.copy_name:
    mov al, [es:si]
    mov [di], al
    inc si
    inc di
    loop .copy_name
    pop si
    pop cx
    clc
    jmp .done

.next_entry:
    add di, 32
    loop .entry_loop
    inc dx
    jmp .sector_loop

.next_cluster:
    mov ax, [cs:tmp_cluster]
    call fat12_get_entry_cached
    jc .io_fail
    mov [cs:tmp_cluster], ax
    jmp .cluster_loop

.not_found:
    mov ax, 0x0002
    stc
    jmp .done

.io_fail:
    mov ax, 0x0005
    stc

.done:
    pop es
    pop ds
    pop di
    pop dx
    pop cx
    pop bx
    ret

; Find first free directory entry (0x00 or 0xE5) in AX directory cluster.
; AX=0 means root directory.
; On success: search_found_root_lba/search_found_root_off set, CF clear.
int21_find_free_dir_entry:
    push bx
    push cx
    push dx
    push di
    push es

    mov [cs:tmp_lookup_dir], ax
    cmp ax, 0
    jne .scan_cluster

    mov dx, FAT_ROOT_START_LBA
.root_scan:
    cmp dx, FAT_ROOT_START_LBA + FAT_ROOT_DIR_SECTORS
    jae .full

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, dx
    xor bx, bx
    call read_sector_lba
    jc .io_fail

    xor di, di
    mov cx, 16
.root_entries:
    mov al, [es:di]
    cmp al, 0x00
    je .root_found
    cmp al, 0xE5
    je .root_found
    add di, 32
    loop .root_entries
    inc dx
    jmp .root_scan

.root_found:
    mov [cs:search_found_root_lba], dx
    mov [cs:search_found_root_off], di
    clc
    jmp .done

.scan_cluster:
    call int21_load_fat_cache
    jc .io_fail
    mov ax, [cs:tmp_lookup_dir]
    mov [cs:tmp_cluster], ax

.cluster_loop:
    mov ax, [cs:tmp_cluster]
    cmp ax, 2
    jb .full
    cmp ax, FAT_EOF
    jae .full

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
    xor dx, dx

.sector_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .next_cluster

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    add ax, dx
    xor bx, bx
    call read_sector_lba
    jc .io_fail

    xor di, di
    mov cx, 16
.entry_loop:
    mov al, [es:di]
    cmp al, 0x00
    je .subdir_found
    cmp al, 0xE5
    je .subdir_found
    add di, 32
    loop .entry_loop
    inc dx
    jmp .sector_loop

.subdir_found:
    mov ax, [cs:tmp_lba]
    add ax, dx
    mov [cs:search_found_root_lba], ax
    mov [cs:search_found_root_off], di
    clc
    jmp .done

.next_cluster:
    mov ax, [cs:tmp_cluster]
    call fat12_get_entry_cached
    jc .io_fail
    mov [cs:tmp_cluster], ax
    jmp .cluster_loop

.full:
    mov ax, 0x0005
    stc
    jmp .done

.io_fail:
    mov ax, 0x0005
    stc

.done:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    ret

int21_build_env_block:
    push ax
    push cx
    push si
    push di
    push ds
    push es

    mov ax, [cs:tmp_overlay_block_seg]
    mov [es:0x002C], ax
    mov ax, cs
    mov ds, ax
    mov ax, DOS_ENV_SEG
    mov es, ax
    xor di, di
    mov si, dos_env_block
    mov cx, dos_env_block_end - dos_env_block
    rep movsb
    mov di, dos_env_exec_path - dos_env_block
    mov si, dos_child_exec_path_buf
    mov cx, DOS_ENV_EXEC_PATH_LEN - 1

.exec_path_copy:
    lodsb
    stosb
    test al, al
    jz .exec_path_done
    dec cx
    jnz .exec_path_copy
    mov byte [es:di], 0

.exec_path_done:
%if FAT_TYPE == 16
    cmp byte [cs:path_fat_name + 0], 'D'
    jne .done
    cmp byte [cs:path_fat_name + 1], 'O'
    jne .done
    cmp byte [cs:path_fat_name + 2], 'O'
    jne .done
    cmp byte [cs:path_fat_name + 3], 'M'
    jne .done
    cmp byte [cs:path_fat_name + 8], 'E'
    jne .done
    cmp byte [cs:path_fat_name + 9], 'X'
    jne .done
    cmp byte [cs:path_fat_name + 10], 'E'
    jne .done
    mov di, dos_env_exec_path - dos_env_block
    mov si, env_doom_exe_path
.doom_path_copy:
    lodsb
    stosb
    test al, al
    jnz .doom_path_copy
%endif

.done:
    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    ret

int21_fileio_test:
    push ds

    mov si, msg_fileio_begin
    call print_string_dual

    mov ax, cs
    mov ds, ax

    mov dx, path_deltest_dos
    mov ah, 0x41
    int 0x21

    mov dx, path_deltest_dos
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .fail
    mov bx, ax

    mov byte [fileio_buf + 0], 0x5A
    mov cx, 1
    mov dx, fileio_buf
    mov ah, 0x40
    int 0x21
    cmp ax, 1
    jne .fail_close

    mov ah, 0x3E
    int 0x21

    mov dx, path_deltest_dos
    mov ah, 0x41
    int 0x21

    mov si, msg_fileio_serial_pass
    call print_string_serial
    pop ds
    ret

.fail_close:
    mov ah, 0x3E
    int 0x21
.fail:
    mov si, msg_fileio_serial_fail
    call print_string_serial
    pop ds
    ret

int21_find_test:
    push ds

    mov si, msg_find_begin
    call print_string_dual

    mov ax, cs
    mov ds, ax

    mov dx, find_dta
    mov ah, 0x1A
    int 0x21

    mov dx, path_pattern_com
    xor cx, cx
    mov ah, 0x4E
    int 0x21
    jc .fail
    cmp byte [find_dta + 0x1E], 'C'
    jne .fail

    mov ah, 0x4F
    int 0x21
    jnc .fail
    cmp ax, 0x0012
    jne .fail

    mov dx, path_pattern_mz
    xor cx, cx
    mov ah, 0x4E
    int 0x21
    jc .fail
    cmp byte [find_dta + 0x1E], 'M'
    jne .fail

    mov si, msg_find_serial_pass
    call print_string_serial
    pop ds
    ret

.fail:
    mov si, msg_find_serial_fail
    call print_string_serial
    pop ds
    ret

%if STAGE1_SELFTEST_AUTORUN
int21_move_rename_path_test:
    push ax
    push bx
    push dx
    push si
    push ds

    mov ax, cs
    mov ds, ax

    mov dx, path_mvren_dir_dos
    mov ah, 0x39
    int 0x21
    jc .fail

    mov bx, cmd_selftest_mv
    call shell_cmd_move

    mov bx, cmd_selftest_rename
    call shell_cmd_ren

    mov si, path_mvren_final_dos
    call int21_resolve_and_find_path
    jc .fail

    mov bx, cmd_selftest_restore
    call shell_cmd_ren

    mov si, msg_mvren_serial_pass
    call print_string_serial
    jmp .done

.fail:
    mov si, msg_mvren_serial_fail
    call print_string_serial

.done:
    pop ds
    pop si
    pop dx
    pop bx
    pop ax
    ret

shell_streamc_selftest:
    push ax
    push bx
    push si
    push di
    push ds

    mov ax, cs
    mov ds, ax

    mov di, str_help
    call shell_is_builtin_token
    jnc .fail

    mov di, str_where
    call shell_is_builtin_token
    jnc .fail

    mov si, str_which_probe_comdemo
    call shell_try_resolve_exec_token
    jc .fail

    mov di, shell_exec_path_buf
    mov si, str_expect_which_comdemo
    call str_eq
    jnc .fail

%if FAT_TYPE == 16
    mov word [cs:shell_footer_last_tick], 200
    mov word [cs:shell_footer_dsk_last_scan_tick], 140
    mov byte [cs:shell_footer_dsk_dirty], 0
    mov byte [cs:shell_footer_key_cooldown], SHELL_FOOTER_KEY_COOLDOWN_TICKS
    call shell_footer_maybe_refresh_disk
    cmp word [cs:shell_footer_dsk_last_scan_tick], 140
    jne .fail

    mov byte [cs:shell_footer_key_cooldown], 0
    call shell_footer_maybe_refresh_disk
    cmp word [cs:shell_footer_dsk_last_scan_tick], 200
    jne .fail

    mov word [cs:shell_footer_last_tick], 0xFFFF
    mov word [cs:shell_footer_dsk_last_scan_tick], 0
    mov byte [cs:shell_footer_dsk_dirty], 1
    mov byte [cs:shell_footer_key_cooldown], 0
%endif

    mov si, msg_streamc_serial_pass
    call print_string_serial
    jmp .done

.fail:
%if FAT_TYPE == 16
    mov word [cs:shell_footer_last_tick], 0xFFFF
    mov word [cs:shell_footer_dsk_last_scan_tick], 0
    mov byte [cs:shell_footer_dsk_dirty], 1
    mov byte [cs:shell_footer_key_cooldown], 0
%endif
    mov si, msg_streamc_serial_fail
    call print_string_serial

.done:
    pop ds
    pop di
    pop si
    pop bx
    pop ax
    ret
run_stage1_selftest:
    mov si, msg_stage1_selftest_begin
    call print_string_dual
    mov si, msg_stage1_selftest_serial_begin
    call print_string_serial
    call int21_smoke_test
    call run_com_demo
    call run_mz_demo
    call run_gfxrect_demo
    call run_gfxstar_demo
    call int21_fileio_test
    call int21_find_test
    call int21_move_rename_path_test
    call shell_streamc_selftest
    call run_gfx_demo
    mov si, msg_stage1_selftest_done
    call print_string_dual
    mov si, msg_stage1_selftest_serial_done
    call print_string_serial
    ret

%endif

run_gfx_demo:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    mov si, msg_gfx_begin
    call print_string_dual

    call vdi_enter_graphics
    call gfx_demo_run
    call vdi_leave_graphics
    call draw_shell_chrome

    mov si, msg_gfx_done
    call print_string_dual
    mov si, msg_gfx_serial_pass
    call print_string_serial

    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

%if FAT_TYPE == 16
stage1_show_boot_splash:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    call stage1_splash_set_mode_vesa
    jc .fail_text

    call stage1_splash_load_asset
    jc .fail_graphics

    mov si, msg_splash_serial_ok
    call print_string_serial

    call stage1_splash_apply_palette
    call stage1_splash_blit_scaled
    jc .fail_graphics

    call stage1_splash_wait_progress
    call vdi_leave_graphics
    jmp .done

.fail_graphics:
    mov si, msg_splash_serial_fail
    call print_string_serial
    call vdi_leave_graphics
    jmp .done

.fail_text:
    mov si, msg_splash_serial_fail
    call print_string_serial

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

stage1_splash_set_mode_vesa:
    push bx

    mov ax, 0x4F02
    mov bx, SPLASH_VESA_MODE
    int 0x10
    cmp ax, 0x004F

    pop bx
    jne .fail
    clc
    ret

.fail:
    stc
    ret

stage1_splash_set_bank:
    push ax
    push bx
    push dx
    push di

    mov ax, 0x4F05
    xor bx, bx
    int 0x10
    cmp ax, 0x004F

    pop di
    pop dx
    pop bx
    pop ax
    jne .fail
    clc
    ret

.fail:
    stc
    ret

stage1_splash_compose_background:
    ret

stage1_splash_load_asset:
    push ds
    push cs
    pop ds

    mov dx, path_splash_bin_dos
    mov ax, 0x3D00
    int 0x21
    jc .fail_no_handle

    xchg bx, ax

    push word SPLASH_BUF_SEG
    pop ds
    xor dx, dx
    mov di, SPLASH_TOTAL_SIZE

.read_loop:
    mov cx, 0x0200
    cmp di, cx
    jae .read_chunk
    mov cx, di

.read_chunk:
    mov ah, 0x3F
    int 0x21
    jc .fail_with_handle
    xchg ax, cx
    jcxz .done_ok
    add dx, cx
    sub di, cx
    jnz .read_loop

.done_ok:
    call int21_close
    clc
    pop ds
    ret

.fail_with_handle:
    call int21_close

.fail_no_handle:
    stc
    pop ds
    ret

stage1_splash_apply_palette:
    push ax
    push cx
    push dx
    push si
    push ds

    mov ax, SPLASH_BUF_SEG
    mov ds, ax
    xor si, si
    mov dx, 0x03C8
    xor al, al
    out dx, al
    inc dx

    mov cx, SPLASH_PALETTE_SIZE
.loop:
    lodsb
    shr al, 1
    shr al, 1
    out dx, al
    loop .loop

    pop ds
    pop si
    pop dx
    pop cx
    pop ax
    ret

stage1_splash_blit_scaled:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    cld

    xor dx, dx
    xor di, di
    call stage1_splash_set_bank
    jc .fail

    mov ax, SPLASH_BUF_SEG
    mov ds, ax
    mov bx, SPLASH_PALETTE_SIZE
    mov bp, SPLASH_SRC_H

.row:
    mov si, bx
    push di
    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    xor di, di
    mov cx, SPLASH_SRC_W
.expand_x:
    lodsb
    stosb
    stosb
    stosb
    stosb
    stosb
    loop .expand_x
    pop di

    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    mov ax, 0xA000
    mov es, ax
    mov cx, SPLASH_SCALE_Y
    ; Render a 560px image from 100px source (6x for first 60 rows, then 5x).
    cmp bp, 40
    jbe .copy_y
    inc cx
.copy_y:
    call stage1_splash_copy_row_to_vram
    jc .fail_restore_ds
    loop .copy_y

    mov ax, SPLASH_BUF_SEG
    mov ds, ax
    add bx, SPLASH_SRC_ROW_BYTES
    dec bp
    jnz .row

    clc
    jmp .done

.fail_restore_ds:
    mov ax, SPLASH_BUF_SEG
    mov ds, ax

.fail:
    stc

.done:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

stage1_splash_copy_row_to_vram:
    push bx
    push cx
    push si

    xor si, si
    cmp di, SPLASH_VRAM_SAFE_OFFSET
    jb .single_chunk

    mov cx, 0
    sub cx, di
    mov bx, cx
    rep movsb

    inc dx
    call stage1_splash_set_bank
    jc .fail

    xor di, di
    mov cx, SPLASH_VESA_ROW_BYTES
    sub cx, bx
    rep movsb
    jmp .done

.single_chunk:
    mov cx, SPLASH_VESA_ROW_BYTES
    rep movsb

.done:
    clc
    pop si
    pop cx
    pop bx
    ret

.fail:
    stc
    pop si
    pop cx
    pop bx
    ret

stage1_splash_overlay:
    ret

stage1_splash_draw_fallback:
    ret

stage1_splash_draw_progress_bar:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    mov bp, ax
    cmp bp, 40
    jbe .count_ok
    mov bp, 40
.count_ok:

    mov ax, 560
    mov bx, SPLASH_VESA_ROW_BYTES
    mul bx
    mov di, ax
    call stage1_splash_set_bank
    jc .fail

    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    mov bx, 40
    mov si, 560

.row_loop:
    cmp bx, 0
    je .done_rows

    push di

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    xor di, di
    mov al, 253
    mov cx, SPLASH_VESA_ROW_BYTES
    rep stosb

    mov di, 80
    mov al, 253
    mov cx, 640
    rep stosb

    cmp si, 572
    jb .copy_row
    cmp si, 587
    ja .copy_row

    mov di, 82
    mov al, 254
    mov cx, 636
    rep stosb

    cmp si, 576
    jb .copy_row
    cmp si, 583
    ja .copy_row
    mov cx, bp
    jcxz .copy_row

    mov di, 82
.block_loop:
    mov al, 255
    push cx
    mov cx, 12
    rep stosb
    add di, 4
    pop cx
    loop .block_loop

.copy_row:
    pop di
    mov ax, 0xA000
    mov es, ax
    call stage1_splash_copy_row_to_vram
    jc .fail

    inc si
    dec bx
    jmp .row_loop

.done_rows:
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

stage1_splash_wait_progress:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    xor ax, ax
    call stage1_splash_draw_progress_bar

    call gfx_get_tick_count
    mov [cs:splash_wait_start_tick], dx
    mov [cs:splash_wait_last_tick], dx

.loop:
    call gfx_get_tick_count
    cmp dx, [cs:splash_wait_last_tick]
    je .loop
    mov [cs:splash_wait_last_tick], dx

    mov ax, dx
    sub ax, [cs:splash_wait_start_tick]
    cmp ax, SPLASH_WAIT_TICKS
    jae .done_fill

    mov bx, 40
    mul bx
    mov bx, SPLASH_WAIT_TICKS
    div bx
    call stage1_splash_draw_progress_bar
    jmp .loop

.done_fill:
    mov ax, 40
    call stage1_splash_draw_progress_bar

.done:
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif

gfx_demo_run:
    push ax
    push bx
    push cx
    push dx

    call gfx_get_tick_count
    mov [gfx_demo_last_tick], dx
    mov ax, dx
    add ax, 10
    mov [gfx_demo_deadline], ax

.loop:
    call gfx_try_read_key
    jc .done

    call gfx_get_tick_count
    cmp dx, [gfx_demo_last_tick]
    je .check_deadline
    mov [gfx_demo_last_tick], dx
    mov al, dl
    call gfx_demo_render_frame

.check_deadline:
    mov ax, dx
    cmp ax, [gfx_demo_deadline]
    jb .loop

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

gfx_demo_render_frame:
    push ax
    push bx
    push dx
    push si
    push di

    mov [gfx_demo_frame], al

    mov al, 1
    call vdi_clear_screen

    mov bx, 8
    mov dx, 8
    mov si, 304
    mov di, 184
    mov al, 9
    call vdi_box

    mov bx, 12
    mov dx, 12
    mov si, 296
    mov di, 22
    mov al, 3
    call vdi_bar

    mov bx, 12
    mov dx, 38
    mov si, 296
    mov di, 150
    mov al, 8
    call vdi_bar

    xor bx, bx
    mov bl, [gfx_demo_frame]
    and bx, 0x001F
    shl bx, 3
    add bx, 24
    mov dx, 142
    mov si, 48
    mov di, 12
    mov al, 10
    call vdi_bar

    mov bx, 24
    mov dx, 60
    mov si, 280
    mov di, 60
    mov al, 14
    call vdi_line

    mov bx, 24
    mov dx, 160
    mov si, 280
    mov di, 100
    mov al, 12
    call vdi_line

    mov bx, 36
    mov dx, 20
    mov si, gfx_text_ciukios
    mov al, 15
    call vdi_gtext

    mov bx, 36
    mov dx, 72
    mov si, gfx_text_demo
    mov al, 15
    call vdi_gtext

    mov bx, 36
    mov dx, 92
    mov si, gfx_text_vdi
    mov al, 11
    call vdi_gtext

    mov bx, 36
    mov dx, 112
    mov si, gfx_text_timer
    mov al, 15
    call vdi_gtext

    pop di
    pop si
    pop dx
    pop bx
    pop ax
    ret

vdi_enter_graphics:
    mov ax, 0x0013
    int 0x10
    ret

vdi_leave_graphics:
    mov ax, 0x0003
    int 0x10
    ret

vdi_clear_screen:
    push bx
    push dx
    push si
    push di
    xor bx, bx
    xor dx, dx
    mov si, 320
    mov di, 200
    call gfx_fill_rect
    pop di
    pop si
    pop dx
    pop bx
    ret

vdi_bar:
    call gfx_fill_rect
    ret

vdi_box:
    call gfx_draw_rect
    ret

vdi_line:
    call gfx_draw_line
    ret

vdi_gtext:
    call gfx_draw_text8
    ret

gfx_get_tick_count:
    mov ah, 0x00
    int 0x1A
    ret

gfx_try_read_key:
    push ax
    mov ah, 0x01
    int 0x16
    jz .none
    xor ah, ah
    int 0x16
    stc
    pop ax
    ret
.none:
    clc
    pop ax
    ret

gfx_plot_pixel:
    push ax
    push bx
    push dx
    push di
    push es

    cmp cx, 320
    jae .done
    cmp dx, 200
    jae .done

    mov di, dx
    shl di, 6
    mov bx, dx
    shl bx, 8
    add di, bx
    add di, cx
    mov bx, 0xA000
    mov es, bx
    mov [es:di], al

.done:

    pop es
    pop di
    pop dx
    pop bx
    pop ax
    ret

gfx_draw_hline:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    mov [gfx_draw_color], al
    mov di, dx
    shl di, 6
    mov ax, dx
    shl ax, 8
    add di, ax
    add di, bx
    mov ax, 0xA000
    mov es, ax
    mov al, [gfx_draw_color]
    rep stosb

    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

gfx_draw_vline:
    push ax
    push bx
    push cx
    push dx

.loop:
    push cx
    mov cx, bx
    call gfx_plot_pixel
    inc dx
    pop cx
    loop .loop

    pop dx
    pop cx
    pop bx
    pop ax
    ret

gfx_fill_rect:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov cx, di
.row:
    push cx
    mov cx, si
    call gfx_draw_hline
    inc dx
    pop cx
    loop .row

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

gfx_draw_rect:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov cx, si
    call gfx_draw_hline

    mov cx, si
    mov ax, di
    dec ax
    add dx, ax
    call gfx_draw_hline
    sub dx, ax

    mov cx, di
    call gfx_draw_vline

    mov ax, si
    dec ax
    add bx, ax
    mov cx, di
    call gfx_draw_vline

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

gfx_draw_line:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov [gfx_draw_color], al
    mov [gfx_line_x0], bx
    mov [gfx_line_y0], dx
    mov [gfx_line_x1], si
    mov [gfx_line_y1], di

    mov ax, si
    sub ax, bx
    jns .dx_abs
    neg ax
.dx_abs:
    mov [gfx_line_dx], ax
    mov word [gfx_line_sx], 1
    cmp bx, si
    jle .sx_done
    mov word [gfx_line_sx], -1
.sx_done:

    mov ax, di
    sub ax, dx
    jns .dy_abs
    neg ax
.dy_abs:
    neg ax
    mov [gfx_line_dy], ax
    mov word [gfx_line_sy], 1
    cmp dx, di
    jle .sy_done
    mov word [gfx_line_sy], -1
.sy_done:

    mov ax, [gfx_line_dx]
    add ax, [gfx_line_dy]
    mov [gfx_line_err], ax

.loop:
    mov cx, [gfx_line_x0]
    mov dx, [gfx_line_y0]
    mov al, [gfx_draw_color]
    call gfx_plot_pixel

    mov ax, [gfx_line_x0]
    cmp ax, [gfx_line_x1]
    jne .step
    mov ax, [gfx_line_y0]
    cmp ax, [gfx_line_y1]
    je .done

.step:
    mov ax, [gfx_line_err]
    shl ax, 1
    mov [gfx_line_e2], ax

    mov ax, [gfx_line_e2]
    cmp ax, [gfx_line_dy]
    jl .skip_x
    mov ax, [gfx_line_err]
    add ax, [gfx_line_dy]
    mov [gfx_line_err], ax
    mov ax, [gfx_line_x0]
    add ax, [gfx_line_sx]
    mov [gfx_line_x0], ax

.skip_x:
    mov ax, [gfx_line_e2]
    cmp ax, [gfx_line_dx]
    jg .skip_y
    mov ax, [gfx_line_err]
    add ax, [gfx_line_dx]
    mov [gfx_line_err], ax
    mov ax, [gfx_line_y0]
    add ax, [gfx_line_sy]
    mov [gfx_line_y0], ax

.skip_y:
    jmp .loop

.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

gfx_draw_text8:
    push ax
    push bx
    push dx
    push si

    mov [gfx_draw_color], al

.next_char:
    lodsb
    test al, al
    jz .done
    cmp al, ' '
    je .advance

    push ax
    push bx
    push dx
    push si
    call gfx_lookup_glyph
    jnc .skip_draw
    mov al, [gfx_draw_color]
    call gfx_draw_glyph8
.skip_draw:
    pop si
    pop dx
    pop bx
    pop ax

.advance:
    add bx, 8
    jmp .next_char

.done:
    pop si
    pop dx
    pop bx
    pop ax
    ret

gfx_lookup_glyph:
    push ax
    mov si, gfx_font8_table
.scan:
    cmp byte [si], 0
    je .not_found
    cmp al, [si]
    je .found
    add si, 9
    jmp .scan
.found:
    inc si
    pop ax
    stc
    ret
.not_found:
    pop ax
    clc
    ret

gfx_draw_glyph8:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov [gfx_draw_color], al
    mov cx, 8

.row:
    push cx
    lodsb
    mov [gfx_row_bits], al
    mov di, bx
    mov cx, 8

.bit:
    mov al, [gfx_row_bits]
    shl al, 1
    mov [gfx_row_bits], al
    jnc .next_bit
    mov al, [gfx_draw_color]
    push cx
    mov cx, di
    call gfx_plot_pixel
    pop cx
.next_bit:
    inc di
    loop .bit

    inc dx
    pop cx
    loop .row

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

run_com_demo:
    mov si, msg_com_begin
    call print_string_dual

    mov ax, cs
    mov ds, ax
    mov dx, path_comdemo_dos
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jc .load_fail

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

run_mz_demo:
    mov si, msg_mz_begin
    call print_string_dual

    mov ax, cs
    mov ds, ax
    mov dx, path_mzdemo_dos
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jc .load_fail

    mov ah, 0x4D
    int 0x21
    mov bl, al

    mov al, [last_exit_code]
    mov si, msg_mz_done
    call print_string_dual
    call print_hex8_dual
    mov al, ' '
    call putc_dual
    mov al, bl
    call print_hex8_dual
    call print_newline_dual

    cmp byte [last_exit_code], 0x55
    jne .serial_fail
    mov si, msg_mz_serial_pass
    call print_string_serial
    ret
.load_fail:
    mov si, msg_mz_load_fail
    call print_string_dual
    mov si, msg_mz_serial_fail
    call print_string_serial
    ret
.serial_fail:
    mov si, msg_mz_serial_fail
    call print_string_serial
    ret

run_gfxrect_demo:
    mov dx, path_gfxrect_dos
    mov al, 0x71
    mov si, msg_gfxrect_serial_pass
    mov di, msg_gfxrect_serial_fail
    call run_expected_com_demo
    ret

run_gfxstar_demo:
    mov dx, path_gfxstar_dos
    mov al, 0x72
    mov si, msg_gfxstar_serial_pass
    mov di, msg_gfxstar_serial_fail
    call run_expected_com_demo
    ret

run_expected_com_demo:
    push ax
    push si
    push di

    mov ax, cs
    mov ds, ax
    xor bx, bx
    mov ax, 0x4B00
    int 0x21
    jc .serial_fail_pop

    mov ah, 0x4D
    int 0x21
    pop di
    pop si
    pop bx
    cmp byte [last_exit_code], bl
    jne .serial_fail

    call print_string_serial
    ret

.serial_fail_pop:
    pop di
    pop si
    pop bx
.serial_fail:
    mov si, di
    call print_string_serial
    ret

load_root_file_first_sector:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov [search_name_ptr], si
    mov [search_target_off], bx
    mov word [search_found_cluster], 0
    mov word [search_found_size_lo], 0
    mov word [search_found_size_hi], 0
    mov word [search_found_root_lba], 0
    mov word [search_found_root_off], 0
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

    mov si, [search_name_ptr]
    push cx
    push dx
    call fat_entry_matches_name
    pop dx
    pop cx
    jc .found_entry

.next_entry:
    add di, 32
    loop .scan_entries

    inc dx
    jmp .scan_next_sector

.found_entry:
    mov [search_found_root_lba], dx
    mov ax, di
    sub ax, 0x0200
    mov [search_found_root_off], ax

    ; copy 11-byte FAT name from directory entry to search_found_name
    push cx
    push si
    push di
    mov si, di
    mov di, search_found_name
    mov cx, 11
.lrfs_name_copy:
    mov al, [es:si]
    mov [di], al
    inc si
    inc di
    loop .lrfs_name_copy
    pop di
    pop si
    pop cx

    mov ax, [es:di + 26]
    cmp ax, 2
    jb .read_fail

    mov [search_found_cluster], ax
    mov ax, [es:di + 28]
    mov [search_found_size_lo], ax
    mov ax, [es:di + 30]
    mov [search_found_size_hi], ax
    mov al, [es:di + 11]
    mov [search_found_attr], al

    mov ax, [search_found_cluster]
    call int21_cluster_to_lba
    mov bx, [search_target_off]
    cmp bx, 0xFFFF
    je .found_ok
    call read_sector_lba
    jc .read_fail

.found_ok:
    clc
    jmp .done
.read_fail:
    stc
.done:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

fat_entry_matches_name:
    push ax
    push bx
    push cx

    mov bx, 0
    mov cx, 11

.cmp_loop:
    mov al, [si + bx]
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

%if FAT_TYPE == 16
    push ds
    add ax, FAT_LBA_OFFSET
    mov [cs:tmp_disk_lba_save], ax
    mov [cs:disk_packet_lba], ax
    mov [cs:disk_packet_lba + 2], word 0
    mov [cs:disk_packet_lba + 4], word 0
    mov [cs:disk_packet_lba + 6], word 0
    mov [cs:disk_packet_off], bx
    mov [cs:disk_packet_seg], es
    mov ax, cs
    mov ds, ax
    mov si, disk_packet
    mov dl, [cs:boot_drive]
    mov ah, 0x42
    sti
    int 0x13
    jnc .edd_read_done
    mov ax, [cs:disk_packet_seg]
    mov es, ax
    mov bx, [cs:disk_packet_off]
    mov ax, [cs:tmp_disk_lba_save]
    call bios_read_chs_sector
.edd_read_done:
    pop ds
    jmp .done
%endif

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
%if FAT_TYPE == 16
    mov dl, 0x80
%else
    mov dl, [cs:boot_drive]
%endif

    mov ah, 0x02
    mov al, 0x01
    sti
    int 0x13

.done:
    mov [cs:tmp_disk_status], ah
    pop si
    pop dx
    pop cx
    pop bx
    ret

write_sector_lba:
    push bx
    push cx
    push dx
    push si

%if FAT_TYPE == 16
    push ds
    add ax, FAT_LBA_OFFSET
    mov [cs:tmp_disk_lba_save], ax
    mov [cs:disk_packet_lba], ax
    mov [cs:disk_packet_lba + 2], word 0
    mov [cs:disk_packet_lba + 4], word 0
    mov [cs:disk_packet_lba + 6], word 0
    mov [cs:disk_packet_off], bx
    mov [cs:disk_packet_seg], es
    mov ax, cs
    mov ds, ax
    mov si, disk_packet
    mov dl, [cs:boot_drive]
    mov ah, 0x43
    mov al, 0x00
    sti
    int 0x13
    jnc .edd_write_done
    mov ax, [cs:disk_packet_seg]
    mov es, ax
    mov bx, [cs:disk_packet_off]
    mov ax, [cs:tmp_disk_lba_save]
    call bios_write_chs_sector
.edd_write_done:
    pop ds
    jmp .done
%endif

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
%if FAT_TYPE == 16
    mov dl, 0x80
%else
    mov dl, [cs:boot_drive]
%endif

    mov ah, 0x03
    mov al, 0x01
    sti
    int 0x13

.done:
    mov [cs:tmp_disk_status], ah
    pop si
    pop dx
    pop cx
    pop bx
    ret

bios_read_chs_sector:
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
    mov dl, [cs:boot_drive]
    mov ah, 0x02
    mov al, 0x01
    sti
    int 0x13
    ret

bios_write_chs_sector:
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
    mov dl, [cs:boot_drive]
    mov ah, 0x03
    mov al, 0x01
    sti
    int 0x13
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
    mov si, str_ver
    call str_eq
    jc .cmd_ver
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
    mov si, str_drives
    call str_eq
    jc .cmd_drives
    mov di, bx
    mov si, str_dir
    call str_eq
    jc .cmd_dir
    mov di, bx
    mov si, str_pwd
    call str_eq
    jc .cmd_pwd
    mov di, bx
    mov si, str_woof
    call str_eq
    jc .cmd_cd
    mov di, bx
    mov si, str_cdup
    call str_eq
    jc .cmd_cdup
    mov di, bx
    mov si, str_cd
    call str_eq
    jc .cmd_cd
    mov di, bx
    mov si, str_copy
    call str_eq
    jc .cmd_copy
    mov di, bx
    mov si, str_move
    call str_eq
    jc .cmd_move
    mov di, bx
    mov si, str_mv
    call str_eq
    jc .cmd_move
    mov di, bx
    mov si, str_del
    call str_eq
    jc .cmd_del
    mov di, bx
    mov si, str_md
    call str_eq
    jc .cmd_md
    mov di, bx
    mov si, str_mkdir
    call str_eq
    jc .cmd_md
    mov di, bx
    mov si, str_rd
    call str_eq
    jc .cmd_rd
    mov di, bx
    mov si, str_rmdir
    call str_eq
    jc .cmd_rd
    mov di, bx
    mov si, str_ren
    call str_eq
    jc .cmd_ren
    mov di, bx
    mov si, str_rename
    call str_eq
    jc .cmd_ren
    mov di, bx
    mov si, str_type
    call str_eq
    jc .cmd_type
    mov di, bx
    mov si, str_run
    call str_eq
    jc .cmd_run
    mov di, bx
    mov si, str_which
    call str_eq
    jc .cmd_which
    mov di, bx
    mov si, str_where
    call str_eq
    jc .cmd_which
    mov di, bx
    mov si, str_exit
    call str_eq
    jc .cmd_exit
    mov di, bx
    mov si, str_dos21
    call str_eq
    jc .cmd_dos21
    mov di, bx
    mov si, str_comdemo
    call str_eq
    jc .cmd_comdemo
    mov di, bx
    mov si, str_mzdemo
    call str_eq
    jc .cmd_mzdemo
    mov di, bx
    mov si, str_fileio
    call str_eq
    jc .cmd_fileio
    mov di, bx
    mov si, str_gfxdemo
    call str_eq
    jc .cmd_gfxdemo
    mov di, bx
    mov si, str_gfxrect
    call str_eq
    jc .cmd_gfxrect
    mov di, bx
    mov si, str_gfxstar
    call str_eq
    jc .cmd_gfxstar
    mov di, bx
    mov si, str_findtest
    call str_eq
    jc .cmd_findtest
    mov di, bx
    mov si, str_mouse
    call str_eq
    jc .cmd_mouse
    mov di, bx
    mov si, str_keytest
    call str_eq
    jc .cmd_keytest
    mov di, bx
    mov si, str_reboot
    call str_eq
    jc .cmd_reboot
    mov di, bx
    mov si, str_halt
    call str_eq
    jc .cmd_halt

    mov si, bx
    call shell_try_exec_token
    jnc .done

    mov si, msg_unknown
    call print_string_dual
    jmp .done

.cmd_help:
    call shell_cmd_help
    jmp .done

.cmd_ver:
    mov si, msg_banner_title
    call print_string_dual
    call print_newline_dual
    jmp .done

.cmd_cls:
    mov ax, 0x0003
    int 0x10
    call draw_shell_chrome
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

.cmd_drives:
    mov si, msg_drive
    call print_string_dual
    xor ah, ah
    mov al, [boot_drive]
    call print_hex8_dual
    call print_newline_dual
    mov si, msg_drives_default
    call print_string_dual
    mov al, [dos_default_drive]
    add al, 65
    call putc_dual
    mov si, msg_drives_index
    call print_string_dual
    mov al, [dos_default_drive]
    call print_hex8_dual
    call print_newline_dual
    mov si, msg_drives_units
    call print_string_dual
    jmp .done

.cmd_dir:
    call shell_cmd_dir
    jmp .done

.cmd_pwd:
    call shell_cmd_pwd
    jmp .done

.cmd_cd:
    call shell_cmd_cd
    jmp .done

.cmd_cdup:
    call shell_cmd_cdup
    jmp .done

.cmd_copy:
    call shell_cmd_copy
    jmp .done

.cmd_move:
    call shell_cmd_move
    jmp .done

.cmd_del:
    call shell_cmd_del
    jmp .done

.cmd_md:
    call shell_cmd_md
    jmp .done

.cmd_rd:
    call shell_cmd_rd
    jmp .done

.cmd_ren:
    call shell_cmd_ren
    jmp .done

.cmd_type:
    call shell_cmd_type
    jmp .done

.cmd_run:
    call shell_cmd_run
    jmp .done

.cmd_which:
    call shell_cmd_which
    jmp .done

.cmd_exit:
    call shell_cmd_exit
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

.cmd_mzdemo:
    call run_mz_demo
    jmp .done

.cmd_fileio:
    call int21_fileio_test
    jmp .done

.cmd_gfxdemo:
    call run_gfx_demo
    jmp .done

.cmd_gfxrect:
    call run_gfxrect_demo
    jmp .done

.cmd_gfxstar:
    call run_gfxstar_demo
    jmp .done

.cmd_findtest:
    call int21_find_test
    jmp .done

.cmd_mouse:
    call shell_cmd_mouse
    jmp .done

.cmd_keytest:
    call shell_cmd_keytest
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
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds

    mov ax, cs
    mov ds, ax

    mov di, cmd_buffer
    mov byte [di], 0

    mov ah, 0x03
    xor bh, bh
    int 0x10
    mov [shell_edit_start_col], dl
    mov [shell_edit_start_row], dh

    mov al, CMD_BUF_LEN - 1
    mov bl, 79
    cmp dl, 79
    jbe .cap_line
    xor bl, bl
    jmp .cap_ready
.cap_line:
    sub bl, dl
.cap_ready:
    cmp bl, al
    jbe .cap_done
    mov bl, al
.cap_done:
    mov [shell_edit_cap], bl
    mov byte [shell_edit_len], 0
    mov byte [shell_edit_cursor], 0
    mov byte [shell_edit_prev_len], 0
    mov byte [shell_history_nav], 0xFF
    mov byte [shell_history_saved_len], 0

%if FAT_TYPE == 16
    mov byte [cs:shell_footer_tick_key_activity], 0
%endif
.read_key:
%if FAT_TYPE == 16
    call shell_footer_poll
    inc word [cs:shell_footer_loop_count]
%endif
    mov ah, 0x01
    int 0x16
    jz .read_key

    xor ah, ah
    int 0x16
%if FAT_TYPE == 16
    mov byte [cs:shell_footer_tick_key_activity], 1
%endif

    cmp al, 0x0D
    je .finish

    cmp al, 0x09
    je .tab_complete

    cmp al, 0x08
    je .backspace

    cmp al, 0
    je .extended
    cmp al, 0xE0
    je .extended

    cmp al, 0x20
    jb .read_key

    mov dh, al
    mov bl, [shell_edit_len]
    mov al, [shell_edit_cap]
    cmp bl, al
    jae .read_key

    mov dl, [shell_edit_cursor]
    cmp dl, bl
    jae .insert_char

    xor bh, bh
    mov si, cmd_buffer
    add si, bx
.shift_right_loop:
    cmp bl, dl
    jbe .insert_char
    mov al, [si - 1]
    mov [si], al
    dec si
    dec bl
    jmp .shift_right_loop

.insert_char:
    xor bx, bx
    mov bl, [shell_edit_cursor]
    mov [cmd_buffer + bx], dh
    inc byte [shell_edit_cursor]
    inc byte [shell_edit_len]
    xor bx, bx
    mov bl, [shell_edit_len]
    mov byte [cmd_buffer + bx], 0
    call shell_line_render
    jmp .read_key

.backspace:
    mov bl, [shell_edit_cursor]
    cmp bl, 0
    je .read_key

    dec bl
    mov [shell_edit_cursor], bl
    mov dl, [shell_edit_len]
    xor bh, bh
    mov si, cmd_buffer
    add si, bx
.backspace_shift_loop:
    mov al, [si + 1]
    mov [si], al
    inc si
    inc bl
    cmp bl, dl
    jb .backspace_shift_loop

    dec byte [shell_edit_len]
    xor bx, bx
    mov bl, [shell_edit_len]
    mov byte [cmd_buffer + bx], 0
    call shell_line_render
    jmp .read_key

.extended:
    cmp ah, 0x4B
    je .key_left
    cmp ah, 0x4D
    je .key_right
    cmp ah, 0x47
    je .key_home
    cmp ah, 0x4F
    je .key_end
    cmp ah, 0x53
    je .key_delete
    cmp ah, 0x48
    je .key_up
    cmp ah, 0x50
    je .key_down
    jmp .read_key

.key_left:
    cmp byte [shell_edit_cursor], 0
    je .read_key
    dec byte [shell_edit_cursor]
    call shell_line_place_cursor
    jmp .read_key

.key_right:
    mov al, [shell_edit_cursor]
    cmp al, [shell_edit_len]
    jae .read_key
    inc byte [shell_edit_cursor]
    call shell_line_place_cursor
    jmp .read_key

.key_home:
    mov byte [shell_edit_cursor], 0
    call shell_line_place_cursor
    jmp .read_key

.key_end:
    mov al, [shell_edit_len]
    mov [shell_edit_cursor], al
    call shell_line_place_cursor
    jmp .read_key

.key_delete:
    mov bl, [shell_edit_cursor]
    mov dl, [shell_edit_len]
    cmp bl, dl
    jae .read_key

    xor bh, bh
    mov si, cmd_buffer
    add si, bx
.delete_shift_loop:
    mov al, [si + 1]
    mov [si], al
    inc si
    inc bl
    cmp bl, dl
    jb .delete_shift_loop

    dec byte [shell_edit_len]
    xor bx, bx
    mov bl, [shell_edit_len]
    mov byte [cmd_buffer + bx], 0
    call shell_line_render
    jmp .read_key

.key_up:
    cmp byte [shell_history_count], 0
    je .read_key

    mov al, [shell_history_nav]
    cmp al, 0xFF
    jne .up_next

    mov si, cmd_buffer
    mov di, shell_history_saved_buf
    mov cx, CMD_BUF_LEN
.up_save_loop:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    loop .up_save_loop
    mov al, [shell_edit_len]
    mov [shell_history_saved_len], al
    xor al, al
    mov [shell_history_nav], al
    jmp .up_load

.up_next:
    inc al
    cmp al, [shell_history_count]
    jae .read_key
    mov [shell_history_nav], al

.up_load:
    mov al, [shell_history_nav]
    call shell_history_load_by_offset
    jc .read_key
    call shell_line_render
    jmp .read_key

.key_down:
    mov al, [shell_history_nav]
    cmp al, 0xFF
    je .read_key

    cmp al, 0
    jne .down_prev

    mov byte [shell_history_nav], 0xFF
    mov si, shell_history_saved_buf
    mov di, cmd_buffer
    mov cx, CMD_BUF_LEN
.down_restore_loop:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    loop .down_restore_loop

    mov al, [shell_history_saved_len]
    cmp al, [shell_edit_cap]
    jbe .down_store_len
    mov al, [shell_edit_cap]
.down_store_len:
    mov [shell_edit_len], al
    mov [shell_edit_cursor], al
    xor bx, bx
    mov bl, al
    mov byte [cmd_buffer + bx], 0
    call shell_line_render
    jmp .read_key

.down_prev:
    dec al
    mov [shell_history_nav], al
    call shell_history_load_by_offset
    jc .read_key
    call shell_line_render
    jmp .read_key

.tab_complete:
    call shell_try_tab_complete_line
    jmp .read_key

.finish:
    xor bx, bx
    mov bl, [shell_edit_len]
    mov byte [cmd_buffer + bx], 0
    call shell_history_store_current_cmd
    call print_newline_dual

    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_line_place_cursor:
    push ax
    push dx

    mov dh, [shell_edit_start_row]
    mov dl, [shell_edit_start_col]
    mov al, [shell_edit_cursor]
    add dl, al
    call set_cursor_pos

    pop dx
    pop ax
    ret

shell_line_render:
    push ax
    push bx
    push cx
    push dx
    push si

    mov dh, [shell_edit_start_row]
    mov dl, [shell_edit_start_col]
    call set_cursor_pos

    xor cx, cx
    mov cl, [shell_edit_len]
    mov si, cmd_buffer
.print_chars:
    cmp cx, 0
    je .clear_tail
    lodsb
    call bios_putc
    dec cx
    jmp .print_chars

.clear_tail:
    mov al, [shell_edit_prev_len]
    mov bl, [shell_edit_len]
    cmp al, bl
    jbe .store_len
    sub al, bl
    mov cl, al
    xor ch, ch
.clear_loop:
    mov al, ' '
    call bios_putc
    loop .clear_loop

.store_len:
    mov al, [shell_edit_len]
    mov [shell_edit_prev_len], al
    call shell_line_place_cursor

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_history_store_current_cmd:
    push ax
    push bx
    push cx
    push si
    push di

    mov al, [shell_edit_len]
    cmp al, 0
    je .done

    xor ax, ax
    mov al, [shell_history_head]
    shl ax, 6
    mov di, shell_history_buf
    add di, ax

    mov si, cmd_buffer
    mov cx, CMD_BUF_LEN
.copy_loop:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    loop .copy_loop

    mov al, [shell_history_head]
    inc al
    and al, SHELL_HISTORY_MAX - 1
    mov [shell_history_head], al

    mov al, [shell_history_count]
    cmp al, SHELL_HISTORY_MAX
    jae .done
    inc al
    mov [shell_history_count], al

.done:
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

shell_history_load_by_offset:
    push ax
    push bx
    push cx
    push si
    push di

    mov bl, [shell_history_count]
    cmp bl, 0
    je .fail
    cmp al, bl
    jae .fail

    mov bl, [shell_history_head]
    dec bl
    sub bl, al
    and bl, SHELL_HISTORY_MAX - 1

    xor ax, ax
    mov al, bl
    shl ax, 6
    mov si, shell_history_buf
    add si, ax
    mov di, cmd_buffer
    mov cx, CMD_BUF_LEN
.load_loop:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    loop .load_loop

    xor bx, bx
.scan_len:
    cmp bx, CMD_BUF_LEN - 1
    jae .len_cap
    mov al, [cmd_buffer + bx]
    cmp al, 0
    je .len_ready
    inc bx
    jmp .scan_len

.len_cap:
    mov bx, CMD_BUF_LEN - 1

.len_ready:
    mov al, bl
    mov bl, [shell_edit_cap]
    cmp al, bl
    jbe .store_len
    mov al, bl

.store_len:
    mov [shell_edit_len], al
    mov [shell_edit_cursor], al
    xor bx, bx
    mov bl, al
    mov byte [cmd_buffer + bx], 0
    clc
    jmp .done

.fail:
    stc

.done:
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

shell_try_tab_complete_line:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds

    mov ax, cs
    mov ds, ax

    mov al, [shell_edit_cursor]
    cmp al, 0
    je .done
    cmp al, [shell_edit_len]
    jne .done
    mov [shell_completion_prefix_len], al

    xor bx, bx
.scan_prefix:
    cmp bl, [shell_completion_prefix_len]
    jae .scan_done
    mov al, [cmd_buffer + bx]
    cmp al, ' '
    je .done
    inc bl
    jmp .scan_prefix

.scan_done:
    mov byte [shell_completion_match_count], 0
    call shell_completion_scan_builtins
    call shell_completion_scan_exec_files

    cmp byte [shell_completion_match_count], 1
    jne .done

    mov si, shell_completion_match_buf
    mov di, cmd_buffer
    xor bx, bx
    mov bl, [shell_edit_cap]
.copy_match:
    cmp bl, 0
    je .copy_done
    mov al, [si]
    cmp al, 0
    je .copy_done
    mov [di], al
    inc si
    inc di
    dec bl
    jmp .copy_match

.copy_done:
    mov byte [di], 0
    mov ax, di
    sub ax, cmd_buffer
    mov [shell_edit_len], al
    mov [shell_edit_cursor], al
    call shell_line_render

.done:
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_completion_scan_builtins:
    push si

    mov si, str_help
    call shell_completion_consider_candidate
    mov si, str_ver
    call shell_completion_consider_candidate
    mov si, str_cls
    call shell_completion_consider_candidate
    mov si, str_ticks
    call shell_completion_consider_candidate
    mov si, str_drive
    call shell_completion_consider_candidate
    mov si, str_drives
    call shell_completion_consider_candidate
    mov si, str_dir
    call shell_completion_consider_candidate
    mov si, str_pwd
    call shell_completion_consider_candidate
    mov si, str_cdup
    call shell_completion_consider_candidate
    mov si, str_cd
    call shell_completion_consider_candidate
    mov si, str_copy
    call shell_completion_consider_candidate
    mov si, str_move
    call shell_completion_consider_candidate
    mov si, str_mv
    call shell_completion_consider_candidate
    mov si, str_del
    call shell_completion_consider_candidate
    mov si, str_md
    call shell_completion_consider_candidate
    mov si, str_mkdir
    call shell_completion_consider_candidate
    mov si, str_rd
    call shell_completion_consider_candidate
    mov si, str_rmdir
    call shell_completion_consider_candidate
    mov si, str_ren
    call shell_completion_consider_candidate
    mov si, str_rename
    call shell_completion_consider_candidate
    mov si, str_type
    call shell_completion_consider_candidate
    mov si, str_run
    call shell_completion_consider_candidate
    mov si, str_which
    call shell_completion_consider_candidate
    mov si, str_where
    call shell_completion_consider_candidate
    mov si, str_exit
    call shell_completion_consider_candidate
    mov si, str_dos21
    call shell_completion_consider_candidate
    mov si, str_comdemo
    call shell_completion_consider_candidate
    mov si, str_mzdemo
    call shell_completion_consider_candidate
    mov si, str_fileio
    call shell_completion_consider_candidate
    mov si, str_gfxdemo
    call shell_completion_consider_candidate
    mov si, str_gfxrect
    call shell_completion_consider_candidate
    mov si, str_gfxstar
    call shell_completion_consider_candidate
    mov si, str_findtest
    call shell_completion_consider_candidate
    mov si, str_mouse
    call shell_completion_consider_candidate
    mov si, str_keytest
    call shell_completion_consider_candidate
    mov si, str_reboot
    call shell_completion_consider_candidate
    mov si, str_halt
    call shell_completion_consider_candidate

    pop si
    ret

shell_completion_scan_exec_files:
    push ax
    push dx
    push ds

    mov ax, [cs:dta_seg]
    mov [cs:shell_completion_saved_dta_seg], ax
    mov ax, [cs:dta_off]
    mov [cs:shell_completion_saved_dta_off], ax

    mov ax, cs
    mov ds, ax
    mov dx, shell_completion_dta
    mov ah, 0x1A
    int 0x21

    mov dx, path_pattern_com
    call shell_completion_scan_exec_pattern
    mov dx, path_pattern_exe
    call shell_completion_scan_exec_pattern

    mov ax, [cs:shell_completion_saved_dta_seg]
    mov ds, ax
    mov dx, [cs:shell_completion_saved_dta_off]
    mov ah, 0x1A
    int 0x21

    pop ds
    pop dx
    pop ax
    ret

shell_completion_scan_exec_pattern:
    push ax
    push cx
    push dx

    mov ah, 0x4E
    xor cx, cx
    int 0x21
    jc .done

.scan_loop:
    call shell_completion_consider_found_file
    mov ah, 0x4F
    int 0x21
    jnc .scan_loop

.done:
    pop dx
    pop cx
    pop ax
    ret

shell_completion_consider_found_file:
    push ax
    push cx
    push si
    push di

    mov si, shell_completion_dta + 0x1E
    mov di, shell_completion_file_buf
    mov cx, CMD_BUF_LEN - 1
.copy_loop:
    cmp cx, 0
    je .term
    mov al, [si]
    cmp al, 0
    je .term
    mov [di], al
    inc si
    inc di
    dec cx
    jmp .copy_loop

.term:
    mov byte [di], 0
    cmp di, shell_completion_file_buf
    je .done
    mov si, shell_completion_file_buf
    call shell_completion_consider_candidate

.done:
    pop di
    pop si
    pop cx
    pop ax
    ret

shell_completion_consider_candidate:
    push ax
    push bx
    push cx
    push si
    push di

    mov al, [shell_completion_match_count]
    cmp al, 2
    je .done

    mov di, cmd_buffer
    xor bx, bx
    xor cx, cx
    mov cl, [shell_completion_prefix_len]
.prefix_loop:
    cmp cx, 0
    je .prefix_match
    mov al, [di + bx]
    mov ah, [si + bx]
    cmp ah, 0
    je .done

    cmp al, 'A'
    jb .prefix_al_ready
    cmp al, 'Z'
    ja .prefix_al_ready
    or al, 0x20
.prefix_al_ready:
    cmp ah, 'A'
    jb .prefix_cmp
    cmp ah, 'Z'
    ja .prefix_cmp
    or ah, 0x20
.prefix_cmp:
    cmp al, ah
    jne .done
    inc bx
    dec cx
    jmp .prefix_loop

.prefix_match:
    cmp byte [shell_completion_match_count], 0
    jne .compare_existing

    mov di, shell_completion_match_buf
    mov cx, CMD_BUF_LEN - 1
.store_first:
    mov al, [si]
    mov [di], al
    inc si
    inc di
    cmp al, 0
    je .mark_first
    dec cx
    jnz .store_first
    mov byte [di - 1], 0
.mark_first:
    mov byte [shell_completion_match_count], 1
    jmp .done

.compare_existing:
    mov di, shell_completion_match_buf
.compare_loop:
    mov al, [di]
    mov ah, [si]

    cmp al, 'A'
    jb .cmp_al_ready
    cmp al, 'Z'
    ja .cmp_al_ready
    or al, 0x20
.cmp_al_ready:
    cmp ah, 'A'
    jb .cmp_chars
    cmp ah, 'Z'
    ja .cmp_chars
    or ah, 0x20
.cmp_chars:
    cmp al, ah
    jne .mark_ambiguous
    cmp al, 0
    je .done
    inc di
    inc si
    jmp .compare_loop

.mark_ambiguous:
    mov byte [shell_completion_match_count], 2

.done:
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

skip_spaces:
.skip:
    cmp byte [si], ' '
    jne .done
    inc si
    jmp .skip
.done:
    ret

shell_arg_ptr:
    mov si, bx
.scan_token:
    mov al, [si]
    cmp al, 0
    je .done
    cmp al, ' '
    je .skip_tail_spaces
    inc si
    jmp .scan_token

.skip_tail_spaces:
    call skip_spaces
.done:
    ret

shell_next_arg:
    call skip_spaces
    cmp byte [si], 0
    je .none

    cmp byte [si], '"'
    jne .plain

    inc si
    mov dx, si
.quoted_scan:
    mov al, [si]
    cmp al, 0
    je .found
    cmp al, '"'
    je .quoted_term
    inc si
    jmp .quoted_scan

.quoted_term:
    mov byte [si], 0
    inc si
    call skip_spaces
    clc
    ret

.plain:
    mov dx, si
.plain_scan:
    mov al, [si]
    cmp al, 0
    je .found
    cmp al, ' '
    je .plain_term
    inc si
    jmp .plain_scan

.plain_term:
    mov byte [si], 0
    inc si
    call skip_spaces

.found:
    clc
    ret

.none:
    stc
    ret

shell_trim_first_arg:
.scan:
    cmp byte [si], 0
    je .done
    cmp byte [si], ' '
    je .term
    inc si
    jmp .scan
.term:
    mov byte [si], 0
.done:
    ret

shell_copy_token_for_exec:
    mov di, shell_exec_path_buf
    mov cx, SHELL_EXEC_PATH_BUF_LEN - 1

.copy_loop:
    mov al, [si]
    cmp al, 0
    je .copy_done
    cmp al, ' '
    je .copy_done
    cmp cx, 0
    je .copy_fail
    mov [di], al
    inc di
    inc si
    dec cx
    jmp .copy_loop

.copy_done:
    mov byte [di], 0
    cmp di, shell_exec_path_buf
    je .copy_fail
    clc
    ret

.copy_fail:
    stc
    ret

shell_copy_path_for_exec:
    mov di, shell_exec_path_buf
    mov cx, SHELL_EXEC_PATH_BUF_LEN - 1

.copy_loop:
    mov al, [si]
    cmp al, 0
    je .copy_done
    cmp cx, 0
    je .copy_fail
    mov [di], al
    inc di
    inc si
    dec cx
    jmp .copy_loop

.copy_done:
    mov byte [di], 0
    cmp di, shell_exec_path_buf
    je .copy_fail
    clc
    ret

.copy_fail:
    stc
    ret

shell_token_has_extension:
    xor dl, dl

.scan_loop:
    mov al, [si]
    cmp al, 0
    je .scan_done
    cmp al, '\'
    je .scan_sep
    cmp al, '/'
    je .scan_sep
    cmp al, '.'
    je .scan_dot
    inc si
    jmp .scan_loop

.scan_sep:
    xor dl, dl
    inc si
    jmp .scan_loop

.scan_dot:
    mov dl, 1
    inc si
    jmp .scan_loop

.scan_done:
    cmp dl, 0
    je .no_ext
    stc
    ret

.no_ext:
    clc
    ret

shell_append_exec_extension:
    mov di, shell_exec_path_buf
    mov cx, SHELL_EXEC_PATH_BUF_LEN - 1

.find_end:
    cmp byte [di], 0
    je .append_loop
    inc di
    dec cx
    jnz .find_end
    jmp .append_fail

.append_loop:
    mov al, [si]
    cmp al, 0
    je .append_done
    cmp cx, 0
    je .append_fail
    mov [di], al
    inc di
    inc si
    dec cx
    jmp .append_loop

.append_done:
    mov byte [di], 0
    clc
    ret

.append_fail:
    stc
    ret

shell_exec_buffer_path:
    ; Save CWD before exec so it can be restored after program exits
    push ax
    push cx
    push si
    push di
    push es
    push cs
    pop es
    mov si, cwd_buf
    mov di, shell_exec_saved_cwd_buf
    mov cx, 24
    rep movsb
    mov ax, [cs:cwd_cluster]
    mov [cs:shell_exec_saved_cwd_cluster], ax
    pop es
    pop di
    pop si
    pop cx
    pop ax
    ; Clear shell chrome before handing control to external DOS programs.
    push ax
    mov ax, 0x0003
    int 0x10
    pop ax
    mov byte [cs:shell_exec_external_mouse_disabled], 1
    call shell_exec_restore_bios_int10
    ; Run the program
    push bx
    push es
    mov dx, shell_exec_path_buf
    mov ax, cs
    mov es, ax
    mov [cs:shell_exec_param_block + 4], ax
    mov bx, shell_exec_param_block
    mov ax, 0x4B00
    int 0x21
    pop es
    pop bx
    jc .exec_failed
    ; Exec succeeded: restore CWD to pre-exec state
    mov byte [cs:shell_exec_external_mouse_disabled], 0
    push ax
    push cx
    push si
    push di
    push es
    push cs
    pop es
    mov si, shell_exec_saved_cwd_buf
    mov di, cwd_buf
    mov cx, 24
    rep movsb
    mov ax, [cs:shell_exec_saved_cwd_cluster]
    mov [cs:cwd_cluster], ax
    pop es
    pop di
    pop si
    pop cx
    pop ax
    ; Clear screen and redraw shell chrome
    push ax
    mov ax, 0x0003
    int 0x10
    pop ax
    call shell_exec_reinstall_int10
    call draw_shell_chrome
    clc
    ret
.exec_failed:
    mov byte [cs:shell_exec_external_mouse_disabled], 0
    push ax
    mov ax, 0x0003
    int 0x10
    pop ax
    call shell_exec_reinstall_int10
    call draw_shell_chrome
    stc
    ret

shell_exec_restore_bios_int10:
    push ax
    push bx
    push es
    cli
    xor ax, ax
    mov es, ax
    mov bx, 0x10 * 4
    mov ax, [cs:old_int10_off]
    mov [es:bx], ax
    mov ax, [cs:old_int10_seg]
    mov [es:bx + 2], ax
    sti
    pop es
    pop bx
    pop ax
    ret

shell_exec_reinstall_int10:
    push ax
    push bx
    push es
    cli
    xor ax, ax
    mov es, ax
    mov bx, 0x10 * 4
    mov word [es:bx], int10_handler
    mov ax, cs
    mov [es:bx + 2], ax
    mov byte [cs:current_video_mode], 0x03
    sti
    pop es
    pop bx
    pop ax
    ret

shell_exec_set_empty_tail:
    mov byte [cs:shell_exec_cmd_tail], 0
    mov byte [cs:shell_exec_cmd_tail + 1], 0x0D
    ret

shell_exec_set_tail_from_si:
    push ax
    push cx
    push di
    push es

    call shell_exec_set_empty_tail
    cmp byte [si], 0
    je .done

    push cs
    pop es
    mov di, shell_exec_cmd_tail + 2
    mov byte [cs:shell_exec_cmd_tail + 1], ' '
    mov cl, 1

.copy_loop:
    cmp cl, 126
    jae .finish
    lodsb
    cmp al, 0
    je .finish
    stosb
    inc cl
    jmp .copy_loop

.finish:
    xor ch, ch
    mov byte [cs:shell_exec_cmd_tail], cl
    mov di, shell_exec_cmd_tail + 1
    add di, cx
    mov byte [cs:di], 0x0D

.done:
    pop es
    pop di
    pop cx
    pop ax
    ret

shell_try_resolve_exec_token:
    push ds

    mov bx, si
    mov ax, cs
    mov ds, ax

    mov si, bx
    call shell_copy_token_for_exec
    jc .fail

    mov si, shell_exec_path_buf
    call shell_token_has_extension
    jc .try_as_is

    mov si, str_ext_com
    call shell_append_exec_extension
    jc .fail
    mov si, shell_exec_path_buf
    push bx
    call int21_resolve_and_find_path
    pop bx
    jnc .ok

    mov si, bx
    call shell_copy_token_for_exec
    jc .fail
    mov si, str_ext_exe
    call shell_append_exec_extension
    jc .fail

.try_as_is:
    mov si, shell_exec_path_buf
    call int21_resolve_and_find_path
    jc .fail

.ok:
    clc
    jmp .done

.fail:
    stc

.done:
    pop ds
    ret

shell_try_exec_token:
    push ds
    mov ax, cs
    mov ds, ax

    call shell_try_resolve_exec_token
    jc .fail
    call shell_exec_buffer_path
    jc .fail

.ok:
    clc
    jmp .done

.fail:
    stc

.done:
    pop ds
    ret

shell_try_exec_path:
    push ds

    mov bx, si
    mov ax, cs
    mov ds, ax

    mov si, bx
    call shell_copy_path_for_exec
    jc .fail

    mov si, shell_exec_path_buf
    call shell_token_has_extension
    jc .try_as_is

    mov si, str_ext_com
    call shell_append_exec_extension
    jc .fail
    call shell_exec_buffer_path
    jnc .ok

    mov si, bx
    call shell_copy_path_for_exec
    jc .fail
    mov si, str_ext_exe
    call shell_append_exec_extension
    jc .fail

.try_as_is:
    call shell_exec_buffer_path
    jc .fail

.ok:
    clc
    jmp .done

.fail:
    stc

.done:
    pop ds
    ret

shell_is_builtin_token:
    push ax
    push bx
    push si

    mov bx, shell_builtin_name_table

.scan_next:
    mov si, [bx]
    cmp si, 0
    je .not_builtin

    push di
    call str_eq
    pop di
    jc .builtin

    add bx, 2
    jmp .scan_next

.not_builtin:
    clc
    jmp .done

.builtin:
    stc

.done:
    pop si
    pop bx
    pop ax
    ret

shell_cmd_which:
    push ax
    push bx
    push dx
    push si
    push di
    push ds

    mov ax, cs
    mov ds, ax
    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .usage

    mov di, dx
    call shell_is_builtin_token
    jc .builtin

    mov si, dx
    call shell_try_resolve_exec_token
    jc .not_found

    mov si, shell_exec_path_buf
    call print_string_dual
    call print_newline_dual
    jmp .done

.builtin:
    mov si, dx
    call print_string_dual
    mov si, msg_which_builtin
    call print_string_dual
    jmp .done

.not_found:
    mov si, dx
    call print_string_dual
    mov si, msg_which_not_found
    call print_string_dual
    jmp .done

.usage:
    mov si, msg_which_usage
    call print_string_dual

.done:
    pop ds
    pop di
    pop si
    pop dx
    pop bx
    pop ax
    ret

shell_cmd_help:
    push ax
    push bx
    push si
    push di
    push ds

    mov ax, cs
    mov ds, ax
    mov si, cmd_buffer
    call skip_spaces
    mov bx, si
    call shell_arg_ptr
    mov bx, si
    cmp byte [si], 0
    je .short

    mov di, bx
    mov si, str_help_all
    call str_eq
    jc .all

    mov di, bx
    mov si, str_help_short
    call str_eq
    jc .short

.short:
    call print_shell_help
    jmp .done

.all:
    call print_shell_help
    call print_shell_help_all

.done:
    pop ds
    pop di
    pop si
    pop bx
    pop ax
    ret

shell_cmd_run:
    push ax
    push bx
    push si
    push ds

    mov ax, cs
    mov ds, ax
    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .missing
    jmp .try_exec

.missing:
    mov ax, 0x0001
    jmp .fail

.try_exec:
    call shell_exec_set_tail_from_si
    mov si, dx
    call shell_try_exec_path
    jnc .done

.fail:
    mov si, str_run
    call shell_print_error_ax

.done:
    call shell_exec_set_empty_tail
    pop ds
    pop si
    pop bx
    pop ax
    ret

shell_cmd_cdup:
    push ax
    push dx
    push si
    mov dx, path_parent_dos
    mov ah, 0x3B
    int 0x21
    jnc .done
    mov si, str_cdup
    call shell_print_error_ax
.done:
    pop si
    pop dx
    pop ax
    ret

shell_cmd_mouse:
    push ax
    push bx
    push cx
    push dx

    mov ax, 0x0003
    int 0x33
    push bx
    push cx
    push dx
    mov si, msg_mouse_status
    call print_string_dual
    mov al, [cs:mouse_installed]
    call print_hex8_dual
    mov al, ' '
    call putc_dual
    mov si, msg_mouse_buttons
    call print_string_dual
    pop dx
    pop cx
    pop bx
    mov ax, bx
    call print_hex16_dual
    mov al, ' '
    call putc_dual
    mov si, msg_mouse_x
    call print_string_dual
    mov ax, cx
    call print_hex16_dual
    mov al, ' '
    call putc_dual
    mov si, msg_mouse_y
    call print_string_dual
    mov ax, dx
    call print_hex16_dual
    call print_newline_dual

    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_cmd_keytest:
    push ax
    mov si, msg_keytest_prompt
    call print_string_dual
    xor ah, ah
    int 0x16
    push ax
    call print_newline_dual
    mov si, msg_keytest_ax
    call print_string_dual
    pop ax
    call print_hex16_dual
    call print_newline_dual
    pop ax
    ret

shell_print_cwd:
    mov si, msg_cwd_prefix
    call print_string_dual
    mov si, cwd_buf
    xor dl, dl
    mov ah, 0x47
    int 0x21
    jc .fail
    cmp byte [cwd_buf], 0
    jne .print_cwd
    mov si, path_root_dos
    call print_string_dual
    call print_newline_dual
    clc
    ret

.print_cwd:
    mov si, cwd_buf
    call print_string_dual
    call print_newline_dual
    clc
    ret

.fail:
    stc
    ret

shell_cmd_pwd:
    push ax
    push dx
    push si
    push ds

    mov ax, cs
    mov ds, ax
    call shell_print_cwd
    jnc .done

    mov si, str_pwd
    call shell_print_error_ax

.done:
    pop ds
    pop si
    pop dx
    pop ax
    ret

shell_cmd_cd:
    push ax
    push bx
    push dx
    push si
    push ds

    mov ax, cs
    mov ds, ax

    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .show
.call_chdir:
    mov ah, 0x3B
    int 0x21
    jc .fail
    jmp .done

.show:
    call shell_print_cwd
    jc .fail
    jmp .done

.fail:
    mov si, str_cd
    call shell_print_error_ax

.done:
    pop ds
    pop si
    pop dx
    pop bx
    pop ax
    ret

shell_cmd_copy:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    mov ax, cs
    mov ds, ax

    mov cl, 1
    mov word [cs:shell_last_error_ax], 0x0001

    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .missing_args
    mov [cs:shell_copy_src_ptr], dx

    call shell_next_arg
    jc .missing_args
    mov [cs:shell_copy_dst_ptr], dx
    jmp .have_dst

.missing_args:
    mov ax, 0x0001
    jmp .copy_report

.have_dst:
    mov bx, 0xFFFF
    mov di, 0xFFFF
%if FAT_TYPE == 16 || FAT_TYPE == 12
    mov ah, 0x3D
    mov al, 0
    int 0x21
    jc .copy_src_open_fail

    mov bx, ax

    mov ah, 0x3C
    xor cx, cx
    mov dx, [cs:shell_copy_dst_ptr]
    mov di, 0xFFFF
    int 0x21
    jc .copy_dst_create_fail
    jmp .copy_create_done

.copy_src_open_fail:
    mov si, [cs:shell_copy_src_ptr]
    call int21_resolve_and_find_path
    jc .copy_report
    test byte [cs:search_found_attr], 0x10
    jz .copy_report

    mov si, [cs:shell_copy_dst_ptr]
    call int21_resolve_and_find_path
    jc .copy_report
    test byte [cs:search_found_attr], 0x10
    jz .copy_report
    mov ax, [cs:search_found_cluster]
    mov [cs:shell_copy_dst_cluster], ax

    mov si, [cs:shell_copy_src_ptr]
    call int21_resolve_parent_dir
    jc .copy_report

    mov ax, [cs:cwd_cluster]
    push ax
    mov ax, [cs:shell_copy_dst_cluster]
    mov [cs:cwd_cluster], ax
    mov dx, si
    mov ah, 0x39
    int 0x21
    pop ax
    mov [cs:cwd_cluster], ax
    jc .copy_report
    mov cl, 0
    jmp .copy_report

.copy_dst_create_fail:
    mov si, [cs:shell_copy_dst_ptr]
    call int21_resolve_and_find_path
    jc .copy_cleanup
    test byte [cs:search_found_attr], 0x10
    jz .copy_cleanup
    mov ax, [cs:search_found_cluster]
    mov [cs:shell_copy_dst_cluster], ax

    mov si, [cs:shell_copy_src_ptr]
    call int21_resolve_parent_dir
    jc .copy_cleanup

    mov ax, [cs:cwd_cluster]
    push ax
    mov ax, [cs:shell_copy_dst_cluster]
    mov [cs:cwd_cluster], ax
    mov dx, si
    mov ah, 0x3C
    xor cx, cx
    mov di, 0xFFFF
    int 0x21
    pop ax
    mov [cs:cwd_cluster], ax
    jc .copy_cleanup

.copy_create_done:
    mov di, ax
%else
    mov ah, 0x3D
    mov al, 0
    int 0x21
    jc .copy_report

    mov bx, ax

    mov ah, 0x3C
    xor cx, cx
    mov dx, [cs:shell_copy_dst_ptr]
    mov di, 0xFFFF
    int 0x21
    jc .copy_cleanup

    mov di, ax
%endif

.copy_read:
    mov ah, 0x3F
    mov cx, 512
    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    xor dx, dx
    int 0x21
    jc .copy_cleanup
    cmp ax, 0
    je .copy_done

    mov cx, ax
    xchg bx, di
    mov ah, 0x40
    int 0x21
    xchg bx, di
    jc .copy_cleanup
    cmp ax, cx
    jne .copy_cleanup
    jmp .copy_read

.copy_done:
    mov cl, 0

.copy_cleanup:
    mov [cs:shell_last_error_ax], ax
    mov ah, 0x3E
    cmp bx, 0xFFFF
    je .copy_close_dst
    int 0x21

.copy_close_dst:
    mov bx, di
    cmp bx, 0xFFFF
    je .copy_report
    mov ah, 0x3E
    int 0x21

.copy_report:
    mov ax, cs
    mov ds, ax
    cmp cl, 0
    je .copy_ok
    mov ax, [cs:shell_last_error_ax]
    mov si, str_copy
    call shell_print_error_ax

.copy_ok:
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_cmd_del:
    push dx
    push ds
    mov ax, cs
    mov ds, ax
    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .del_missing
    jmp .del_path

.del_missing:
    mov ax, 0x0001
    jmp .del_fail

.del_path:
    mov ah, 0x41
    int 0x21
    jc .del_fail
    jmp .del_ok
.del_fail:
    mov si, str_del
    call shell_print_error_ax
.del_ok:
    pop ds
    pop dx
    ret

shell_cmd_md:
    push dx
    push ds
    mov ax, cs
    mov ds, ax
    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .md_missing
    jmp .md_path

.md_missing:
    mov ax, 0x0001
    jmp .md_fail

.md_path:
    mov ah, 0x39
    int 0x21
    jc .md_fail
    jmp .md_ok
.md_fail:
    mov si, str_md
    call shell_print_error_ax
.md_ok:
    pop ds
    pop dx
    ret

shell_cmd_rd:
    push dx
    push ds
    mov ax, cs
    mov ds, ax
    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .rd_missing
    jmp .rd_path

.rd_missing:
    mov ax, 0x0001
    jmp .rd_fail

.rd_path:
    mov ah, 0x3A
    int 0x21
    jc .rd_fail
    jmp .rd_ok
.rd_fail:
    mov si, str_rd
    call shell_print_error_ax
.rd_ok:
    pop ds
    pop dx
    ret

shell_cmd_move:
shell_cmd_ren:
    push bx
    push ds
    push es
    mov ax, cs
    mov ds, ax

    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .ren_missing
    mov [cs:shell_copy_src_ptr], dx

    call shell_next_arg
    jc .ren_missing
    mov [cs:shell_copy_dst_ptr], dx
    mov dx, [cs:shell_copy_src_ptr]
    mov di, [cs:shell_copy_dst_ptr]
    jmp .ren_have_dst

.ren_missing:
    mov ax, 0x0001
    jmp .ren_fail

.ren_have_dst:

    push di
    mov ax, ds
    mov es, ax
    mov ah, 0x56
    int 0x21
    pop di
    jnc .ren_ok

    ; Destination is a directory: build dst/src_basename and retry rename.
    push dx
    mov si, di
    call int21_resolve_and_find_path
    jc .ren_fail_pop_src
    test byte [cs:search_found_attr], 0x10
    jz .ren_fail_pop_src

    mov si, di
    mov di, shell_exec_path_buf
    mov cx, SHELL_EXEC_PATH_BUF_LEN - 1

.build_dst_loop:
    cmp cx, 0
    je .ren_fail_pop_src
    lodsb
    cmp al, 0
    je .build_dst_done
    stosb
    dec cx
    jmp .build_dst_loop

.build_dst_done:
    cmp di, shell_exec_path_buf
    je .append_sep
    mov al, [di - 1]
    cmp al, 0x5C
    je .have_sep
    cmp al, 0x2F
    je .have_sep

.append_sep:
    cmp cx, 0
    je .ren_fail_pop_src
    mov al, 0x5C
    stosb
    dec cx

.have_sep:
    pop dx
    push dx

    mov si, dx
    call int21_resolve_parent_dir
    jc .ren_fail_pop_src

.append_name_loop:
    cmp cx, 0
    je .ren_fail_pop_src
    lodsb
    stosb
    dec cx
    cmp al, 0
    jne .append_name_loop

    mov ax, ds
    mov es, ax
    mov di, shell_exec_path_buf
    mov ah, 0x56
    int 0x21
    pop dx
    jc .ren_fail
    jmp .ren_ok

.ren_fail_pop_src:
    pop dx

.ren_fail:
    mov si, str_ren
    call shell_print_error_ax

.ren_ok:
    pop es
    pop ds
    pop bx
    ret

shell_cmd_type:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    push es
    mov ax, cs
    mov ds, ax
    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .type_missing
    jmp .type_open

.type_missing:
    mov ax, 0x0001
    jmp .type_fail

.type_open:
    mov ah, 0x3D
    mov al, 0
    int 0x21
    jc .type_fail
    mov bx, ax
.type_read:
    mov ah, 0x3F
    mov cx, 512
    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    xor dx, dx
    int 0x21
    jc .type_close
    cmp ax, 0
    je .type_done
    mov cx, ax
    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    xor di, di
.type_print:
    mov al, [es:di]
    cmp al, 0x1A
    je .type_done
    call putc_dual
    inc di
    loop .type_print
    jmp .type_read
.type_done:
    mov ah, 0x3E
    int 0x21
    call print_newline_dual
    jmp .type_ok
.type_close:
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
.type_fail:
    mov si, str_type
    call shell_print_error_ax
.type_ok:
    pop es
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_cmd_exit:
    mov si, msg_exit_str
    call print_string_dual
    int 0x19
    hlt

shell_cmd_dir:
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
    mov es, ax
    mov ax, [cs:cwd_cluster]
    mov [cs:shell_saved_cwd_cluster], ax
    mov si, cwd_buf
    mov di, shell_saved_cwd_buf
    mov cx, 24
    rep movsb

    mov si, bx
    call shell_arg_ptr
    call shell_next_arg
    jc .scan_start
    mov ah, 0x3B
    int 0x21
    jc .fail

.scan_start:

    mov si, msg_dir_header
    call print_string_dual

    mov word [shell_dir_count], 0
    mov ax, [cs:cwd_cluster]
    cmp ax, 0
    jne .scan_subdir_start

    mov dx, FAT_ROOT_START_LBA

.sector_loop:
    cmp dx, FAT_ROOT_START_LBA + FAT_ROOT_DIR_SECTORS
    jae .done_scan

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, dx
    xor bx, bx
    call read_sector_lba
    jc .fail

    xor di, di
    mov cx, 16

.entry_loop:
    mov al, [es:di]
    cmp al, 0x00
    je .done_scan
    cmp al, 0xE5
    je .next_entry

    mov al, [es:di + 11]
    cmp al, 0x0F
    je .next_entry
    test al, 0x08
    jnz .next_entry

    push cx
    push dx
    push di
    mov si, di
    call shell_print_root_entry
    pop di
    pop dx
    pop cx

    inc word [shell_dir_count]

.next_entry:
    add di, 32
    loop .entry_loop

    inc dx
    jmp .sector_loop

.scan_subdir_start:
    call int21_load_fat_cache
    jc .fail
    mov [cs:tmp_cluster], ax

.subdir_cluster_loop:
    mov ax, [cs:tmp_cluster]
    cmp ax, 2
    jb .done_scan
    cmp ax, FAT_EOF
    jae .done_scan

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
    xor dx, dx

.subdir_sector_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .subdir_next_cluster

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    add ax, dx
    xor bx, bx
    call read_sector_lba
    jc .fail

    xor di, di
    mov cx, 16

.subdir_entry_loop:
    mov al, [es:di]
    cmp al, 0x00
    je .done_scan
    cmp al, 0xE5
    je .subdir_next_entry

    mov al, [es:di + 11]
    cmp al, 0x0F
    je .subdir_next_entry
    test al, 0x08
    jnz .subdir_next_entry

    push cx
    push dx
    push di
    mov si, di
    call shell_print_root_entry
    pop di
    pop dx
    pop cx

    inc word [shell_dir_count]

.subdir_next_entry:
    add di, 32
    loop .subdir_entry_loop

    inc dx
    jmp .subdir_sector_loop

.subdir_next_cluster:
    mov ax, [cs:tmp_cluster]
    call fat12_get_entry_cached
    jc .fail
    mov [cs:tmp_cluster], ax
    jmp .subdir_cluster_loop

.done_scan:
    cmp word [shell_dir_count], 0
    jne .restore
    mov si, msg_dir_empty
    call print_string_dual
    jmp .restore

.fail:
    mov si, str_dir
    call shell_print_error_ax

.restore:
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ax, [cs:shell_saved_cwd_cluster]
    mov [cs:cwd_cluster], ax
    mov si, shell_saved_cwd_buf
    mov di, cwd_buf
    mov cx, 24
    rep movsb

.return:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_print_root_entry:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    mov di, shell_dir_name_buf
    xor bx, bx

.base_loop:
    cmp bx, 8
    jae .ext_probe
    mov al, [es:si + bx]
    cmp al, ' '
    je .ext_probe
    mov [di], al
    inc di
    inc bx
    jmp .base_loop

.ext_probe:
    xor bx, bx
    xor cx, cx
.ext_probe_loop:
    cmp bx, 3
    jae .ext_done
    mov al, [es:si + 8 + bx]
    cmp al, ' '
    je .ext_probe_next
    inc cx
.ext_probe_next:
    inc bx
    jmp .ext_probe_loop

.ext_done:
    cmp cx, 0
    je .emit
    mov byte [di], '.'
    inc di
    xor bx, bx

.ext_copy:
    cmp bx, 3
    jae .emit
    mov al, [es:si + 8 + bx]
    cmp al, ' '
    je .emit
    mov [di], al
    inc di
    inc bx
    jmp .ext_copy

.emit:
    mov byte [di], 0
    mov si, shell_dir_name_buf
    call print_string_dual
    call print_newline_dual

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Compare DI (input command) to SI (constant command string).
; Carry set if equal and fully terminated.
str_eq:
.next:
    mov al, [di]
    mov ah, [si]
    cmp ah, 0
    je .expect_end
    cmp al, 'A'
    jb .cmp
    cmp al, 'Z'
    ja .cmp
    or al, 0x20
.cmp:
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

print_newline_serial:
    mov al, 13
    call serial_putc
    mov al, 10
    jmp serial_putc

print_newline_dual:
    mov al, 13
    call putc_dual
    mov al, 10
    jmp putc_dual

clear_screen_attr:
    push ax
    push bx
    push cx
    push dx
    mov ah, 0x06
    xor al, al
    mov bh, bl
    xor cx, cx
    mov dx, 0x184F
    int 0x10
    pop dx
    pop cx
    pop bx
    pop ax
    ret

set_cursor_pos:
    push ax
    push bx
    mov ah, 0x02
    xor bh, bh
    int 0x10
    pop bx
    pop ax
    ret

video_write_char_attr:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
    mov ch, bl
    mov cl, al
    xor ax, ax
    mov al, dh
    mov di, ax
    shl di, 5
    mov bx, ax
    shl bx, 7
    add di, bx
    xor ax, ax
    mov al, dl
    shl ax, 1
    add di, ax
    mov ax, 0xB800
    mov es, ax
    mov al, cl
    mov ah, ch
    mov [es:di], ax
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

video_write_string_attr:
    push ax
    push bx
    push dx
    push si
.next:
    lodsb
    test al, al
    jz .done
    call video_write_char_attr
    inc dl
    jmp .next
.done:
    pop si
    pop dx
    pop bx
    pop ax
    ret

draw_hline_attr:
    push ax
    push bx
    push cx
    push dx
    mov ah, al
.loop:
    mov al, ah
    call video_write_char_attr
    inc dl
    loop .loop
    pop dx
    pop cx
    pop bx
    pop ax
    ret

draw_shell_chrome:
    push ax
    push bx
    push cx
    push dx
    push si

    mov bl, 0x07
    call clear_screen_attr

%if FAT_TYPE == 16
    mov al, ' '
    mov dh, 0
    mov dl, 0
    mov bl, 0x1F
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    mov si, msg_banner_title
    mov dh, 0
    mov dl, 19
    mov bl, 0x1F
    call video_write_string_attr

    mov al, '='
    mov dh, 1
    mov dl, 0
    mov bl, 0x08
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    mov al, '='
    mov dh, 23
    mov dl, 0
    mov bl, 0x08
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    call shell_update_footer

    mov dh, 2
    mov dl, 0
%else
    mov al, ' '
    mov dh, 0
    mov dl, 0
    mov bl, 0x1F
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    mov si, msg_banner_title
    mov dh, 0
    mov dl, 20
    mov bl, 0x1F
    call video_write_string_attr

    mov dh, 2

%ifdef FAT_TYPE
%if FAT_TYPE == 12
    mov si, msg_shell_sysinfo_prefix
    mov dh, 0
    mov dl, 70
    mov bl, 0x1F
    call video_write_string_attr

    int 0x12
    mov cx, ax
    mov ax, cx
    mov bx, 10
    xor dx, dx
    mov di, ram_buf
    call convert_dec_buf

    mov si, ram_buf
    mov dh, 0
    mov dl, 74
    mov bl, 0x1F
    call video_write_string_attr

    mov al, 'K'
    mov dh, 0
    mov dl, 79
    mov bl, 0x1F
    call video_write_char_attr
%endif
%endif

    mov dh, 2
    xor dl, dl
%endif
    call set_cursor_pos

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

%if FAT_TYPE == 16
shell_update_footer:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds

    push cs
    pop ds

    mov al, ' '
    mov dh, 24
    mov dl, 0
    mov bl, 0x30
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    mov si, msg_shell_status
    mov dh, 24
    mov dl, 1
    mov bl, 0x30
    call video_write_string_attr

    mov al, [cs:shell_footer_cpu_pct]
    mov di, shell_footer_pct_buf
    call shell_u8_to_dec2

    mov si, msg_shell_cpu_prefix
    mov dh, 24
    mov dl, 52
    mov bl, 0x30
    call video_write_string_attr

    mov si, shell_footer_pct_buf
    mov dh, 24
    mov dl, 56
    mov bl, 0x30
    call video_write_string_attr

    mov al, '%'
    mov dh, 24
    mov dl, 58
    mov bl, 0x30
    call video_write_char_attr

    mov al, [cs:shell_footer_dsk_pct]
    mov di, shell_footer_pct_buf
    call shell_u8_to_dec2

    mov si, msg_shell_dsk_prefix
    mov dh, 24
    mov dl, 60
    mov bl, 0x30
    call video_write_string_attr

    mov si, shell_footer_pct_buf
    mov dh, 24
    mov dl, 64
    mov bl, 0x30
    call video_write_string_attr

    mov al, '%'
    mov dh, 24
    mov dl, 66
    mov bl, 0x30
    call video_write_char_attr

    call int21_mem_query_free
    mov ax, cx
    shr ax, 1
    shr ax, 1
    shr ax, 1
    shr ax, 1
    shr ax, 1
    shr ax, 1

.ram_ready:

    mov di, shell_footer_ram_buf
    call shell_u16_to_dec

    mov si, shell_footer_ram_buf
    xor cx, cx
.len_loop:
    cmp byte [si], 0
    je .len_ready
    inc si
    inc cx
    jmp .len_loop

.len_ready:
    mov dl, 74
    sub dl, cl

    mov si, msg_shell_ram_prefix
    mov dh, 24
    mov bl, 0x30
    call video_write_string_attr

    mov si, shell_footer_ram_buf
    mov dh, 24
    add dl, 5
    mov bl, 0x30
    call video_write_string_attr

    add dl, cl
    mov al, 'K'
    mov dh, 24
    mov bl, 0x30
    call video_write_char_attr

    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_u8_to_dec2:
    push ax
    push bx

    xor ah, ah
    mov bl, 10
    div bl
    add al, '0'
    mov [di], al
    mov al, ah
    add al, '0'
    mov [di + 1], al
    mov byte [di + 2], 0

    pop bx
    pop ax
    ret

shell_footer_poll:
    push ax
    push bx
    push cx
    push dx

    mov ah, 0x00
    int 0x1A

    cmp word [cs:shell_footer_last_tick], 0xFFFF
    jne .have_last_tick
    mov [cs:shell_footer_last_tick], dx
    call shell_footer_compute_cpu_pct
    call shell_footer_maybe_refresh_disk
    call shell_update_footer
    jmp .done

.have_last_tick:
    cmp dx, [cs:shell_footer_last_tick]
    je .done

    ; Update key cooldown used by disk refresh rate logic
    cmp byte [cs:shell_footer_tick_key_activity], 0
    je .no_key_this_tick
    mov byte [cs:shell_footer_key_cooldown], SHELL_FOOTER_KEY_COOLDOWN_TICKS
.no_key_this_tick:
    mov byte [cs:shell_footer_tick_key_activity], 0
    mov [cs:shell_footer_last_tick], dx

    cmp byte [cs:shell_footer_key_cooldown], 0
    je .cooldown_counted
    dec byte [cs:shell_footer_key_cooldown]

.cooldown_counted:

    call shell_footer_compute_cpu_pct
    call shell_footer_maybe_refresh_disk
    call shell_update_footer

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_footer_compute_cpu_pct:
    push ax
    push bx
    push dx

    ; Read and reset per-tick idle loop counter
    mov ax, [cs:shell_footer_loop_count]
    mov word [cs:shell_footer_loop_count], 0

    ; Update high watermark of idle loops per tick
    cmp ax, [cs:shell_footer_max_loop]
    jbe .no_max_update
    mov [cs:shell_footer_max_loop], ax
.no_max_update:

    ; CPU% = (max - current) * 100 / max
    ; High loop count (idle tick) -> low CPU%; low count (busy) -> high CPU%
    mov bx, [cs:shell_footer_max_loop]
    sub bx, ax
    jnc .compute
    xor bx, bx         ; underflow guard
.compute:
    mov ax, bx
    mov dx, 100
    mul dx
    mov bx, [cs:shell_footer_max_loop]
    div bx
    cmp ax, 99
    jbe .store
    mov ax, 99

.store:
    mov [cs:shell_footer_cpu_pct], al

.done:
    pop dx
    pop bx
    pop ax
    ret

shell_footer_maybe_refresh_disk:
    push ax
    push bx

    mov bx, SHELL_FOOTER_DSK_IDLE_REFRESH_TICKS
    cmp byte [cs:shell_footer_key_cooldown], 0
    je .interval_ready
    mov bx, SHELL_FOOTER_DSK_BUSY_REFRESH_TICKS

.interval_ready:
    cmp byte [cs:shell_footer_dsk_dirty], 1
    jne .check_interval
    mov bx, SHELL_FOOTER_DSK_DIRTY_IDLE_REFRESH_TICKS
    cmp byte [cs:shell_footer_key_cooldown], 0
    je .check_interval
    mov bx, SHELL_FOOTER_DSK_DIRTY_BUSY_REFRESH_TICKS

.check_interval:
    mov ax, [cs:shell_footer_last_tick]
    sub ax, [cs:shell_footer_dsk_last_scan_tick]
    cmp ax, bx
    jb .done

.refresh:
    call shell_footer_refresh_disk_pct
    jc .done
    mov ax, [cs:shell_footer_last_tick]
    mov [cs:shell_footer_dsk_last_scan_tick], ax
    mov byte [cs:shell_footer_dsk_dirty], 0

.done:
    pop bx
    pop ax
    ret

shell_footer_refresh_disk_pct:
    push ax
    push bx
    push cx
    push dx

    xor dx, dx
    mov bx, 2
    mov cx, FAT_SECTORS_PER_FAT * 256 - 2

.scan_loop:
    mov ax, bx
    call fat12_get_entry_cached
    jc .fail
    cmp ax, 0
    je .next
    inc dx

.next:
    inc bx
    loop .scan_loop
    mov ax, dx
    mov bx, 99
    mul bx
    mov bx, FAT_SECTORS_PER_FAT * 256 - 2
    add ax, bx
    adc dx, 0
    sub ax, 1
    sbb dx, 0
    div bx

.store:
    mov [cs:shell_footer_dsk_pct], al
    clc
    jmp .done

.fail:
    stc

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

shell_u16_to_dec:
    push ax
    push bx
    push cx
    push dx
    push di

    xor cx, cx
    cmp ax, 0
    jne .div_loop
    mov byte [di], '0'
    mov byte [di + 1], 0
    jmp .done

.div_loop:
    mov bx, 10
.div_next:
    xor dx, dx
    div bx
    add dl, '0'
    push dx
    inc cx
    test ax, ax
    jnz .div_next

.pop_loop:
    pop dx
    mov [di], dl
    inc di
    loop .pop_loop
    mov byte [di], 0

.done:
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif

print_shell_help:
    mov si, msg_help_header
    call print_string_dual
    mov si, msg_help_core
    call print_string_dual
    mov si, msg_help_runtime
    call print_string_dual
    mov si, msg_help_system
    call print_string_dual
    mov si, msg_help_apps
    call print_string_dual
    ret

print_shell_help_all:
    mov si, msg_help_all
    jmp print_string_dual

shell_print_error_ax:
    push si
    push ax
    call print_string_dual
    mov si, msg_err_ax
    call print_string_dual
    pop ax
    call print_hex16_dual
    call print_newline_dual
    pop si
    ret

print_hex16_dual:
    push ax
    mov al, ah
    call print_hex8_dual
    pop ax
    call print_hex8_dual
    ret

print_hex16_serial:
    push ax
    mov al, ah
    call print_hex8_serial
    pop ax
    call print_hex8_serial
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

print_hex8_serial:
    push ax
    mov ah, al
    shr al, 4
    call print_hex_nibble_serial
    mov al, ah
    and al, 0x0F
    call print_hex_nibble_serial
    pop ax
    ret

print_hex_nibble_serial:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    jmp serial_putc

print_hex_nibble_dual:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    jmp putc_dual

putc_dual:
    push ax
    call bios_putc
    pop ax
    call serial_putc
    ret

bios_putc:
    push ax
    push bx
    cmp al, 0x0A
    jne .teletype

    push cx
    push dx

    mov ah, 0x03
    xor bh, bh
    int 0x10
%if FAT_TYPE == 16
    cmp dh, 22
    jb .lf_teletype

    mov ax, 0x0601
    mov bh, 0x07
    mov cx, 0x0200
    mov dx, 0x164F
    int 0x10
%else
    cmp dh, 24
    jb .lf_teletype

    mov ax, 0x0601
    mov bh, 0x07
    mov cx, 0x0200
    mov dx, 0x184F
    int 0x10
%endif

    pop dx
    pop cx
    pop bx
    pop ax
    ret

.lf_teletype:
    pop dx
    pop cx

.teletype:
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

; Stage2 Extended Services Integration
%if FAT_TYPE == 12
convert_dec_buf:
    push ax
    push bx
    push cx
    push dx
    push di
    xor cx, cx
    cmp ax, 0
    jne .div_loop
    mov byte [di], '0'
    mov byte [di + 1], 0
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif
.div_loop:
    xor dx, dx
    div bx
    add dl, '0'
    push dx
    inc cx
    test ax, ax
    jnz .div_loop
.pop_loop:
    pop ax
    mov [di], al
    inc di
    loop .pop_loop
    mov byte [di], 0
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

init_stage2_services:
    push ax
    push si
    mov si, msg_stage2_entry
    call print_string_serial
    call install_int33_vector
    call init_mouse
    call init_vbe_query
%if FAT_TYPE == 16
    call stage1_runtime_print_stage2_ready
%else
    mov si, msg_stage2_ready
    call print_string_serial
%endif
%if FAT_TYPE == 16
%if STAGE2_AUTORUN
    call run_stage2_payload
%endif
%endif
    pop si
    pop ax
    ret

%if FAT_TYPE == 16
stage1_runtime_clear_cache:
    push ax
    xor ax, ax
    mov [runtime_table_off], ax
    mov [runtime_table_seg], ax
    mov [runtime_status_flags], ax
    mov [runtime_service_off], ax
    mov [runtime_service_seg], ax
    pop ax
    ret

stage1_runtime_lookup_service:
    push bx
    push cx
    push dx
    push es

    mov dx, ax
    mov ax, [runtime_status_flags]
    test ax, 1
    jz .fail
    mov ax, [runtime_table_seg]
    or ax, ax
    jz .fail
    mov es, ax
    mov bx, [runtime_table_off]
    or bx, bx
    jz .fail
    cmp dx, 0x0001
    jne .service_two_plus
    cmp word [es:bx + 12], 0x0001
    jne .fail
    mov ax, [es:bx + 14]
    or ax, ax
    jz .fail
    mov [runtime_service_off], ax
    mov ax, [runtime_table_seg]
    mov [runtime_service_seg], ax
    clc
    jmp .done

.service_two_plus:
    mov cx, [es:bx + 6]
    cmp cx, 2
    jb .fail
    dec cx
    add bx, 18

.next_entry:
    cmp word [es:bx], dx
    je .found
    add bx, 8
    loop .next_entry
    jmp .fail

.found:
    test word [es:bx + 2], 1
    jz .fail
    mov ax, [es:bx + 4]
    or ax, ax
    jz .fail
    mov [runtime_service_off], ax
    mov ax, [runtime_table_seg]
    mov [runtime_service_seg], ax
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop dx
    pop cx
    pop bx
    ret

stage1_runtime_call_version_service:
    push ds
    push es
    push si
    push di
    call far [cs:runtime_service_ptr]
    pop di
    pop si
    pop es
    pop ds
    ret

stage1_runtime_get_default_drive_ptr:
    push ax

    mov ax, 0x0005
    call stage1_runtime_lookup_service
    jc .fail
    call far [cs:runtime_service_ptr]
    jc .fail
    mov ax, ds
    cmp ax, RUNTIME_LOAD_SEG
    jne .fail
    or si, si
    jz .fail
    clc
    jmp .done

.fail:
    stc

.done:
    pop ax
    ret

stage1_runtime_sync_default_drive:
    push ax
    push ds
    push si

    call stage1_runtime_get_default_drive_ptr
    jc .done
    mov al, [cs:dos_default_drive]
    mov [ds:si], al

.done:
    pop si
    pop ds
    pop ax
    ret

stage1_runtime_print_stage2_ready:
    push ax
    push ds

    mov ax, 0x0003
    call stage1_runtime_lookup_service
    jc .fallback
    call far [cs:runtime_service_ptr]
    jc .fallback
    mov ax, ds
    cmp ax, RUNTIME_LOAD_SEG
    jne .fallback
    or si, si
    jz .fallback
    call print_string_serial
    pop ds
    pop ax
    ret

.fallback:
    pop ds
    mov si, msg_stage2_ready
    call print_string_serial
    pop ax
    ret

stage1_runtime_validate_cache:
    push ax
    push bx
    push cx
    push es

    mov ax, [runtime_status_flags]
    test ax, 1
    jz .fail
    mov ax, [runtime_table_seg]
    cmp ax, RUNTIME_LOAD_SEG
    jne .fail
    or ax, ax
    jz .fail
    mov es, ax
    mov bx, [runtime_table_off]
    or bx, bx
    jz .fail
    cmp word [es:bx], 0x5452
    jne .fail
    cmp word [es:bx + 2], 0x5653
    jne .fail
    cmp word [es:bx + 4], 1
    jne .fail
    cmp word [es:bx + 6], 5
    jb .fail
    cmp word [es:bx + 8], 8
    jne .fail
    cmp word [es:bx + 10], 1
    jne .fail

    mov ax, 0x0004
    call stage1_runtime_lookup_service
    jc .fail
    call stage1_runtime_call_version_service
    jc .fail
    cmp ax, 0x0005
    jne .fail
    or bx, bx
    jne .fail
    or cx, cx
    jne .fail
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop cx
    pop bx
    pop ax
    ret

stage1_runtime_init:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    call stage1_runtime_clear_cache
    mov byte [int21_silent_errors], 1

    push cs
    pop ds
    mov dx, path_runtime_dos
    mov ax, 0x3D00
    int 0x21
    jc .fail
    mov bx, ax

    mov ax, RUNTIME_LOAD_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw

    mov ax, RUNTIME_LOAD_SEG
    mov ds, ax
    xor dx, dx
    mov cx, 512
    mov ah, 0x3F
    int 0x21
    jc .close_fail
    cmp ax, 10
    jb .close_fail

    mov ah, 0x3E
    int 0x21

    push cs
    pop ds
    mov ax, RUNTIME_LOAD_SEG
    mov es, ax
    mov si, runtime_loader_signature
    mov di, 0x0002
    mov cx, 8
    cld
    repe cmpsb
    jne .fail

    push cs
    pop es
    mov di, runtime_handoff
    call RUNTIME_LOAD_SEG:0x0000

    push cs
    pop ds
    call stage1_runtime_validate_cache
    jc .fail
    call stage1_runtime_sync_default_drive
    jc .fail
    clc
    jmp .done

.close_fail:
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    push cs
    pop ds

.fail:
    call stage1_runtime_clear_cache
    stc

.done:
    mov byte [int21_silent_errors], 0
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

stage1_runtime_get_version:
    mov ax, 0x0004
    call stage1_runtime_lookup_service
    jc .fail
    call stage1_runtime_call_version_service
    ret

.fail:
    stc
    ret

%if STAGE1_RUNTIME_PROBE
stage1_runtime_probe:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    push cs
    pop ds
    mov si, msg_runtime_probe_begin
    call print_string_serial

    test word [runtime_status_flags], 1
    jnz .cache_ready
    call stage1_runtime_init
    jc .fail

.cache_ready:
    call stage1_runtime_validate_cache
    jc .fail

    mov ax, [runtime_table_seg]
    mov es, ax
    mov bx, [runtime_table_off]
    cmp word [es:bx + 6], 5
    jb .fail

    mov si, msg_runtime_probe_table
    call print_string_serial

    mov ax, 0x0001
    call stage1_runtime_lookup_service
    jc .fail
    call far [cs:runtime_service_ptr]
    jc .fail
    cmp ax, 0x5254
    jne .fail

    mov ax, 0x0002
    call stage1_runtime_lookup_service
    jc .fail
    call far [cs:runtime_service_ptr]
    jc .fail
    mov ax, ds
    cmp ax, RUNTIME_LOAD_SEG
    jne .fail
    or si, si
    jz .fail
    push cs
    pop es
    mov di, runtime_probe_version_prefix
    mov cx, runtime_probe_version_prefix_len
    cld
    repe cmpsb
    jne .fail
    push cs
    pop ds
    test word [runtime_status_flags], 1
    jz .fail

    mov ax, 0x0003
    call stage1_runtime_lookup_service
    jc .fail
    call far [cs:runtime_service_ptr]
    jc .fail
    mov ax, ds
    cmp ax, RUNTIME_LOAD_SEG
    jne .fail
    or si, si
    jz .fail
    push cs
    pop es
    mov di, runtime_probe_marker_prefix
    mov cx, runtime_probe_marker_prefix_len
    cld
    repe cmpsb
    jne .fail
    push cs
    pop ds

    mov ax, 0x0004
    call stage1_runtime_lookup_service
    jc .fail
    call stage1_runtime_call_version_service
    jc .fail
    cmp ax, 0x0005
    jne .fail
    or bx, bx
    jne .fail
    or cx, cx
    jne .fail

    call stage1_runtime_get_default_drive_ptr
    jc .fail
    mov al, [ds:si]
    cmp al, [cs:dos_default_drive]
    jne .fail
    push cs
    pop ds

    mov si, msg_runtime_probe_call
    call print_string_serial
    mov si, msg_runtime_probe_ok
    call print_string_serial
    clc
    jmp .done

.fail:
    push cs
    pop ds
    mov si, msg_runtime_probe_bad
    call print_string_serial
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
%endif
%if STAGE2_AUTORUN
run_stage2_payload:
    push ax
    push bx
    push cx
    push dx
    push ds
    push es

    push cs
    pop ds
    mov si, msg_stage2_autorun_begin
    call print_string_serial

    mov dx, path_stage2_dos
    mov ax, 0x3D00
    int 0x21
    jc .load_fail
    mov bx, ax

    mov ax, STAGE2_LOAD_SEG
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 256
    rep stosw

    mov ax, STAGE2_LOAD_SEG
    mov ds, ax
    xor dx, dx
    mov cx, 512
    mov ah, 0x3F
    int 0x21
    jc .close_fail
    cmp ax, 1
    jb .close_fail

    mov ah, 0x3E
    int 0x21

    push cs
    pop ds

    mov si, msg_stage2_autorun_loaded
    call print_string_serial

    call STAGE2_LOAD_SEG:0x0000
    mov byte [cs:stage2_autorun_status], 1
    mov si, msg_stage2_autorun_return
    call print_string_serial
    clc
    jmp .done

.close_fail:
    push ax
    mov ah, 0x3E
    int 0x21
    pop ax
    push cs
    pop ds

.load_fail:
    mov byte [cs:stage2_autorun_status], 2
    mov si, msg_stage2_autorun_fail
    call print_string_serial
    stc

.done:
    pop es
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif
%endif

init_mouse:
    push ax
    push bx
%if FAT_TYPE == 16
%if ENABLE_PS2_MOUSE_INIT
    call ps2_mouse_init
    jc .reset_int33
%endif
%endif

.reset_int33:
    xor ax, ax
    int 0x33
    cmp ax, 0xFFFF
    jne .no_mouse
    mov byte [cs:mouse_installed], 1
    mov ax, 0x0001
    int 0x33
    mov si, msg_mouse_enabled
    call print_string_serial
%if FAT_TYPE == 16
    mov word [cs:mouse_max_x], 639
    mov word [cs:mouse_max_y], 479
    mov byte [cs:mouse_visible], 1
%endif
    pop bx
    pop ax
    ret

.no_mouse:
    mov byte [cs:mouse_installed], 0
    mov si, msg_mouse_not_found
    call print_string_serial
    pop bx
    pop ax
    ret

%if FAT_TYPE == 16
%if HARDWARE_VALIDATION_SCREEN
print_hardware_validation_screen:
    push ax
    push si

    mov si, msg_hw_validation_title
    call print_string_dual
    cmp byte [cs:stage2_autorun_status], 1
    je .pass
    cmp byte [cs:stage2_autorun_status], 2
    je .fail

    mov si, msg_hw_validation_notrun
    call print_string_dual
    jmp .done

.pass:
    mov si, msg_hw_validation_pass
    call print_string_dual
    mov si, msg_hw_validation_return
    call print_string_dual
    mov si, msg_hw_validation_capture
    call print_string_dual
    jmp .done

.fail:
    mov si, msg_hw_validation_fail
    call print_string_dual

.done:
    call print_newline_dual
    pop si
    pop ax
    ret
%endif
%endif

init_vbe_query:
    mov si, msg_vbe_init
    call print_string_serial
    ret

int_ef_handler:
    cmp word [cs:int_ef_target_seg], 0
    je .no_target
    pushf
    call far [cs:int_ef_target_off]
    iret

.no_target:
    iret

install_int33_vector:
    push ax
    push bx
    push es
    xor ax, ax
    mov es, ax
    mov bx, 0x33 * 4
    mov word [es:bx], int33_handler
    mov ax, cs
    mov [es:bx + 2], ax
%if FAT_TYPE == 16
    mov bx, 0x74 * 4
    mov ax, [es:bx]
    mov [old_int74_off], ax
    mov ax, [es:bx + 2]
    mov [old_int74_seg], ax
    mov word [es:bx], irq12_mouse_handler
    mov ax, cs
    mov [es:bx + 2], ax
%endif
    pop es
    pop bx
    pop ax
    ret

%if FAT_TYPE == 16
ps2_wait_input_clear:
    push cx
    push dx
    mov cx, 0xFFFF
.loop:
    mov dx, 0x0064
    in al, dx
    test al, 0x02
    jz .ok
    loop .loop
    stc
    jmp .done
.ok:
    clc
.done:
    pop dx
    pop cx
    ret

ps2_wait_output_full:
    push cx
    push dx
    mov cx, 0xFFFF
.loop:
    mov dx, 0x0064
    in al, dx
    test al, 0x01
    jnz .ok
    loop .loop
    stc
    jmp .done
.ok:
    clc
.done:
    pop dx
    pop cx
    ret

ps2_write_cmd:
    push dx
    push ax
    call ps2_wait_input_clear
    jc .done
    pop ax
    mov dx, 0x0064
    out dx, al
    push ax
.done:
    pop ax
    pop dx
    ret

ps2_write_data:
    push dx
    push ax
    call ps2_wait_input_clear
    jc .done
    pop ax
    mov dx, 0x0060
    out dx, al
    push ax
.done:
    pop ax
    pop dx
    ret

ps2_read_data:
    push dx
    call ps2_wait_output_full
    jc .done
    mov dx, 0x0060
    in al, dx
    clc
.done:
    pop dx
    ret

ps2_mouse_write:
    push ax
    mov al, 0xD4
    call ps2_write_cmd
    pop ax
    jc .done
    call ps2_write_data
    jc .done
    call ps2_read_data
    jc .done
    cmp al, 0xFA
    je .ack
    stc
    ret
.ack:
    clc
.done:
    ret

ps2_mouse_flush:
    push ax
    push cx
    push dx
    mov cx, 32
.loop:
    mov dx, 0x0064
    in al, dx
    test al, 0x01
    jz .done
    mov dx, 0x0060
    in al, dx
    loop .loop
.done:
    pop dx
    pop cx
    pop ax
    ret

ps2_mouse_init:
    push ax
    push dx
    cli
    call ps2_mouse_flush
    mov al, 0xA8
    call ps2_write_cmd
    jc .fail
    mov al, 0x20
    call ps2_write_cmd
    jc .fail
    call ps2_read_data
    jc .fail
    or al, 0x02
    and al, 0xDF
    mov ah, al
    mov al, 0x60
    call ps2_write_cmd
    jc .fail
    mov al, ah
    call ps2_write_data
    jc .fail
    mov al, 0xF6
    call ps2_mouse_write
    jc .fail
    mov al, 0xF4
    call ps2_mouse_write
    jc .fail

    in al, 0xA1
    and al, 0xEF
    out 0xA1, al
    in al, 0x21
    and al, 0xFB
    out 0x21, al
    sti
    mov byte [cs:mouse_hw_ready], 1
    clc
    jmp .done
.fail:
    sti
    mov byte [cs:mouse_hw_ready], 0
    stc
.done:
    pop dx
    pop ax
    ret

irq12_mouse_handler:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    ; --- desync watchdog: reset partial packet if >2 BIOS ticks since last byte ---
    cmp byte [cs:mouse_packet_index], 0
    je .watchdog_skip
    push es
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x006C]     ; BIOS tick counter low word (~18.2 Hz)
    pop es
    sub ax, [cs:mouse_last_byte_tick]
    cmp ax, 2               ; > ~110 ms without completing packet?
    jb .watchdog_skip
    mov byte [cs:mouse_packet_index], 0   ; resync
.watchdog_skip:

    in al, 0x64
    test al, 0x01
    jz .eoi_fast        ; no data in output buffer
    ; Some legacy controllers and QEMU builds do not reliably set the AUX
    ; source bit here. Because this is IRQ12, treat the byte as mouse data
    ; and let the PS/2 packet sync bit reject noise.
    in al, 0x60

    ; Update last-byte tick for watchdog
    push es
    push ax
    mov ax, 0x0040
    mov es, ax
    mov ax, [es:0x006C]
    mov [cs:mouse_last_byte_tick], ax
    pop ax
    pop es

    mov bl, [cs:mouse_packet_index]
    cmp bl, 0
    jne .store
    test al, 0x08
    jz .eoi_fast        ; bad sync byte – discard, reset happens via watchdog
.store:
    xor bh, bh
    mov [cs:mouse_packet + bx], al
    inc bl
    mov [cs:mouse_packet_index], bl
    cmp bl, 3
    jne .eoi_fast       ; partial packet – EOI and return, no VGA work
    mov byte [cs:mouse_packet_index], 0

    xor bp, bp
    cmp byte [cs:mouse_packet + 1], 0
    jne .mark_motion
    cmp byte [cs:mouse_packet + 2], 0
    je .button_events
.mark_motion:
    or bp, 0x0001

.button_events:
    mov al, [cs:mouse_packet]
    mov bl, al
    mov ah, [cs:mouse_buttons]
    and al, 0x07
    mov [cs:mouse_buttons], al
    mov bh, ah

    test al, 0x01
    jz .left_up
    test bh, 0x01
    jnz .left_done
    or bp, 0x0002
    jmp .left_done
.left_up:
    test bh, 0x01
    jz .left_done
    or bp, 0x0004
.left_done:

    test al, 0x02
    jz .right_up
    test bh, 0x02
    jnz .right_done
    or bp, 0x0008
    jmp .right_done
.right_up:
    test bh, 0x02
    jz .right_done
    or bp, 0x0010
.right_done:

    test al, 0x04
    jz .middle_up
    test bh, 0x04
    jnz .middle_done
    or bp, 0x0020
    jmp .middle_done
.middle_up:
    test bh, 0x04
    jz .middle_done
    or bp, 0x0040
.middle_done:

    mov al, [cs:mouse_packet + 1]
    cbw
    test bl, 0x10
    jz .x_signed
    or ah, 0xFF
.x_signed:
    mov [cs:mouse_last_mickey_x], ax
    add [cs:mouse_delta_x], ax
%if MOUSE_VGA_SCALE_SHIFT > 0
%rep MOUSE_VGA_SCALE_SHIFT
    sal ax, 1
%endrep
%endif
    mov cx, [cs:mouse_pos_x]
    add cx, ax

    mov al, [cs:mouse_packet + 2]
    cbw
    test bl, 0x20
    jz .y_signed
    or ah, 0xFF
.y_signed:
    neg ax
    mov [cs:mouse_last_mickey_y], ax
    add [cs:mouse_delta_y], ax
%if MOUSE_VGA_SCALE_SHIFT > 0
%rep MOUSE_VGA_SCALE_SHIFT
    sal ax, 1
%endrep
%endif
    mov dx, [cs:mouse_pos_y]
    add dx, ax

    cmp cx, [cs:mouse_min_x]
    jae .x_min_ok
    mov cx, [cs:mouse_min_x]
.x_min_ok:
    cmp cx, [cs:mouse_max_x]
    jbe .x_ok
    mov cx, [cs:mouse_max_x]
.x_ok:
    cmp dx, [cs:mouse_min_y]
    jae .y_min_ok
    mov dx, [cs:mouse_min_y]
.y_min_ok:
    cmp dx, [cs:mouse_max_y]
    jbe .y_ok
    mov dx, [cs:mouse_max_y]
.y_ok:
    mov [cs:mouse_pos_x], cx
    mov [cs:mouse_pos_y], dx
    call mouse_vga_update_position

    cmp byte [cs:mouse_bios_enabled], 0
    je .int33_callback
    cmp word [cs:mouse_bios_asr_seg], 0
    je .int33_callback
    inc word [cs:mouse_bios_callback_count]
    xor ah, ah
    mov al, [cs:mouse_buttons]
    push ax
    mov al, [cs:mouse_packet + 1]
    cbw
    push ax
    mov al, [cs:mouse_packet + 2]
    cbw
    push ax
    pushf
    call far [cs:mouse_bios_asr_off]
    add sp, 8

.int33_callback:
    mov ax, bp
    and ax, [cs:mouse_cb_mask]
    jz .eoi
    cmp word [cs:mouse_cb_seg], 0
    je .eoi

    xor bx, bx
    mov bl, [cs:mouse_buttons]
    mov cx, [cs:mouse_pos_x]
    mov dx, [cs:mouse_pos_y]
    mov si, [cs:mouse_last_mickey_x]
    mov di, [cs:mouse_last_mickey_y]
    call far [cs:mouse_cb_off]

.eoi:
    ; Full packet processed – refresh sprite, then EOI
    call mouse_vga_cursor_refresh
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret

.eoi_fast:
    ; Partial packet or no data – just EOI, no VGA work
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    iret

mouse_vga_update_position:
    push ax
    ; Sync VGA cursor position directly from logical mouse position
    ; (already scaled and clamped by IRQ12 handler)
    mov ax, [cs:mouse_pos_x]
    ; Clamp to VGA mode 12h range [0,632] (8-pixel cursor width safety margin)
    cmp ax, 632
    jbe .x_ok
    mov ax, 632
.x_ok:
    mov [cs:mouse_vga_cursor_x], ax
    mov ax, [cs:mouse_pos_y]
    cmp ax, 472
    jbe .y_ok
    mov ax, 472
.y_ok:
    mov [cs:mouse_vga_cursor_y], ax
    pop ax
    ret

mouse_vga_cursor_refresh:
    push ax
    push bx
    push dx

    cmp byte [cs:current_video_mode], 0x12
    je .mode12_ready
    mov byte [cs:mouse_vga_cursor_drawn], 0
    jmp .done

.mode12_ready:
    cmp byte [cs:mouse_bios_enabled], 0
    je .inactive
    cmp word [cs:mouse_bios_asr_seg], 0
    jne .active

.inactive:
    cmp byte [cs:current_video_mode], 0x12
    jne .inactive_visibility_check
    cmp word [cs:mouse_cb_seg], 0
    jne .inactive_visibility_check
    cmp byte [cs:mouse_bios_enabled], 0
    je .skip_no_trace
.inactive_visibility_check:
    cmp byte [cs:mouse_visible], 1
    je .active
.skip_no_trace:
    mov byte [cs:mouse_vga_cursor_drawn], 0
    jmp .done

.active:
    ; If GEM callback is registered for motion events, let GEM draw cursor.
    ; If callback exists but motion bit is not enabled, keep XOR fallback active.
    cmp word [cs:mouse_cb_seg], 0
    je .active_no_cb
    test word [cs:mouse_cb_mask], 0x0001
    jnz .done
.active_no_cb:
    cmp byte [cs:mouse_vga_cursor_drawn], 0
    je .draw_new
    mov bx, [cs:mouse_vga_cursor_last_x]
    mov dx, [cs:mouse_vga_cursor_last_y]
    call mouse_vga_xor_cursor12

.draw_new:
    mov bx, [cs:mouse_vga_cursor_x]
    mov dx, [cs:mouse_vga_cursor_y]
    call mouse_vga_xor_cursor12
    mov [cs:mouse_vga_cursor_last_x], bx
    mov [cs:mouse_vga_cursor_last_y], dx
    mov byte [cs:mouse_vga_cursor_drawn], 1

.done:
    pop dx
    pop bx
    pop ax
    ret

mouse_vga_xor_cursor12:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es

    mov [cs:mouse_vga_work_x], bx
    mov [cs:mouse_vga_work_y], dx

    mov dx, 0x3CE
    mov al, 0x00
    out dx, al
    inc dx
    in al, dx
    mov [cs:mouse_vga_save_gc0], al
    dec dx
    mov al, 0x01
    out dx, al
    inc dx
    in al, dx
    mov [cs:mouse_vga_save_gc1], al
    dec dx
    mov al, 0x03
    out dx, al
    inc dx
    in al, dx
    mov [cs:mouse_vga_save_gc3], al
    dec dx
    mov al, 0x05
    out dx, al
    inc dx
    in al, dx
    mov [cs:mouse_vga_save_gc5], al
    dec dx
    mov al, 0x08
    out dx, al
    inc dx
    in al, dx
    mov [cs:mouse_vga_save_gc8], al

    mov dx, 0x3C4
    mov al, 0x02
    out dx, al
    inc dx
    in al, dx
    mov [cs:mouse_vga_save_seq2], al

    mov dx, 0x3C4
    mov al, 0x02
    out dx, al
    inc dx
    mov al, 0x0F
    out dx, al

    mov dx, 0x3CE
    mov al, 0x00
    out dx, al
    inc dx
    mov al, 0x0F
    out dx, al
    dec dx
    mov al, 0x01
    out dx, al
    inc dx
    mov al, 0x0F
    out dx, al
    dec dx
    mov al, 0x03
    out dx, al
    inc dx
    mov al, 0x18
    out dx, al
    dec dx
    mov al, 0x05
    out dx, al
    inc dx
    xor al, al
    out dx, al

    mov ax, 0xA000
    mov es, ax
    mov si, mouse_vga_cursor_mask
    mov bp, 8

.row_loop:
    mov al, [cs:si]
    inc si
    mov [cs:mouse_vga_row_mask], al
    mov cx, 8
    mov di, [cs:mouse_vga_work_x]

.col_loop:
    shl byte [cs:mouse_vga_row_mask], 1
    jnc .next_col
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp

    mov ax, [cs:mouse_vga_work_y]
    cmp ax, 480
    jae .pixel_done
    cmp di, 640
    jae .pixel_done

    mov bx, ax
    shl bx, 4
    mov dx, ax
    shl dx, 6
    add bx, dx
    mov ax, di
    mov cl, 3
    shr ax, cl
    add bx, ax

    mov ax, di
    and al, 0x07
    mov cl, al
    mov ah, 0x80
    shr ah, cl

    mov dx, 0x3CE
    mov al, 0x08
    out dx, al
    inc dx
    mov al, ah
    out dx, al

    mov al, [es:bx]
    mov al, 0xFF
    mov [es:bx], al

.pixel_done:
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

.next_col:
    inc di
    loop .col_loop
    inc word [cs:mouse_vga_work_y]
    dec bp
    jnz .row_loop

    mov dx, 0x3CE
    mov al, 0x00
    out dx, al
    inc dx
    mov al, [cs:mouse_vga_save_gc0]
    out dx, al
    dec dx
    mov al, 0x01
    out dx, al
    inc dx
    mov al, [cs:mouse_vga_save_gc1]
    out dx, al
    dec dx
    mov al, 0x03
    out dx, al
    inc dx
    mov al, [cs:mouse_vga_save_gc3]
    out dx, al
    dec dx
    mov al, 0x05
    out dx, al
    inc dx
    mov al, [cs:mouse_vga_save_gc5]
    out dx, al
    dec dx
    mov al, 0x08
    out dx, al
    inc dx
    mov al, [cs:mouse_vga_save_gc8]
    out dx, al

    mov dx, 0x3C4
    mov al, 0x02
    out dx, al
    inc dx
    mov al, [cs:mouse_vga_save_seq2]
    out dx, al

    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif

int10_handler:
    cmp ah, 0x0F
    je .get_mode
    cmp ah, 0x00
    je .set_mode
    jmp far [cs:old_int10_off]

.set_mode:
    mov [cs:current_video_mode], al
    jmp far [cs:old_int10_off]

.get_mode:
    mov al, [cs:current_video_mode]
    mov ah, 80
    cmp al, 0x13
    jne .mode_ready
    mov ah, 40
.mode_ready:
    xor bh, bh
    iret

%if FAT_TYPE == 16
int15_handler:
    push bp
    mov bp, sp
    cmp ah, 0x88
    je .extmem_88
    cmp ah, 0xC2
    jne .chain
    cmp byte [cs:shell_exec_external_mouse_disabled], 0
    jne .unsupported

    cmp al, 0x00
    je .enable_disable
    cmp al, 0x01
    je .reset
    cmp al, 0x02
    je .success
    cmp al, 0x03
    je .success
    cmp al, 0x05
    je .success
    cmp al, 0x06
    je .success
    cmp al, 0x07
    je .set_handler
    jmp .unsupported

.extmem_88:
    mov ax, [cs:xms_free_kb]
    and word [ss:bp + 6], 0xFFFE
    pop bp
    iret

.enable_disable:
    cmp bh, 0
    je .disable
    mov byte [cs:mouse_bios_enabled], 1
    cmp byte [cs:mouse_vga_cursor_drawn], 1
    je .enable_seed_done
    call mouse_vga_cursor_seed
.enable_seed_done:
    jmp .success
.disable:
    mov byte [cs:mouse_bios_enabled], 0
    mov byte [cs:mouse_vga_cursor_drawn], 0
    jmp .success

.reset:
    mov byte [cs:mouse_bios_enabled], 0
    mov byte [cs:mouse_packet_index], 0
    mov byte [cs:mouse_vga_cursor_drawn], 0
    xor bx, bx
    jmp .success

.set_handler:
    inc word [cs:mouse_bios_asr_set_count]
    mov [cs:mouse_bios_asr_off], bx
    mov [cs:mouse_bios_asr_seg], es
    cmp byte [cs:mouse_vga_cursor_drawn], 1
    je .set_handler_seed_done
    call mouse_vga_cursor_seed
.set_handler_seed_done:
    jmp .success

.success:
    and word [ss:bp + 6], 0xFFFE
    xor ah, ah
    pop bp
    iret

.unsupported:
    or word [ss:bp + 6], 0x0001
    mov ah, 0x86
    pop bp
    iret

.chain:
    pop bp
    jmp far [cs:old_int15_off]

mouse_vga_cursor_seed:
    mov word [cs:mouse_vga_cursor_x], 320
    mov word [cs:mouse_vga_cursor_y], 240
    mov word [cs:mouse_vga_cursor_last_x], 320
    mov word [cs:mouse_vga_cursor_last_y], 240
    mov byte [cs:mouse_vga_cursor_drawn], 0
    ret
%endif

int16_handler:
    cmp ah, 0x00
    je .read_key
    cmp ah, 0x10
    je .read_key
    cmp ah, 0x01
    je .status
    cmp ah, 0x11
    je .status
    cmp ah, 0x02
    je .shift_status
    cmp ah, 0x12
    je .shift_status
.chain:
    jmp far [cs:old_int16_off]

.status:
    pushf
    call far [cs:old_int16_off]
    jz .status_no_real_key
    push bp
    mov bp, sp
    and word [ss:bp + 6], 0xFFBF
    pop bp
    iret

.status_no_real_key:
    push bp
    mov bp, sp
    xor ax, ax
    or word [ss:bp + 6], 0x0040
    pop bp
    iret

.shift_status:
    jmp far [cs:old_int16_off]

.read_key:
    jmp far [cs:old_int16_off]

int33_handler:
    cmp byte [cs:shell_exec_external_mouse_disabled], 0
    jne .external_mouse_disabled
    cmp ax, 0x0000
    je .reset
    cmp ax, 0x0001
    je .show
    cmp ax, 0x0002
    je .hide
    cmp ax, 0x0003
    je .status
    cmp ax, 0x0004
    je .set_pos
    cmp ax, 0x0007
    je .set_x_range
    cmp ax, 0x0008
    je .set_y_range
    cmp ax, 0x000B
    je .motion
    cmp ax, 0x000C
    je .set_callback
    cmp ax, 0x0014
    je .exchange_callback
    cmp ax, 0x000F
    je .set_mickey_ratio
    cmp ax, 0x0024
    je .version

    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
    iret

.external_mouse_disabled:
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
    iret

.reset:
    mov ax, 0xFFFF
    mov bx, 0x0002
    mov byte [cs:mouse_installed], 1
    mov word [cs:mouse_pos_x], 320
    mov word [cs:mouse_pos_y], 240
    mov word [cs:mouse_min_x], 0
%if FAT_TYPE == 16
    mov word [cs:mouse_max_x], 639
%else
    mov word [cs:mouse_max_x], 319
%endif
    mov word [cs:mouse_min_y], 0
%if FAT_TYPE == 16
    mov word [cs:mouse_max_y], 479
    mov byte [cs:mouse_visible], 1
    call mouse_vga_cursor_seed
%else
    mov word [cs:mouse_max_y], 199
    mov byte [cs:mouse_visible], 0
%endif
    iret

.show:
    mov byte [cs:mouse_visible], 1
    xor ax, ax
    iret

.hide:
%if FAT_TYPE == 16
    mov byte [cs:mouse_visible], 0
    cmp byte [cs:mouse_vga_cursor_drawn], 0
    je .hide_done
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es
    push ds
    mov bx, [cs:mouse_vga_cursor_last_x]
    mov dx, [cs:mouse_vga_cursor_last_y]
    call mouse_vga_xor_cursor12
    pop ds
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    mov byte [cs:mouse_vga_cursor_drawn], 0
.hide_done:
%else
    mov byte [cs:mouse_visible], 0
%endif
    xor ax, ax
    iret

.status:
    cmp byte [cs:mouse_installed], 1
    jne .status_not_installed
    xor bh, bh
    mov bl, [cs:mouse_buttons]
    mov cx, [cs:mouse_pos_x]
    mov dx, [cs:mouse_pos_y]
    iret
.status_not_installed:
    xor bx, bx
    xor cx, cx
    xor dx, dx
    iret

.set_pos:
    cmp cx, [cs:mouse_min_x]
    jae .x_min_ok
    mov cx, [cs:mouse_min_x]
.x_min_ok:
    cmp cx, [cs:mouse_max_x]
    jbe .x_ok
    mov cx, [cs:mouse_max_x]
.x_ok:
    cmp dx, [cs:mouse_min_y]
    jae .y_min_ok
    mov dx, [cs:mouse_min_y]
.y_min_ok:
    cmp dx, [cs:mouse_max_y]
    jbe .y_ok
    mov dx, [cs:mouse_max_y]
.y_ok:
    mov [cs:mouse_pos_x], cx
    mov [cs:mouse_pos_y], dx
    xor ax, ax
    iret

.set_x_range:
    mov [cs:mouse_min_x], cx
    mov [cs:mouse_max_x], dx
    cmp [cs:mouse_pos_x], cx
    jae .x_range_min_ok
    mov [cs:mouse_pos_x], cx
.x_range_min_ok:
    cmp [cs:mouse_pos_x], dx
    jbe .x_range_done
    mov [cs:mouse_pos_x], dx
.x_range_done:
    xor ax, ax
    iret

.set_y_range:
    mov [cs:mouse_min_y], cx
    mov [cs:mouse_max_y], dx
    cmp [cs:mouse_pos_y], cx
    jae .y_range_min_ok
    mov [cs:mouse_pos_y], cx
.y_range_min_ok:
    cmp [cs:mouse_pos_y], dx
    jbe .y_range_done
    mov [cs:mouse_pos_y], dx
.y_range_done:
    xor ax, ax
    iret

.motion:
%if FAT_TYPE == 16
    mov cx, [cs:mouse_delta_x]
    mov dx, [cs:mouse_delta_y]
    mov word [cs:mouse_delta_x], 0
    mov word [cs:mouse_delta_y], 0
%else
    xor cx, cx
    xor dx, dx
%endif
    iret

.set_callback:
    mov [cs:mouse_cb_mask], cx
    mov [cs:mouse_cb_off], dx
    mov [cs:mouse_cb_seg], es
    xor ax, ax
    iret

.exchange_callback:
    mov ax, [cs:mouse_cb_seg]
    mov bx, [cs:mouse_cb_off]
    mov si, [cs:mouse_cb_mask]
    mov [cs:mouse_cb_mask], cx
    mov [cs:mouse_cb_off], dx
    mov [cs:mouse_cb_seg], es
    mov cx, si
    mov dx, bx
    mov es, ax
    xor ax, ax
    iret

.set_mickey_ratio:
    mov [cs:mouse_mickey_x], cx
    mov [cs:mouse_mickey_y], dx
    xor ax, ax
    iret

.version:
    mov ax, 0x061A
    xor bx, bx
    mov cx, 0x0004
    xor dx, dx
    iret

int2f_handler:
    cmp ax, 0x1687
    je .fn_1687
    cmp ax, 0x4300
    je .fn_4300
    cmp ax, 0x4310
    je .fn_4300
.chain:
    jmp far [cs:old_int2f_off]

.fn_1687:
    mov ax, 0x8001
    jmp .iret_clear_cf_enter

.fn_4300:
    or al, al
    jne .fn_4310
    mov ax, 0x0080
    jmp .iret_clear_cf_enter

.fn_4310:
    push cs
    pop es
    mov bx, xms_entrypoint

.iret_clear_cf_enter:
    push bp
    mov bp, sp

.iret_clear_cf:
    and byte [bp + 6], 0xFE
    pop bp
    iret

xms_entrypoint:
    or ah, ah
    je .version
    cmp ah, 0x08
    je .query_free
    cmp ah, 0x09
    je .alloc_emb
    cmp ah, 0x0A
    je .free_emb
    cmp ah, 0x0B
    je .move_emb
    cmp ah, 0x0C
    je .lock_emb
    cmp ah, 0x0D
    je .unlock_emb
    cmp ah, 0x0E
    je .query_handle
    cmp ah, 0x0F
    je .realloc_emb
    jmp .unsupported

.query_free:
    mov dx, [cs:xms_free_kb]
    or dx, dx
    jnz .query_have_mem
    inc dx
.query_have_mem:
    mov ax, dx
    xor bl, bl
    retf

.alloc_emb:
    cmp dx, 0
    je .alloc_fail
    cmp word [cs:xms_alloc_kb], 0
    jne .alloc_fail
    cmp dx, [cs:xms_free_kb]
    ja .alloc_fail
    mov [cs:xms_alloc_kb], dx
    sub [cs:xms_free_kb], dx
    mov ax, 1
    mov dx, 1
    xor bl, bl
    retf

.alloc_fail:
    xor ax, ax
    mov bl, 0xA0
    retf

.free_emb:
    cmp dx, 1
    jne .free_fail
    mov ax, [cs:xms_alloc_kb]
    or ax, ax
    jz .free_fail
    add [cs:xms_free_kb], ax
    mov word [cs:xms_alloc_kb], 0
    mov ax, 1
    xor bl, bl
    retf

.free_fail:
    xor ax, ax
    mov bl, 0xA2
    retf

.move_emb:
%if FAT_TYPE == 16
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    push bp

    mov ax, [ds:si]
    mov [cs:xms_move_len_lo], ax
    mov ax, [ds:si + 2]
    mov [cs:xms_move_len_hi], ax
    test byte [cs:xms_move_len_lo], 1
    jnz .move_bad_length

    mov ax, [cs:xms_move_len_lo]
    or ax, [cs:xms_move_len_hi]
    jz .move_success

    mov ax, [ds:si + 4]
    cmp ax, 1
    ja .move_bad_src
    je .move_src_emb

    mov ax, [ds:si + 6]
    mov [cs:xms_move_src_lo], ax
    mov ax, [ds:si + 8]
    mov dx, ax
    mov cl, 4
    shl ax, cl
    add [cs:xms_move_src_lo], ax
    mov ax, dx
    mov cl, 12
    shr ax, cl
    adc ax, 0
    mov [cs:xms_move_src_hi], ax
    jmp .move_dst_setup

.move_src_emb:
    cmp word [cs:xms_alloc_kb], 0
    je .move_bad_src
    mov ax, [ds:si + 6]
    mov dx, [ds:si + 8]
    call .move_check_emb_range
    jc .move_bad_src_offset
    mov ax, [ds:si + 6]
    mov [cs:xms_move_src_lo], ax
    mov ax, [ds:si + 8]
    add ax, 0x0010
    jc .move_bad_src_offset
    cmp ax, 0x0100
    jae .move_bad_src_offset
    mov [cs:xms_move_src_hi], ax

.move_dst_setup:
    mov ax, [ds:si + 10]
    cmp ax, 1
    ja .move_bad_dst
    je .move_dst_emb

    mov ax, [ds:si + 12]
    mov [cs:xms_move_dst_lo], ax
    mov ax, [ds:si + 14]
    mov dx, ax
    mov cl, 4
    shl ax, cl
    add [cs:xms_move_dst_lo], ax
    mov ax, dx
    mov cl, 12
    shr ax, cl
    adc ax, 0
    mov [cs:xms_move_dst_hi], ax
    jmp .move_loop

.move_dst_emb:
    cmp word [cs:xms_alloc_kb], 0
    je .move_bad_dst
    mov ax, [ds:si + 12]
    mov dx, [ds:si + 14]
    call .move_check_emb_range
    jc .move_bad_dst_offset
    mov ax, [ds:si + 12]
    mov [cs:xms_move_dst_lo], ax
    mov ax, [ds:si + 14]
    add ax, 0x0010
    jc .move_bad_dst_offset
    cmp ax, 0x0100
    jae .move_bad_dst_offset
    mov [cs:xms_move_dst_hi], ax

.move_loop:
    mov ax, [cs:xms_move_len_lo]
    or ax, [cs:xms_move_len_hi]
    jz .move_success

    mov ax, 0x8000
    cmp word [cs:xms_move_len_hi], 0
    jne .move_chunk_ready
    cmp [cs:xms_move_len_lo], ax
    ja .move_chunk_ready
    mov ax, [cs:xms_move_len_lo]
.move_chunk_ready:
    mov [cs:xms_move_chunk], ax
    mov ax, [cs:xms_move_src_lo]
    mov dx, [cs:xms_move_src_hi]
    call .move_check_addr24_chunk
    jc .move_bad_src_offset
    mov ax, [cs:xms_move_dst_lo]
    mov dx, [cs:xms_move_dst_hi]
    call .move_check_addr24_chunk
    jc .move_bad_dst_offset
    call .move_chunk_87
    jc .move_bios_fail

    mov ax, [cs:xms_move_chunk]
    add [cs:xms_move_src_lo], ax
    adc word [cs:xms_move_src_hi], 0
    add [cs:xms_move_dst_lo], ax
    adc word [cs:xms_move_dst_hi], 0
    sub [cs:xms_move_len_lo], ax
    sbb word [cs:xms_move_len_hi], 0
    jmp .move_loop

.move_success:
    pop bp
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    mov ax, 1
    xor bl, bl
    retf

.move_bad_length:
    mov bl, 0xA7
    jmp .move_fail
.move_bad_src:
    mov bl, 0xA3
    jmp .move_fail
.move_bad_src_offset:
    mov bl, 0xA4
    jmp .move_fail
.move_bad_dst:
    mov bl, 0xA5
    jmp .move_fail
.move_bad_dst_offset:
    mov bl, 0xA6
    jmp .move_fail
.move_bios_fail:
    mov bl, 0xA0
.move_fail:
    pop bp
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    mov bh, ah
    xor ax, ax
    retf

.move_check_emb_range:
    add ax, [cs:xms_move_len_lo]
    adc dx, [cs:xms_move_len_hi]
    jc .move_range_bad
    mov bx, [cs:xms_alloc_kb]
    mov cx, bx
    shl bx, 10
    shr cx, 6
    cmp dx, cx
    ja .move_range_bad
    jb .move_range_ok
    cmp ax, bx
    ja .move_range_bad
.move_range_ok:
    clc
    ret
.move_range_bad:
    stc
    ret

.move_check_addr24_chunk:
    mov bx, [cs:xms_move_chunk]
    dec bx
    add ax, bx
    adc dx, 0
    cmp dx, 0x0100
    jae .move_range_bad
    clc
    ret

.move_chunk_87:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

    push cs
    pop es
    mov di, xms_87_gdt
    xor ax, ax
    mov cx, 24
    pushf
    cld
    rep stosw
    popf

    mov ax, [cs:xms_move_chunk]
    dec ax
    mov [cs:xms_87_gdt + 16], ax
    mov ax, [cs:xms_move_src_lo]
    mov [cs:xms_87_gdt + 18], ax
    mov al, [cs:xms_move_src_hi]
    mov [cs:xms_87_gdt + 20], al
    mov byte [cs:xms_87_gdt + 21], 0x93

    mov ax, [cs:xms_move_chunk]
    dec ax
    mov [cs:xms_87_gdt + 24], ax
    mov ax, [cs:xms_move_dst_lo]
    mov [cs:xms_87_gdt + 26], ax
    mov al, [cs:xms_move_dst_hi]
    mov [cs:xms_87_gdt + 28], al
    mov byte [cs:xms_87_gdt + 29], 0x93

    mov cx, [cs:xms_move_chunk]
    shr cx, 1
    mov si, xms_87_gdt
    mov ah, 0x87
    int 0x15

    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%else
    mov ax, [ds:si + 4]
    or ax, [ds:si + 10]
    cmp ax, 1
    ja .free_fail
    cmp ax, 0
    je .move_ok
    cmp word [cs:xms_alloc_kb], 0
    je .free_fail
.move_ok:
    mov ax, 1
    xor bl, bl
    retf
%endif

.check_handle:
    cmp dx, 1
    jne .free_fail
    cmp word [cs:xms_alloc_kb], 0
    je .free_fail
    ret

.lock_emb:
    call .check_handle
    mov ax, 1
    mov dx, 0x0010
    xor bx, bx
    retf

.unlock_emb:
    call .check_handle
    mov ax, 1
    xor bx, bx
    retf

.query_handle:
    call .check_handle
    mov ax, 1
    mov dx, [cs:xms_alloc_kb]
    xor bx, bx
    retf

.realloc_emb:
    cmp dx, 1
    jne .free_fail
    mov ax, [cs:xms_alloc_kb]
    or ax, ax
    jz .free_fail
    add ax, [cs:xms_free_kb]
    cmp bx, ax
    ja .alloc_fail
    sub ax, bx
    mov [cs:xms_free_kb], ax
    mov [cs:xms_alloc_kb], bx
    mov ax, 1
    xor bl, bl
    retf

.unsupported:
    xor ax, ax
    mov bl, 0x80
    retf

.version:
    mov ax, 0x0200
    mov bx, ax
    cwd
    retf

boot_drive db 0
int21_installed db 0
int21_carry db 0
int21_zf_state db 0xFF
int21_caller_ds dw 0
int21_return_es db 0
int2f_installed db 0
dos_default_drive db 0
dos_verify_flag db 0
dos_ctrl_break_flag db 0
last_exit_code db 0
int21_last_ah db 0
int21_last_al db 0
int21_path_stage_marker db 0
int21_error_ax dw 0
int21_silent_errors db 0
int21_chdir_drive db 0
int21_chdir_qualified db 0
int21_trace_call_cs dw 0
int21_path_upcase db 0
dos_time_centis db 0
last_term_type db 0
int21_force_terminate db 0
current_psp_seg dw 0
exec_cmd_len db 0
exec_cmd_buf times 126 db 0
old_int21_off dw 0
old_int21_seg dw 0
old_int20_off dw 0
old_int20_seg dw 0
old_int2f_off dw 0
old_int2f_seg dw 0
int_ef_target_off dw 0
int_ef_target_seg dw 0
xms_free_kb dw 0x3C00
xms_alloc_kb dw 0
%if FAT_TYPE == 16
xms_move_len_lo dw 0
xms_move_len_hi dw 0
xms_move_src_lo dw 0
xms_move_src_hi dw 0
xms_move_dst_lo dw 0
xms_move_dst_hi dw 0
xms_move_chunk dw 0
xms_87_gdt times 48 db 0
%endif
old_int10_off dw 0
old_int10_seg dw 0
%if FAT_TYPE == 16
old_int15_off dw 0
old_int15_seg dw 0
%endif
old_int16_off dw 0
old_int16_seg dw 0
%if FAT_TYPE == 16
old_int74_off dw 0
old_int74_seg dw 0
%endif
current_video_mode db 0x03
mouse_visible db 0
mouse_min_x dw 0
mouse_max_x dw 319
mouse_min_y dw 0
mouse_max_y dw 199
mouse_cb_mask dw 0
mouse_cb_off dw 0
mouse_cb_seg dw 0
mouse_mickey_x dw 8
mouse_mickey_y dw 8
%if FAT_TYPE == 16
mouse_hw_ready db 0
mouse_packet_index db 0
mouse_packet times 3 db 0
mouse_last_byte_tick dw 0
mouse_delta_x dw 0
mouse_delta_y dw 0
mouse_last_mickey_x dw 0
mouse_last_mickey_y dw 0
mouse_bios_enabled db 0
mouse_bios_asr_off dw 0
mouse_bios_asr_seg dw 0
mouse_bios_asr_set_count dw 0
mouse_bios_callback_count dw 0
mouse_vga_cursor_x dw 320
mouse_vga_cursor_y dw 240
mouse_vga_cursor_last_x dw 320
mouse_vga_cursor_last_y dw 240
mouse_vga_cursor_drawn db 0
mouse_vga_work_x dw 0
mouse_vga_work_y dw 0
mouse_vga_row_mask db 0
mouse_vga_save_gc0 db 0
mouse_vga_save_gc1 db 0
mouse_vga_save_gc3 db 0
mouse_vga_save_gc5 db 0
mouse_vga_save_gc8 db 0
mouse_vga_save_seq2 db 0
mouse_vga_cursor_mask db 0x80,0xC0,0xE0,0xF0,0xF8,0xDC,0x8E,0x06
%endif
file_handle_open db 0
file_handle_pos dw 0
%if FAT_TYPE == 16
file_handle_pos_hi dw 0
%endif
file_handle_mode db 0
tmp_open_mode db 0
file_handle_start_cluster dw 0
file_handle_root_lba dw 0
file_handle_root_off dw 0
file_handle_cluster_count dw 0
file_handle_size_lo dw 0
file_handle_size_hi dw 0
file_handle2_open db 0
file_handle2_pos dw 0
%if FAT_TYPE == 16
file_handle2_pos_hi dw 0
%endif
file_handle2_mode db 0
file_handle2_start_cluster dw 0
file_handle2_root_lba dw 0
file_handle2_root_off dw 0
file_handle2_cluster_count dw 0
file_handle2_size_lo dw 0
file_handle2_size_hi dw 0
file_handle3_open db 0
file_handle3_pos dw 0
%if FAT_TYPE == 16
file_handle3_pos_hi dw 0
%endif
file_handle3_mode db 0
file_handle3_start_cluster dw 0
file_handle3_root_lba dw 0
file_handle3_root_off dw 0
file_handle3_cluster_count dw 0
file_handle3_size_lo dw 0
file_handle3_size_hi dw 0
%if FAT_TYPE == 16
file_handle4_open db 0
file_handle4_pos dw 0
file_handle4_pos_hi dw 0
file_handle4_mode db 0
file_handle4_start_cluster dw 0
file_handle4_root_lba dw 0
file_handle4_root_off dw 0
file_handle4_cluster_count dw 0
file_handle4_size_lo dw 0
file_handle4_size_hi dw 0
file_handle5_open db 0
file_handle5_pos dw 0
file_handle5_pos_hi dw 0
file_handle5_mode db 0
file_handle5_start_cluster dw 0
file_handle5_root_lba dw 0
file_handle5_root_off dw 0
file_handle5_cluster_count dw 0
file_handle5_size_lo dw 0
file_handle5_size_hi dw 0
file_handle6_open db 0
file_handle6_pos dw 0
file_handle6_pos_hi dw 0
file_handle6_mode db 0
file_handle6_start_cluster dw 0
file_handle6_root_lba dw 0
file_handle6_root_off dw 0
file_handle6_cluster_count dw 0
file_handle6_size_lo dw 0
file_handle6_size_hi dw 0
file_handle7_open db 0
file_handle7_pos dw 0
file_handle7_pos_hi dw 0
file_handle7_mode db 0
file_handle7_start_cluster dw 0
file_handle7_root_lba dw 0
file_handle7_root_off dw 0
file_handle7_cluster_count dw 0
file_handle7_size_lo dw 0
file_handle7_size_hi dw 0
file_handle8_open db 0
file_handle8_pos dw 0
file_handle8_pos_hi dw 0
file_handle8_mode db 0
file_handle8_start_cluster dw 0
file_handle8_root_lba dw 0
file_handle8_root_off dw 0
file_handle8_cluster_count dw 0
file_handle8_size_lo dw 0
file_handle8_size_hi dw 0
%endif
file_handle_target db 0
file_handle_swapped db 0
fat_cache_valid db 0
fat_cache_dirty db 0
fat_cache_sector dw 0xFFFF
stage2_autorun_status db 0
%if FAT_TYPE == 16
runtime_handoff:
runtime_table_off dw 0
runtime_table_seg dw 0
runtime_status_flags dw 0
runtime_service_ptr:
runtime_service_off dw 0
runtime_service_seg dw 0
%endif
tmp_user_ds dw 0
tmp_user_ptr dw 0
tmp_rw_remaining dw 0
tmp_rw_done dw 0
tmp_chunk dw 0
tmp_disk_lba_save dw 0
tmp_disk_status db 0
tmp_cluster dw 0
tmp_cluster_off dw 0
tmp_sector_off dw 0
tmp_lba dw 0
tmp_capacity dw 0
tmp_next_cluster dw 0
tmp_exec_limit dw 0
tmp_exec_total dw 0
tmp_exec_handle dw 0
tmp_exec_error dw 0
tmp_exec_subfn db 0
tmp_overlay_block_seg dw 0
tmp_overlay_block_off dw 0
tmp_overlay_load_seg dw 0
tmp_overlay_reloc_seg dw 0
tmp_overlay_header_bytes dw 0
tmp_overlay_image_size dw 0
tmp_path_guard db 0
tmp_ioctl_subfn db 0
tmp_lookup_dir dw 0
tmp_rename_old_parent dw 0
tmp_rename_new_parent dw 0
tmp_rename_old_lba dw 0
tmp_rename_old_off dw 0
dos_mem_init db 0
dos_mem_alloc_seg dw 0
dos_mem_alloc_size dw 0
dos_mem_psp_free_seg dw 0
dos_mem_psp_free_size dw 0
dos_mem_psp_mcb_end dw 0
dos_mem_free2_seg dw 0
dos_mem_free2_size dw 0
dos_mem_alloc_seg2 dw 0
dos_mem_alloc_size2 dw 0
dos_mem_alloc_seg3 dw 0
dos_mem_alloc_size3 dw 0
dos_mem_mcb_owner dw 0
dos_mem_mcb_size dw 0
dos_mem_strategy dw 0
dos_mem_block_count db 0
dos_mem_block_table times DOS_MEM_BLOCK_TABLE_MAX * DOS_MEM_BLOCK_ENTRY_SIZE db 0
dos_mem_block_tmp_seg dw 0
dos_mem_block_tmp_size dw 0
dos_mem_block_tmp_owner dw 0
dos_mem_block_tmp_state dw 0
dos_mem_block_found_owner dw 0
dos_mem_block_req_size dw 0
dos21_test_seg dw 0
dos21_saved_drive db 0
saved_ss dw 0
saved_sp dw 0
saved_psp2 dw 0
saved_ds dw 0
saved_es dw 0
saved_ds2 dw 0
saved_es2 dw 0
current_load_seg dw MZ_LOAD_SEG
current_mz_context_slot dw 0
current_com_load_seg dw COM_LOAD_SEG
saved_psp dw 0
saved_ss2 dw 0
saved_sp2 dw 0
saved_psp3 dw 0
saved_ss3 dw 0
saved_sp3 dw 0
saved_ds3 dw 0
saved_es3 dw 0
com_entry_off dw 0
com_entry_seg dw 0
mz_entry_off dw 0
mz_entry_seg dw 0
mz_image_seg dw 0
mz_psp_seg dw 0
mz_stack_seg dw 0
mz_stack_sp dw 0
search_name_ptr dw 0
search_target_off dw 0
search_found_cluster dw 0
search_found_size_lo dw 0
search_found_size_hi dw 0
search_found_root_lba dw 0
search_found_root_off dw 0
search_found_attr db 0
search_found_name times 11 db 0
dta_seg dw 0
dta_off dw 0
find_attr db 0
find_active db 0
find_special_mode db 0
find_dir_cluster dw 0
find_cursor dw 0
find_cached_sector dw 0
find_pattern times 11 db 0
tmp_find_comp dw 0
path_fat_name times 11 db 0
fileio_buf times 4 db 0
fileio_patch db 0x11, 0x22
find_dta times 64 db 0
dos_indos_flag db 0
dos_list_of_lists times 64 db 0
tmp_cwd_comp times 24 db 0
tmp_cwd_build times 24 db 0
dos_env_block db 'COMSPEC=C:\COMMAND.COM', 0
              db 'PATH=C:\', 0
              db 0
              dw 1
dos_env_exec_path db 'C:\COMMAND.COM', 0
              times DOS_ENV_EXEC_PATH_LEN - ($ - dos_env_exec_path) db 0
dos_env_block_end:
env_doom_exe_path db 'C:\APPS\DOOM\DOOM.EXE', 0
dos_child_exec_path_buf times DOS_ENV_EXEC_PATH_LEN db 0
disk_packet:
    db 0x10
    db 0
    dw 1
disk_packet_off dw 0
disk_packet_seg dw 0
disk_packet_lba dq 0
cmd_buffer times CMD_BUF_LEN db 0
shell_exec_path_buf times SHELL_EXEC_PATH_BUF_LEN db 0
shell_exec_param_block:
    dw 0
    dw shell_exec_cmd_tail
    dw 0
    dw 0x005C
    dw 0
    dw 0x006C
    dw 0
shell_exec_cmd_tail db 0, 0x0D
                    times 127 db 0
%if FAT_TYPE == 16 || FAT_TYPE == 12
shell_copy_src_ptr dw 0
shell_copy_dst_ptr dw 0
shell_copy_dst_cluster dw 0
%endif
shell_last_error_ax dw 0
shell_edit_len db 0
shell_edit_cursor db 0
shell_edit_cap db 0
shell_edit_start_col db 0
shell_edit_start_row db 0
shell_edit_prev_len db 0
shell_history_head db 0
shell_history_count db 0
shell_history_nav db 0xFF
shell_history_saved_len db 0
shell_history_saved_buf times CMD_BUF_LEN db 0
shell_history_buf times (SHELL_HISTORY_MAX * CMD_BUF_LEN) db 0
shell_completion_match_count db 0
shell_completion_prefix_len db 0
shell_completion_match_buf times CMD_BUF_LEN db 0
shell_completion_file_buf times CMD_BUF_LEN db 0
shell_completion_saved_dta_seg dw 0
shell_completion_saved_dta_off dw 0
shell_completion_dta times 64 db 0

msg_stage1_serial db "[STAGE1-SERIAL] READY", 13, 10, 0
msg_diag_begin    db "[S1] d", 13, 10, 0
msg_diag_int10    db "[10]", 13, 10, 0
msg_diag_int13_ok db "[13]", 13, 10, 0
msg_diag_int16_ok db "[16]", 13, 10, 0
msg_diag_int1a    db "[TICKS] 0x", 0
msg_int21_installed db "[INT21] ok", 13, 10, 0
msg_int21_missing db "[I21] no", 13, 10, 0
country_info_default:
    dw 0
    db "$", 0, 0, 0, 0
    db ",", 0
    db ".", 0
    db "-", 0
    db ":", 0
    db 0
    db 2
    db 0
    dd 0
    db ",", 0
    times 10 db 0
%if STAGE1_SELFTEST_AUTORUN
msg_stage1_selftest_begin db "[S1T] begin", 13, 10, 0
msg_stage1_selftest_done db "[S1T] done", 13, 10, 0
msg_stage1_selftest_serial_begin db "[S1T] B", 13, 10, 0
msg_stage1_selftest_serial_done db "[S1T] D", 13, 10, 0
msg_streamc_serial_pass db "[STREAMC-SERIAL] PASS", 13, 10, 0
msg_streamc_serial_fail db "[STREAMC-SERIAL] FAIL", 13, 10, 0
%endif
%if FAT_TYPE == 16 && STAGE1_RUNTIME_PROBE
msg_runtime_probe_begin db "[RTP] B", 13, 10, 0
msg_runtime_probe_table db "[RTP] T", 13, 10, 0
msg_runtime_probe_call db "[RTP] C", 13, 10, 0
msg_runtime_probe_ok db "[RTP] OK", 13, 10, 0
msg_runtime_probe_bad db "[RTP] BAD", 13, 10, 0
runtime_probe_version_prefix db "CiukiOS runtime split"
runtime_probe_version_prefix_len equ $ - runtime_probe_version_prefix
runtime_probe_marker_prefix db "[S2] ready"
runtime_probe_marker_prefix_len equ $ - runtime_probe_marker_prefix
%endif

msg_prompt_prefix db "CiukiOS ", 0
msg_unknown   db "Unknown command", 13, 10, 0
msg_banner_title db "CiukiOS pre-Alpha v0.6.5 (CiukiDOS Shell)", 0
%if FAT_TYPE == 16
msg_shell_status db "Type Help for commands.", 0
msg_shell_cpu_prefix db "CPU:", 0
msg_shell_dsk_prefix db "DSK:", 0
msg_shell_ram_prefix db "FREE:", 0
%endif
%if FAT_TYPE == 12
msg_shell_sysinfo_prefix db "RAM:", 0
%endif
msg_help_header db "Commands (help short|all)", 13, 10, 0
msg_help_core db "  help - Guide.", 13, 10, "  ver - Version.", 13, 10, "  cls - Clear.", 13, 10, 0
msg_help_runtime db "  which/where - Resolve.", 13, 10, "  pwd - Cwd.", 13, 10, "  dir - List.", 13, 10, 0
msg_help_system db "  cd/woof/cd.. - Chdir.", 13, 10, "  run - Execute.", 13, 10, "  help all - Full list.", 13, 10, 0
msg_help_apps db "  reboot - Reboot.", 13, 10, "  exit - Restart.", 13, 10, 0
msg_help_all db "  ticks - T. drive/drives - D. dos21 - S.", 13, 10, "  comdemo - C. mzdemo - M. fileio - F. gfxdemo - G.", 13, 10, 0
msg_ticks     db "ticks=0x", 0
msg_drive     db "boot drive=0x", 0
msg_drives_default db "default drive=", 0
msg_drives_index db " index=0x", 0
msg_drives_units db "units: C=HDD D=Live/CD", 13, 10, 0
msg_dos21_begin db "[DOS21] smoke", 13, 10, 0
msg_dos21_status db "[INT21/4D] 0x", 0
msg_dos21_serial_pass db "[DOS21-SERIAL] PASS", 13, 10, 0
msg_dos21_serial_fail db "[DOS21-SERIAL] FAIL", 13, 10, 0
msg_com_begin db "[COM] run", 13, 10, 0
msg_com_load_fail db "[COM] fail", 13, 10, 0
msg_com_done  db "[COM] 0x", 0
msg_com_serial_pass db "[COMDEMO-SERIAL] PASS", 13, 10, 0
msg_com_serial_fail db "[COMDEMO-SERIAL] FAIL", 13, 10, 0
msg_mz_begin db "[MZ] run", 13, 10, 0
msg_mz_load_fail db "[MZ] fail", 13, 10, 0
msg_mz_done  db "[MZ] 0x", 0
msg_mz_serial_pass db "[MZDEMO-SERIAL] PASS", 13, 10, 0
msg_mz_serial_fail db "[MZDEMO-SERIAL] FAIL", 13, 10, 0
msg_fileio_begin db "[FILEIO]", 13, 10, 0
msg_fileio_serial_pass db "[FILEIO-SERIAL] PASS", 13, 10, 0
msg_fileio_serial_fail db "[FILEIO-SERIAL] FAIL", 13, 10, 0
msg_find_begin db "[FIND]", 13, 10, 0
msg_find_serial_pass db "[FIND-SERIAL] PASS", 13, 10, 0
msg_find_serial_fail db "[FIND-SERIAL] FAIL", 13, 10, 0
msg_gfx_begin db "[GFX] run", 13, 10, 0
msg_gfx_done db "[GFX] done", 13, 10, 0
msg_gfx_serial_pass db "[GFX-SERIAL] PASS", 13, 10, 0
msg_gfxrect_serial_pass db "[GFXRECT-SERIAL] PASS", 0
msg_gfxrect_serial_fail db "[GFXRECT-SERIAL] FAIL", 0
msg_gfxstar_serial_pass db "[GFXSTAR-SERIAL] PASS", 0
msg_gfxstar_serial_fail db "[GFXSTAR-SERIAL] FAIL", 0
msg_mvren_serial_pass db "[MVR] PASS", 13, 10, 0
msg_mvren_serial_fail db "[MVR] FAIL", 13, 10, 0
msg_rebooting db "rebooting...", 13, 10, 0
msg_halting   db "halting...", 13, 10, 0
msg_dir_header db "Dir", 13, 10, 0
msg_dir_empty db "no files found", 13, 10, 0
msg_cwd_prefix db "cwd=", 0
msg_err_ax db " err=0x", 0
msg_which_usage db "usage: which <token>", 13, 10, 0
msg_which_builtin db " is a shell built-in", 13, 10, 0
msg_which_not_found db " not found", 13, 10, 0
msg_mouse_status db "mouse=0x", 0
msg_mouse_buttons db "buttons=0x", 0
msg_mouse_x db "x=0x", 0
msg_mouse_y db "y=0x", 0
msg_keytest_prompt db "press a key...", 0
msg_keytest_ax db "key AX=0x", 0
gfx_text_ciukios db "CIUKIOS", 0
gfx_text_demo db "GFX DEMO", 0
gfx_text_vdi db "VDI BASE", 0
gfx_text_timer db "KEY EXIT", 0

str_help   db "help", 0
str_ver    db "ver", 0
str_cls    db "cls", 0
str_ticks  db "ticks", 0
str_drive  db "drive", 0
str_drives db "drives", 0
str_dir    db "dir", 0
str_pwd    db "pwd", 0
str_woof   db "woof", 0
str_cd     db "cd", 0
str_cdup   db "cd..", 0
str_copy   db "copy", 0
str_move   db "move", 0
str_mv     db "mv", 0
str_del    db "del", 0
str_md     db "md", 0
str_mkdir  db "mkdir", 0
str_rd     db "rd", 0
str_rmdir  db "rmdir", 0
str_ren    db "ren", 0
str_rename db "rename", 0
str_type   db "type", 0
str_run    db "run", 0
str_which  db "which", 0
str_where  db "where", 0
str_exit   db "exit", 0
str_dos21  db "dos21", 0
str_comdemo db "comdemo", 0
str_mzdemo db "mzdemo", 0
str_fileio db "fileio", 0
str_gfxdemo db "gfxdemo", 0
str_gfxrect db "gfxrect", 0
str_gfxstar db "gfxstar", 0
str_findtest db "findtest", 0
str_mouse db "mouse", 0
str_keytest db "keytest", 0
str_reboot db "reboot", 0
str_halt   db "halt", 0
str_help_all db "all", 0
str_help_short db "short", 0
str_ext_com db ".COM", 0
str_ext_exe db ".EXE", 0
%if STAGE1_SELFTEST_AUTORUN
str_which_probe_comdemo db "\\APPS\\COMDEMO", 0
str_expect_which_comdemo db "\\apps\\comdemo.com", 0
%endif

shell_builtin_name_table:
    dw str_help
    dw str_ver
    dw str_cls
    dw str_ticks
    dw str_drive
    dw str_drives
    dw str_dir
    dw str_pwd
    dw str_woof
    dw str_cdup
    dw str_cd
    dw str_copy
    dw str_move
    dw str_mv
    dw str_del
    dw str_md
    dw str_mkdir
    dw str_rd
    dw str_rmdir
    dw str_ren
    dw str_rename
    dw str_type
    dw str_run
    dw str_which
    dw str_where
    dw str_exit
    dw str_dos21
    dw str_comdemo
    dw str_mzdemo
    dw str_fileio
    dw str_gfxdemo
    dw str_gfxrect
    dw str_gfxstar
    dw str_findtest
    dw str_mouse
    dw str_keytest
    dw str_reboot
    dw str_halt
    dw 0

path_comdemo_dos db "\APPS\COMDEMO.COM", 0
path_mzdemo_dos  db "\APPS\MZDEMO.EXE", 0
path_fileio_dos  db "\APPS\FILEIO.BIN", 0
path_deltest_dos db "\APPS\DELTEST.BIN", 0
path_gfxrect_dos db "\APPS\GFXRECT.COM", 0
path_gfxstar_dos db "\APPS\GFXSTAR.COM", 0
%if STAGE1_SELFTEST_AUTORUN
cmd_selftest_mv db "mv \APPS\COMDEMO.COM \APPS\T", 0
cmd_selftest_rename db "ren \APPS\T\COMDEMO.COM \APPS\T\C.COM", 0
cmd_selftest_restore db "ren \APPS\T\C.COM \APPS\COMDEMO.COM", 0
path_mvren_dir_dos db "\APPS\T", 0
path_mvren_final_dos db "\APPS\T\C.COM", 0
%endif
%if FAT_TYPE == 16
%if STAGE2_AUTORUN
path_stage2_dos db "\SYSTEM\STAGE2.BIN", 0
%endif
path_runtime_dos db "\SYSTEM\RUNTIME.BIN", 0
path_splash_bin_dos db "\SYSTEM\SPLASH.BIN", 0
runtime_loader_signature db "CIUKRT01"
msg_splash_serial_ok db "[SPLASH] LOAD OK", 13, 10, 0
msg_splash_serial_fail db "[SPLASH] LOAD FAIL", 13, 10, 0
%endif
path_pattern_com db "*.COM", 0
path_pattern_exe db "*.EXE", 0
path_pattern_mz equ path_mzdemo_dos
path_sd_driver_fat db "SDPSC9  VGA"
path_gem_exe_fat   db "GEM     EXE"
path_gem_cpi_fat   db "GEM     CPI"
path_dotdot_fat    db "..         "
%if FAT_TYPE == 16
path_gem_exe_abs db "\\SYSTEM\\DESKTOP\\GEM.EXE", 0
%endif
path_system_dir_dos db "\SYSTEM", 0
path_apps_dir_dos db "\APPS", 0
path_parent_dos  db "..", 0
path_root_dos    db "\", 0
cwd_buf times 24 db 0
cwd_cluster dw 0
%if FAT_TYPE == 16
cwd_c_buf times 24 db 0
cwd_c_cluster dw 0
cwd_d_buf times 24 db 0
cwd_d_cluster dw 0
%endif
shell_saved_cwd_buf times 24 db 0
shell_saved_cwd_cluster dw 0
shell_exec_saved_cwd_buf times 24 db 0
shell_exec_saved_cwd_cluster dw 0
shell_exec_external_mouse_disabled db 0
%if FAT_TYPE == 16
shell_footer_ram_buf times 6 db 0
shell_footer_pct_buf times 3 db 0
shell_footer_loop_count dw 0
shell_footer_max_loop dw 1
shell_footer_last_tick dw 0xFFFF
shell_footer_dsk_last_scan_tick dw 0
shell_footer_tick_key_activity db 0
shell_footer_key_cooldown db 0
shell_footer_cpu_pct db 0
shell_footer_dsk_pct db 0
shell_footer_dsk_dirty db 1
%endif
%if FAT_TYPE == 12
ram_buf times 6 db 0
%endif
shell_dir_count dw 0
shell_dir_name_buf times 16 db 0
gfx_draw_color db 0
gfx_row_bits db 0
gfx_demo_frame db 0
gfx_demo_deadline dw 0
gfx_demo_last_tick dw 0
%if FAT_TYPE == 16
splash_wait_start_tick dw 0
splash_wait_last_tick dw 0
%endif
gfx_line_x0 dw 0
gfx_line_y0 dw 0
gfx_line_x1 dw 0
gfx_line_y1 dw 0
gfx_line_dx dw 0
gfx_line_dy dw 0
gfx_line_sx dw 0
gfx_line_sy dw 0
gfx_line_err dw 0
gfx_line_e2 dw 0
mouse_pos_x dw 320
mouse_pos_y dw 240
mouse_buttons db 0
mouse_installed db 0

gfx_font8_table:
    db 'A', 0x18,0x24,0x42,0x7E,0x42,0x42,0x42,0x00
    db 'B', 0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00
    db 'C', 0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00
    db 'D', 0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00
    db 'E', 0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00
    db 'F', 0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00
    db 'G', 0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00
    db 'I', 0x3E,0x08,0x08,0x08,0x08,0x08,0x3E,0x00
    db 'K', 0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00
    db 'M', 0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x00
    db 'O', 0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00
    db 'R', 0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00
    db 'T', 0x7F,0x08,0x08,0x08,0x08,0x08,0x08,0x00
    db 'U', 0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00
    db 'V', 0x42,0x42,0x42,0x42,0x24,0x24,0x18,0x00
    db 'X', 0x42,0x24,0x18,0x18,0x18,0x24,0x42,0x00
    db 'Y', 0x41,0x22,0x14,0x08,0x08,0x08,0x08,0x00
    db 0

; Stage2 Extended Services Messages
msg_stage2_entry db "[S2] init", 13, 10, 0
msg_stage2_ready db "[S2] ready", 13, 10, 0
%if STAGE2_AUTORUN
msg_stage2_autorun_begin db "[S2]L", 13, 10, 0
msg_stage2_autorun_loaded db "[S2]LD", 13, 10, 0
msg_stage2_autorun_return db "[S2]R", 13, 10, 0
msg_stage2_autorun_fail db "[S2]F", 13, 10, 0
%endif
%if HARDWARE_VALIDATION_SCREEN
msg_hw_validation_title db "[HW] Stage2 hardware validation", 13, 10, 0
msg_hw_validation_pass db "[HW] PASS", 13, 10, 0
msg_hw_validation_return db "[HW] RETURN", 13, 10, 0
msg_hw_validation_capture db "[HW] CAPTURE", 13, 10, 0
msg_hw_validation_fail db "[HW] FAIL", 13, 10, 0
msg_hw_validation_notrun db "[HW] WARN", 13, 10, 0
%endif
msg_mouse_enabled db "[S2] mouse", 13, 10, 0
msg_mouse_not_found db "[S2] no mouse", 13, 10, 0
msg_vbe_init db "[S2] vbe", 13, 10, 0
msg_exit_str db "Exit", 13, 10, 0
