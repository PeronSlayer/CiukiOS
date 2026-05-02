; setup.asm - CiukiOS SETUP.COM MVP (FULL-only stream)
; Text-mode keyboard wizard + minimal install pipeline for DOS COM runtime.

bits 16
org 0x0100

%define FILE_COUNT 9

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

    call show_welcome
    jc user_abort

    call choose_profile
    jc user_abort

    call confirm_target
    jc user_abort

    call compute_planned_files
    mov [files_planned], ax
    cmp ax, 0
    jne .have_plan
    mov word [fail_code], 0x0008
    jmp install_fail

.have_plan:
    mov dx, msg_marker_start
    call print_line

    call preflight_space
    jc install_fail

    call create_base_dirs
    jc install_fail

    call copy_manifest
    jc install_fail

    call write_config_file
    jc install_fail

    mov byte [install_ok], 1
    mov dx, msg_success
    call print_line
    mov dx, msg_marker_done
    call print_line
    jmp finalize

user_abort:
    mov word [fail_code], 0x0001

install_fail:
    mov byte [install_ok], 0
    mov dx, msg_failed
    call print_line
    mov dx, msg_marker_fail
    call print_line

finalize:
    call write_install_report
    mov ax, 0x4C00
    cmp byte [install_ok], 1
    je .exit
    mov ax, 0x4C01
.exit:
    int 0x21

; -----------------------------------------------------------------------------
; Wizard screens
; -----------------------------------------------------------------------------

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

.abort:
    stc
    ret

confirm_target:
    call print_crlf
    mov dx, msg_target_1
    call print_line
    mov dx, msg_target_2
    call print_z
    mov dx, path_target_root
    call print_z
    call print_crlf
    mov dx, msg_target_prompt
    call print_line
    call wait_enter_or_esc
    ret

; -----------------------------------------------------------------------------
; Install pipeline
; -----------------------------------------------------------------------------

preflight_space:
    call print_crlf
    mov dx, msg_preflight_start
    call print_line

    mov ah, 0x36
    xor dl, dl              ; current/default drive
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

    ; Compare free_bytes (free_hi:free_lo) >= required_bytes (req_hi:req_lo)
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

compute_planned_files:
    xor ax, ax
    xor si, si
    mov bl, [selected_profile]

.count_loop:
    cmp si, FILE_COUNT
    jae .done
    mov dl, [file_min_profile + si]
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
    xor si, si

.loop:
    cmp si, FILE_COUNT
    jae .done

    mov al, [file_min_profile + si]
    cmp al, [selected_profile]
    ja .next

    mov bx, si
    shl bx, 1
    mov ax, [file_src_ptrs + bx]
    mov [curr_src], ax
    mov ax, [file_dst_ptrs + bx]
    mov [curr_dst], ax

    call copy_one_file
    jc .fail

.next:
    inc si
    jmp .loop

.done:
    clc
    ret

.fail:
    stc
    ret

copy_one_file:
    mov word [src_handle], 0xFFFF
    mov word [dst_handle], 0xFFFF

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
    mov word [active_handle], 0xFFFF

    ; Best-effort: create target root if report is requested after early failures.
    mov dx, path_target_root
    call ensure_directory

    mov dx, path_report
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .done
    mov [active_handle], ax

    mov dx, rpt_title
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
    mov dx, rpt_profile_prefix
    call write_cstr_active
    call get_profile_name_ptr
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

    mov si, dx
    xor cx, cx
.len_loop:
    cmp byte [si], 0
    je .len_done
    inc si
    inc cx
    jmp .len_loop

.len_done:
    mov bx, [active_handle]
    cmp bx, 0xFFFF
    je .fail
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

print_profile_name:
    call get_profile_name_ptr
    call print_z
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

read_key:
    mov ah, 0x08
    int 0x21
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

msg_welcome_1       db 'CiukiOS Setup MVP', 0
msg_welcome_2       db 'FULL-only installer stream', 0
msg_welcome_3       db 'Keyboard only: Enter to continue, Esc to cancel', 0
msg_welcome_4       db 'Default target: \CIUKIOS', 0
msg_enter_esc       db 'Press Enter to continue or Esc to abort.', 0

msg_profile_1       db 'Select install profile:', 0
msg_profile_2       db '1 - Minimal', 0
msg_profile_3       db '2 - Standard', 0
msg_profile_4       db '3 - Full', 0
msg_profile_prompt  db 'Choose 1/2/3 (Esc abort).', 0
msg_profile_selected db 'Profile selected: ', 0

msg_target_1        db 'Target confirmation', 0
msg_target_2        db 'Install path: ', 0
msg_target_prompt   db 'Enter confirm / Esc cancel.', 0

msg_preflight_start db 'Preflight: checking free space...', 0
msg_preflight_ok    db 'Preflight OK.', 0
msg_preflight_error db 'Preflight failed: INT21h AH=36 unavailable.', 0
msg_preflight_nospace db 'Preflight failed: not enough free space.', 0

msg_dirs_start      db 'Creating target directories...', 0
msg_dirs_ok         db 'Directory layout ready.', 0
msg_dirs_fail       db 'Directory creation failed.', 0

msg_copy_prefix     db 'Copy ', 0
msg_copy_sep        db ': ', 0

msg_cfg_start       db 'Generating config...', 0
msg_cfg_ok          db 'Config generated.', 0
msg_cfg_fail        db 'Config generation failed.', 0

msg_success         db 'Installation completed.', 0
msg_failed          db 'Installation failed or aborted.', 0

msg_marker_start    db 'START', 0
msg_marker_copy_ok  db 'COPY_OK', 0
msg_marker_done     db 'DONE', 0
msg_marker_fail     db 'FAIL', 0

str_crlf            db 13, 10, 0

name_min            db 'MINIMAL', 0
name_std            db 'STANDARD', 0
name_full           db 'FULL', 0

profile_name_ptrs   dw name_min, name_std, name_full

required_lo_table   dw 0x6000, 0x8000, 0x0000
required_hi_table   dw 0x0000, 0x0001, 0x0003

path_target_root    db '\CIUKIOS', 0
path_target_system  db '\CIUKIOS\SYSTEM', 0
path_target_apps    db '\CIUKIOS\APPS', 0
path_cfg            db '\CIUKIOS\CIUKIOS.CFG', 0
path_report         db '\CIUKIOS\INSTALL.RPT', 0

cfg_profile_prefix  db 'PROFILE=', 0
cfg_target_line     db 'TARGET=\CIUKIOS', 13, 10, 0

rpt_title           db 'CIUKIOS INSTALL REPORT', 13, 10, 0
rpt_status_ok       db 'STATUS=OK', 13, 10, 0
rpt_status_fail     db 'STATUS=FAIL', 13, 10, 0
rpt_profile_prefix  db 'PROFILE=', 0
rpt_planned_prefix  db 'FILES_PLANNED_HEX=', 0
rpt_copied_prefix   db 'FILES_COPIED_HEX=', 0
rpt_bytes_prefix    db 'BYTES_COPIED_HEX=', 0
rpt_fail_prefix     db 'FAIL_CODE_HEX=', 0

src_stage2          db '\SYSTEM\STAGE2.BIN', 0
src_comdemo         db '\APPS\COMDEMO.COM', 0
src_splash          db '\SYSTEM\SPLASH.BIN', 0
src_ciukedit        db '\APPS\CIUKEDIT.COM', 0
src_fileio          db '\APPS\FILEIO.BIN', 0
src_mzdemo          db '\APPS\MZDEMO.EXE', 0
src_deltest         db '\APPS\DELTEST.BIN', 0
src_gfxrect         db '\APPS\GFXRECT.COM', 0
src_gfxstar         db '\APPS\GFXSTAR.COM', 0

dst_stage2          db '\CIUKIOS\SYSTEM\STAGE2.BIN', 0
dst_comdemo         db '\CIUKIOS\APPS\COMDEMO.COM', 0
dst_splash          db '\CIUKIOS\SYSTEM\SPLASH.BIN', 0
dst_ciukedit        db '\CIUKIOS\APPS\CIUKEDIT.COM', 0
dst_fileio          db '\CIUKIOS\APPS\FILEIO.BIN', 0
dst_mzdemo          db '\CIUKIOS\APPS\MZDEMO.EXE', 0
dst_deltest         db '\CIUKIOS\APPS\DELTEST.BIN', 0
dst_gfxrect         db '\CIUKIOS\APPS\GFXRECT.COM', 0
dst_gfxstar         db '\CIUKIOS\APPS\GFXSTAR.COM', 0

file_src_ptrs       dw src_stage2, src_comdemo, src_splash, src_ciukedit, src_fileio, src_mzdemo, src_deltest, src_gfxrect, src_gfxstar
file_dst_ptrs       dw dst_stage2, dst_comdemo, dst_splash, dst_ciukedit, dst_fileio, dst_mzdemo, dst_deltest, dst_gfxrect, dst_gfxstar
file_min_profile    db 1, 1, 2, 2, 2, 3, 3, 3, 3

; -----------------------------------------------------------------------------
; State
; -----------------------------------------------------------------------------

selected_profile        db 1
install_ok              db 0
fail_code               dw 0

files_planned           dw 0
files_copied            dw 0
bytes_copied            dd 0

required_bytes          dd 0
free_bytes              dd 0

tmp_free_clusters       dw 0
tmp_sectors_per_cluster dw 0
tmp_bytes_per_sector    dw 0
tmp_cluster_bytes       dw 0

src_handle              dw 0xFFFF
dst_handle              dw 0xFFFF
active_handle           dw 0xFFFF
last_chunk              dw 0

curr_src                dw 0
curr_dst                dw 0

hex_word_buf            times 5 db 0
hex_dword_buf           times 9 db 0

io_buffer               times 512 db 0