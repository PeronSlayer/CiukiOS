bits 16
org 0x0000

%define CMD_BUF_LEN 64
%define COM_LOAD_SEG 0x2000
%define MZ_LOAD_SEG 0x3000
%define MZ2_LOAD_SEG 0x3800
%define STAGE2_LOAD_SEG 0x5000
%define DOS_META_BUF_SEG 0x7000
%define DOS_FAT_BUF_SEG  0x7200
%define DOS_IO_BUF_SEG   0x7400
%define DOS_ENV_SEG      0x7600
%define DOS_HEAP_BASE_SEG 0x8000
%define DOS_HEAP_LIMIT_SEG 0x9F00
%define DOS_HEAP_MAX_PARAS (DOS_HEAP_LIMIT_SEG - DOS_HEAP_BASE_SEG)
%define DOS_HEAP_USER_SEG (DOS_HEAP_BASE_SEG + 1)
%define DOS_HEAP_USER_MAX_PARAS (DOS_HEAP_MAX_PARAS - 1)
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
%if FAT_TYPE == 16
%define FAT_EOF 0xFFF8
%else
%define FAT_EOF 0xFF8
%endif
%ifndef STAGE1_SELFTEST_AUTORUN
%define STAGE1_SELFTEST_AUTORUN 0
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
    call show_boot_splash
    mov al, 16
    call splash_set_progress

    mov si, msg_stage1
    call print_string_serial
    mov si, msg_stage1_serial
    call print_string_serial

    mov al, 40
    call splash_set_progress
    call run_bios_diagnostics
    mov al, 68
    call splash_set_progress
    call install_int21_vector
    call init_stage2_services
%if STAGE1_SELFTEST_AUTORUN
    mov al, 84
    call splash_set_progress
    call run_stage1_selftest
%endif

    mov al, 100
    call splash_set_progress
    call splash_hold
    call draw_shell_chrome

main_loop:
    mov ax, cs
    mov ds, ax
    mov es, ax

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
    mov byte [int2f_installed], 0
    mov byte [last_exit_code], 0
    mov byte [last_term_type], 0
    mov byte [dos_default_drive], 0
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

    pop es
    pop bx
    pop ax

    mov si, msg_int21_installed
    call print_string_dual
    ret

int20_handler:
    push ax
    push bp
    mov byte [last_exit_code], 0
    mov byte [last_term_type], 0
    mov ax, [current_psp_seg]
    or ax, ax
    jz .done
    mov bp, sp
    mov word [ss:bp + 6], 0x0005
    mov [ss:bp + 8], ax

.done:
    pop bp
    pop ax
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

    mov byte [cs:int21_carry], 0
    mov byte [cs:int21_return_es], 0
    mov byte [cs:int21_last_ah], ah

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
    cmp ah, 0x0E
    je .fn_0e
    cmp ah, 0x1A
    je .fn_1a
    cmp ah, 0x19
    je .fn_19
    cmp ah, 0x2A
    je .fn_2a
    cmp ah, 0x2C
    je .fn_2c
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
    cmp ah, 0x3B
    je .fn_3b
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
    cmp ah, 0x51
    je .fn_51
    cmp ah, 0x52
    je .fn_52
    cmp ah, 0x54
    je .fn_54
    cmp ah, 0x58
    je .fn_58
    cmp ah, 0x62
    je .fn_62
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
    jmp .success

.fn_06_no_key:
    xor al, al
    xor ah, ah
    jmp .success

.fn_07:
.fn_08:
    mov ah, 0x00
    int 0x16
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

.fn_2c:
    call int21_get_time
    jc .error
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

.fn_3b:
    call int21_chdir
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
    call int21_exec
    jc .error
    jmp .success

.fn_48:
    call int21_alloc
    jc .error
    jmp .success

.fn_49:
    call int21_free
    jc .error
    jmp .success

.fn_4a:
    call int21_resize
    jc .error
    jmp .success

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
    mov byte [cs:int21_force_terminate], 1
.fn_31_done:
    xor ax, ax
    jmp .success

.fn_52:
    call int21_get_list_of_lists
    mov byte [cs:int21_return_es], 1
    jc .error
    jmp .success

.fn_54:
    xor al, al
    xor ah, ah
    jmp .success

.fn_58:
    call int21_mem_strategy
    jc .error
    jmp .success

.fn_62:
    call int21_get_psp
    jc .error
    jmp .success

.unsupported:
    push ax
    mov si, msg_int21_unsup
    call print_string_serial
    pop ax
    mov al, ah
    call print_hex8_serial
    mov al, 13
    call serial_putc
    mov al, 10
    call serial_putc
    mov ax, 0x0001
    jmp .error

.success:
    mov byte [cs:int21_carry], 0
    jmp .done

.error:
    push ax
    mov si, msg_int21_err
    call print_string_serial
    mov al, [cs:int21_last_ah]
    call print_hex8_serial
    mov al, ':'
    call serial_putc
    pop ax
    call print_hex8_serial
    mov al, 13
    call serial_putc
    mov al, 10
    call serial_putc
    mov byte [cs:int21_carry], 1

.done:
    mov bp, sp
    cmp byte [cs:int21_force_terminate], 0
    je .term_done
    mov byte [cs:int21_force_terminate], 0
    mov ax, [cs:current_psp_seg]
    mov word [bp + 16], 0x0005
    mov [bp + 18], ax
.term_done:
    cmp byte [cs:int21_return_es], 0
    je .flags_only
    mov [bp + 0], es
.flags_only:
    cmp byte [cs:int21_carry], 0
    jne .set_carry
    and word [bp + 20], 0xFFFE
    jmp .restore
.set_carry:
    or word [bp + 20], 0x0001
.restore:
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
    cmp al, 0
    jne .fail

    xor dx, dx
    mov ah, 0x0E
    int 0x21
    jc .fail

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

    cmp al, 0x00
    jne .bad_function

    mov byte [cs:exec_cmd_len], 0
    cmp bx, 0
    je .no_param_block
    call int21_exec_capture_tail
.no_param_block:

    mov si, dx
    call int21_path_to_fat_name
    jc .path_fail

    mov al, [cs:path_fat_name + 8]
    cmp al, 'C'
    jne .check_exe
    mov al, [cs:path_fat_name + 9]
    cmp al, 'O'
    jne .check_exe
    mov al, [cs:path_fat_name + 10]
    cmp al, 'M'
    jne .check_exe

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

.exec_mz:
    cmp word [cs:current_psp_seg], 0
    jne .nested_exec_seg
    mov word [cs:current_load_seg], MZ_LOAD_SEG
    jmp .do_exec_mz
.nested_exec_seg:
    mov word [cs:current_load_seg], MZ2_LOAD_SEG
.do_exec_mz:
    call int21_exec_load_mz
    jc .done
    call int21_exec_run_mz
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
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_exec_capture_tail:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
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
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

int21_exec_write_tail:
    push ax
    push bx
    push cx
    push di
    push si
    push ds

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

    pop ds
    pop si
    pop di
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

    mov ax, cs
    mov ds, ax
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov si, path_fat_name
    mov bx, 0xFFFF
    call load_root_file_first_sector
    jc .open_fail

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
    cmp ax, DOS_META_BUF_SEG
    jae .copy_too_large
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

    mov ax, COM_LOAD_SEG
    mov es, ax

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
    mov word [es:0x002C], DOS_ENV_SEG
    call int21_build_env_block
    call int21_exec_write_tail

    mov di, 0x0100
    mov cx, 0xFE00
    call int21_exec_load_to_es
    jc .fail

    mov word [cs:com_entry_off], 0x0100
    mov word [cs:com_entry_seg], COM_LOAD_SEG
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
    ; save parent PSP before overwriting with new process
    mov ax, [cs:current_psp_seg]
    mov [cs:saved_psp], ax
    mov [cs:saved_ss], ss
    mov [cs:saved_sp], sp
    mov ax, ds
    mov [cs:saved_ds], ax
    mov [cs:saved_es], ax

    cli
    mov ax, COM_LOAD_SEG
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

    call far [cs:com_entry_off]

    cli
    mov ax, cs
    mov ds, ax
    mov ax, [cs:saved_ss]
    mov ss, ax
    mov sp, [cs:saved_sp]
    sti

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

    mov ax, [cs:current_load_seg]
    mov es, ax
    xor ax, ax
    xor di, di
    mov cx, 128
    rep stosw

    xor di, di
    xor cx, cx
    call int21_exec_load_to_es
    jc .fail
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop di
    pop cx
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
    mov ax, [cs:mz_psp_seg]
    mov es, ax
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
    mov word [es:0x0002], DOS_HEAP_LIMIT_SEG
    mov [es:0x0016], ax
    mov word [es:0x002C], DOS_ENV_SEG
    call int21_build_env_block
    call int21_exec_write_tail

    push ds
    mov ax, DOS_META_BUF_SEG
    mov ds, ax
    xor ax, ax
    mov [0x0000], ax
    pop ds

    mov [cs:saved_ss], ss
    mov [cs:saved_sp], sp
    mov ax, ds
    mov [cs:saved_ds], ax
    mov [cs:saved_es], ax

    cli
    mov ax, [cs:mz_psp_seg]
    mov [cs:current_psp_seg], ax
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

    call far [cs:mz_entry_off]

    cli
    mov ax, cs
    mov ds, ax
    ; restore SS:SP from appropriate slot
    cmp word [cs:current_load_seg], MZ2_LOAD_SEG
    jne .restore_primary_ss
    mov ax, [cs:saved_ss2]
    mov ss, ax
    mov sp, [cs:saved_sp2]
    jmp .done_ss_restore
.restore_primary_ss:
    mov ax, [cs:saved_ss]
    mov ss, ax
    mov sp, [cs:saved_sp]
.done_ss_restore:
    sti
    ; restore parent PSP instead of clearing to 0
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

int21_set_dta:
    mov ax, ds
    mov [cs:dta_seg], ax
    mov [cs:dta_off], dx
    xor ax, ax
    clc
    ret

int21_get_default_drive:
    xor ah, ah
    mov al, [cs:dos_default_drive]
    clc
    ret

int21_set_default_drive:
    cmp dl, 1
    ja .invalid
    mov [cs:dos_default_drive], dl
    mov al, 1
    xor ah, ah
    clc
    ret
.invalid:
    mov ax, 0x000F
    stc
    ret

int21_get_version:
    mov ax, 0x0005
    xor bx, bx
    xor cx, cx
    clc
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
    mov ch, 12
    xor cl, cl
    xor dh, dh
    xor dl, dl
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
    xor dl, dl
    xor ax, ax
    clc
    ret
.set_state:
    xor dl, dl
    xor ax, ax
    clc
    ret

int21_get_free_space:
    cmp dl, 0
    je .ok
    cmp dl, 1
    je .ok
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
    mov bx, dos_list_of_lists + 2
    mov ax, cs
    mov es, ax
    xor ax, ax
    clc
    ret

int21_mem_strategy:
    cmp al, 0x00
    je .get
    cmp al, 0x01
    je .set
    mov ax, 0x0001
    stc
    ret
.get:
    xor bx, bx
    xor ax, ax
    clc
    ret
.set:
    xor ax, ax
    clc
    ret

int21_set_vector:
    push ax
    push bx
    push es
    xor ah, ah
    mov bx, ax
    shl bx, 1
    shl bx, 1
    xor ax, ax
    mov es, ax
    mov [es:bx], dx
    mov ax, ds
    mov [es:bx + 2], ax
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
    xor dx, dx
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
    mov al, [si]
    cmp al, 0
    je .root

    ; skip drive prefix (X:)
    cmp byte [si + 1], ':'
    jne .scan_components
    add si, 2

.scan_components:
    ; absolute path starts from root in this minimal model
    cmp byte [si], '\'
    je .abs_skip
    cmp byte [si], '/'
    jne .comp_loop_entry
.abs_skip:
    mov byte [cs:cwd_buf], 0
    inc si

.comp_loop_entry:
    mov al, [si]
    cmp al, 0
    je .ok

    ; isolate current component
    xor bx, bx
.comp_copy:
    mov al, [si]
    cmp al, 0
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

    ; skip separator for next component (if present)
    mov al, [si]
    cmp al, '\'
    je .skip_sep_after_comp
    cmp al, '/'
    jne .handle_comp
.skip_sep_after_comp:
    inc si

.handle_comp:
    ; ignore empty component
    cmp byte [cs:tmp_cwd_comp], 0
    je .comp_loop_entry

    ; ignore "."
    cmp byte [cs:tmp_cwd_comp], '.'
    jne .check_parent
    cmp byte [cs:tmp_cwd_comp + 1], 0
    je .comp_loop_entry

.check_parent:
    ; handle ".." as root fallback in this simplified cwd model
    cmp byte [cs:tmp_cwd_comp], '.'
    jne .set_component
    cmp byte [cs:tmp_cwd_comp + 1], '.'
    jne .set_component
    cmp byte [cs:tmp_cwd_comp + 2], 0
    jne .set_component
    mov byte [cs:cwd_buf], 0
    jmp .comp_loop_entry

.set_component:
    ; keep last canonical component as cwd token
    xor bx, bx
.set_copy:
    mov al, [cs:tmp_cwd_comp + bx]
    mov [cs:cwd_buf + bx], al
    cmp al, 0
    je .comp_loop_entry
    inc bx
    cmp bx, 23
    jb .set_copy
    mov byte [cs:cwd_buf + 23], 0
    jmp .comp_loop_entry

.ok:
    xor ax, ax
    clc
    ret
.root:
    mov byte [cs:cwd_buf], 0
    xor ax, ax
    clc
    ret

int21_getcwd:
    xor bx, bx
.copy_loop:
    mov al, [cs:cwd_buf + bx]
    mov [ds:si], al
    inc si
    cmp al, 0
    je .done
    inc bx
    jmp .copy_loop
.done:
    xor ax, ax
    clc
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
    call int21_path_to_fat_pattern
    jc .path_fail

    call int21_find_try_gem_special
    jnc .done_ok

    mov word [cs:find_cursor], 0
    call int21_find_scan_from_cursor
    jc .scan_fail

    call int21_find_write_dta
    jc .io_fail

.done_ok:
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
    mov ax, 0x0012
    stc
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

int21_find_try_gem_special:
    push bx
    push cx
    push si
    push ds
    push es

    mov si, find_pattern
    cmp byte [cs:si], 'S'
    jne .miss
    cmp byte [cs:si + 1], 'D'
    jne .miss
    add si, 2
    mov cx, 9
.match_loop:
    cmp byte [cs:si], '?'
    jne .miss
    inc si
    loop .match_loop

    mov si, find_pattern
    cmp byte [cs:si], 'S'
    jne .check_pd
    cmp byte [cs:si + 1], 'D'
    jne .miss
    mov si, path_sd_driver_fat
    jmp .match_root

.check_pd:
    jmp .miss

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
    xor bx, bx
    call read_sector_lba
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
    mov byte [cs:find_active], 0
    mov ax, 0x0012
    stc
    jmp .done

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

    cmp byte [si], 0
    je .fail

    cmp byte [si + 1], ':'
    jne .find_last
    add si, 2

.find_last:
    mov [cs:tmp_find_comp], si
.walk:
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
    call int21_upcase_al
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
    call int21_upcase_al
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

int21_open:
    push bx
    push dx
    push si
    push ds
    push es

    cmp al, 2
    ja .access_denied

    mov [cs:file_handle_mode], al

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

    mov byte [file_handle_open], 1
    mov word [file_handle_pos], 0
    mov ax, [search_found_cluster]
    mov [file_handle_start_cluster], ax
    mov ax, [search_found_size_lo]
    mov [file_handle_size_lo], ax
    mov ax, [search_found_size_hi]
    mov [file_handle_size_hi], ax
    mov ax, [search_found_root_lba]
    mov [file_handle_root_lba], ax
    mov ax, [search_found_root_off]
    mov [file_handle_root_off], ax

    call int21_load_fat_cache
    jc .io_fail
    mov ax, [file_handle_start_cluster]
    call int21_count_chain
    mov [file_handle_cluster_count], ax

    mov ax, 0x0005
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

.access_denied:
    mov ax, 0x0005
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
    pop bx
    ret

int21_close:
    cmp bx, 0x0005
    jne .bad_handle
    cmp byte [cs:file_handle_open], 1
    jne .bad_handle
    mov byte [cs:file_handle_open], 0
    xor ax, ax
    clc
    ret
.bad_handle:
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
    mov ax, ds
    mov [cs:tmp_user_ds], ax
    mov [cs:tmp_user_ptr], dx

    mov ax, [cs:file_handle_pos]
    cmp ax, [cs:file_handle_size_lo]
    jae .eof

    mov ax, [cs:file_handle_size_lo]
    sub ax, [cs:file_handle_pos]
    cmp [cs:tmp_rw_remaining], ax
    jbe .loop
    mov [cs:tmp_rw_remaining], ax

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
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_write:
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

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

    cmp cx, 0
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
    mov ax, [cs:file_handle_cluster_count]
    mov cl, FAT_CLUSTER_SHIFT
    shl ax, cl
    mov [cs:tmp_capacity], ax

    mov ax, [cs:file_handle_pos]
    cmp ax, [cs:tmp_capacity]
    jb .have_space
    xor ax, ax
    clc
    jmp .done

.have_space:
    mov ax, [cs:tmp_capacity]
    sub ax, [cs:file_handle_pos]
    cmp [cs:tmp_rw_remaining], ax
    jbe .loop
    mov [cs:tmp_rw_remaining], ax

.loop:
    cmp word [cs:tmp_rw_remaining], 0
    je .finish

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
    mov ax, [cs:file_handle_pos]
    cmp ax, [cs:file_handle_size_lo]
    jbe .done_ok
    mov [cs:file_handle_size_lo], ax
    mov word [cs:file_handle_size_hi], 0
    call int21_update_root_entry_size
    jc .io_error

.done_ok:
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
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_delete:
    push bx
    push dx
    push si
    push ds
    push es

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

int21_seek:
    push bx
    push cx

    cmp bx, 0x0005
    jne .bad_handle
    cmp byte [cs:file_handle_open], 1
    jne .bad_handle
    cmp cx, 0
    jne .bad_function

    cmp al, 0
    je .from_start
    cmp al, 1
    je .from_current
    cmp al, 2
    je .from_end
    jmp .bad_function

.from_start:
    mov ax, dx
    jmp .set_pos

.from_current:
    mov ax, [cs:file_handle_pos]
    add ax, dx
    jmp .set_pos

.from_end:
    mov ax, [cs:file_handle_size_lo]
    add ax, dx

.set_pos:
    mov [cs:file_handle_pos], ax
    xor dx, dx
    mov ax, [cs:file_handle_pos]
    clc
    jmp .done

.bad_function:
    mov ax, 0x0001
    stc
    jmp .done

.bad_handle:
    mov ax, 0x0006
    stc

.done:
    pop cx
    pop bx
    ret

int21_mem_init:
    cmp byte [cs:dos_mem_init], 1
    je .done
    mov byte [cs:dos_mem_init], 1
    mov word [cs:dos_mem_alloc_seg], 0
    mov word [cs:dos_mem_alloc_size], 0
    mov word [cs:dos_mem_mcb_owner], 0
    mov word [cs:dos_mem_mcb_size], DOS_HEAP_USER_MAX_PARAS
    ; initialise list-of-lists: first word (BX-2) = first MCB segment
    mov word [cs:dos_list_of_lists], DOS_HEAP_BASE_SEG
    call int21_mem_write_mcb
.done:
    ret

int21_mem_write_mcb:
    push ax
    push es

    mov ax, DOS_HEAP_BASE_SEG
    mov es, ax
    mov byte [es:0x0000], 'Z'
    mov ax, [cs:dos_mem_mcb_owner]
    mov [es:0x0001], ax
    mov ax, [cs:dos_mem_mcb_size]
    mov [es:0x0003], ax

    pop es
    pop ax
    ret

int21_alloc:
    call int21_mem_init

    cmp bx, 0
    je .no_memory

    cmp word [cs:dos_mem_alloc_size], 0
    jne .busy

    cmp bx, DOS_HEAP_USER_MAX_PARAS
    ja .no_memory

    mov word [cs:dos_mem_alloc_seg], DOS_HEAP_USER_SEG
    mov [cs:dos_mem_alloc_size], bx
    mov word [cs:dos_mem_mcb_owner], DOS_HEAP_USER_SEG
    mov [cs:dos_mem_mcb_size], bx
    call int21_mem_write_mcb
    mov ax, DOS_HEAP_USER_SEG
    clc
    ret

.busy:
    ; block 1 busy: check if block 2 is free
    cmp word [cs:dos_mem_alloc_size2], 0
    jne .both_busy
    ; compute start of free space = end of block 1
    mov ax, [cs:dos_mem_alloc_seg]
    add ax, [cs:dos_mem_alloc_size]
    ; compute available paras
    mov cx, DOS_HEAP_LIMIT_SEG
    sub cx, ax
    cmp bx, cx
    ja .no_memory_b2
    ; allocate block 2
    mov [cs:dos_mem_alloc_seg2], ax
    mov [cs:dos_mem_alloc_size2], bx
    ; write MCB for block2 at (block2_seg - 1)
    push es
    push ax
    push bx
    dec ax
    mov es, ax
    mov byte [es:0x0000], 'Z'
    pop bx
    pop ax
    mov [es:0x0001], ax   ; owner = block2 data segment
    mov [es:0x0003], bx   ; size in paragraphs
    pop es
    ; update block1 MCB type to 'M' (middle, not last)
    push es
    mov ax, DOS_HEAP_BASE_SEG
    mov es, ax
    mov byte [es:0x0000], 'M'
    pop es
    mov ax, [cs:dos_mem_alloc_seg2]
    clc
    ret
.no_memory_b2:
    mov bx, cx
    mov ax, 0x0008
    stc
    ret
.both_busy:
    xor bx, bx
    mov ax, 0x0008
    stc
    ret

.no_memory:
    mov bx, DOS_HEAP_USER_MAX_PARAS
    mov ax, 0x0008
    stc
    ret

int21_free:
    call int21_mem_init

    cmp word [cs:dos_mem_alloc_size], 0
    je .invalid
    mov ax, es
    cmp ax, [cs:dos_mem_alloc_seg]
    je .free_block1
    ; check block 2
    cmp ax, [cs:dos_mem_alloc_seg2]
    jne .invalid
    mov word [cs:dos_mem_alloc_seg2], 0
    mov word [cs:dos_mem_alloc_size2], 0
    ; restore block1 MCB type to 'Z' (now last block)
    push es
    mov ax, DOS_HEAP_BASE_SEG
    mov es, ax
    mov byte [es:0x0000], 'Z'
    pop es
    xor ax, ax
    clc
    ret
.free_block1:

    mov word [cs:dos_mem_alloc_seg], 0
    mov word [cs:dos_mem_alloc_size], 0
    mov word [cs:dos_mem_mcb_owner], 0
    mov word [cs:dos_mem_mcb_size], DOS_HEAP_USER_MAX_PARAS
    call int21_mem_write_mcb
    xor ax, ax
    clc
    ret

.invalid:
    mov ax, 0x0009
    stc
    ret

int21_resize:
    call int21_mem_init

    ; Many DOS programs first resize their own PSP block (ES=PSP).
    ; Accept it as a compatibility no-op in this minimal allocator.
    mov ax, es
    cmp ax, [cs:current_psp_seg]
    jne .check_heap_block
    cmp ax, 0
    je .check_heap_block
    cmp bx, 0
    je .no_memory
    mov ax, es
    clc
    ret

.check_heap_block:

    cmp word [cs:dos_mem_alloc_size], 0
    je .invalid
    mov ax, es
    cmp ax, [cs:dos_mem_alloc_seg]
    je .resize_block1
    ; check block2
    cmp ax, [cs:dos_mem_alloc_seg2]
    jne .invalid
    cmp bx, 0
    je .no_memory
    mov [cs:dos_mem_alloc_size2], bx
    xor dx, dx
    mov ax, es
    clc
    ret
.resize_block1:
    cmp bx, 0
    je .no_memory
    cmp bx, DOS_HEAP_USER_MAX_PARAS
    ja .no_memory

    mov [cs:dos_mem_alloc_size], bx
    mov [cs:dos_mem_mcb_size], bx
    call int21_mem_write_mcb
    xor dx, dx
    mov ax, es
    clc
    ret

.invalid:
    mov ax, 0x0009
    stc
    ret

.no_memory:
    mov bx, DOS_HEAP_USER_MAX_PARAS
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
    mov cx, ax

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
    cmp al, '.'
    je .ext_start
    cmp al, '\'
    je .next_component
    cmp al, '/'
    je .next_component
    cmp bx, 8
    jae .name_advance
    call int21_upcase_al
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
    inc si
    xor bx, bx
.ext_loop:
    mov al, [si]
    cmp al, 0
    je .success
    cmp al, '\'
    je .next_component
    cmp al, '/'
    je .next_component
    cmp bx, 3
    jae .ext_advance
    call int21_upcase_al
    mov [es:path_fat_name + 8 + bx], al
    inc bx
.ext_advance:
    inc si
    jmp .ext_loop

.success:
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

int21_build_env_block:
    push ax
    push cx
    push si
    push di
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov ax, DOS_ENV_SEG
    mov es, ax
    xor di, di
    mov si, dos_env_block
    mov cx, dos_env_block_end - dos_env_block
    rep movsb

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

    mov dx, path_comdemo_dos
    mov ax, 0x3D00
    int 0x21
    cmp ax, 0x0005
    jne .fail

    mov bx, ax
    mov cx, 3
    mov dx, fileio_buf
    mov ah, 0x3F
    int 0x21
    cmp ax, 3
    jne .fail_close

    cmp byte [fileio_buf + 0], 0xBA
    jne .fail_close
    cmp byte [fileio_buf + 1], 0x0D
    jne .fail_close
    cmp byte [fileio_buf + 2], 0x01
    jne .fail_close

    mov ah, 0x3E
    int 0x21
    cmp ax, 0
    jne .fail

    mov dx, path_fileio_dos
    mov ax, 0x3D02
    int 0x21
    cmp ax, 0x0005
    jne .fail
    mov bx, ax

    xor cx, cx
    mov dx, 510
    mov ax, 0x4200
    int 0x21
    cmp ax, 510
    jne .fail_close

    mov cx, 4
    mov dx, fileio_buf
    mov ah, 0x3F
    int 0x21
    cmp ax, 4
    jne .fail_close
    cmp byte [fileio_buf + 0], 0xBE
    jne .fail_close
    cmp byte [fileio_buf + 1], 0xEF
    jne .fail_close
    cmp byte [fileio_buf + 2], 0xCA
    jne .fail_close
    cmp byte [fileio_buf + 3], 0xFE
    jne .fail_close

    xor cx, cx
    xor dx, dx
    mov ax, 0x4200
    int 0x21
    cmp ax, 0
    jne .fail_close

    mov cx, 2
    mov dx, fileio_patch
    mov ah, 0x40
    int 0x21
    cmp ax, 2
    jne .fail_close

    xor cx, cx
    xor dx, dx
    mov ax, 0x4200
    int 0x21
    cmp ax, 0
    jne .fail_close

    mov cx, 2
    mov dx, fileio_buf
    mov ah, 0x3F
    int 0x21
    cmp ax, 2
    jne .fail_close
    cmp byte [fileio_buf + 0], 0x11
    jne .fail_close
    cmp byte [fileio_buf + 1], 0x22
    jne .fail_close

    mov ah, 0x3E
    int 0x21
    cmp ax, 0
    jne .fail

    mov dx, path_deltest_dos
    mov ah, 0x41
    int 0x21
    jc .fail
    cmp ax, 0
    jne .fail

    mov dx, path_deltest_dos
    mov ax, 0x3D00
    int 0x21
    jnc .fail
    cmp ax, 0x0002
    jne .fail

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

run_stage1_selftest:
    mov si, msg_stage1_selftest_begin
    call print_string_dual
    mov si, msg_stage1_selftest_serial_begin
    call print_string_serial
    call int21_smoke_test
    call run_com_demo
    call run_mz_demo
    call int21_fileio_test
    call int21_find_test
    call run_gfx_demo
    mov si, msg_stage1_selftest_done
    call print_string_dual
    mov si, msg_stage1_selftest_serial_done
    call print_string_serial
    ret

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

    mov di, dx
    shl di, 6
    mov bx, dx
    shl bx, 8
    add di, bx
    add di, cx
    mov bx, 0xA000
    mov es, bx
    mov [es:di], al

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
    int 0x13

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
    mov si, str_ver
    call str_eq
    jc .cmd_ver
    mov di, bx
    mov si, str_tree
    call str_eq
    jc .cmd_tree
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
    mov si, str_dir
    call str_eq
    jc .cmd_dir
    mov di, bx
    mov si, str_cdup
    call str_eq
    jc .cmd_cdup
    mov di, bx
    mov si, str_cd
    call str_eq
    jc .cmd_cd
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
    mov si, str_findtest
    call str_eq
    jc .cmd_findtest
%if FAT_TYPE == 16
    mov di, bx
    mov si, str_opengem
    call str_eq
    jc .cmd_opengem
%endif
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
    call print_shell_help
    jmp .done

.cmd_ver:
    mov si, msg_version_line
    call print_string_dual
    jmp .done

.cmd_tree:
    call print_shell_tree
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

.cmd_dir:
    call shell_cmd_dir
    jmp .done

.cmd_cd:
    call shell_cmd_cd
    jmp .done

.cmd_cdup:
    call shell_cmd_cdup
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

.cmd_findtest:
    call int21_find_test
    jmp .done

%if FAT_TYPE == 16
.cmd_opengem:
    call run_stage2_payload
    jc .load_fail
    jmp .done

.load_fail:
    mov si, msg_mz_load_fail
    call print_string_dual
    jmp .done
%endif

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

shell_cmd_cdup:
    push ax
    push dx
    push ds

    mov ax, cs
    mov ds, ax
    mov dx, path_root_dos
    mov ah, 0x3B
    int 0x21
    jc .fail
    jmp .ok

.fail:
    mov si, msg_cd_fail
    call print_string_dual
.ok:
    pop ds
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

    call shell_arg_ptr
    cmp byte [si], 0
    je .show

    cmp byte [si], '.'
    jne .not_dot
    cmp byte [si + 1], 0
    je .done
    cmp byte [si + 1], '.'
    jne .not_dot
    cmp byte [si + 2], 0
    je .go_root

.not_dot:
    push si
    call shell_trim_first_arg
    pop si
    cmp byte [si], '/'
    jne .call_chdir
    mov byte [si], '\'

.call_chdir:
    mov dx, si
    mov ah, 0x3B
    int 0x21
    jc .fail
    jmp .done

.go_root:
    mov dx, path_root_dos
    mov ah, 0x3B
    int 0x21
    jc .fail
    jmp .done

.show:
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
    jmp .done

.print_cwd:
    mov si, cwd_buf
    call print_string_dual
    call print_newline_dual
    jmp .done

.fail:
    mov si, msg_cd_fail
    call print_string_dual

.done:
    pop ds
    pop si
    pop dx
    pop bx
    pop ax
    ret

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

    mov si, msg_dir_header
    call print_string_dual

    mov word [shell_dir_count], 0
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

.done_scan:
    cmp word [shell_dir_count], 0
    jne .ok
    mov si, msg_dir_empty
    call print_string_dual

.ok:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

.fail:
    mov si, msg_dir_fail
    call print_string_dual
    jmp .ok

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

show_boot_splash:
    push ax
    push bx
    push cx
    push dx
    push si

    mov ax, 0x0003
    int 0x10
    mov bl, 0x1F
    call clear_screen_attr

    mov si, splash_title
    mov dh, 7
    mov dl, 28
    mov bl, 0x1F
    call video_write_string_attr

    mov si, splash_subtitle
    mov dh, 9
    mov dl, 21
    mov bl, 0x1E
    call video_write_string_attr

    mov si, splash_status
    mov dh, 15
    mov dl, 23
    mov bl, 0x1F
    call video_write_string_attr

    mov si, splash_wait_hint
    mov dh, 19
    mov dl, 21
    mov bl, 0x1E
    call video_write_string_attr

    mov al, '['
    mov dh, 17
    mov dl, 17
    mov bl, 0x1F
    call video_write_char_attr
    mov al, ']'
    mov dl, 62
    call video_write_char_attr

    mov al, 0
    call splash_set_progress

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

splash_set_progress:
    push ax
    push bx
    push cx
    push dx

    xor ah, ah
    mov bl, 45
    mul bl
    mov bl, 100
    div bl
    mov ch, al

    mov al, ' '
    mov dh, 17
    mov dl, 18
    mov bl, 0x17
    xor cx, cx
    mov cl, 45
    call draw_hline_attr

    cmp ch, 0
    je .percent
    mov al, 0xDB
    mov dh, 17
    mov dl, 18
    mov bl, 0x1F
    mov cl, ch
    xor ch, ch
    call draw_hline_attr

.percent:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

splash_hold:
    push ax
    push bx
    push dx

    mov ah, 0x00
    int 0x1A
    mov bx, dx
    add bx, 91

.loop:
    mov ah, 0x01
    int 0x16
    jnz .consume

    mov ah, 0x00
    int 0x1A
    cmp dx, bx
    jb .loop
    jmp .done

.consume:
    xor ah, ah
    int 0x16

.done:
    pop dx
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

    mov al, ' '
    mov dh, 0
    mov dl, 0
    mov bl, 0x70
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    mov al, 0xC4
    mov dh, 1
    mov dl, 0
    mov bl, 0x17
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    mov si, msg_banner_title
    mov dh, 0
    mov dl, 22
    mov bl, 0x70
    call video_write_string_attr

    mov si, msg_shell_hint
    mov dh, 2
    mov dl, 2
    mov bl, 0x1E
    call video_write_string_attr

    mov si, msg_shell_quick
    mov dh, 3
    mov dl, 2
    mov bl, 0x0F
    call video_write_string_attr

    mov al, 0xC4
    mov dh, 24
    mov dl, 0
    mov bl, 0x17
    xor cx, cx
    mov cl, 80
    call draw_hline_attr

    mov si, msg_shell_footer
    mov dh, 24
    mov dl, 2
    mov bl, 0x1E
    call video_write_string_attr

%ifdef FAT_TYPE
%if FAT_TYPE == 12
    mov si, msg_shell_sysinfo_prefix
    mov dh, 24
    mov dl, 40
    mov bl, 0x1E
    call video_write_string_attr

    int 0x12
    mov cx, ax
    mov ax, cx
    mov bx, 10
    xor dx, dx
    mov di, ram_buf
    call convert_dec_buf

    mov si, ram_buf
    mov dh, 24
    mov dl, 45
    mov bl, 0x1E
    call video_write_string_attr

    mov al, 'K'
    mov dh, 24
    mov dl, 49
    mov bl, 0x1E
    call video_write_char_attr
%endif
%endif

    mov dh, 6
    xor dl, dl
    call set_cursor_pos

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

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

print_shell_tree:
    mov si, msg_tree_header
    call print_string_dual
    mov si, msg_tree_root
    call print_string_dual
    mov si, msg_tree_system
    call print_string_dual
    mov si, msg_tree_apps
    call print_string_dual
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
    call serial_putc
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
    call init_mouse
    call init_vbe_query
    call install_int33_vector
    mov si, msg_stage2_ready
    call print_string_serial
%if FAT_TYPE == 16
%if STAGE2_AUTORUN
    call run_stage2_payload
%endif
%endif
    pop si
    pop ax
    ret

%if FAT_TYPE == 16
run_stage2_payload:
    push ax
    push bx
    push ds
    push es

    mov si, msg_stage2_autorun_begin
    call print_string_serial

    mov ax, cs
    mov ds, ax
    mov ax, STAGE2_LOAD_SEG
    mov es, ax
    xor bx, bx
    mov si, path_stage2_dos
    call load_root_file_first_sector
    jc .load_fail

    mov si, msg_stage2_autorun_loaded
    call print_string_serial

    call STAGE2_LOAD_SEG:0x0000
    mov si, msg_stage2_autorun_return
    call print_string_serial
    clc
    jmp .done

.load_fail:
    mov si, msg_stage2_autorun_fail
    call print_string_serial
    stc

.done:
    pop es
    pop ds
    pop bx
    pop ax
    ret
%endif

init_mouse:
    push ax
    push bx
    mov ax, 0x0000
    int 0x33
    cmp ax, 0xFFFF
    jne .no_mouse
    mov ax, 0x0001
    int 0x33
    mov si, msg_mouse_enabled
    call print_string_serial
    pop bx
    pop ax
    ret
.no_mouse:
    mov si, msg_mouse_not_found
    call print_string_serial
    pop bx
    pop ax
    ret

init_vbe_query:
    mov si, msg_vbe_init
    call print_string_serial
    ret

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
    pop es
    pop bx
    pop ax
    ret

int33_handler:
    push ax
    xor ax, ax
    pop ax
    iret

int2f_handler:
    cmp ax, 0x1680
    je .idle
    cmp ax, 0x1600
    je .query_win
    iret
.idle:
    iret
.query_win:
    xor ax, ax
    iret

boot_drive db 0
int21_installed db 0
int21_carry db 0
int21_return_es db 0
int2f_installed db 0
dos_default_drive db 0
last_exit_code db 0
int21_last_ah db 0
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
file_handle_open db 0
file_handle_pos dw 0
file_handle_mode db 0
file_handle_start_cluster dw 0
file_handle_root_lba dw 0
file_handle_root_off dw 0
file_handle_cluster_count dw 0
file_handle_size_lo dw 0
file_handle_size_hi dw 0
fat_cache_valid db 0
fat_cache_dirty db 0
fat_cache_sector dw 0xFFFF
tmp_user_ds dw 0
tmp_user_ptr dw 0
tmp_rw_remaining dw 0
tmp_rw_done dw 0
tmp_chunk dw 0
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
dos_mem_init db 0
dos_mem_alloc_seg dw 0
dos_mem_alloc_size dw 0
dos_mem_alloc_seg2 dw 0
dos_mem_alloc_size2 dw 0
dos_mem_mcb_owner dw 0
dos_mem_mcb_size dw 0
dos21_test_seg dw 0
saved_ss dw 0
saved_sp dw 0
saved_ds dw 0
saved_es dw 0
current_load_seg dw MZ_LOAD_SEG
saved_psp dw 0
saved_ss2 dw 0
saved_sp2 dw 0
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
dos_env_block db 'COMSPEC=C:\COMMAND.COM', 0
              db 'PATH=C:\', 0
              db 0
dos_env_block_end:
cmd_buffer times CMD_BUF_LEN db 0

msg_stage1        db "[STAGE1] run", 13, 10, 0
msg_stage1_serial db "[STAGE1-SERIAL] READY", 13, 10, 0
msg_diag_begin    db "[STAGE1] diag", 13, 10, 0
msg_diag_int10    db "[INT10] OK", 13, 10, 0
msg_diag_int13_ok db "[INT13] OK", 13, 10, 0
msg_diag_int13_fail db "[INT13] FAIL", 13, 10, 0
msg_diag_int16_ok db "[INT16] OK", 13, 10, 0
msg_diag_int1a    db "[TICKS] 0x", 0
msg_int21_installed db "[INT21] ok", 13, 10, 0
msg_int21_missing db "[I21] no", 13, 10, 0
msg_int21_unsup db "[INT21-UNSUP] AH=", 0
msg_int21_err db "[IERR] ", 0
msg_stage1_selftest_begin db "[S1T] begin", 13, 10, 0
msg_stage1_selftest_done db "[S1T] done", 13, 10, 0
msg_stage1_selftest_serial_begin db "[S1T] B", 13, 10, 0
msg_stage1_selftest_serial_done db "[S1T] D", 13, 10, 0

msg_prompt    db "root:\> ", 0
msg_unknown   db "Unknown command. Type 'help' for available commands.", 13, 10, 0
msg_banner_title db " CiukiOS pre-Alpha v0.5.8 ", 0
msg_shell_hint db "CiukiDOS Shell", 0
msg_shell_quick db "Type 'help' for available commands", 0
msg_shell_footer db "Ready", 0
%if FAT_TYPE == 12
msg_shell_sysinfo_prefix db "RAM:", 0
%endif
msg_help_header db "--- CiukiDOS Commands ---", 13, 10, 0
msg_help_core db "  help  dir  cd  cls  tree  ver", 13, 10, 0
msg_help_runtime db "  dos21  comdemo  mzdemo  fileio  findtest", 13, 10, 0
msg_help_system db "  gfxdemo  ticks  drive  reboot  halt", 13, 10, 0
msg_help_apps db "  opengem", 13, 10, 0
msg_version_line db "CiukiOS v0.5.8", 13, 10, 0
msg_tree_header db "tree", 13, 10, 0
msg_tree_root db "  ROOT", 13, 10, 0
msg_tree_system db "   |- SYSTEM", 13, 10, 0
msg_tree_apps db "   `- APPS", 13, 10, 0
msg_ticks     db "ticks=0x", 0
msg_drive     db "boot drive=0x", 0
msg_dos21_begin db "[DOS21] smoke", 13, 10, 0
msg_dos21_ah09 db "[INT21/09] ok", 13, 10, '$'
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
msg_rebooting db "rebooting...", 13, 10, 0
msg_halting   db "halting...", 13, 10, 0
msg_dir_header db "Dir", 13, 10, 0
msg_dir_empty db "no files found", 13, 10, 0
msg_dir_fail db "dir failed", 13, 10, 0
msg_cd_fail db "cd failed", 13, 10, 0
msg_cwd_prefix db "cwd=", 0
splash_title db "CiukiOS", 0
splash_subtitle db "loading", 0
splash_status db "init", 0
splash_wait_hint db "shell in 5s", 0
gfx_text_ciukios db "CIUKIOS", 0
gfx_text_demo db "GFX DEMO", 0
gfx_text_vdi db "VDI BASE", 0
gfx_text_timer db "KEY EXIT", 0

str_help   db "help", 0
str_ver    db "ver", 0
str_tree   db "tree", 0
str_cls    db "cls", 0
str_ticks  db "ticks", 0
str_drive  db "drive", 0
str_dir    db "dir", 0
str_cd     db "cd", 0
str_cdup   db "cd..", 0
str_dos21  db "dos21", 0
str_comdemo db "comdemo", 0
str_mzdemo db "mzdemo", 0
str_fileio db "fileio", 0
str_gfxdemo db "gfxdemo", 0
str_findtest db "findtest", 0
%if FAT_TYPE == 16
str_opengem db "opengem", 0
%endif
str_reboot db "reboot", 0
str_halt   db "halt", 0

path_comdemo_dos db "COMDEMO.COM", 0
path_mzdemo_dos  db "MZDEMO.EXE", 0
path_fileio_dos  db "FILEIO.BIN", 0
path_deltest_dos db "DELTEST.BIN", 0
%if FAT_TYPE == 16
path_stage2_dos db "STAGE2  BIN"
%endif
path_pattern_com db "*.COM", 0
path_pattern_mz  db "MZDEMO.EXE", 0
path_sd_driver_fat db "SDPSC9  VGA"
path_root_dos    db "\", 0
cwd_buf times 24 db 0
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
msg_stage2_autorun_begin db "[S2] autorun", 13, 10, 0
msg_stage2_autorun_loaded db "[S2] stage2 loaded", 13, 10, 0
msg_stage2_autorun_return db "[S2] stage2 return", 13, 10, 0
msg_stage2_autorun_fail db "[S2] stage2 load fail", 13, 10, 0
msg_mouse_enabled db "[S2] mouse", 13, 10, 0
msg_mouse_not_found db "[S2] no mouse", 13, 10, 0
msg_vbe_init db "[S2] vbe", 13, 10, 0
