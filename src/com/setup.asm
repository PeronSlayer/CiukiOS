; setup.asm - CiukiOS SETUP.COM MVP (FULL-only stream)
; Text-mode keyboard wizard + install pipeline for DOS COM runtime.

bits 16
org 0x0100

%define FILE_COUNT 9
%define MANIFEST_HEADER_SIZE 5
%define MANIFEST_RECORD_SIZE 4
%define PROMPT_TIMEOUT_TICKS 90
%define CLEANUP_FILE_COUNT 12
%define RAW_FAT_SPT 63
%define RAW_FAT_HEADS 16
%define RAW_BOOT_DRIVE 0x80
%define RAW_DATA_LBA 359
%define RAW_APPS_DIR_LBA (RAW_DATA_LBA + 8)
%define RAW_HDD_SOURCE_DRIVE 0x80
%define RAW_HDD_TARGET_DRIVE 0x81
%define RAW_HDD_CLONE_SECTORS_LO 0x003F
%define RAW_HDD_CLONE_SECTORS_HI 0x0004
%define RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA 64
%define RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF 0x0133
%define RAW_STAGE1_LIVE_DRIVE_INDEX 3
%define RAW_STAGE1_INSTALLED_DRIVE_INDEX 2
%define RAW_HDD_SECTORS_PER_CYL 1008
%ifndef SETUP_ENABLE_RAW_HDD_INSTALL
%define SETUP_ENABLE_RAW_HDD_INSTALL 0
%endif
%ifndef SETUP_ENABLE_RAW_HDD_DESTRUCTIVE
%define SETUP_ENABLE_RAW_HDD_DESTRUCTIVE 0
%endif
%ifndef SETUP_LIVE_CD_MODE
%define SETUP_LIVE_CD_MODE 0
%endif
%ifndef SETUP_RAW_TARGET_DRIVE_INDEX
%if SETUP_LIVE_CD_MODE
%define SETUP_RAW_TARGET_DRIVE_INDEX 2
%else
%define SETUP_RAW_TARGET_DRIVE_INDEX 3
%endif
%endif

start:
    cld
    push cs
    pop ds
    push cs
    pop es

    mov byte [selected_profile], 1
    mov byte [install_ok], 0
    mov word [fail_code], 0
    mov word [files_planned], 0
    mov word [files_copied], 0
    mov word [bytes_copied], 0
    mov word [bytes_copied+2], 0
    mov word [active_handle], 0xFFFF
    mov byte [step_id], 0x00
    mov byte [retry_count], 0
    mov word [kb_key_total], 0
    mov byte [kb_nav_count], 0
    mov byte [media_swap_count], 0
    mov byte [current_media_id], 0
    mov byte [expected_media_id], 0
    mov byte [source_drive], 0
    mov byte [target_drive], 0
    mov byte [valid_target_count], 0
    mov word [prompt_tick_start], 0
    mov byte [bios_probe_present_mask], 0
    mov byte [bios_probe_blank_mask], 0
    mov byte [bios_probe_mbrsig_mask], 0
    mov byte [raw_hdd_install_mode], 0

    mov byte [step_id], 0x10
    call detect_targets
    jc install_fail

    mov byte [step_id], 0x11
    call show_welcome
    jc user_abort

    mov byte [step_id], 0x12
    call choose_profile
    jc user_abort
    call guard_profile_selection
    jc install_fail

    mov byte [step_id], 0x13
    call confirm_target
    jc user_abort

    mov byte [step_id], 0x14
    call guard_target_selection
    jc install_fail

    cmp byte [raw_hdd_install_mode], 1
    jne .int21_install
    call confirm_raw_hdd_destroy
    jc install_fail
    mov byte [step_id], 0x40
    call raw_hdd_clone_install
    jc install_fail
    mov byte [step_id], 0x30
    mov byte [install_ok], 1
    mov dx, msg_success
    call print_line
    mov dx, msg_marker_done
    call print_line
    jmp finalize

.int21_install:

    mov byte [step_id], 0x20
    call load_payload_manifest
    jc install_fail

    call compute_planned_files
    mov [files_planned], ax
    cmp ax, 0
    jne .have_plan
    mov word [fail_code], 0x0008
    jmp install_fail

.have_plan:
    mov dx, msg_marker_start
    call print_line

    mov byte [step_id], 0x21
    call preflight_space
    jc install_fail

    mov byte [step_id], 0x22
    call prepare_target_fs
    jc install_fail

    mov byte [step_id], 0x23
    call postformat_sanity
    jc install_fail

    mov byte [step_id], 0x24
    call copy_manifest
    jc install_fail

    mov byte [step_id], 0x25
    call write_config_file
    jc install_fail

    mov byte [step_id], 0x30
    mov byte [install_ok], 1
    mov dx, msg_success
    call print_line
    mov dx, msg_marker_done
    call print_line
    jmp finalize

user_abort:
    mov byte [step_id], 0xE0
    mov word [fail_code], 0x0001

install_fail:
    mov byte [install_ok], 0
    mov dx, msg_failed
    call print_line
    mov dx, msg_marker_fail
    call print_line

finalize:
    cmp byte [raw_hdd_install_mode], 1
    je .skip_report
    call write_install_report
.skip_report:
    mov ax, 0x4C00
    cmp byte [install_ok], 1
    je .exit
    mov ax, 0x4C01
.exit:
    int 0x21

; -----------------------------------------------------------------------------
; Wizard screens
; -----------------------------------------------------------------------------

detect_targets:
    call print_crlf
    mov dx, msg_target_scan_start
    call print_line

    mov ah, 0x19
    int 0x21
    mov [source_drive], al
    mov [target_drive], al

    mov ah, 0x36
    xor dl, dl             ; runtime-stable path: query current/default drive
    int 0x21
    cmp ax, 0xFFFF
    jne .ok
    mov byte [valid_target_count], 0
    mov word [fail_code], 0x0203
    mov dx, msg_target_scan_fail
    call print_line
    stc
    ret

.ok:
    mov byte [valid_target_count], 1
    mov dx, msg_marker_target_scan
    call print_line
    call probe_bios_hdds_readonly
    call print_disk_status_panel
    clc
    ret

show_welcome:
    mov dx, msg_welcome_1
    call print_line
    mov dx, msg_welcome_2
    call print_line
    mov dx, msg_welcome_3
    call print_line
    mov dx, msg_welcome_4
    call print_line
    call print_crlf
    mov dx, msg_enter_esc
    call print_line
    call wait_enter_or_esc
    ret

choose_profile:
    call print_crlf
    mov dx, msg_profile_1
    call print_line
    mov dx, msg_profile_2
    call print_line
    mov dx, msg_profile_3
    call print_line
    mov dx, msg_profile_4
    call print_line
    mov dx, msg_profile_prompt
    call print_line

.wait_key:
    call read_key
    cmp al, 13
    je .selected
    cmp al, 0xC8
    je .up
    cmp al, 0xD0
    je .down
    cmp al, '1'
    je .set_min
    cmp al, '2'
    je .set_std
    cmp al, '3'
    je .set_full
    cmp al, 27
    je .abort
    jmp .wait_key

.set_min:
    mov byte [selected_profile], 1
    jmp .selected

.set_std:
    mov byte [selected_profile], 2
    jmp .selected

.set_full:
    mov byte [selected_profile], 3

.selected:
    mov dx, msg_profile_selected
    call print_z
    call print_profile_name
    call print_crlf
    clc
    ret

.up:
    cmp byte [selected_profile], 1
    jbe .wait_key
    dec byte [selected_profile]
    jmp .wait_key

.down:
    cmp byte [selected_profile], 3
    jae .wait_key
    inc byte [selected_profile]
    jmp .wait_key

.abort:
    stc
    ret

confirm_target:
    call print_crlf
    mov dx, msg_target_1
    call print_line
    call print_disk_status_panel

.show_target:
    mov dx, msg_target_drive_prefix
    call print_z
    call print_target_drive
    call print_crlf

    mov dx, msg_target_2
    call print_z
    mov dx, path_target_root
    call print_z
    call print_crlf

    mov dx, msg_target_prompt
    call print_line

.wait_key:
    call read_key
    cmp al, 13
    je .ok
    cmp al, 27
    je .esc

    call key_to_drive_index
    jc .wait_key

    mov [target_drive], al
    mov dx, msg_target_selected
    call print_z
    call print_target_drive
    call print_crlf
    jmp .wait_key

.ok:
    clc
    ret

.esc:
    stc
    ret

print_disk_status_panel:
    push ax
    push dx
    call print_crlf
    mov dx, msg_disk_panel_header
    call print_line
%if SETUP_LIVE_CD_MODE
    mov dx, msg_disk_live_d
    call print_z
    mov al, 0x01
    call print_probe_status
    call print_crlf
    mov dx, msg_disk_target_c
    call print_z
    mov al, 0x02
    call print_probe_status
    call print_crlf
%else
    mov dx, msg_disk_bios80
    call print_z
    mov al, 0x01
    call print_probe_status
    call print_crlf
    mov dx, msg_disk_bios81
    call print_z
    mov al, 0x02
    call print_probe_status
    call print_crlf
%endif
    pop dx
    pop ax
    ret

print_probe_status:
    push ax
    push bx
    push dx
    mov bl, al
    mov al, [bios_probe_present_mask]
    test al, bl
    jnz .present
    mov dx, msg_disk_absent
    call print_z
    jmp .done
.present:
    mov dx, msg_disk_present
    call print_z
    mov al, [bios_probe_blank_mask]
    test al, bl
    jz .has_data
    mov dx, msg_disk_blank
    call print_z
    jmp .sig
.has_data:
    mov dx, msg_disk_data
    call print_z
.sig:
    mov al, [bios_probe_mbrsig_mask]
    test al, bl
    jz .no_sig
    mov dx, msg_disk_mbr
    call print_z
    jmp .done
.no_sig:
    mov dx, msg_disk_no_mbr
    call print_z
.done:
    pop dx
    pop bx
    pop ax
    ret

; -----------------------------------------------------------------------------
; Install pipeline
; -----------------------------------------------------------------------------

guard_target_selection:
    mov byte [raw_hdd_install_mode], 0
    mov al, [target_drive]
    cmp al, 2
    jb .invalid_target

    cmp al, [source_drive]
    jne .maybe_raw_hdd_target

    call probe_target_drive
    jc .invalid_target

    clc
    ret

.maybe_raw_hdd_target:
%if SETUP_ENABLE_RAW_HDD_INSTALL
    cmp al, SETUP_RAW_TARGET_DRIVE_INDEX
    jne .unsupported_target
    call guard_raw_hdd_topology
    jc .unsupported_target
    mov byte [raw_hdd_install_mode], 1
    clc
    ret
%else
    jmp .unsupported_target
%endif

.invalid_target:
    mov word [fail_code], 0x0203
    mov dx, msg_target_invalid
    call print_line
    stc
    ret

.unsupported_target:
    mov word [fail_code], 0x0204
    mov dx, msg_target_unsupported
    call print_line
    stc
    ret

probe_target_drive:
    mov ah, 0x36
    xor dl, dl             ; target is constrained to current/source drive
    int 0x21
    cmp ax, 0xFFFF
    jne .ok
    stc
    ret
.ok:
    clc
    ret

guard_raw_hdd_topology:
    cmp byte [bios_probe_present_mask], 0x03
    jne .fail
    cmp byte [bios_probe_mbrsig_mask], 0x01
    jne .fail
%if SETUP_ENABLE_RAW_HDD_DESTRUCTIVE
    clc
    ret
%else
    cmp byte [bios_probe_blank_mask], 0x02
    jne .fail
    clc
    ret
%endif
.fail:
    stc
    ret

raw_hdd_clone_install:
    push ax
    push bx
    push cx
    push dx
    push si
    push es
    push cs
    pop ds
    push cs
    pop es

    call serial_init_com1
    mov dx, msg_serial_hdd_install_start
    call serial_write_z
    call serial_write_crlf
    call raw_init_drive_geometries
    mov word [raw_clone_lba_lo], 0
    mov word [raw_clone_lba_hi], 0
    mov word [raw_clone_remaining_lo], RAW_HDD_CLONE_SECTORS_LO
    mov word [raw_clone_remaining_hi], RAW_HDD_CLONE_SECTORS_HI
    mov byte [raw_edd_status], 0
    mov byte [raw_chs_status], 0

.copy_loop:
    mov ax, [raw_clone_remaining_lo]
    or ax, [raw_clone_remaining_hi]
    jz .done

    mov dl, RAW_HDD_SOURCE_DRIVE
    mov bx, io_buffer
    call raw_edd_read_current_lba
    jc .fail

    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    call raw_edd_write_current_lba
    jc .fail
    inc word [raw_clone_lba_lo]
    jnz .dec_remaining
    inc word [raw_clone_lba_hi]

.dec_remaining:
    sub word [raw_clone_remaining_lo], 1
    sbb word [raw_clone_remaining_hi], 0
    jmp .copy_loop

.done:
%if SETUP_LIVE_CD_MODE
    call raw_patch_installed_default_drive
    jc .fail
%endif
    mov dx, msg_serial_hdd_install_done
    call serial_write_z
    call serial_write_crlf
    clc
    jmp .out

.fail:
    mov word [fail_code], 0x0701
    mov dx, msg_serial_hdd_install_fail
    call serial_write_z
    mov al, [raw_last_stage]
    call serial_write_char
    mov dx, msg_serial_hdd_install_path
    call serial_write_z
    mov al, [raw_last_path]
    call serial_write_char
    mov dx, msg_serial_hdd_install_lba
    call serial_write_z
    mov ax, [raw_clone_lba_hi]
    call serial_write_hex_word
    mov al, ':'
    call serial_write_char
    mov ax, [raw_clone_lba_lo]
    call serial_write_hex_word
    mov dx, msg_serial_hdd_install_status
    call serial_write_z
    mov al, [raw_last_status]
    call serial_write_hex_byte
    mov dx, msg_serial_hdd_install_edd
    call serial_write_z
    mov al, [raw_edd_status]
    call serial_write_hex_byte
    mov dx, msg_serial_hdd_install_chs
    call serial_write_z
    mov al, [raw_chs_status]
    call serial_write_hex_byte
    call serial_write_crlf
    stc

.out:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

raw_patch_installed_default_drive:
    push ax
    push bx
    push dx
    mov byte [raw_last_stage], 80
    mov byte [raw_last_path], 69
    mov word [raw_clone_lba_lo], RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA
    mov word [raw_clone_lba_hi], 0
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    call raw_edd_read_current_lba
    jc .fail
    cmp byte [io_buffer + RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF], RAW_STAGE1_LIVE_DRIVE_INDEX
    jne .fail
    mov byte [io_buffer + RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF], RAW_STAGE1_INSTALLED_DRIVE_INDEX
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    call raw_edd_write_current_lba
    jc .fail
    clc
    jmp .out
.fail:
    stc
.out:
    pop dx
    pop bx
    pop ax
    ret

raw_edd_read_current_lba:
    mov byte [raw_last_stage], 'R'
    mov byte [raw_last_path], 'E'
    mov ah, 0x42
    call raw_edd_transfer_current_lba
    jnc .ok
    mov byte [raw_last_path], 'C'
    mov ah, 0x02
    call raw_chs_transfer_current_lba
.ok:
    ret

raw_edd_write_current_lba:
    mov byte [raw_last_stage], 'W'
    mov byte [raw_last_path], 'E'
    mov ah, 0x43
    call raw_edd_transfer_current_lba
    jnc .ok
    mov byte [raw_last_path], 'C'
    mov ah, 0x03
    call raw_chs_transfer_current_lba
.ok:
    ret

raw_init_drive_geometries:
    mov dl, RAW_HDD_SOURCE_DRIVE
    mov si, raw_source_spt
    call raw_get_drive_geometry
    mov dl, RAW_HDD_TARGET_DRIVE
    mov si, raw_target_spt
    call raw_get_drive_geometry
    ret

raw_get_drive_geometry:
    push ax
    push bx
    push cx
    push dx
    push si
    push cs
    pop ds

    mov word [si], RAW_FAT_SPT
    mov word [si + 2], RAW_FAT_HEADS
    mov word [si + 4], RAW_HDD_SECTORS_PER_CYL

    mov ah, 0x08
    int 0x13
    jc .done

    push cs
    pop ds
    and cl, 0x3F
    jz .done

    xor ax, ax
    mov al, cl
    mov [si], ax
    xor ax, ax
    mov al, dh
    inc ax
    mov [si + 2], ax
    mul word [si]
    or dx, dx
    jnz .done
    mov [si + 4], ax

.done:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

raw_edd_transfer_current_lba:
    push ax
    push bx
    push dx
    push si
    push cs
    pop ds
    mov word [bios_probe_dap + 2], 0x0001
    mov word [bios_probe_dap + 4], bx
    mov bx, cs
    mov word [bios_probe_dap + 6], bx
    mov bx, [raw_clone_lba_lo]
    mov word [bios_probe_dap + 8], bx
    mov bx, [raw_clone_lba_hi]
    mov word [bios_probe_dap + 10], bx
    mov word [bios_probe_dap + 12], 0
    mov word [bios_probe_dap + 14], 0
    mov si, bios_probe_dap
    push cs
    pop ds
    xor al, al
    cmp ah, 0x43
    jne .int13
    xor al, al
.int13:
    int 0x13
    jc .fail
    pop si
    pop dx
    pop bx
    pop ax
    clc
    ret
.fail:
    mov [raw_last_status], ah
    mov [raw_edd_status], ah
    pop si
    pop dx
    pop bx
    pop ax
    stc
    ret

raw_chs_transfer_current_lba:
    push ax
    push bx
    push dx
    push si
    push es
    push cs
    pop ds

    mov si, bx
    mov [raw_chs_drive], dl
    mov [raw_chs_op], ah

    cmp dl, RAW_HDD_SOURCE_DRIVE
    jne .target_geometry
    mov bx, [raw_source_spc]
    mov [raw_chs_spc], bx
    mov bx, [raw_source_spt]
    mov [raw_chs_spt], bx
    jmp .have_geometry

.target_geometry:
    mov bx, [raw_target_spc]
    mov [raw_chs_spc], bx
    mov bx, [raw_target_spt]
    mov [raw_chs_spt], bx

.have_geometry:

    mov ax, [raw_clone_lba_lo]
    mov dx, [raw_clone_lba_hi]
    mov bx, [raw_chs_spc]
    div bx
    mov [raw_chs_cylinder], ax

    mov ax, dx
    xor dx, dx
    mov bx, [raw_chs_spt]
    div bx

    mov dh, al
    mov cl, dl
    inc cl
    mov ax, [raw_chs_cylinder]
    mov ch, al
    mov al, ah
    and al, 0x03
    shl al, 6
    or cl, al

    push cs
    pop es
    mov bx, si
    mov dl, [raw_chs_drive]
    mov ah, [raw_chs_op]
    mov al, 0x01
    int 0x13
    jc .fail

    pop es
    pop si
    pop dx
    pop bx
    pop ax
    clc
    ret
.fail:
    mov [raw_last_status], ah
    mov [raw_chs_status], ah
    pop es
    pop si
    pop dx
    pop bx
    pop ax
    stc
    ret

probe_bios_hdds_readonly:
    push ax
    push bx
    push dx

    mov byte [bios_probe_present_mask], 0
    mov byte [bios_probe_blank_mask], 0
    mov byte [bios_probe_mbrsig_mask], 0

    mov dl, 0x80
    mov bl, 0x01
    call bios_probe_one_readonly
    mov dl, 0x81
    mov bl, 0x02
    call bios_probe_one_readonly

    call serial_emit_bios_probe

    pop dx
    pop bx
    pop ax
    ret

bios_probe_one_readonly:
    push ax
    push bx
    push cx
    push dx
    push si
    push es
    push ds
    push cs
    pop ds

    mov [bios_probe_drive], dl
    mov [bios_probe_bit], bl

    mov word [bios_probe_dap + 4], io_buffer
    mov ax, cs
    mov [bios_probe_dap + 6], ax
    mov word [bios_probe_dap + 8], 0
    mov word [bios_probe_dap + 10], 0
    mov word [bios_probe_dap + 12], 0
    mov word [bios_probe_dap + 14], 0

    mov si, bios_probe_dap
    mov dl, [bios_probe_drive]
    mov ah, 0x42
    int 0x13
    jnc .read_ok

    push cs
    pop es
    mov bx, io_buffer
    mov ax, 0x0201
    mov cx, 0x0001
    xor dh, dh
    mov dl, [bios_probe_drive]
    int 0x13
    jc .done

.read_ok:
    mov al, [bios_probe_bit]
    or byte [bios_probe_present_mask], al

    cmp byte [io_buffer + 510], 0x55
    jne .check_blank
    cmp byte [io_buffer + 511], 0xAA
    jne .check_blank
    mov al, [bios_probe_bit]
    or byte [bios_probe_mbrsig_mask], al

.check_blank:
    mov si, io_buffer
    mov cx, 256
    xor ax, ax
.blank_loop:
    cmp [si], ax
    jne .done
    add si, 2
    loop .blank_loop
    mov al, [bios_probe_bit]
    or byte [bios_probe_blank_mask], al

.done:
    pop ds
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

serial_emit_bios_probe:
    push ax
    push dx
    call serial_init_com1
    mov dx, msg_serial_bios_probe
    call serial_write_z
    mov al, [bios_probe_present_mask]
    call serial_write_hex_byte
    mov dx, msg_serial_probe_blank
    call serial_write_z
    mov al, [bios_probe_blank_mask]
    call serial_write_hex_byte
    mov dx, msg_serial_probe_sig
    call serial_write_z
    mov al, [bios_probe_mbrsig_mask]
    call serial_write_hex_byte
    call serial_write_crlf
    pop dx
    pop ax
    ret

serial_init_com1:
    push ax
    push dx
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 0x01
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x03
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al
    pop dx
    pop ax
    ret

serial_write_z:
    push ax
    push dx
    push si
    mov si, dx
.serial_loop:
    lodsb
    or al, al
    jz .serial_done
    call serial_write_char
    jmp .serial_loop
.serial_done:
    pop si
    pop dx
    pop ax
    ret

serial_write_crlf:
    mov al, 13
    call serial_write_char
    mov al, 10
    call serial_write_char
    ret

serial_write_hex_byte:
    push ax
    mov ah, al
    shr al, 4
    call serial_write_hex_nibble
    mov al, ah
    and al, 0x0F
    call serial_write_hex_nibble
    pop ax
    ret

serial_write_hex_word:
    push ax
    mov al, ah
    call serial_write_hex_byte
    pop ax
    call serial_write_hex_byte
    ret

serial_write_hex_nibble:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    call serial_write_char
    ret

serial_write_char:
    push ax
    push cx
    push dx
    mov ah, al
    mov dx, 0x3FD
    mov cx, 0xFFFF
.wait_tx:
    in al, dx
    test al, 0x20
    jnz .ready_tx
    loop .wait_tx
    jmp .out
.ready_tx:
    mov dx, 0x3F8
    mov al, ah
    out dx, al
.out:
    pop dx
    pop cx
    pop ax
    ret

confirm_raw_hdd_destroy:
    push ax
    push bx
    push dx
    push si

    call print_crlf
    mov dx, msg_raw_destroy_1
    call print_line
    mov dx, msg_raw_destroy_2
    call print_line
    mov dx, msg_raw_destroy_3
    call print_line
    mov si, str_destroy_confirm

.next_char:
    lodsb
    or al, al
    jz .wait_enter
    mov bl, al
    call read_key
    cmp al, 27
    je .abort
    cmp al, 'a'
    jb .compare
    cmp al, 'z'
    ja .compare
    sub al, 32
.compare:
    cmp al, bl
    jne .bad
    jmp .next_char

.wait_enter:
    call read_key
    cmp al, 13
    je .ok
    cmp al, 27
    je .abort

.bad:
    mov word [fail_code], 0x0702
    mov dx, msg_raw_destroy_bad
    call print_line
    pop si
    pop dx
    pop bx
    pop ax
    stc
    ret

.abort:
    pop si
    pop dx
    pop bx
    pop ax
    stc
    ret

.ok:
    pop si
    pop dx
    pop bx
    pop ax
    clc
    ret

preflight_space:
    call print_crlf
    mov dx, msg_preflight_start
    call print_line

    mov ah, 0x36
    xor dl, dl             ; target is constrained to current/source drive
    int 0x21
    cmp ax, 0xFFFF
    jne .calc
    mov word [fail_code], 0x0201
    mov dx, msg_preflight_error
    call print_line
    stc
    ret

.calc:
    mov [tmp_free_clusters], bx
    mov [tmp_sectors_per_cluster], ax
    mov [tmp_bytes_per_sector], cx

    cmp word [tmp_bytes_per_sector], 512
    jne .fs_invalid

    mov ax, [tmp_sectors_per_cluster]
    cmp ax, 0
    je .fs_invalid
    mov bx, ax
    dec bx
    and bx, ax
    jnz .fs_invalid

    ; cluster_bytes = sectors_per_cluster * bytes_per_sector
    mov ax, [tmp_sectors_per_cluster]
    mul word [tmp_bytes_per_sector]
    or dx, dx
    jnz .api_error
    mov [tmp_cluster_bytes], ax

    ; free_bytes = free_clusters * cluster_bytes
    mov ax, [tmp_free_clusters]
    mul word [tmp_cluster_bytes]
    mov [free_bytes], ax
    mov [free_bytes+2], dx

    call load_required_bytes
    mov [required_bytes], ax
    mov [required_bytes+2], dx

    ; Compare free_bytes >= required_bytes
    mov bx, [free_bytes+2]
    cmp bx, dx
    jb .no_space
    ja .ok
    mov bx, [free_bytes]
    cmp bx, ax
    jb .no_space

.ok:
    mov dx, msg_preflight_ok
    call print_line
    clc
    ret

.fs_invalid:
    mov word [fail_code], 0x0206
    mov dx, msg_preflight_fstype
    call print_line
    stc
    ret

.api_error:
    mov word [fail_code], 0x0201
    mov dx, msg_preflight_error
    call print_line
    stc
    ret

.no_space:
    mov word [fail_code], 0x0202
    mov dx, msg_preflight_nospace
    call print_line
    stc
    ret

prepare_target_fs:
    call print_crlf
    mov dx, msg_format_start
    call print_line

    call cleanup_target_tree

    call create_base_dirs
    jc .fail

    mov dx, msg_marker_format_ok
    call print_line
    clc
    ret

.fail:
    mov dx, msg_format_fail
    call print_line
    stc
    ret

cleanup_target_tree:
    xor si, si
.file_loop:
    cmp si, CLEANUP_FILE_COUNT
    jae .dir_cleanup
    mov bx, si
    shl bx, 1
    mov dx, [cleanup_file_ptrs + bx]
    mov ah, 0x41
    int 0x21
    inc si
    jmp .file_loop

.dir_cleanup:
    ; Keep directory tree in place: current stage2 mkdir/rmdir on absolute roots
    ; is not stable enough for destructive format emulation.
    ret

create_base_dirs:
    mov dx, msg_dirs_start
    call print_line

    mov dx, path_target_root
    call ensure_directory
    jc .root_fail

    mov dx, path_target_system
    call ensure_directory
    jc .system_fail

    mov dx, path_target_apps
    call ensure_directory
    jc .apps_fail

    mov dx, msg_dirs_ok
    call print_line
    clc
    ret

.root_fail:
    mov word [fail_code], 0x0301
    mov dx, msg_dirs_fail
    call print_line
    stc
    ret

.system_fail:
    mov word [fail_code], 0x0302
    mov dx, msg_dirs_fail
    call print_line
    stc
    ret

.apps_fail:
    mov word [fail_code], 0x0303
    mov dx, msg_dirs_fail
    call print_line
    stc
    ret

postformat_sanity:
    mov dx, msg_sanity_start
    call print_line

    mov word [active_handle], 0xFFFF

    mov dx, path_sanity
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .fail
    mov [active_handle], ax
    mov bx, ax

    mov dx, str_ok2
    mov cx, 2
    mov ah, 0x40
    int 0x21
    jc .fail
    cmp ax, 2
    jne .fail

    call close_active_handle

    mov dx, path_sanity
    mov ah, 0x41
    int 0x21

    mov dx, msg_marker_sanity_ok
    call print_line
    clc
    ret

.fail:
    mov word [fail_code], 0x0304
    call close_active_handle
    mov dx, msg_sanity_fail
    call print_line
    stc
    ret

load_payload_manifest:
    push cs
    pop ds

    call print_crlf
    mov dx, msg_manifest_start
    call print_line
    mov byte [manifest_loaded_from_media], 0

    xor si, si
.copy_defaults:
    cmp si, FILE_COUNT
    jae .open
    mov al, [file_min_profile_default + si]
    mov [manifest_min_profile + si], al
    mov al, [file_media_default + si]
    mov [manifest_media_id + si], al
    inc si
    jmp .copy_defaults

.open:
    mov word [active_handle], 0xFFFF
    mov dx, path_manifest_rel
    mov ax, 0x3D00
    int 0x21
    jnc .opened

    mov dx, path_manifest_abs
    mov ax, 0x3D00
    int 0x21
    jc .open_fail

.opened:
    mov [active_handle], ax
    mov bx, ax

    xor cx, cx
    xor dx, dx
    mov ax, 0x4202
    int 0x21
    jc .read_fail
    mov [manifest_dbg_size_lo], ax
    mov [manifest_dbg_size_hi], dx
    xor cx, cx
    xor dx, dx
    mov ax, 0x4200
    int 0x21
    jc .read_fail

    mov dx, manifest_buf
    mov cx, MANIFEST_HEADER_SIZE
    mov ah, 0x3F
    int 0x21
    mov [last_io_error], ax
    jc .read_fail
    cmp ax, MANIFEST_HEADER_SIZE
    jne .read_fail

    cmp byte [manifest_buf + 0], 'S'
    jne .bad_header
    cmp byte [manifest_buf + 1], 'M'
    jne .bad_header
    cmp byte [manifest_buf + 2], 'F'
    jne .bad_header
    cmp byte [manifest_buf + 3], '1'
    jne .bad_header
    cmp byte [manifest_buf + 4], FILE_COUNT
    jne .bad_header

    xor si, si
.rec_loop:
    cmp si, FILE_COUNT
    jae .ok

    mov dx, manifest_buf
    mov cx, MANIFEST_RECORD_SIZE
    mov ah, 0x3F
    int 0x21
    mov [last_io_error], ax
    jc .read_fail
    cmp ax, MANIFEST_RECORD_SIZE
    jne .read_fail

    mov al, [manifest_buf + 0]
    cmp al, 1
    jb .bad_record
    cmp al, 3
    ja .bad_record
    mov [manifest_min_profile + si], al

    mov al, [manifest_buf + 1]
    cmp al, 1
    jb .bad_record
    mov [manifest_media_id + si], al

    inc si
    jmp .rec_loop

.ok:
    call close_active_handle
    mov byte [manifest_loaded_from_media], 1
    mov dx, msg_marker_manifest_ok
    call print_line
    clc
    ret

.open_fail:
    call load_payload_manifest_raw
    jnc .raw_handled
    mov dx, msg_manifest_fallback_open
    call print_line
    clc
    ret

.read_fail:
    call load_payload_manifest_raw
    jnc .raw_handled_close
    mov dx, msg_manifest_fallback_read
    call print_z
    mov dx, msg_manifest_dbg_h
    call print_z
    mov ax, bx
    call print_u8_dec
    mov dx, msg_manifest_dbg_ax
    call print_z
    mov ax, [last_io_error]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call print_z
    mov dx, msg_manifest_dbg_sz
    call print_z
    mov ax, [manifest_dbg_size_lo]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call print_z
    call print_crlf
    call close_active_handle
    clc
    ret

.raw_handled_close:
    call close_active_handle

.raw_handled:
    clc
    ret

.bad_header:
    call close_active_handle
    mov dx, msg_manifest_fallback_header
    call print_line
    clc
    ret

.bad_record:
    call close_active_handle
    mov dx, msg_manifest_fallback_record
    call print_line
    clc
    ret

load_payload_manifest_raw:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    push cs
    pop ds
    push cs
    pop es

    mov ax, RAW_APPS_DIR_LBA
    mov bx, io_buffer
    call raw_read_sector_lba
    jc .fail

    mov si, io_buffer
    mov cx, 16

.scan_entry:
    mov al, [si]
    cmp al, 0
    je .fail
    cmp al, 0xE5
    je .next_entry
    mov al, [si + 11]
    cmp al, 0x0F
    je .next_entry
    test al, 0x08
    jnz .next_entry

    mov di, fat_name_setupmft
    call raw_match_entry_name
    jc .entry_found
    mov di, fat_name_manifst
    call raw_match_entry_name
    jc .entry_found

.next_entry:
    add si, 32
    loop .scan_entry
    jmp .fail

.entry_found:
    mov ax, [si + 28]
    mov [manifest_dbg_size_lo], ax
    mov ax, [si + 30]
    mov [manifest_dbg_size_hi], ax

    mov ax, [si + 26]
    cmp ax, 2
    jb .fail

    sub ax, 2
    shl ax, 1
    shl ax, 1
    shl ax, 1
    add ax, RAW_DATA_LBA
    mov bx, io_buffer
    call raw_read_sector_lba
    jc .fail

    cmp byte [io_buffer + 0], 'S'
    jne .bad_header
    cmp byte [io_buffer + 1], 'M'
    jne .bad_header
    cmp byte [io_buffer + 2], 'F'
    jne .bad_header
    cmp byte [io_buffer + 3], '1'
    jne .bad_header
    cmp byte [io_buffer + 4], FILE_COUNT
    jne .bad_header

    xor si, si
    mov bx, MANIFEST_HEADER_SIZE

.rec_loop:
    cmp si, FILE_COUNT
    jae .ok

    mov al, [io_buffer + bx]
    cmp al, 1
    jb .bad_record
    cmp al, 3
    ja .bad_record
    mov [manifest_min_profile + si], al

    mov al, [io_buffer + bx + 1]
    cmp al, 1
    jb .bad_record
    mov [manifest_media_id + si], al

    add bx, MANIFEST_RECORD_SIZE
    inc si
    jmp .rec_loop

.ok:
    mov byte [manifest_loaded_from_media], 1
    mov dx, msg_marker_manifest_ok
    call print_line
    clc
    jmp .out

.bad_header:
    mov dx, msg_manifest_fallback_header
    call print_line
    clc
    jmp .out

.bad_record:
    mov dx, msg_manifest_fallback_record
    call print_line
    clc
    jmp .out

.fail:
    stc

.out:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

raw_match_entry_name:
    push ax
    push cx
    push si
    push di
    mov cx, 11

.cmp_loop:
    mov al, [si]
    cmp al, [di]
    jne .not_match
    inc si
    inc di
    loop .cmp_loop
    stc
    jmp .done

.not_match:
    clc

.done:
    pop di
    pop si
    pop cx
    pop ax
    ret

raw_read_sector_lba:
    push bx
    push cx
    push dx
    push si
    push es

    mov si, bx

    xor dx, dx
    mov cx, RAW_FAT_SPT
    div cx

    mov cl, dl
    inc cl

    xor dx, dx
    mov bx, RAW_FAT_HEADS
    div bx

    mov ch, al
    mov dh, dl

    push cs
    pop es
    mov bx, si
    mov dl, RAW_BOOT_DRIVE
    mov ah, 0x02
    mov al, 0x01
    int 0x13

    pop es
    pop si
    pop dx
    pop cx
    pop bx
    ret

compute_planned_files:
    xor ax, ax
    xor si, si
    mov bl, [selected_profile]

.count_loop:
    cmp si, FILE_COUNT
    jae .done
    mov dl, [manifest_min_profile + si]
    cmp dl, bl
    ja .skip
    inc ax
.skip:
    inc si
    jmp .count_loop

.done:
    ret

copy_manifest:
    mov word [files_copied], 0
    mov byte [current_media_id], 0
    xor si, si

.loop:
    cmp si, FILE_COUNT
    jae .done

    mov al, [manifest_min_profile + si]
    cmp al, [selected_profile]
    ja .next

    mov al, [manifest_media_id + si]
    mov [expected_media_id], al

    cmp byte [current_media_id], 0
    je .set_media
    cmp al, [current_media_id]
    je .media_ready

    push si
    call media_swap_prompt
    pop si
    jc .swap_fail
    mov al, [expected_media_id]
    mov [current_media_id], al
    inc byte [media_swap_count]
    jmp .media_ready

.set_media:
    mov [current_media_id], al

.media_ready:
    mov bx, si
    shl bx, 1
    mov ax, [file_src_ptrs + bx]
    mov [curr_src], ax
    mov ax, [file_dst_ptrs + bx]
    mov [curr_dst], ax

    push si
    call copy_one_file
    pop si
    jnc .next

.fail_prompt:
    push si
    call copy_failure_prompt
    pop si
    cmp al, 1
    je .retry
    cmp al, 2
    je .back
    stc
    ret

.retry:
    inc byte [retry_count]
    push si
    call copy_one_file
    pop si
    jc .fail_prompt
    jmp .next

.back:
    mov word [fail_code], 0x0602
    stc
    ret

.swap_fail:
    stc
    ret

.next:
    inc si
    jmp .loop

.done:
    clc
    ret

copy_one_file:
    mov word [src_handle], 0xFFFF
    mov word [dst_handle], 0xFFFF

    cmp word [curr_src], 0
    jne .have_src
    mov word [fail_code], 0x0406
    stc
    ret

.have_src:
    cmp word [curr_dst], 0
    jne .show_progress
    mov word [fail_code], 0x0407
    stc
    ret

.show_progress:
    ; Textual progress: Copy n/total: <source>
    mov dx, msg_copy_prefix
    call print_z
    mov ax, [files_copied]
    inc al
    call print_u8_dec
    mov dl, '/'
    call print_char_dl
    mov ax, [files_planned]
    call print_u8_dec
    mov dx, msg_copy_sep
    call print_z
    mov dx, [curr_src]
    call print_z
    call print_crlf

    mov dx, [curr_src]
    mov ax, 0x3D00
    int 0x21
    jc .src_open_fail
    mov [src_handle], ax

    mov dx, [curr_dst]
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .dst_create_fail
    mov [dst_handle], ax

.rw_loop:
    mov bx, [src_handle]
    mov dx, io_buffer
    mov cx, 512
    mov ah, 0x3F
    int 0x21
    jc .read_fail
    or ax, ax
    jz .done
    mov [last_chunk], ax

    mov bx, [dst_handle]
    mov cx, ax
    mov dx, io_buffer
    mov ah, 0x40
    int 0x21
    jc .write_fail
    cmp ax, [last_chunk]
    jne .short_write

    add [bytes_copied], ax
    adc word [bytes_copied+2], 0
    jmp .rw_loop

.done:
    call close_copy_handles
    inc word [files_copied]
    mov dx, msg_marker_copy_ok
    call print_line
    clc
    ret

.src_open_fail:
    mov word [fail_code], 0x0401
    jmp .copy_fail

.dst_create_fail:
    mov word [fail_code], 0x0402
    jmp .copy_fail

.read_fail:
    mov word [fail_code], 0x0403
    jmp .copy_fail

.write_fail:
    mov word [fail_code], 0x0404
    jmp .copy_fail

.short_write:
    mov word [fail_code], 0x0405

.copy_fail:
    call close_copy_handles
    stc
    ret

write_config_file:
    mov dx, msg_cfg_start
    call print_line

    mov word [active_handle], 0xFFFF
    mov dx, path_cfg
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .create_fail
    mov [active_handle], ax
    mov bx, ax

    mov dx, cfg_profile_prefix
    call write_cstr_active
    jc .write_fail
    call get_profile_name_ptr
    call write_cstr_active
    jc .write_fail
    mov dx, str_crlf
    call write_cstr_active
    jc .write_fail
    mov dx, cfg_target_line
    call write_cstr_active
    jc .write_fail

    call close_active_handle
    mov dx, msg_cfg_ok
    call print_line
    clc
    ret

.create_fail:
    mov word [fail_code], 0x0501
    mov dx, msg_cfg_fail
    call print_line
    stc
    ret

.write_fail:
    mov word [fail_code], 0x0502
    call close_active_handle
    mov dx, msg_cfg_fail
    call print_line
    stc
    ret

write_install_report:
    push cs
    pop ds
    push cs
    pop es

    ; Best-effort: create target root if report is requested after early failures.
    mov dx, path_target_root
    call ensure_directory

    push cs
    pop ds
    mov dx, path_report
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .done
    mov [cs:active_handle], ax
    mov bx, ax

    mov dx, rpt_title
    call write_cstr_active

    mov dx, rpt_schema
    call write_cstr_active
    mov dx, rpt_input_media
    call write_cstr_active
    mov dx, rpt_input_target
    call write_cstr_active

    mov dx, rpt_input_profile_prefix
    call write_cstr_active
    call get_profile_name_ptr
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    cmp byte [install_ok], 1
    je .status_ok
    mov dx, rpt_status_fail
    call write_cstr_active
    jmp .status_done

.status_ok:
    mov dx, rpt_status_ok
    call write_cstr_active

.status_done:
    mov dx, rpt_step_prefix
    call write_cstr_active
    xor ax, ax
    mov al, [step_id]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_retry_prefix
    call write_cstr_active
    xor ax, ax
    mov al, [retry_count]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_target_drive_prefix
    call write_cstr_active
    xor ax, ax
    mov al, [target_drive]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_targets_valid_prefix
    call write_cstr_active
    xor ax, ax
    mov al, [valid_target_count]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_media_swaps_prefix
    call write_cstr_active
    xor ax, ax
    mov al, [media_swap_count]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_manifest_source_prefix
    call write_cstr_active
    xor ax, ax
    mov al, [manifest_loaded_from_media]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_manifest_dbg_size_prefix
    call write_cstr_active
    mov ax, [manifest_dbg_size_lo]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_key_total_prefix
    call write_cstr_active
    mov ax, [kb_key_total]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_key_nav_prefix
    call write_cstr_active
    xor ax, ax
    mov al, [kb_nav_count]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_planned_prefix
    call write_cstr_active
    mov ax, [files_planned]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_copied_prefix
    call write_cstr_active
    mov ax, [files_copied]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_bytes_prefix
    call write_cstr_active
    mov ax, [bytes_copied]
    mov dx, [bytes_copied+2]
    mov di, hex_dword_buf
    call format_dword_hex_z
    mov dx, hex_dword_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    mov dx, rpt_fail_prefix
    call write_cstr_active
    mov ax, [fail_code]
    mov di, hex_word_buf
    call format_word_hex_z
    mov dx, hex_word_buf
    call write_cstr_active
    mov dx, str_crlf
    call write_cstr_active

    call close_active_handle

.done:
    ret

; -----------------------------------------------------------------------------
; DOS helpers
; -----------------------------------------------------------------------------

ensure_directory:
    push ax
    push cx
    push dx

    mov ah, 0x39
    int 0x21
    jnc .ok

    ; If mkdir failed, treat existing directory as success.
    pop dx
    push dx
    mov ax, 0x4300
    int 0x21
    jc .fail
    test cx, 0x10
    jz .fail

.ok:
    pop dx
    pop cx
    pop ax
    clc
    ret

.fail:
    pop dx
    pop cx
    pop ax
    stc
    ret

close_copy_handles:
    push ax
    push bx

    mov bx, [dst_handle]
    cmp bx, 0xFFFF
    je .skip_dst
    mov ah, 0x3E
    int 0x21

.skip_dst:
    mov bx, [src_handle]
    cmp bx, 0xFFFF
    je .skip_src
    mov ah, 0x3E
    int 0x21

.skip_src:
    mov word [src_handle], 0xFFFF
    mov word [dst_handle], 0xFFFF
    pop bx
    pop ax
    ret

close_active_handle:
    push ax
    push bx
    mov bx, [active_handle]
    cmp bx, 0xFFFF
    je .done
    mov ah, 0x3E
    int 0x21
    mov word [active_handle], 0xFFFF
.done:
    pop bx
    pop ax
    ret

write_cstr_active:
    push ax
    push bx
    push cx
    push dx
    push si
    push cs
    pop ds

    mov si, dx
    xor cx, cx
.len_loop:
    cmp byte [si], 0
    je .len_done
    inc si
    inc cx
    jmp .len_loop

.len_done:
    mov ah, 0x40
    int 0x21
    jc .fail
    cmp ax, cx
    jne .fail
    clc
    jmp .out

.fail:
    stc

.out:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

read_exact_active:
    push ax
    push bx
    push cx
    push dx

.loop:
    cmp cx, 0
    je .ok

    push cs
    pop ds
    mov bx, [active_handle]
    mov ah, 0x3F
    int 0x21
    jc .fail
    or ax, ax
    jz .fail

    add dx, ax
    sub cx, ax
    jmp .loop

.ok:
    mov word [last_io_error], 0
    clc
    jmp .out

.fail:
    or ax, ax
    jne .have_err
    mov ax, 0x0012
.have_err:
    mov [last_io_error], ax
    stc

.out:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

load_required_bytes:
    mov al, [selected_profile]
    dec al
    xor ah, ah
    shl ax, 1
    mov si, ax
    mov ax, [required_lo_table + si]
    mov dx, [required_hi_table + si]
    ret

get_profile_name_ptr:
    mov al, [selected_profile]
    dec al
    xor ah, ah
    shl ax, 1
    mov si, profile_name_ptrs
    add si, ax
    mov dx, [si]
    ret

guard_profile_selection:
    mov al, [selected_profile]
    cmp al, 1
    jb .bad
    cmp al, 3
    ja .bad
    clc
    ret

.bad:
    mov word [fail_code], 0x0102
    stc
    ret

media_swap_prompt:
    mov dx, msg_media_swap
    call print_z
    xor ax, ax
    mov al, [expected_media_id]
    call print_u8_dec
    mov dx, msg_media_swap_suffix
    call print_line

.wait:
    mov bx, PROMPT_TIMEOUT_TICKS
    call wait_key_timeout
    jc .timeout

    cmp al, 13
    je .ok
    cmp al, 'r'
    je .ok
    cmp al, 'R'
    je .ok
    cmp al, 27
    je .cancel
    jmp .wait

.ok:
    clc
    ret

.cancel:
    mov word [fail_code], 0x0601
    stc
    ret

.timeout:
    mov word [fail_code], 0x0603
    mov dx, msg_prompt_timeout
    call print_line
    stc
    ret

copy_failure_prompt:
    mov dx, msg_copy_fail_prompt
    call print_line

.wait:
    mov bx, PROMPT_TIMEOUT_TICKS
    call wait_key_timeout
    jc .timeout

    cmp al, 13
    je .retry
    cmp al, 'r'
    je .retry
    cmp al, 'R'
    je .retry
    cmp al, 'b'
    je .back
    cmp al, 'B'
    je .back
    cmp al, 27
    je .cancel
    jmp .wait

.retry:
    mov al, 1
    ret

.back:
    cmp word [files_copied], 0
    je .back_ok
    mov dx, msg_copy_back_denied
    call print_line
    jmp .wait

.back_ok:
    mov al, 2
    ret

.cancel:
    mov word [fail_code], 0x0601
    mov al, 3
    ret

.timeout:
    mov word [fail_code], 0x0603
    mov dx, msg_prompt_timeout
    call print_line
    mov al, 3
    ret

print_profile_name:
    call get_profile_name_ptr
    call print_z
    ret

print_target_drive:
    push ax
    push dx
    xor ax, ax
    mov al, [target_drive]
    add al, 'A'
    mov dl, al
    call print_char_dl
    mov dl, ':'
    call print_char_dl
    pop dx
    pop ax
    ret

key_to_drive_index:
    cmp al, 'a'
    jb .check_upper
    cmp al, 'z'
    ja .check_upper
    sub al, 32

.check_upper:
    cmp al, 'A'
    jb .bad
    cmp al, 'Z'
    ja .bad
    sub al, 'A'
    clc
    ret

.bad:
    stc
    ret

wait_enter_or_esc:
.loop:
    call read_key
    cmp al, 13
    je .ok
    cmp al, 27
    je .esc
    jmp .loop

.ok:
    clc
    ret

.esc:
    stc
    ret

track_key_stats:
    inc word [kb_key_total]
    cmp al, 0xC8
    je .nav
    cmp al, 0xD0
    jne .done
.nav:
    inc byte [kb_nav_count]
.done:
    ret

read_key:
    mov ah, 0x08
    int 0x21
    cmp al, 0
    jne .track
    mov ah, 0x08
    int 0x21
    or al, 0x80

.track:
    call track_key_stats
    ret

wait_key_timeout:
    mov ah, 0x00
    int 0x1A
    mov [prompt_tick_start], dx

.poll:
    mov ah, 0x01
    int 0x16
    jnz .have_key

    mov ah, 0x00
    int 0x1A
    mov ax, dx
    sub ax, [prompt_tick_start]
    cmp ax, bx
    jb .poll

    stc
    ret

.have_key:
    mov ah, 0x00
    int 0x16
    cmp al, 0
    jne .track
    mov al, ah
    or al, 0x80

.track:
    call track_key_stats
    clc
    ret

print_u8_dec:
    push ax
    push bx
    push cx
    push dx

    xor ah, ah
    mov bl, 100
    div bl                  ; AL=hundreds, AH=rem
    mov ch, al
    mov al, ah
    xor ah, ah
    mov bl, 10
    div bl                  ; AL=tens, AH=ones
    mov cl, al
    mov bl, ah

    cmp ch, 0
    je .skip_h
    mov dl, ch
    add dl, '0'
    call print_char_dl

.skip_h:
    cmp ch, 0
    jne .print_t
    cmp cl, 0
    je .print_o

.print_t:
    mov dl, cl
    add dl, '0'
    call print_char_dl

.print_o:
    mov dl, bl
    add dl, '0'
    call print_char_dl

    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_char_dl:
    push ax
    mov ah, 0x02
    int 0x21
    pop ax
    ret

print_crlf:
    push dx
    mov dl, 13
    call print_char_dl
    mov dl, 10
    call print_char_dl
    pop dx
    ret

print_line:
    call print_z
    call print_crlf
    ret

print_z:
    push ax
    push dx
    push si
    mov si, dx

.loop:
    lodsb
    or al, al
    jz .done
    mov dl, al
    mov ah, 0x02
    int 0x21
    jmp .loop

.done:
    pop si
    pop dx
    pop ax
    ret

format_word_hex_z:
    call format_word_hex4
    mov byte [di], 0
    ret

format_dword_hex_z:
    push bx
    mov bx, ax
    mov ax, dx
    call format_word_hex4
    mov ax, bx
    call format_word_hex4
    mov byte [di], 0
    pop bx
    ret

format_word_hex4:
    push ax
    push bx
    push cx

    mov bx, ax
    mov cx, 4
.nibble_loop:
    rol bx, 4
    mov al, bl
    and al, 0x0F
    call nibble_to_hex
    stosb
    loop .nibble_loop

    pop cx
    pop bx
    pop ax
    ret

nibble_to_hex:
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    ret

; -----------------------------------------------------------------------------
; Strings / paths / manifest
; -----------------------------------------------------------------------------

msg_welcome_1        db 'CiukiOS Setup MVP', 0
msg_welcome_2        db 'FULL-only installer stream', 0
msg_welcome_3        db 'Enter continue, Esc cancel', 0
msg_welcome_4        db 'Default target: \CIUKIOS', 0
msg_enter_esc        db 'Press Enter to continue or Esc to abort.', 0

msg_profile_1        db 'Select install profile:', 0
msg_profile_2        db '1 - Minimal', 0
msg_profile_3        db '2 - Standard', 0
msg_profile_4        db '3 - Full', 0
msg_profile_prompt   db 'Choose 1/2/3 (Esc abort).', 0
msg_profile_selected db 'Profile selected: ', 0

msg_target_scan_start db 'Scanning install targets...', 0
msg_target_scan_fail db 'No valid target drive found.', 0
msg_target_1         db 'Target confirmation', 0
msg_target_drive_prefix db 'Target drive: ', 0
msg_target_2         db 'Install path: ', 0
msg_target_prompt    db 'Enter confirm / Esc cancel / A-Z set drive.', 0
msg_target_selected  db 'Target set to ', 0
msg_target_invalid   db 'Invalid target drive.', 0
msg_target_unsupported db 'Only source drive is supported.', 0
msg_disk_panel_header db 'Connected disk map:', 0
msg_disk_live_d db 'D: Live media BIOS 80h - ', 0
msg_disk_target_c db 'C: Install target BIOS 81h - ', 0
msg_disk_bios80 db 'BIOS 80h - ', 0
msg_disk_bios81 db 'BIOS 81h - ', 0
msg_disk_present db 'present ', 0
msg_disk_absent db 'absent', 0
msg_disk_blank db 'blank ', 0
msg_disk_data db 'data ', 0
msg_disk_mbr db 'mbr', 0
msg_disk_no_mbr db 'no-mbr', 0
msg_raw_destroy_1 db 'DESTRUCTIVE HDD INSTALL ENABLED.', 0
msg_raw_destroy_2 db 'Target BIOS 81h will be overwritten.', 0
msg_raw_destroy_3 db 'Type DESTROY then Enter to continue.', 0
msg_raw_destroy_bad db 'Destroy confirmation mismatch.', 0

msg_preflight_start  db 'Preflight: checking free space...', 0
msg_preflight_ok     db 'Preflight OK.', 0
msg_preflight_error  db 'Preflight failed: INT21h AH=36 unavailable.', 0
msg_preflight_nospace db 'Preflight failed: not enough free space.', 0
msg_preflight_fstype db 'Preflight failed: FAT16 geometry unsupported.', 0

msg_format_start     db 'FAT16 prepare/format step...', 0
msg_format_fail      db 'FAT16 prepare failed.', 0
msg_dirs_start       db 'Creating target directories...', 0
msg_dirs_ok          db 'Directory layout ready.', 0
msg_dirs_fail        db 'Directory creation failed.', 0
msg_sanity_start     db 'Post-format sanity check...', 0
msg_sanity_fail      db 'Post-format sanity failed.', 0

msg_manifest_start   db 'Loading payload manifest...', 0
msg_manifest_fail    db 'Manifest parse failed.', 0
msg_manifest_fallback db 'Manifest media read unavailable: using built-in defaults.', 0
msg_manifest_fallback_open db 'Manifest fallback: open failed.', 0
msg_manifest_fallback_read db 'Manifest fallback: read failed.', 0
msg_manifest_dbg_h db ' H=', 0
msg_manifest_dbg_ax db ' AX=', 0
msg_manifest_dbg_sz db ' SZ=', 0
msg_manifest_fallback_header db 'Manifest fallback: invalid header.', 0
msg_manifest_fallback_record db 'Manifest fallback: invalid record.', 0

msg_media_swap       db 'Insert media ', 0
msg_media_swap_suffix db ' then Enter (Esc cancel).', 0
msg_prompt_timeout   db 'Prompt timeout: setup canceled safely.', 0

msg_copy_prefix      db 'Copy ', 0
msg_copy_sep         db ': ', 0
msg_copy_fail_prompt db 'Copy fail: R retry, B back, Esc cancel.', 0
msg_copy_back_denied db 'Back disabled after writes.', 0

msg_cfg_start        db 'Generating config...', 0
msg_cfg_ok           db 'Config generated.', 0
msg_cfg_fail         db 'Config generation failed.', 0

msg_success          db 'Installation completed.', 0
msg_failed           db 'Installation failed or aborted.', 0

msg_marker_target_scan db 'TARGET_SCAN_OK', 0
msg_marker_manifest_ok db 'MANIFEST_OK', 0
msg_marker_format_ok db 'FORMAT_OK', 0
msg_marker_sanity_ok db 'SANITY_OK', 0
msg_marker_start     db 'START', 0
msg_marker_copy_ok   db 'COPY_OK', 0
msg_marker_done      db 'DONE', 0
msg_marker_fail      db 'FAIL', 0
msg_serial_bios_probe db '[SETUP-HDD-PROBE] P=', 0
msg_serial_probe_blank db ' B=', 0
msg_serial_probe_sig db ' S=', 0
msg_serial_hdd_install_start db '[SETUP-HDD-INSTALL] START', 0
msg_serial_hdd_install_done db '[SETUP-HDD-INSTALL] DONE', 0
msg_serial_hdd_install_fail db '[SETUP-HDD-INSTALL] FAIL S=', 0
msg_serial_hdd_install_path db ' P=', 0
msg_serial_hdd_install_lba db ' L=', 0
msg_serial_hdd_install_status db ' AH=', 0
msg_serial_hdd_install_edd db ' E=', 0
msg_serial_hdd_install_chs db ' C=', 0

str_crlf             db 13, 10, 0
str_ok2              db 'OK', 0
str_destroy_confirm db 'DESTROY', 0

name_min             db 'MINIMAL', 0
name_std             db 'STANDARD', 0
name_full            db 'FULL', 0

profile_name_ptrs    dw name_min, name_std, name_full

required_lo_table    dw 0x6000, 0x8000, 0x0000
required_hi_table    dw 0x0000, 0x0001, 0x0003

path_target_root     db '\CIUKIOS', 0
path_target_system   db '\CIUKIOS\SYSTEM', 0
path_target_apps     db '\CIUKIOS\APPS', 0
path_cfg             db '\CIUKIOS\CIUKIOS.CFG', 0
path_report          db '\CIUKIOS\INSTALL.RPT', 0
path_sanity          db '\CIUKIOS\FMT.CHK', 0
path_manifest_rel    db 'SETUPMFT.BIN', 0
path_manifest_abs    db '\APPS\SETUPMFT.BIN', 0
fat_name_setupmft   db 'SETUPMFTBIN'
fat_name_manifst    db 'MANIFST BIN'

cfg_profile_prefix   db 'PROFILE=', 0
cfg_target_line      db 'TARGET=\CIUKIOS', 13, 10, 0

rpt_title            db 'CIUKIOS INSTALL REPORT', 13, 10, 0
rpt_schema           db 'REPORT_SCHEMA=SETUP_MVP_V2', 13, 10, 0
rpt_input_media      db 'INPUT_MEDIA=FULL_FAT16', 13, 10, 0
rpt_input_target     db 'INPUT_TARGET=\CIUKIOS', 13, 10, 0
rpt_input_profile_prefix db 'INPUT_PROFILE=', 0
rpt_status_ok        db 'STATUS=OK', 13, 10, 0
rpt_status_fail      db 'STATUS=FAIL', 13, 10, 0
rpt_step_prefix      db 'STEP_HEX=', 0
rpt_retry_prefix     db 'RETRY_COUNT_HEX=', 0
rpt_target_drive_prefix db 'TARGET_DRIVE_HEX=', 0
rpt_targets_valid_prefix db 'TARGETS_VALID_HEX=', 0
rpt_media_swaps_prefix db 'MEDIA_SWAPS_HEX=', 0
rpt_manifest_source_prefix db 'MANIFEST_MEDIA_HEX=', 0
rpt_manifest_dbg_size_prefix db 'MANIFEST_DBG_SIZE_HEX=', 0
rpt_key_total_prefix db 'KB_KEYS_HEX=', 0
rpt_key_nav_prefix   db 'KB_NAV_HEX=', 0
rpt_planned_prefix   db 'FILES_PLANNED_HEX=', 0
rpt_copied_prefix    db 'FILES_COPIED_HEX=', 0
rpt_bytes_prefix     db 'BYTES_COPIED_HEX=', 0
rpt_fail_prefix      db 'FAIL_CODE_HEX=', 0

src_stage2           db '\SYSTEM\STAGE2.BIN', 0
src_comdemo          db '\APPS\COMDEMO.COM', 0
src_splash           db '\SYSTEM\SPLASH.BIN', 0
src_ciukedit         db '\APPS\CIUKEDIT.COM', 0
src_fileio           db '\APPS\FILEIO.BIN', 0
src_mzdemo           db '\APPS\MZDEMO.EXE', 0
src_deltest          db '\APPS\DELTEST.BIN', 0
src_gfxrect          db '\APPS\GFXRECT.COM', 0
src_gfxstar          db '\APPS\GFXSTAR.COM', 0

dst_stage2           db '\CIUKIOS\SYSTEM\STAGE2.BIN', 0
dst_comdemo          db '\CIUKIOS\APPS\COMDEMO.COM', 0
dst_splash           db '\CIUKIOS\SYSTEM\SPLASH.BIN', 0
dst_ciukedit         db '\CIUKIOS\APPS\CIUKEDIT.COM', 0
dst_fileio           db '\CIUKIOS\APPS\FILEIO.BIN', 0
dst_mzdemo           db '\CIUKIOS\APPS\MZDEMO.EXE', 0
dst_deltest          db '\CIUKIOS\APPS\DELTEST.BIN', 0
dst_gfxrect          db '\CIUKIOS\APPS\GFXRECT.COM', 0
dst_gfxstar          db '\CIUKIOS\APPS\GFXSTAR.COM', 0

file_src_ptrs        dw src_stage2, src_comdemo, src_splash, src_ciukedit, src_fileio, src_mzdemo, src_deltest, src_gfxrect, src_gfxstar
file_dst_ptrs        dw dst_stage2, dst_comdemo, dst_splash, dst_ciukedit, dst_fileio, dst_mzdemo, dst_deltest, dst_gfxrect, dst_gfxstar

file_min_profile_default db 1, 1, 2, 2, 2, 3, 3, 3, 3
file_media_default   db 1, 1, 1, 1, 1, 1, 1, 1, 1

cleanup_file_ptrs    dw path_cfg, path_report, path_sanity, dst_stage2, dst_comdemo, dst_splash, dst_ciukedit, dst_fileio, dst_mzdemo, dst_deltest, dst_gfxrect, dst_gfxstar

; -----------------------------------------------------------------------------
; State
; -----------------------------------------------------------------------------

selected_profile        db 1
install_ok              db 0
fail_code               dw 0
step_id                 db 0
retry_count             db 0
media_swap_count        db 0
current_media_id        db 0
expected_media_id       db 0
manifest_loaded_from_media db 0
source_drive            db 0
target_drive            db 0
valid_target_count      db 0
bios_probe_present_mask db 0
bios_probe_blank_mask   db 0
bios_probe_mbrsig_mask  db 0
raw_hdd_install_mode    db 0
bios_probe_drive        db 0
bios_probe_bit          db 0
raw_clone_lba_lo        dw 0
raw_clone_lba_hi        dw 0
raw_clone_remaining_lo  dw 0
raw_clone_remaining_hi  dw 0
raw_chs_cylinder        dw 0
raw_chs_drive           db 0
raw_chs_op              db 0
raw_source_spt          dw RAW_FAT_SPT
raw_source_heads        dw RAW_FAT_HEADS
raw_source_spc          dw RAW_HDD_SECTORS_PER_CYL
raw_target_spt          dw RAW_FAT_SPT
raw_target_heads        dw RAW_FAT_HEADS
raw_target_spc          dw RAW_HDD_SECTORS_PER_CYL
raw_chs_spt             dw RAW_FAT_SPT
raw_chs_spc             dw RAW_HDD_SECTORS_PER_CYL
raw_last_stage          db 0
raw_last_path           db 0
raw_last_status         db 0
raw_edd_status          db 0
raw_chs_status          db 0
prompt_tick_start       dw 0
kb_key_total            dw 0
kb_nav_count            db 0

files_planned           dw 0
files_copied            dw 0
bytes_copied            dd 0

required_bytes          dd 0
free_bytes              dd 0

tmp_free_clusters       dw 0
tmp_sectors_per_cluster dw 0
tmp_bytes_per_sector    dw 0
tmp_cluster_bytes       dw 0
last_io_error           dw 0

src_handle              dw 0xFFFF
dst_handle              dw 0xFFFF
active_handle           dw 0xFFFF
last_chunk              dw 0

curr_src                dw 0
curr_dst                dw 0

manifest_buf            times MANIFEST_HEADER_SIZE db 0
manifest_min_profile    times FILE_COUNT db 0
manifest_media_id       times FILE_COUNT db 0
manifest_dbg_size_lo    dw 0
manifest_dbg_size_hi    dw 0

hex_word_buf            times 5 db 0
hex_dword_buf           times 9 db 0
bios_probe_dap         db 0x10, 0x00
                       dw 0x0001
                       dw 0x0000
                       dw 0x0000
                       dq 0x0000000000000000

io_buffer               times 512 db 0
