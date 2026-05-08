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
%define RAW_HDD_BATCH_SECTORS    8           ; multi-sector batch size; matches io_buffer 4 KB
%define RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA 64
%define RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF 0x0136
%define RAW_STAGE1_LIVE_DRIVE_INDEX 3
%define RAW_STAGE1_INSTALLED_DRIVE_INDEX 2
%define RAW_HDD_SECTORS_PER_CYL 1008

; Direct ATA port I/O — used for all target HDD writes to avoid BIOS INT 13h
; wedge on the ThinkPad T23 (and similar hardware) where the BIOS write
; handler sometimes never returns after many sequential calls.
%define ATA_PRI_DATA    0x1F0   ; 16-bit data register
%define ATA_PRI_NSECT   0x1F2   ; sector count
%define ATA_PRI_LBAL    0x1F3   ; LBA bits [7:0]
%define ATA_PRI_LBAM    0x1F4   ; LBA bits [15:8]
%define ATA_PRI_LBAH    0x1F5   ; LBA bits [23:16]
%define ATA_PRI_DEV     0x1F6   ; device/head (bit6=LBA, bit4=slave)
%define ATA_PRI_STATUS  0x1F7   ; status (read) / command (write)
%define ATA_PRI_CTRL    0x3F6   ; alt-status / device control
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

%if SETUP_LIVE_CD_MODE
    call visual_main_loop
    jc user_abort
    ; visual flow has prepared selected_profile/target_drive and confirmed destroy.
    mov byte [step_id], 0x14
    call guard_target_selection
    jc install_fail
    jmp .live_cd_install_path
%else
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
%endif

.live_cd_install_path:

    cmp byte [raw_hdd_install_mode], 1
    jne .int21_install
    call confirm_raw_hdd_destroy
    jc install_fail
%if SETUP_LIVE_CD_MODE
    call vis_install_screen_init
    call vis_install_phase_clone
%endif
    mov byte [step_id], 0x40
    call raw_hdd_clone_install
    jc install_fail
    mov byte [step_id], 0x30
%if SETUP_LIVE_CD_MODE
    call vis_install_phase_done
%endif
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
    cmp byte [install_ok], 1
    je .do_reboot
%if SETUP_LIVE_CD_MODE
    ; In live-CD mode, never return to the shell on failure — the BIOS may be
    ; in a degraded state after INT 13h errors. Show the error and reboot so
    ; the user can try again from a clean BIOS state.
    call vis_install_fail_prompt
%endif
    mov ax, 0x4C01
    int 0x21
.do_reboot:
%if SETUP_LIVE_CD_MODE
    ; Live-CD install: prompt the user to remove the CD before reboot, so
    ; the BIOS picks the freshly-installed HDD as the next boot device
    ; instead of looping back into the live CD.
    call vis_install_eject_prompt
%endif
    call reboot_system
    ; fallback if reboot fails
    mov ax, 0x4C00
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
    ; Both BIOS HDDs must be present (0x80=CD source, 0x81=target HDD).
    cmp byte [bios_probe_present_mask], 0x03
    jne .fail
    ; The CD source (bit 0) must have a valid MBR signature so we know
    ; the clone source is bootable. The target HDD (bit 1) may or may
    ; not have a signature — after a previous install attempt it will,
    ; and the user has explicitly confirmed destruction.
    test byte [bios_probe_mbrsig_mask], 0x01
    jz .fail
%if SETUP_ENABLE_RAW_HDD_DESTRUCTIVE
    ; Destructive mode: user has confirmed wipe — don't gate on target
    ; being blank. This is what the live-CD installer always uses.
    clc
    ret
%else
    ; Non-destructive: target must be blank to avoid clobbering data.
    cmp byte [bios_probe_blank_mask], 0x02
    jne .fail
    clc
    ret
%endif
.fail:
    stc
    ret

; format_target_hdd: write a minimal valid FAT16 structure to the target HDD.
;
; Layout written (absolute disk LBAs):
;   LBA  0      : MBR (partition table, one FAT16B entry at LBA 63)
;   LBA  63     : FAT16 VBR / BPB
;   LBA  64-66  : Reserved sectors 2-4 (zeros)
;   LBA  67-194 : FAT1 (128 sectors; sector 0 has media-byte entry, rest zeros)
;   LBA 195-322 : FAT2 (identical to FAT1)
;   LBA 323-354 : Root directory (32 sectors of zeros)
;
; Total I/O: ~294 sectors vs 262144 for zero-fill — ~900× reduction.
;
format_target_hdd:
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

    call serial_init_com1
    mov dx, msg_serial_hdd_format_start
    call serial_write_z
    call serial_write_crlf

    ; Hard reset target drive before any I/O to clear BIOS state.
    xor ax, ax
    mov dl, RAW_HDD_TARGET_DRIVE
    int 0x13

    call raw_init_drive_geometries

    ; ---------------------------------------------------------------
    ; Prepare zero-filled io_buffer (used as the base for all writes).
    ; ---------------------------------------------------------------
    xor ax, ax
    mov di, io_buffer
    mov cx, 2048            ; 4096 bytes / 2
    rep stosw

    ; ---------------------------------------------------------------
    ; Step 1: Write MBR at LBA 0
    ; ---------------------------------------------------------------
    ; Partition table entry at offset 446 (16 bytes):
    ;   +0  status      = 0x80 (active)
    ;   +1  CHS_start   = {0x01,0x01,0x00}  (H=1 S=1 C=0 → LBA 63)
    ;   +4  type        = 0x06 (FAT16B, >32 MB)
    ;   +5  CHS_end     = {0xFE,0xFF,0xFF}  (saturated for large disk)
    ;   +8  LBA_start   = 63  (LE32)
    ;   +12 LBA_size    = 262144 (LE32)
    mov byte [io_buffer + 446], 0x80
    mov byte [io_buffer + 447], 0x01
    mov byte [io_buffer + 448], 0x01
    mov byte [io_buffer + 449], 0x00
    mov byte [io_buffer + 450], 0x06
    mov byte [io_buffer + 451], 0xFE
    mov byte [io_buffer + 452], 0xFF
    mov byte [io_buffer + 453], 0xFF
    mov word [io_buffer + 454], 63      ; LBA_start low word
    mov word [io_buffer + 456], 0       ; LBA_start high word
    mov word [io_buffer + 458], 0x0000  ; LBA_size = 0x00040000
    mov word [io_buffer + 460], 0x0004
    mov byte [io_buffer + 510], 0x55
    mov byte [io_buffer + 511], 0xAA

    mov word [raw_clone_lba_lo], 0
    mov word [raw_clone_lba_hi], 0
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    mov cx, 1
    call raw_ata_write_n
    jc .format_fail

%if SETUP_LIVE_CD_MODE
    mov al, 10
    call vis_format_phase_update
%endif

    ; ---------------------------------------------------------------
    ; Step 2: Write VBR (FAT16 BPB) at LBA 63
    ; ---------------------------------------------------------------
    ; Zero io_buffer first, then fill BPB fields in place.
    xor ax, ax
    mov di, io_buffer
    mov cx, 2048
    rep stosw

    ; Jump + NOP (JMP SHORT +0x58, NOP) — jumps over the BPB to boot code area.
    mov byte [io_buffer + 0], 0xEB
    mov byte [io_buffer + 1], 0x58
    mov byte [io_buffer + 2], 0x90
    ; OEM name: "CIUKIOS "
    mov byte [io_buffer + 3],  'C'
    mov byte [io_buffer + 4],  'I'
    mov byte [io_buffer + 5],  'U'
    mov byte [io_buffer + 6],  'K'
    mov byte [io_buffer + 7],  'I'
    mov byte [io_buffer + 8],  'O'
    mov byte [io_buffer + 9],  'S'
    mov byte [io_buffer + 10], ' '
    ; BPB_BytsPerSec = 512
    mov word [io_buffer + 11], 512
    ; BPB_SecPerClus = 8
    mov byte [io_buffer + 13], 8
    ; BPB_RsvdSecCnt = 4
    mov word [io_buffer + 14], 4
    ; BPB_NumFATs = 2
    mov byte [io_buffer + 16], 2
    ; BPB_RootEntCnt = 512
    mov word [io_buffer + 17], 512
    ; BPB_TotSec16 = 0 (use TotSec32)
    mov word [io_buffer + 19], 0
    ; BPB_Media = 0xF8
    mov byte [io_buffer + 21], 0xF8
    ; BPB_FATSz16 = 128
    mov word [io_buffer + 22], 128
    ; BPB_SecPerTrk = 63
    mov word [io_buffer + 24], 63
    ; BPB_NumHeads = 255
    mov word [io_buffer + 26], 255
    ; BPB_HiddSec = 63
    mov dword [io_buffer + 28], 63
    ; BPB_TotSec32 = 262144 = 0x00040000
    mov dword [io_buffer + 32], 0x00040000
    ; BS_DrvNum = 0x80, BS_Reserved1 = 0, BS_BootSig = 0x29
    mov byte [io_buffer + 36], 0x80
    mov byte [io_buffer + 37], 0
    mov byte [io_buffer + 38], 0x29
    ; BS_VolID = 0x4B49554B ("KIUK")
    mov dword [io_buffer + 39], 0x4B49554B
    ; BS_VolLab = "NO NAME    " (11 bytes)
    mov byte [io_buffer + 43], 'N'
    mov byte [io_buffer + 44], 'O'
    mov byte [io_buffer + 45], ' '
    mov byte [io_buffer + 46], 'N'
    mov byte [io_buffer + 47], 'A'
    mov byte [io_buffer + 48], 'M'
    mov byte [io_buffer + 49], 'E'
    mov byte [io_buffer + 50], ' '
    mov byte [io_buffer + 51], ' '
    mov byte [io_buffer + 52], ' '
    mov byte [io_buffer + 53], ' '
    ; BS_FilSysType = "FAT16   " (8 bytes)
    mov byte [io_buffer + 54], 'F'
    mov byte [io_buffer + 55], 'A'
    mov byte [io_buffer + 56], 'T'
    mov byte [io_buffer + 57], '1'
    mov byte [io_buffer + 58], '6'
    mov byte [io_buffer + 59], ' '
    mov byte [io_buffer + 60], ' '
    mov byte [io_buffer + 61], ' '
    ; Boot signature
    mov byte [io_buffer + 510], 0x55
    mov byte [io_buffer + 511], 0xAA

    mov word [raw_clone_lba_lo], 63
    mov word [raw_clone_lba_hi], 0
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    mov cx, 1
    call raw_ata_write_n
    jc .format_fail

%if SETUP_LIVE_CD_MODE
    mov al, 20
    call vis_format_phase_update
%endif

    ; ---------------------------------------------------------------
    ; Step 3: Write 3 reserved sectors at LBA 64-66 (zeros)
    ; ---------------------------------------------------------------
    ; Clear BPB area from io_buffer (leave buffer as all zeros)
    xor ax, ax
    mov di, io_buffer
    mov cx, 2048
    rep stosw

    mov word [raw_clone_lba_lo], 64
    mov word [raw_clone_lba_hi], 0
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    mov cx, 3
    call raw_ata_write_n
    jc .format_fail

%if SETUP_LIVE_CD_MODE
    mov al, 25
    call vis_format_phase_update
%endif

    ; ---------------------------------------------------------------
    ; Step 4: Write FAT1 at LBA 67 (128 sectors)
    ; Sector 0: media-byte entry (0xF8FF FFFF, rest zeros)
    ; Sectors 1-127: zeros
    ; ---------------------------------------------------------------
    mov byte [io_buffer + 0], 0xF8
    mov byte [io_buffer + 1], 0xFF
    mov byte [io_buffer + 2], 0xFF
    mov byte [io_buffer + 3], 0xFF

    mov word [raw_clone_lba_lo], 67
    mov word [raw_clone_lba_hi], 0
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    mov cx, 1
    call raw_ata_write_n
    jc .format_fail

    ; Clear media bytes, then write sectors 1-127 as zeros in batches.
    mov dword [io_buffer], 0
    mov word [raw_clone_lba_lo], 68
    mov word [raw_clone_lba_hi], 0
    mov word [format_sectors_done], 127  ; reuse as countdown

.fat1_loop:
    cmp word [format_sectors_done], 0
    je .fat1_done
    mov cx, RAW_HDD_BATCH_SECTORS
    cmp [format_sectors_done], cx
    jae .fat1_full
    mov cx, [format_sectors_done]
.fat1_full:
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    call raw_ata_write_n
    jc .format_fail
    sub [format_sectors_done], cx
    add [raw_clone_lba_lo], cx
    adc word [raw_clone_lba_hi], 0
    jmp .fat1_loop
.fat1_done:

%if SETUP_LIVE_CD_MODE
    mov al, 50
    call vis_format_phase_update
%endif

    ; ---------------------------------------------------------------
    ; Step 5: Write FAT2 at LBA 195 (128 sectors, identical to FAT1)
    ; ---------------------------------------------------------------
    mov byte [io_buffer + 0], 0xF8
    mov byte [io_buffer + 1], 0xFF
    mov byte [io_buffer + 2], 0xFF
    mov byte [io_buffer + 3], 0xFF

    mov word [raw_clone_lba_lo], 195
    mov word [raw_clone_lba_hi], 0
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    mov cx, 1
    call raw_ata_write_n
    jc .format_fail

    mov dword [io_buffer], 0
    mov word [raw_clone_lba_lo], 196
    mov word [raw_clone_lba_hi], 0
    mov word [format_sectors_done], 127

.fat2_loop:
    cmp word [format_sectors_done], 0
    je .fat2_done
    mov cx, RAW_HDD_BATCH_SECTORS
    cmp [format_sectors_done], cx
    jae .fat2_full
    mov cx, [format_sectors_done]
.fat2_full:
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    call raw_ata_write_n
    jc .format_fail
    sub [format_sectors_done], cx
    add [raw_clone_lba_lo], cx
    adc word [raw_clone_lba_hi], 0
    jmp .fat2_loop
.fat2_done:

%if SETUP_LIVE_CD_MODE
    mov al, 80
    call vis_format_phase_update
%endif

    ; ---------------------------------------------------------------
    ; Step 6: Write root directory at LBA 323 (32 sectors of zeros)
    ; ---------------------------------------------------------------
    mov word [raw_clone_lba_lo], 323
    mov word [raw_clone_lba_hi], 0
    mov word [format_sectors_done], 32

.rootdir_loop:
    cmp word [format_sectors_done], 0
    je .format_done
    mov cx, RAW_HDD_BATCH_SECTORS
    cmp [format_sectors_done], cx
    jae .rootdir_full
    mov cx, [format_sectors_done]
.rootdir_full:
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    call raw_ata_write_n
    jc .format_fail
    sub [format_sectors_done], cx
    add [raw_clone_lba_lo], cx
    adc word [raw_clone_lba_hi], 0
    jmp .rootdir_loop

.format_done:
    mov dx, msg_serial_hdd_format_done
    call serial_write_z
    call serial_write_crlf
    clc
    jmp .format_out

.format_fail:
    mov word [fail_code], 0x0703
    mov dx, msg_serial_hdd_format_fail
    call serial_write_z
    mov al, [raw_edd_status]
    call serial_write_hex_byte
    call serial_write_crlf
    stc

.format_out:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; -----------------------------------------------------------------------------
; Visual UI primitives (text mode 80x25 with CP437 box drawing + colors).
; Active only when SETUP_LIVE_CD_MODE=1.
; -----------------------------------------------------------------------------
%define VIS_ATTR_BG       0x17     ; white on blue (background)
%define VIS_ATTR_TITLE    0x1F     ; bright white on blue
%define VIS_ATTR_FRAME    0x17     ; white on blue
%define VIS_ATTR_ITEM     0x17     ; normal item
%define VIS_ATTR_SELECTED 0x70     ; black on white (highlighted)
%define VIS_ATTR_HINT     0x1B     ; bright cyan on blue
%define VIS_ATTR_OK       0x1A     ; bright green on blue
%define VIS_ATTR_ERR      0x1C     ; bright red on blue

vis_clear_screen:
    push ax
    push bx
    push cx
    push dx
    mov ax, 0x0600
    mov bh, VIS_ATTR_BG
    xor cx, cx
    mov dx, 0x184F
    int 0x10
    pop dx
    pop cx
    pop bx
    pop ax
    ret

vis_set_cursor:
    ; DH=row, DL=col
    push ax
    push bx
    mov ah, 0x02
    xor bh, bh
    int 0x10
    pop bx
    pop ax
    ret

vis_putc_attr:
    ; AL=char, BL=attribute, CX=count
    push ax
    push bx
    mov ah, 0x09
    xor bh, bh
    int 0x10
    pop bx
    pop ax
    ret

vis_print_z_at:
    ; DH=row, DL=col, BL=attribute, SI=zero-terminated string
    push ax
    push bx
    push cx
    push dx
    push si
    call vis_set_cursor
.loop:
    lodsb
    or al, al
    jz .done
    push si
    push dx
    mov ah, 0x09
    xor bh, bh
    mov cx, 1
    int 0x10
    pop dx
    inc dl
    push dx
    mov ah, 0x02
    xor bh, bh
    int 0x10
    pop dx
    pop si
    jmp .loop
.done:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

vis_draw_box:
    ; DH=top row, DL=left col, CH=height, CL=width, BL=attr
    push ax
    push bx
    push cx
    push dx
    push si

    ; top-left corner
    call vis_set_cursor
    mov al, 0xC9
    mov cx, 1
    call vis_putc_attr
    inc dl
    call vis_set_cursor
    mov al, 0xCD
    mov cl, [bp_box_w]
    sub cl, 2
    xor ch, ch
    call vis_putc_attr
    add dl, [bp_box_w]
    sub dl, 2
    call vis_set_cursor
    mov al, 0xBB
    mov cx, 1
    call vis_putc_attr

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Simpler box drawing using a register-only approach.
vis_box_draw:
    ; DH=top, DL=left, BH=height, BL=width, AH=attribute
    push ax
    push bx
    push cx
    push dx
    push si

    mov [bp_box_attr], ah

    ; Top row
    call vis_set_cursor
    mov al, 0xC9
    mov bh, [bp_box_attr]
    push bx
    mov bl, bh
    mov cx, 1
    mov ah, 0x09
    xor bh, bh
    int 0x10
    pop bx
    inc dl
    call vis_set_cursor
    mov al, 0xCD
    mov bh, [bp_box_attr]
    push bx
    mov bl, bh
    mov cl, [bp_box_w_in]
    xor ch, ch
    sub cl, 2
    mov ah, 0x09
    xor bh, bh
    int 0x10
    pop bx

    ; (top-right + sides + bottom omitted — simplified version below)
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; -----------------------------------------------------------------------------
; vis_box: minimal frame draw (top, sides, bottom) using BIOS scroll for solid
; background fill, then writing CP437 corner/edge chars.
; Inputs: top row in DH, left col in DL, height in CH, width in CL, attr in AH
; -----------------------------------------------------------------------------
vis_box:
    push ax
    push bx
    push cx
    push dx
    push si

    mov [box_top], dh
    mov [box_left], dl
    mov [box_height], ch
    mov [box_width], cl
    mov [box_attr], ah

    ; Fill background
    mov ah, 0x06
    xor al, al
    mov bh, [box_attr]
    mov ch, [box_top]
    mov cl, [box_left]
    mov dh, [box_top]
    add dh, [box_height]
    dec dh
    mov dl, [box_left]
    add dl, [box_width]
    dec dl
    int 0x10

    ; Top edge
    mov dh, [box_top]
    mov dl, [box_left]
    call vis_set_cursor
    mov al, 0xC9
    mov bl, [box_attr]
    mov cx, 1
    call vis_putc_attr
    mov dh, [box_top]
    mov dl, [box_left]
    inc dl
    call vis_set_cursor
    mov al, 0xCD
    mov bl, [box_attr]
    xor ch, ch
    mov cl, [box_width]
    sub cl, 2
    call vis_putc_attr
    mov dh, [box_top]
    mov dl, [box_left]
    add dl, [box_width]
    dec dl
    call vis_set_cursor
    mov al, 0xBB
    mov bl, [box_attr]
    mov cx, 1
    call vis_putc_attr

    ; Sides
    xor ch, ch
    mov cl, [box_height]
    sub cl, 2
    mov dh, [box_top]
    inc dh
.side_loop:
    push cx
    mov dl, [box_left]
    call vis_set_cursor
    mov al, 0xBA
    mov bl, [box_attr]
    mov cx, 1
    call vis_putc_attr
    mov dl, [box_left]
    add dl, [box_width]
    dec dl
    call vis_set_cursor
    mov al, 0xBA
    mov bl, [box_attr]
    mov cx, 1
    call vis_putc_attr
    pop cx
    inc dh
    loop .side_loop

    ; Bottom edge
    mov dh, [box_top]
    add dh, [box_height]
    dec dh
    mov dl, [box_left]
    call vis_set_cursor
    mov al, 0xC8
    mov bl, [box_attr]
    mov cx, 1
    call vis_putc_attr
    mov dh, [box_top]
    add dh, [box_height]
    dec dh
    mov dl, [box_left]
    inc dl
    call vis_set_cursor
    mov al, 0xCD
    mov bl, [box_attr]
    xor ch, ch
    mov cl, [box_width]
    sub cl, 2
    call vis_putc_attr
    mov dh, [box_top]
    add dh, [box_height]
    dec dh
    mov dl, [box_left]
    add dl, [box_width]
    dec dl
    call vis_set_cursor
    mov al, 0xBC
    mov bl, [box_attr]
    mov cx, 1
    call vis_putc_attr

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; -----------------------------------------------------------------------------
; vis_progress_bar: render a progress bar at (DH,DL) of width CL.
; AL = percent 0..100. Filled with 0xDB, empty with 0xB0.
; -----------------------------------------------------------------------------
vis_progress_bar:
    push ax
    push bx
    push cx
    push dx

    mov [pb_top], dh
    mov [pb_left], dl
    mov [pb_width], cl
    mov [pb_pct], al

    ; Compute filled count = pct * width / 100
    xor ah, ah
    mov bl, [pb_width]
    mul bl                         ; AX = pct * width
    mov bl, 100
    div bl                         ; AL = filled, AH = remainder
    mov [pb_filled], al

    mov dh, [pb_top]
    mov dl, [pb_left]
    call vis_set_cursor
    mov al, 0xDB
    mov bl, VIS_ATTR_OK
    xor ch, ch
    mov cl, [pb_filled]
    cmp cl, 0
    je .skip_fill
    call vis_putc_attr
.skip_fill:
    mov dh, [pb_top]
    mov dl, [pb_left]
    add dl, [pb_filled]
    call vis_set_cursor
    mov al, 0xB0
    mov bl, VIS_ATTR_FRAME
    xor ch, ch
    mov cl, [pb_width]
    sub cl, [pb_filled]
    cmp cl, 0
    je .skip_empty
    call vis_putc_attr
.skip_empty:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; -----------------------------------------------------------------------------
; visual_main_loop: render menu, accept F/I/R/Esc, return.
; CF=0 if user picked Install (proceed with install pipeline), CF=1 if Esc.
; Side effects: when picking Format, exec FORMAT.COM and redraw.
; -----------------------------------------------------------------------------
visual_main_loop:
    mov byte [selected_profile], 1               ; minimal profile = clone all
    mov byte [target_drive], 2                   ; C:
    mov byte [visual_destroy_confirmed], 0

.redraw:
    call vis_clear_screen
    ; Title bar
    mov dh, 0
    mov dl, 0
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_TITLE
    mov cx, 80
    call vis_putc_attr
    mov dh, 0
    mov dl, 26
    mov bl, VIS_ATTR_TITLE
    mov si, msg_vis_title
    call vis_print_z_at

    ; Main panel box
    mov dh, 5
    mov dl, 18
    mov ch, 13
    mov cl, 44
    mov ah, VIS_ATTR_FRAME
    call vis_box

    ; Header inside the box
    mov dh, 6
    mov dl, 22
    mov bl, VIS_ATTR_TITLE
    mov si, msg_vis_header
    call vis_print_z_at

    mov dh, 9
    mov dl, 22
    mov bl, VIS_ATTR_ITEM
    mov si, msg_vis_item_format
    call vis_print_z_at

    mov dh, 11
    mov dl, 22
    mov bl, VIS_ATTR_ITEM
    mov si, msg_vis_item_install
    call vis_print_z_at

    mov dh, 13
    mov dl, 22
    mov bl, VIS_ATTR_ITEM
    mov si, msg_vis_item_reboot
    call vis_print_z_at

    mov dh, 15
    mov dl, 22
    mov bl, VIS_ATTR_ITEM
    mov si, msg_vis_item_exit
    call vis_print_z_at

    ; Hint bar
    mov dh, 22
    mov dl, 12
    mov bl, VIS_ATTR_HINT
    mov si, msg_vis_hint
    call vis_print_z_at

    ; Park cursor off-screen
    mov dh, 24
    mov dl, 79
    call vis_set_cursor

.wait_key:
    call read_key
    cmp al, 27
    je .esc
    cmp al, 'F'
    je .do_format
    cmp al, 'f'
    je .do_format
    cmp al, 'I'
    je .do_install
    cmp al, 'i'
    je .do_install
    cmp al, 'R'
    je .do_reboot
    cmp al, 'r'
    je .do_reboot
    cmp al, 13
    je .do_install
    jmp .wait_key

.do_format:
    call vis_run_format
    jmp .redraw

.do_install:
    call vis_confirm_destroy
    jc .redraw
    mov byte [visual_destroy_confirmed], 1
    clc
    ret

.do_reboot:
    call reboot_system

.esc:
    stc
    ret

; -----------------------------------------------------------------------------
; vis_confirm_destroy: visual Yes/No dialog before clobbering target HDD.
; CF=0 if confirmed, CF=1 if cancelled.
; -----------------------------------------------------------------------------
vis_confirm_destroy:
    mov dh, 9
    mov dl, 20
    mov ch, 7
    mov cl, 40
    mov ah, VIS_ATTR_ERR
    call vis_box

    mov dh, 10
    mov dl, 24
    mov bl, VIS_ATTR_ERR
    mov si, msg_vis_destroy_1
    call vis_print_z_at

    mov dh, 12
    mov dl, 24
    mov bl, VIS_ATTR_ERR
    mov si, msg_vis_destroy_2
    call vis_print_z_at

    mov dh, 14
    mov dl, 24
    mov bl, VIS_ATTR_HINT
    mov si, msg_vis_destroy_3
    call vis_print_z_at

.wait:
    call read_key
    cmp al, 'Y'
    je .yes
    cmp al, 'y'
    je .yes
    cmp al, 'N'
    je .no
    cmp al, 'n'
    je .no
    cmp al, 27
    je .no
    jmp .wait

.yes:
    clc
    ret

.no:
    stc
    ret

; -----------------------------------------------------------------------------
; vis_run_format: invoke the internal format_target_hdd routine (which uses
; the same INT 13h EDD+retry+CHS-fallback hardening as install) with a visual
; progress bar drawn live, then redraw the menu.
;
; The standalone FORMAT.COM payload at \APPS\FORMAT.COM remains available
; from the shell prompt (`run FORMAT.COM /F`) for users who want the
; standalone DOS tool path.
; -----------------------------------------------------------------------------
vis_run_format:
    push ax
    push bx
    push cx
    push dx
    push si

    call vis_clear_screen

    mov dh, 0
    mov dl, 0
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_TITLE
    mov cx, 80
    call vis_putc_attr
    mov dh, 0
    mov dl, 26
    mov bl, VIS_ATTR_TITLE
    mov si, msg_vis_format_title
    call vis_print_z_at

    mov dh, 8
    mov dl, 8
    mov ch, 7
    mov cl, 64
    mov ah, VIS_ATTR_FRAME
    call vis_box

    mov dh, 10
    mov dl, 12
    mov bl, VIS_ATTR_ITEM
    mov si, msg_vis_install_phase_format
    call vis_print_z_at

    xor al, al
    call vis_format_phase_update

    mov dh, 24
    mov dl, 79
    call vis_set_cursor

    call format_target_hdd
    jc .fail

    mov al, 100
    call vis_format_phase_update

    mov dh, 13
    mov dl, 12
    mov bl, VIS_ATTR_OK
    mov si, msg_vis_format_done
    call vis_print_z_at
    call vis_press_any_key
    jmp .out

.fail:
    mov dh, 13
    mov dl, 12
    mov bl, VIS_ATTR_ERR
    mov si, msg_vis_format_fail
    call vis_print_z_at
    call vis_press_any_key

.out:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

vis_press_any_key:
    mov dh, 14
    mov dl, 22
    mov bl, VIS_ATTR_HINT
    mov si, msg_vis_press_any
    call vis_print_z_at
    call read_key
    ret

; -----------------------------------------------------------------------------
; vis_install_screen_init: draw the install progress screen (3 phases) once.
; -----------------------------------------------------------------------------
; -----------------------------------------------------------------------------
; Single-bar install progress UI. Layout:
;   row 0           : title bar (white-on-blue)
;   row 4..18       : framed panel
;   row 6           : 'Installing CiukiOS...' centered
;   row 9, col 12   : phase label (e.g. 'Phase 1/3: Formatting target HDD')
;   row 11, col 12  : progress bar (width 56)  + pct on right (col 70)
;   row 14          : status text (changes during operation)
;   row 22 (footer) : hint
; -----------------------------------------------------------------------------
%define VIS_INST_TITLE_ROW    0
%define VIS_INST_BOX_TOP      4
%define VIS_INST_BOX_LEFT     8
%define VIS_INST_BOX_HEIGHT   16
%define VIS_INST_BOX_WIDTH    64
%define VIS_INST_HEADER_ROW   6
%define VIS_INST_PHASE_ROW    9
%define VIS_INST_BAR_ROW      11
%define VIS_INST_BAR_COL      12
%define VIS_INST_BAR_WIDTH    52
%define VIS_INST_PCT_COL      66
%define VIS_INST_STATUS_ROW   14

vis_install_screen_init:
    push ax
    push bx
    push cx
    push dx
    push si

    call vis_clear_screen

    ; Top title bar
    mov dh, VIS_INST_TITLE_ROW
    mov dl, 0
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_TITLE
    mov cx, 80
    call vis_putc_attr
    mov dh, VIS_INST_TITLE_ROW
    mov dl, 23
    mov bl, VIS_ATTR_TITLE
    mov si, msg_vis_install_titlebar
    call vis_print_z_at

    ; Outer frame (panel)
    mov dh, VIS_INST_BOX_TOP
    mov dl, VIS_INST_BOX_LEFT
    mov ch, VIS_INST_BOX_HEIGHT
    mov cl, VIS_INST_BOX_WIDTH
    mov ah, VIS_ATTR_FRAME
    call vis_box

    ; Header inside the panel
    mov dh, VIS_INST_HEADER_ROW
    mov dl, 28
    mov bl, VIS_ATTR_TITLE
    mov si, msg_vis_install_header
    call vis_print_z_at

    ; Footer hint
    mov dh, 22
    mov dl, 22
    mov bl, VIS_ATTR_HINT
    mov si, msg_vis_install_hint
    call vis_print_z_at

    ; Park cursor off panel
    mov dh, 24
    mov dl, 79
    call vis_set_cursor

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; vis_install_set_phase: write a fresh phase label at row 9 and clear the bar.
; Inputs: SI = phase label string (zero-terminated)
vis_install_set_phase:
    push ax
    push bx
    push cx
    push dx
    push si

    push si
    mov dh, VIS_INST_PHASE_ROW
    mov dl, VIS_INST_BAR_COL
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_BG
    mov cx, VIS_INST_BAR_WIDTH
    call vis_putc_attr
    pop si

    mov dh, VIS_INST_PHASE_ROW
    mov dl, VIS_INST_BAR_COL
    mov bl, VIS_ATTR_TITLE
    call vis_print_z_at

    xor al, al
    call vis_install_set_pct

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; vis_install_set_pct: render the unified bar at row 11 with the given percent.
; Input: AL = 0..100
vis_install_set_pct:
    push ax
    push bx
    push cx
    push dx

    mov ah, al                          ; preserve pct
    mov dh, VIS_INST_BAR_ROW
    mov dl, VIS_INST_BAR_COL
    xor ch, ch
    mov cl, VIS_INST_BAR_WIDTH
    mov al, ah
    call vis_progress_bar

    ; Clear pct field
    mov dh, VIS_INST_BAR_ROW
    mov dl, VIS_INST_PCT_COL
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_OK
    mov cx, 5
    call vis_putc_attr

    ; Re-cursor and print decimal + '%'
    mov dh, VIS_INST_BAR_ROW
    mov dl, VIS_INST_PCT_COL
    call vis_set_cursor
    mov al, ah
    call vis_print_u8_dec_attr
    mov al, '%'
    mov bl, VIS_ATTR_OK
    mov cx, 1
    call vis_putc_attr

    pop dx
    pop cx
    pop bx
    pop ax
    ret

; vis_install_set_status: write status text at row 14, col 12
; Input: SI = zero-terminated string
vis_install_set_status:
    push ax
    push bx
    push cx
    push dx
    push si

    push si
    mov dh, VIS_INST_STATUS_ROW
    mov dl, VIS_INST_BAR_COL
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_BG
    mov cx, VIS_INST_BAR_WIDTH
    call vis_putc_attr
    pop si

    mov dh, VIS_INST_STATUS_ROW
    mov dl, VIS_INST_BAR_COL
    mov bl, VIS_ATTR_HINT
    call vis_print_z_at

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; --- Compatibility shims used by format_target_hdd / raw_hdd_clone_install ---
vis_format_phase_update:
    push si
    cmp byte [vis_install_phase_active], 1
    jne .skip
    call vis_install_set_pct
.skip:
    pop si
    ret

vis_clone_phase_update:
    push si
    cmp byte [vis_install_phase_active], 2
    jne .skip
    call vis_install_set_pct
.skip:
    pop si
    ret

vis_install_phase_clone:
    push ax
    push si
    mov al, 100
    call vis_install_set_pct
    mov byte [vis_install_phase_active], 2
    mov si, msg_vis_install_phase_clone
    call vis_install_set_phase
    mov si, msg_vis_install_status_clone
    call vis_install_set_status
    pop si
    pop ax
    ret

vis_install_phase_format:
    push ax
    push si
    mov byte [vis_install_phase_active], 1
    mov si, msg_vis_install_phase_format
    call vis_install_set_phase
    mov si, msg_vis_install_status_format
    call vis_install_set_status
    pop si
    pop ax
    ret

vis_install_phase_done:
    push ax
    push si
    mov al, 100
    call vis_install_set_pct
    mov byte [vis_install_phase_active], 3
    mov si, msg_vis_install_phase_patch
    call vis_install_set_phase
    mov al, 100
    call vis_install_set_pct
    mov si, msg_vis_install_done
    call vis_install_set_status
    pop si
    pop ax
    ret

; vis_install_eject_prompt: ask the user to remove the live CD before warm
; reboot, then wait for any key. Without this prompt the BIOS boot order
; (CD first on most laptops) re-enters the live CD instead of booting the
; freshly-installed HDD.
vis_install_eject_prompt:
    push ax
    push bx
    push cx
    push dx
    push si

    ; Bright eject banner inside the install panel
    mov dh, VIS_INST_STATUS_ROW
    mov dl, VIS_INST_BAR_COL
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_BG
    mov cx, VIS_INST_BAR_WIDTH
    call vis_putc_attr
    mov dh, VIS_INST_STATUS_ROW
    mov dl, VIS_INST_BAR_COL
    mov bl, VIS_ATTR_OK
    mov si, msg_vis_install_eject
    call vis_print_z_at

    mov dh, VIS_INST_STATUS_ROW
    add dh, 2
    mov dl, VIS_INST_BAR_COL
    mov bl, VIS_ATTR_HINT
    mov si, msg_vis_install_eject_hint
    call vis_print_z_at

    ; Block on a key (bypasses the keyboard buffer state our shell may have left)
    xor ax, ax
    int 0x16

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; vis_install_fail_prompt: display installation failure, wait for key, then
; warm-reboot. Called on install failure in live-CD mode so the user never
; lands in a potentially broken shell after an INT 13h wedge.
vis_install_fail_prompt:
    push ax
    push bx
    push cx
    push dx
    push si

    mov dh, VIS_INST_STATUS_ROW
    mov dl, VIS_INST_BAR_COL
    call vis_set_cursor
    mov al, ' '
    mov bl, VIS_ATTR_ERR
    mov cx, VIS_INST_BAR_WIDTH
    call vis_putc_attr
    mov dh, VIS_INST_STATUS_ROW
    mov dl, VIS_INST_BAR_COL
    mov bl, VIS_ATTR_ERR
    mov si, msg_vis_install_fail_banner
    call vis_print_z_at

    mov dh, VIS_INST_STATUS_ROW
    add dh, 2
    mov dl, VIS_INST_BAR_COL
    mov bl, VIS_ATTR_HINT
    mov si, msg_vis_install_fail_hint
    call vis_print_z_at

    xor ax, ax
    int 0x16

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ; Fall through: caller does int 0x21 / 4C01h which won't execute in
    ; live-CD mode — we reboot directly here.
    call reboot_system
    ret

; vis_print_u8_dec_attr: write 0..100 as decimal at cursor with attribute in BL
; Cursor is advanced by INT 10h AH=0Ah-style writes (we use AH=0Ah no-advance,
; then manually move). Simpler: emit each digit as char+attr then increment.
vis_print_u8_dec_attr:
    push ax
    push bx
    push cx
    push dx
    push si
    mov si, vis_dec_buf + 3
    mov byte [si], 0
    mov bl, 10
.div_loop:
    xor ah, ah
    div bl                            ; AL=quot, AH=rem
    dec si
    add ah, '0'
    mov [si], ah
    or al, al
    jnz .div_loop
    mov si, si                        ; SI -> first digit
.print:
    mov al, [si]
    or al, al
    jz .done
    push si
    mov bl, VIS_ATTR_OK
    mov bh, 0
    mov cx, 1
    mov ah, 0x09
    int 0x10
    mov ah, 0x03
    xor bh, bh
    int 0x10                          ; get cursor
    inc dl
    mov ah, 0x02
    xor bh, bh
    int 0x10
    pop si
    inc si
    jmp .print
.done:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

reboot_system:
    push cs
    pop ds
    push cs
    pop es
    xor ax, ax
    mov cx, ax
    mov dx, ax
    int 0x19
    hlt
    jmp reboot_system

shutdown_system:
    push cs
    pop ds
    xor ax, ax
    mov cx, ax
    mov dx, ax
    mov sp, 0xFFFC
    hlt
    jmp shutdown_system

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
    mov word [reset_counter], 0

    mov word [clone_done_lo], 0
    mov word [clone_done_hi], 0
    mov byte [clone_progress_pct], 0
    mov word [clone_next_mark_lo], 0
    mov word [clone_next_mark_hi], 0
    mov ax, RAW_HDD_CLONE_SECTORS_LO
    mov dx, RAW_HDD_CLONE_SECTORS_HI
    mov bx, 100
    div bx
    mov [clone_step_lo], ax
    mov word [clone_step_hi], 0      ; quotient fits in 16 bits; high word = 0
    mov ax, [clone_step_lo]
    mov [clone_next_mark_lo], ax
    mov word [clone_next_mark_hi], 0

.copy_loop:
    ; remaining sectors
    mov ax, [raw_clone_remaining_lo]
    or ax, [raw_clone_remaining_hi]
    jz .done

    ; this_batch = min(BATCH, remaining). For >65535-remaining, always BATCH.
    cmp word [raw_clone_remaining_hi], 0
    jne .full_batch
    cmp word [raw_clone_remaining_lo], RAW_HDD_BATCH_SECTORS
    jb .partial_batch
.full_batch:
    mov cx, RAW_HDD_BATCH_SECTORS
    jmp .have_batch
.partial_batch:
    mov cx, [raw_clone_remaining_lo]
.have_batch:
    mov [batch_count], cx

    mov dl, RAW_HDD_SOURCE_DRIVE
    mov bx, io_buffer
    mov cx, [batch_count]
    call raw_edd_read_n
    jc .fail

    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    mov cx, [batch_count]
    call raw_ata_write_n
    jc .fail

    ; advance LBA by batch_count
    mov ax, [batch_count]
    add [raw_clone_lba_lo], ax
    adc word [raw_clone_lba_hi], 0

    ; subtract batch_count from remaining
    sub [raw_clone_remaining_lo], ax
    sbb word [raw_clone_remaining_hi], 0

    ; add batch_count to clone_done
    mov ax, [batch_count]
    add [clone_done_lo], ax
    adc word [clone_done_hi], 0

    ; Periodic recovery: keep BIOS source-drive state fresh, but never call
    ; INT 13h reset on the target while writes are running through ATA PIO.
    ; Some real BIOSes wedge when mixing direct ATA writes with repeated
    ; INT 13h resets of the same target device mid-clone.
    inc word [reset_counter]
    test word [reset_counter], 0x003F
    jnz .no_reset
    push ax
    push dx
    push cx
    xor ax, ax
    mov dl, RAW_HDD_SOURCE_DRIVE
    int 0x13
    call ata_pri_soft_reset
    ; Ignore ATA reset failure here: write path has per-sector timeout+recovery
    ; and will surface the failure with stage/path/status diagnostics.
    pop cx
    pop dx
    pop ax
.no_reset:

    mov al, [clone_progress_pct]
    cmp al, 100
    jae .copy_loop

    mov ax, [clone_done_hi]
    cmp ax, [clone_next_mark_hi]
    jb .copy_loop
    ja .clone_emit
    mov ax, [clone_done_lo]
    cmp ax, [clone_next_mark_lo]
    jb .copy_loop

.clone_emit:
    add byte [clone_progress_pct], 1
%if SETUP_LIVE_CD_MODE
    mov al, [clone_progress_pct]
    call vis_clone_phase_update
%endif
    mov ax, [clone_next_mark_lo]
    add ax, [clone_step_lo]
    mov [clone_next_mark_lo], ax
    mov ax, [clone_next_mark_hi]
    adc ax, [clone_step_hi]
    mov [clone_next_mark_hi], ax
    jmp .copy_loop

.done:
    mov dx, msg_serial_hdd_install_copy_done
    call serial_write_z
    call serial_write_crlf
%if SETUP_LIVE_CD_MODE
    mov dx, msg_serial_hdd_install_patch_start
    call serial_write_z
    call serial_write_crlf
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

    call print_crlf
    mov dx, msg_screen_hdd_install_fail
    call print_z
    mov al, [raw_last_status]
    call print_u8_dec
    mov dx, msg_screen_hdd_install_path
    call print_z
    mov dl, [raw_last_path]
    call print_char_dl
    mov dx, msg_screen_hdd_install_lba
    call print_z
    mov ax, [raw_clone_lba_lo]
    call print_u16_dec
    call print_crlf
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

; Single-sector wrappers (kept for raw_patch_installed_default_drive which
; only touches one sector). Internally call the multi-sector routines with
; CX=1.
raw_edd_read_current_lba:
    push cx
    mov cx, 1
    call raw_edd_read_n
    pop cx
    ret

raw_edd_write_current_lba:
    push cx
    mov cx, 1
    call raw_ata_write_n        ; bypass BIOS for all target-HDD writes
    pop cx
    ret

; Multi-sector read: AH=0x42 EDD with CX sectors. CHS fallback for source
; drive (CD) only -- target HDD writes are EDD-only per d3e2fb7. CX must be
; 1..127 (DAP limit).
raw_edd_read_n:
    mov byte [raw_last_stage], 'R'
    mov byte [raw_last_path], 'E'
    mov ah, 0x42
    push dx
    call raw_edd_transfer_current_lba
    pop dx
    jnc .done
    cmp dl, RAW_HDD_SOURCE_DRIVE
    jne .done
    mov byte [raw_last_path], 'C'
    mov ah, 0x02
    call raw_chs_transfer_current_lba
.done:
    ret

raw_edd_write_n:
    mov byte [raw_last_stage], 'W'
    mov byte [raw_last_path], 'E'
    mov ah, 0x43
    call raw_edd_transfer_current_lba
    ret

; raw_ata_write_n: write CX sectors from CS:BX to primary ATA master HDD
; using direct port I/O, bypassing INT 13h entirely.
;
; Inputs:  BX = buffer offset (in CS), CX = sector count
;          [raw_clone_lba_lo/hi] = starting LBA (28-bit, high word < 0x10)
; Returns: CF=0 OK, CF=1 error
; Clobbers: nothing (all registers preserved via push/pop)
;
; This replaces raw_edd_write_n for target HDD writes on T23 hardware where
; the BIOS INT 13h write handler wedges the CPU after many sequential calls.
;
raw_ata_write_n:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    push cs
    pop ds                      ; DS=CS so rep outsw addresses CS:SI

    mov si, bx                  ; CS:SI = write buffer
    mov ax, [raw_clone_lba_lo]
    mov [ata_cur_lba_lo], ax
    mov ax, [raw_clone_lba_hi]
    mov [ata_cur_lba_hi], ax
    mov [ata_sectors_left], cx

.ata_next_sector:
    ; --- Wait for drive ready (BSY=0) ---
    xor cx, cx                  ; 65536 iterations ≈ 33 ms at 1 GHz
.ata_bsy0:
    mov dx, ATA_PRI_STATUS
    in al, dx
    test al, 0x80               ; BSY?
    jz .ata_bsy0_ok
    loop .ata_bsy0
    ; Timeout: soft-reset and retry once
    call ata_pri_soft_reset
    jc .ata_write_fail
    jmp .ata_next_sector

.ata_bsy0_ok:
    ; --- Program LBA and device registers ---
    ; Device: 0xE0 = LBA mode, master (bit4=0); OR bits [27:24] of LBA
    mov dx, ATA_PRI_DEV
    mov al, 0xE0
    or  al, [ata_cur_lba_hi + 1] ; bits [27:24] (always 0 for disks < 128 GB)
    out dx, al

    mov dx, ATA_PRI_NSECT
    mov al, 1
    out dx, al

    mov ax, [ata_cur_lba_lo]
    mov dx, ATA_PRI_LBAL
    out dx, al                  ; bits [7:0]
    mov dx, ATA_PRI_LBAM
    mov al, ah
    out dx, al                  ; bits [15:8]

    mov ax, [ata_cur_lba_hi]
    mov dx, ATA_PRI_LBAH
    out dx, al                  ; bits [23:16]

    ; --- Issue WRITE SECTORS (0x30) ---
    mov dx, ATA_PRI_STATUS      ; same port address as command register
    mov al, 0x30
    out dx, al

    ; 400 ns settling delay: read alt-status 4× (each I/O ≈ 100 ns)
    mov dx, ATA_PRI_CTRL
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    ; --- Wait for DRQ=1, BSY=0 (drive ready for data) ---
    xor cx, cx
.ata_drq_wait:
    mov dx, ATA_PRI_STATUS
    in al, dx
    test al, 0x80               ; BSY still set?
    jnz .ata_drq_loop
    test al, 0x01               ; ERR?
    jnz .ata_write_fail
    test al, 0x08               ; DRQ?
    jnz .ata_do_write
.ata_drq_loop:
    loop .ata_drq_wait
    jmp .ata_write_fail

.ata_do_write:
    ; --- Transfer 256 words (512 bytes) to ATA data register ---
    mov dx, ATA_PRI_DATA
    mov cx, 256
    rep outsw                   ; DS:SI → port DX; SI auto-advances

    ; --- Wait for BSY=0 (drive processing the write) ---
    xor cx, cx
.ata_bsy1:
    mov dx, ATA_PRI_STATUS
    in al, dx
    test al, 0x80
    jz .ata_sector_ok
    loop .ata_bsy1
    jmp .ata_write_fail

.ata_sector_ok:
    ; Advance per-sector LBA and buffer pointer; loop for next sector
    add word [ata_cur_lba_lo], 1
    adc word [ata_cur_lba_hi], 0
    ; SI already advanced by rep outsw
    dec word [ata_sectors_left]
    jnz .ata_next_sector
    clc
    jmp .ata_write_out

.ata_write_fail:
    mov byte [raw_edd_status], 0xFF
    stc

.ata_write_out:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; ata_pri_soft_reset: ATA SRST on primary channel, waits for BSY to clear.
ata_pri_soft_reset:
    push ax
    push cx
    push dx
    mov dx, ATA_PRI_CTRL
    mov al, 0x04                ; SRST bit
    out dx, al
    ; 5 µs delay via 8 alt-status reads
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    mov al, 0x00
    out dx, al
    ; Wait up to ~130 ms for BSY=0
    xor cx, cx
.rst_bsy_wait:
    in al, dx                   ; read alt-status (no side effects)
    test al, 0x80
    jz .rst_ok
    loop .rst_bsy_wait
    stc
    jmp .rst_out
.rst_ok:
    clc
.rst_out:
    pop dx
    pop cx
    pop ax
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

; raw_edd_transfer_current_lba
; Inputs: AH=0x42 (read) or 0x43 (write), DL=drive, BX=buffer offset (CS:BX),
;         CX=sector count (caller-driven, supports multi-sector batches),
;         [raw_clone_lba_lo/hi]=starting LBA
; The DAP sector count field is loaded from CX so the same routine handles
; both single-sector (CX=1) and batched (CX=8/16/etc.) transfers.
raw_edd_transfer_current_lba:
    push ax
    push bx
    push cx
    push dx
    push si
    push cs
    pop ds
    mov word [bios_probe_dap + 0], 0x0010
    mov [bios_probe_dap + 2], cx
    mov word [bios_probe_dap + 4], bx
    mov bx, cs
    mov word [bios_probe_dap + 6], bx
    mov bx, [raw_clone_lba_lo]
    mov word [bios_probe_dap + 8], bx
    mov bx, [raw_clone_lba_hi]
    mov word [bios_probe_dap + 10], bx
    mov word [bios_probe_dap + 12], 0
    mov word [bios_probe_dap + 14], 0
    mov [raw_edd_retry_op], ah
    mov [raw_edd_retry_drive], dl
    mov [raw_edd_retry_count], cx
    mov si, bios_probe_dap
    xor al, al
    int 0x13
    jnc .success

    mov dl, [raw_edd_retry_drive]
    xor ax, ax
    int 0x13

    push cs
    pop ds
    mov word [bios_probe_dap + 0], 0x0010
    mov cx, [raw_edd_retry_count]
    mov [bios_probe_dap + 2], cx
    mov si, bios_probe_dap
    mov dl, [raw_edd_retry_drive]
    mov ah, [raw_edd_retry_op]
    xor al, al
    int 0x13
    jc .fail
.success:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret
.fail:
    mov [raw_last_status], ah
    mov [raw_edd_status], ah
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

; CHS fallback: takes CX = sector count (1..63) like the EDD routine. Saved
; into raw_chs_count before CL gets repurposed for the CHS register layout.
raw_chs_transfer_current_lba:
    push ax
    push bx
    push cx
    push dx
    push si
    push es
    push cs
    pop ds

    mov si, bx
    mov [raw_chs_drive], dl
    mov [raw_chs_op], ah
    mov [raw_chs_count], cl

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
    mov al, [raw_chs_count]
    int 0x13
    jc .fail

    pop es
    pop si
    pop dx
    pop cx
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
    pop cx
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

%if SETUP_LIVE_CD_MODE
    cmp byte [visual_destroy_confirmed], 1
    jne .interactive
    pop si
    pop dx
    pop bx
    pop ax
    clc
    ret
.interactive:
%endif

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

print_u16_dec:
    push ax
    push bx
    push cx
    push dx
    push si
    mov si, dec_u16_buf + 5
    mov byte [si], 0
    mov bx, 10
.div_loop:
    xor dx, dx
    div bx
    dec si
    add dl, '0'
    mov [si], dl
    or ax, ax
    jnz .div_loop
    mov dx, si
    call print_z
    pop si
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
msg_serial_hdd_install_copy_done db '[SETUP-HDD-INSTALL] COPY-DONE', 0
msg_serial_hdd_install_patch_start db '[SETUP-HDD-INSTALL] PATCH-START', 0
msg_serial_hdd_install_fail db '[SETUP-HDD-INSTALL] FAIL S=', 0
msg_serial_hdd_install_path db ' P=', 0
msg_serial_hdd_install_lba db ' L=', 0
msg_serial_hdd_install_status db ' AH=', 0
msg_serial_hdd_install_edd db ' E=', 0
msg_serial_hdd_install_chs db ' C=', 0
msg_hdd_format_screen_start db 'Formatting target HDD...', 0
msg_serial_hdd_format_start db '[SETUP-HDD-FORMAT] START', 0
msg_serial_hdd_format_progress db '[SETUP-HDD-FORMAT] PROGRESS ', 0
msg_serial_hdd_format_done db '[SETUP-HDD-FORMAT] DONE', 0
msg_serial_hdd_format_fail db '[SETUP-HDD-FORMAT] FAIL S=', 0
msg_screen_hdd_install_fail db 'HDD install failed. AH=', 0
msg_screen_hdd_install_path db ' P=', 0
msg_screen_hdd_install_lba db ' L=', 0
msg_screen_hdd_format_fail db 'HDD format failed. AH=', 0

msg_vis_title           db 'CiukiOS Setup - Live CD Installer', 0
msg_vis_header          db 'Choose an action:', 0
msg_vis_item_format     db '[F]  Format target HDD (native FAT16 MBR)', 0
msg_vis_item_install    db '[I]  Install OS to target HDD', 0
msg_vis_item_reboot     db '[R]  Reboot system', 0
msg_vis_item_exit       db '[Esc] Exit to DOS prompt', 0
msg_vis_hint            db 'Press F / I / R / Esc to choose', 0
msg_vis_destroy_1       db 'Confirm destructive install', 0
msg_vis_destroy_2       db 'BIOS HDD #2 will be wiped.', 0
msg_vis_destroy_3       db 'Press Y to proceed, N to cancel.', 0
msg_vis_format_title    db 'Format target HDD', 0
msg_vis_format_running  db 'Formatting target HDD...', 0
msg_vis_format_done     db 'Format complete.', 0
msg_vis_format_fail     db 'Format failed.', 0
msg_vis_press_any       db 'Press any key to return to menu.', 0
msg_vis_install_title   db 'Installing CiukiOS...', 0
msg_vis_install_titlebar db 'CiukiOS Setup - Installing system', 0
msg_vis_install_header  db 'Installing CiukiOS', 0
msg_vis_install_hint    db 'Please wait. Do not power off the system.', 0
msg_vis_install_phase_format  db 'Phase 1/3: Formatting target HDD', 0
msg_vis_install_phase_clone   db 'Phase 1/2: Cloning system image', 0
msg_vis_install_phase_patch   db 'Phase 2/2: Finalizing installation', 0
msg_vis_install_status_format db 'Writing FAT16 structure to target HDD...', 0
msg_vis_install_status_clone  db 'Cloning live-CD image to target HDD...', 0
msg_vis_install_done    db 'Installation complete. Rebooting...', 0
msg_vis_install_eject      db 'Installation complete. REMOVE the CD now.', 0
msg_vis_install_eject_hint db 'Press any key to reboot from the installed HDD.', 0
msg_vis_install_fail_banner db 'INSTALLATION FAILED. System will reboot.', 0
msg_vis_install_fail_hint   db 'Press any key to reboot and try again.', 0

format_path             db '\APPS\FORMAT.COM', 0
format_cmdtail          db 3, ' /F', 13
format_fcb1             times 16 db 0
format_fcb2             times 16 db 0

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

ata_cur_lba_lo      dw 0    ; working copy of LBA for raw_ata_write_n
ata_cur_lba_hi      dw 0
ata_sectors_left    dw 0

format_sectors_total    dd 0x00040000  ; ~262GB sectors
format_sectors_done     dd 0
format_progress_pct     db 0
format_progress_step_lo dw 0
format_progress_step_hi dw 0
format_next_mark_lo     dw 0
format_next_mark_hi     dw 0
format_last_pct         db 0xFF        ; force first print

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
raw_chs_count           db 1
raw_edd_retry_op        db 0
raw_edd_retry_drive     db 0
raw_edd_retry_count     dw 1
batch_count             dw 0
reset_counter           dw 0
prompt_tick_start       dw 0

box_top                 db 0
box_left                db 0
box_height              db 0
box_width               db 0
box_attr                db 0
bp_box_w                db 0
bp_box_w_in             db 0
bp_box_attr             db 0
pb_top                  db 0
pb_left                 db 0
pb_width                db 0
pb_pct                  db 0
pb_filled               db 0
visual_destroy_confirmed db 0
vis_install_phase_active db 0
clone_done_lo           dw 0
clone_done_hi           dw 0
clone_step_lo           dw 0
clone_step_hi           dw 0
clone_next_mark_lo      dw 0
clone_next_mark_hi      dw 0
clone_progress_pct      db 0
vis_dec_buf             times 4 db 0

exec_param              times 14 db 0

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
dec_u16_buf             times 6 db 0
align 16
bios_probe_dap         db 0x10, 0x00
                       dw 0x0001
                       dw 0x0000
                       dw 0x0000
                       dq 0x0000000000000000

align 16
; Multi-sector I/O buffer: 8 sectors (4 KB) so the install/format loops can
; batch INT 13h transfers and reduce the call count by 8x. Larger batches
; mean fewer chances for a real-HW BIOS (e.g. ThinkPad T23) to wedge.
io_buffer               times 4096 db 0
