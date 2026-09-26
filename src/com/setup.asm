; setup.asm - CiukiOS installer and FAT16 disk preparation.
; The live CD uses the VGA graphical wizard; legacy manifest mode is retained.

bits 16
cpu 386
org 0x0100

%define FILE_COUNT 9
%define MANIFEST_HEADER_SIZE 5
%define MANIFEST_RECORD_SIZE 4
%define PROMPT_TIMEOUT_TICKS 90
%define CLEANUP_FILE_COUNT 12
%define RAW_FAT_SPT 63
%define RAW_FAT_HEADS 16
%define RAW_BOOT_DRIVE 0x80
%define RAW_DATA_LBA (73 + 2 * 128 + 32)
%define RAW_APPS_DIR_LBA (RAW_DATA_LBA + 8)
%define RAW_HDD_SOURCE_DRIVE 0x80
%define RAW_HDD_TARGET_DRIVE [raw_target_bios]
%define RAW_HDD_PARTITION_LBA 63
%ifndef RAW_HDD_PARTITION_SECTORS
%define RAW_HDD_PARTITION_SECTORS 0x00040000
%endif
%define RAW_HDD_CLONE_SECTORS (RAW_HDD_PARTITION_SECTORS + RAW_HDD_PARTITION_LBA)
%define RAW_HDD_CLONE_SECTORS_LO (RAW_HDD_CLONE_SECTORS & 0xFFFF)
%define RAW_HDD_CLONE_SECTORS_HI ((RAW_HDD_CLONE_SECTORS >> 16) & 0xFFFF)
; Keep each optical READ(10) to one native 2048-byte CD block.  Older ATAPI
; mechanisms (and marginal CD-RW media) are materially more reliable when a
; failed block can be retried in isolation instead of as a 4096-byte request.
%define RAW_HDD_BATCH_SECTORS    4
; Patch target for the installed-default-drive byte inside stage1 (the imm8
; of "mov byte [dos_default_drive], DOS_DEFAULT_DRIVE_INDEX"). The build
; computes LBA/offset from the stage1 listing and overrides these via -D;
; the fallbacks below are only for standalone assembly.
%ifndef RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA
%define RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA 64
%endif
%ifndef RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF
%define RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF 0x0136
%endif
%define RAW_STAGE1_LIVE_DRIVE_INDEX 3
%define RAW_STAGE1_INSTALLED_DRIVE_INDEX 2
%define RAW_HDD_SECTORS_PER_CYL 1008

; Direct ATA port I/O — used for all target HDD writes to avoid BIOS INT 13h
; wedge on the ThinkPad T23 (and similar hardware) where the BIOS write
; handler sometimes never returns after many sequential calls.
; ATA register offsets.  The actual command/control bases and master/slave
; bit are derived from the BIOS EDD 3.0 device path before any target write.
; This keeps the raw writer usable on both legacy IDE channels and avoids
; silently writing primary-master when BIOS drive 81h maps elsewhere.
%define ATA_REG_DATA    0x00
%define ATA_REG_FEATURE 0x01
%define ATA_REG_NSECT   0x02
%define ATA_REG_LBAL    0x03
%define ATA_REG_LBAM    0x04
%define ATA_REG_LBAH    0x05
%define ATA_REG_DEV     0x06
%define ATA_REG_STATUS  0x07
%define ATA_CMD_DEVICE_RESET 0x08
%define ATA_CMD_PACKET  0xA0
%define ATAPI_CMD_REQUEST_SENSE 0x03
%define ATAPI_CMD_READ10 0x28
%define ATAPI_READ_RETRIES 12
%ifndef ATAPI_MIRROR_BLOCKS
%define ATAPI_MIRROR_BLOCKS 0
%endif
%ifndef SETUP_ENABLE_RAW_HDD_INSTALL
%define SETUP_ENABLE_RAW_HDD_INSTALL 0
%endif
%ifndef SETUP_ENABLE_RAW_HDD_DESTRUCTIVE
%define SETUP_ENABLE_RAW_HDD_DESTRUCTIVE 0
%endif
%ifndef SETUP_LIVE_CD_MODE
%define SETUP_LIVE_CD_MODE 0
%endif
%ifndef SETUP_FORCE_MEMDISK_SOURCE
%define SETUP_FORCE_MEMDISK_SOURCE 0
%endif
%ifndef SETUP_RAW_TARGET_DRIVE_INDEX
%if SETUP_LIVE_CD_MODE
%define SETUP_RAW_TARGET_DRIVE_INDEX 2
%else
%define SETUP_RAW_TARGET_DRIVE_INDEX 3
%endif
%endif

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, setup_stack_top
    sti
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
    call gui_main
    mov ax, 0x4C00
    int 0x21
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
    ; Keep a failed install on the Live CD.  The target MBR is deliberately
    ; invalid until commit, and returning to D: avoids booting partial media.
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

%include "src/com/setup_disk.inc"
%include "src/com/setup_graphics.inc"

reboot_system:
    ; A bootstrap interrupt is not a hardware reset and preserves stale DMA,
    ; PCI and controller state.  Use the ICH reset register used by the T23,
    ; followed by the two standard PC fallbacks.
    push cs
    pop ds
    mov dx, msg_hardware_reset
    call serial_write_z
    call serial_write_crlf
    cli
    xor ax, ax
    mov ds, ax
    mov word [0x0472], ax
    mov dx, 0x0CF9
    mov al, 0x02
    out dx, al
    or al, 0x04
    out dx, al
    call .settle
    mov cx, 0x1000
.wait_8042:
    in al, 0x64
    test al, 0x02
    jz .pulse_8042
    loop .wait_8042
    jmp .fast_reset
.pulse_8042:
    mov al, 0xFE
    out 0x64, al
    call .settle
.fast_reset:
    in al,0x92
    and al,0xFE
    out 0x92,al
    or al,1
    out 0x92,al
    call .settle
.triple_fault:
    lidt [cs:.null_idt]
    int 3
    hlt
    jmp .triple_fault
.settle:
    ; Port reads provide an I/O delay even on a fast CPU; no BIOS or IRQ
    ; service is required while a controller completes its reset pulse.
    mov cx,0x8000
.delay:
    in al,0x80
    loop .delay
    ret
.null_idt:
    dw 0
    dd 0
msg_hardware_reset db '[SETUP] HARDWARE RESET',0

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
    mov byte [raw_last_status], 0
    mov byte [raw_default_drive_patched], 0
    mov byte [raw_clone_mbr_saved], 0
    mov dword [setup_source_crc], 0xFFFFFFFF
    mov byte [raw_atapi_ready], 0
    call raw_configure_atapi_source
    call setup_preflight
    jc .fail
    ; Transactional safety: invalidate sector zero before copying anything.
    ; The real MBR is committed only after every payload sector and cache
    ; flush succeeds, so an interrupted install cannot boot a partial image.
    call raw_invalidate_target_mbr
    jc .fail
    mov word [raw_clone_lba_lo], 0
    mov word [raw_clone_lba_hi], 0

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
    cmp byte [raw_atapi_ready], 1
    je .batch_track_safe
    ; A legacy CHS request must not cross a track boundary. Keep the batch
    ; count shared by the source read, target write and clone accounting.
    mov eax, [raw_clone_lba_lo]
    xor edx, edx
    movzx ebx, word [raw_source_spt]
    div ebx                    ; track numbers can exceed 65535
    sub bx, dx
    cmp cx, bx
    jbe .batch_track_safe
    mov cx, bx
.batch_track_safe:
    mov [batch_count], cx

    cmp byte [raw_atapi_ready], 1
    jne .bios_source_read
    mov bx, io_buffer
    mov cx, [batch_count]
    call raw_atapi_read_n
    jc .fail
    jmp .source_read_ok
.bios_source_read:
    mov dl, RAW_HDD_SOURCE_DRIVE
    mov bx, io_buffer
    mov cx, [batch_count]
    call raw_edd_read_n
    jnc .source_read_ok
    ; Some optical BIOSes advertise EDD but reject multi-sector requests
    ; after a number of transfers.  Retry the same LBA as one sector before
    ; declaring the medium unreadable; future iterations naturally continue
    ; from the following sector.
    cmp word [batch_count], 1
    jbe .fail
    mov word [batch_count], 1
    mov cx, 1
    call raw_edd_read_n
    jc .fail
.source_read_ok:
    call raw_stage_clone_mbr
    jc .fail
%if SETUP_LIVE_CD_MODE
    ; Patch D: to C: while the Stage1 sector is still in the source buffer.
    ; This avoids a post-clone BIOS read of the freshly written HDD.
    call raw_patch_clone_buffer_default_drive
    jc .fail
%endif

    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, io_buffer
    mov cx, [batch_count]
    mov word [raw_last_stage], 0x4157 ; stage='W', path='A' (ATA PIO)
    call raw_ata_write_n
    jnc .write_ok
    call raw_ata_write_n
    jc .fail
.write_ok:
    mov cx, [batch_count]
    call setup_verify_buffer
    jc .fail
    mov si, io_buffer
    mov eax, [setup_source_crc]
    call setup_crc_buffer
    mov [setup_source_crc], eax
    call gui_cancel_poll
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

    ; Do not periodically reset either device.  On real notebooks the HDD and
    ; optical drive often share one IDE controller; resetting that controller
    ; while the BIOS owns the CD invalidates later source reads.  Both the EDD
    ; reader and ATA writer already perform recovery only after an actual I/O
    ; failure.

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
    mov dx, msg_serial_hdd_install_progress
    call serial_write_z
    mov al, [clone_progress_pct]
    call serial_write_hex_byte
    call serial_write_crlf
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
    cmp byte [raw_default_drive_patched], 1
    je .patch_ok
    mov byte [raw_last_stage], 'P'
    mov byte [raw_last_path], 'B'
    mov byte [raw_last_status], 0xF1
    mov word [raw_clone_lba_lo], RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA
    mov word [raw_clone_lba_hi], 0
    jmp .fail
.patch_ok:
%endif
    cmp byte [raw_clone_mbr_saved], 1
    je .mbr_saved
    mov byte [raw_last_stage], 'C'
    mov byte [raw_last_path], 'M'
    mov byte [raw_last_status], 0xF3
    jmp .fail
.mbr_saved:
    ; Commit all cached payload writes while sector zero is still invalid.
    call raw_ata_flush_cache
    jc .fail
    call setup_verify_installed_bios
    jc .fail
    ; Atomic install commit: write the saved bootable MBR last, then force it
    ; to stable media before announcing success or allowing a reboot.
    mov byte [raw_last_stage], 'C'
    mov byte [raw_last_path], 'M'
    mov word [raw_clone_lba_lo], 0
    mov word [raw_clone_lba_hi], 0
    mov bx, raw_clone_mbr
    mov cx, 1
    call raw_ata_write_n
    jc .commit_failed
    call raw_ata_flush_cache
    jc .commit_failed
    mov bx, raw_clone_mbr
    mov cx, 1
    call setup_verify_expected
    jc .commit_failed
    mov dx, msg_serial_hdd_install_done
    call serial_write_z
    call serial_write_crlf
    clc
    jmp .out

.commit_failed:
    call setup_rollback_commit
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
    call print_hex_byte
    mov dx, msg_screen_hdd_install_detail
    call print_z
    mov al, [raw_last_detail]
    call print_hex_byte
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

; Preserve the source MBR on the first batch, but clear its signature in the
; version written during the main copy.  The saved sector is committed last.
raw_stage_clone_mbr:
    push ax
    push cx
    push di
    push si
    push es
    cmp word [raw_clone_lba_hi], 0
    jne .not_first
    cmp word [raw_clone_lba_lo], 0
    jne .not_first
    push cs
    pop es
    mov si, io_buffer
    mov di, raw_clone_mbr
    mov cx, 256
    rep movsw
    cmp word [raw_clone_mbr + 510], 0xAA55
    jne .fail
    mov word [io_buffer + 510], 0
    mov byte [raw_clone_mbr_saved], 1
.not_first:
    clc
    jmp .out
.fail:
    mov byte [raw_last_stage], 'C'
    mov byte [raw_last_path], 'M'
    mov byte [raw_last_status], 0xF4
    stc
.out:
    pop es
    pop si
    pop di
    pop cx
    pop ax
    ret

raw_invalidate_target_mbr:
    push ax
    push bx
    push cx
    push di
    push es
    push cs
    pop es
    xor ax, ax
    mov di, io_buffer
    mov cx, 256
    rep stosw
    mov byte [raw_last_stage], 'I'
    mov byte [raw_last_path], 'M'
    mov word [raw_clone_lba_lo], 0
    mov word [raw_clone_lba_hi], 0
    mov bx, io_buffer
    mov cx, 1
    call raw_ata_write_n
    jc .out
    call raw_ata_flush_cache
    jc .out
    call setup_verify_buffer
.out:
    pop es
    pop di
    pop cx
    pop bx
    pop ax
    ret

; Patch the one Stage1 byte in-flight when its source sector is in io_buffer.
; CF is set only if the expected Live-CD byte is present but invalid; batches
; which do not contain the patch LBA are left untouched.
raw_patch_clone_buffer_default_drive:
    push ax
    push bx
    push cx
    push dx
    push si

    cmp word [raw_clone_lba_hi], 0
    jne .not_this_batch
    mov ax, [raw_clone_lba_lo]
    cmp ax, RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA
    ja .not_this_batch
    mov dx, RAW_STAGE1_DEFAULT_DRIVE_PATCH_LBA
    sub dx, ax
    cmp dx, [batch_count]
    jae .not_this_batch

    mov ax, dx
    mov bx, 512
    mul bx
    add ax, RAW_STAGE1_DEFAULT_DRIVE_PATCH_OFF
    mov si, io_buffer
    add si, ax
    cmp byte [si], RAW_STAGE1_LIVE_DRIVE_INDEX
    jne .fail
    mov byte [si], RAW_STAGE1_INSTALLED_DRIVE_INDEX
    mov byte [raw_default_drive_patched], 1
.not_this_batch:
    clc
    jmp .out
.fail:
    mov byte [raw_last_stage], 'P'
    mov byte [raw_last_path], 'B'
    mov byte [raw_last_status], 0xF2
    stc
.out:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; BIOS reads use at most the 8-sector I/O buffers. A segment:offset range
; inside 64 KiB can still cross a physical 64 KiB DMA boundary. Keep normal
; batches unchanged, but bounce a crossing request through aligned sectors.
raw_edd_read_n:
    push eax
    push edx
    cmp cx,1
    jb .bad_buffer
    cmp cx,8
    ja .bad_buffer
    movzx edx,cx
    shl edx,9
    movzx eax,bx
    add eax,edx
    cmp eax,0x10000
    ja .bad_buffer
    mov ax,cs
    shl ax,4
    add ax,bx
    movzx eax,ax
    add eax,edx
    cmp eax,0x10000
    ja .bounce
    pop edx
    pop eax
    jmp raw_bios_read_direct
.bounce:
    pop edx
    pop eax
    pushad
    push ds
    push es
    push dword [cs:raw_clone_lba_lo]
    push cs
    pop ds
    push cs
    pop es
    cld
    mov di,bx
    mov bp,cx
    call setup_bios_bounce_address
    mov si,bx
.sector:
    mov bx,si
    mov cx,1
    call raw_bios_read_direct
    jc .restore                  ; never copy a failed/stale sector
    push si
    mov cx,256
    rep movsw
    pop si
    inc dword [raw_clone_lba_lo]
    dec bp
    jnz .sector
    clc
.restore:
    pop dword [cs:raw_clone_lba_lo]
    pop es
    pop ds
    popad
    ret
.bad_buffer:
    pop edx
    pop eax
    mov byte [cs:raw_last_stage],'R'
    mov byte [cs:raw_last_path],'E'
    mov byte [cs:raw_last_status],0x09
    stc
    ret

; BX = private 512-byte buffer, aligned in physical memory (CS may be any
; paragraph). No sector at this address can cross a 64 KiB DMA boundary.
setup_bios_bounce_address:
    push ax
    mov ax,cs
    shl ax,4
    add ax,setup_bios_bounce_storage
    neg ax
    and ax,511
    add ax,setup_bios_bounce_storage
    mov bx,ax
    pop ax
    ret

raw_bios_read_direct:
    mov byte [raw_last_stage], 'R'
    mov byte [raw_last_path], 'E'
    mov ah, 0x42
    push dx
    call raw_edd_transfer_current_lba
    pop dx
    jnc .done
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

; Discover the physical ATAPI source from the El Torito specification packet.
; AH=4Bh/AL=01h is status-only: emulation stays active, but the packet returns
; the boot-image CD LBA, controller index and IDE master/slave bit.
raw_configure_atapi_source:
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    push es
    push cs
    pop es
    push cs
    pop ds

    mov byte [raw_atapi_ready], 0
%if SETUP_FORCE_MEMDISK_SOURCE
    ; The release CD is already running from a MEMDISK-owned BIOS drive.
    ; Never ask the underlying IBM/Phoenix El Torito BIOS for its stale boot
    ; packet here: some implementations return the physical ATAPI mechanism,
    ; which silently puts the clone back on the freeze-prone optical path.
    mov dx, msg_serial_cd_map
    call serial_write_z
    mov al, 'R'                ; forced RAM-backed BIOS source
    call serial_write_char
    call serial_write_crlf
    clc
    jmp .out
%endif
    xor ax, ax
    mov di, eltorito_spec_packet
    mov cx, 10
    rep stosw
    mov byte [eltorito_spec_packet], 0x13
    mov si, eltorito_spec_packet
    mov dl, RAW_HDD_SOURCE_DRIVE
    mov ax, 0x4B01             ; return status, do not terminate emulation
    call setup_bios_disk
    push cs
    pop ds
    jc .fallback
    cmp byte [eltorito_spec_packet], 0x13
    jb .fallback
    mov al, [eltorito_spec_packet + 1]
    test al, 0x80              ; SCSI source cannot use legacy ATAPI ports
    jnz .fallback
    and al, 0x0F
    cmp al, 4                  ; hard-disk emulation
    jne .fallback
    cmp byte [eltorito_spec_packet + 2], RAW_HDD_SOURCE_DRIVE
    jne .fallback
    mov al, [eltorito_spec_packet + 3]
    cmp al, 1                  ; legacy primary or secondary IDE channel
    ja .fallback
    or al, al
    jnz .secondary
    mov word [atapi_cmd_base], 0x01F0
    mov word [atapi_ctrl_base], 0x03F6
    jmp .device
.secondary:
    mov word [atapi_cmd_base], 0x0170
    mov word [atapi_ctrl_base], 0x0376
.device:
    mov byte [atapi_dev_select], 0xA0
    test byte [eltorito_spec_packet + 8], 1
    jz .image_lba
    or byte [atapi_dev_select], 0x10
.image_lba:
    mov ax, [eltorito_spec_packet + 4]
    mov [atapi_image_lba_lo], ax
    mov ax, [eltorito_spec_packet + 6]
    mov [atapi_image_lba_hi], ax
    or ax, [atapi_image_lba_lo]
    jz .fallback
    mov byte [raw_atapi_ready], 1
    call raw_report_atapi_mapping
    clc
    jmp .out
.fallback:
    mov dx, msg_serial_cd_map
    call serial_write_z
    mov al, 'B'                ; BIOS-emulation fallback
    call serial_write_char
    call serial_write_crlf
    stc
.out:
    pop es
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

raw_report_atapi_mapping:
    push ax
    push dx
    mov dx, msg_serial_cd_map
    call serial_write_z
    mov al, 'T'                ; direct ATAPI transport
    call serial_write_char
    mov dx, msg_serial_ata_cmd
    call serial_write_z
    mov ax, [atapi_cmd_base]
    call serial_write_hex_word
    mov dx, msg_serial_ata_ctrl
    call serial_write_z
    mov ax, [atapi_ctrl_base]
    call serial_write_hex_word
    mov dx, msg_serial_ata_dev
    call serial_write_z
    mov al, [atapi_dev_select]
    call serial_write_hex_byte
    mov dx, msg_serial_cd_image
    call serial_write_z
    mov ax, [atapi_image_lba_hi]
    call serial_write_hex_word
    mov al, ':'
    call serial_write_char
    mov ax, [atapi_image_lba_lo]
    call serial_write_hex_word
    call serial_write_crlf
    pop dx
    pop ax
    ret

; Read CX virtual 512-byte sectors from the contiguous El Torito boot image.
; Clone batches are aligned to eight virtual sectors, so one READ(10) fetches
; one or two native 2048-byte CD blocks directly into CS:BX.
raw_atapi_read_n:
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    push bp
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es

    mov byte [raw_last_stage], 'R'
    mov byte [raw_last_path], 'T'
    mov byte [atapi_using_mirror], 0
    mov byte [atapi_sense_key], 0xFF
    mov byte [atapi_sense_asc], 0xFF
    mov byte [atapi_sense_ascq], 0xFF
    test word [raw_clone_lba_lo], 3
    jnz .bad_alignment
    cmp cx, 1
    jb .bad_alignment
    cmp cx, RAW_HDD_BATCH_SECTORS
    ja .bad_alignment
    mov [atapi_buffer_ptr], bx
    mov ax, cx
    add ax, 3
    shr ax, 1
    shr ax, 1
    mov [atapi_block_count], al
    mov cx, ax
    shl ax, 11                 ; native CD block count * 2048
    mov [atapi_bytes_remaining], ax
    mov [atapi_transfer_bytes], ax

    ; Convert virtual 512-byte LBA to native CD LBA and add image start.
    mov ax, [raw_clone_lba_lo]
    mov dx, [raw_clone_lba_hi]
    shr dx, 1
    rcr ax, 1
    shr dx, 1
    rcr ax, 1
    add ax, [atapi_image_lba_lo]
    adc dx, [atapi_image_lba_hi]
    mov [atapi_cd_lba_lo], ax
    mov [atapi_cd_lba_hi], dx

    xor ax, ax
    mov di, atapi_packet
    mov cx, 6
    rep stosw
    mov byte [atapi_packet + 0], ATAPI_CMD_READ10
    mov ax, [atapi_cd_lba_lo]
    mov dx, [atapi_cd_lba_hi]
    mov byte [atapi_packet + 2], dh
    mov byte [atapi_packet + 3], dl
    mov byte [atapi_packet + 4], ah
    mov byte [atapi_packet + 5], al
    mov al, [atapi_block_count]
    mov byte [atapi_packet + 8], al

    ; One initial attempt plus ATAPI_READ_RETRIES recovery attempts.
    mov byte [atapi_retries_left], ATAPI_READ_RETRIES + 1

.retry_read:
    ; A failed PIO phase may already have consumed some bytes.  Always restart
    ; the same native block at the original destination on each retry.
    mov ax, [atapi_transfer_bytes]
    mov [atapi_bytes_remaining], ax
    mov byte [raw_last_detail], 0

    mov dx, [atapi_ctrl_base]
    mov al, 0x02                ; polled command, no BIOS IRQ15/14
    out dx, al
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_DEV
    mov al, [atapi_dev_select]
    out dx, al
    mov dx, [atapi_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    mov bp, 0x0020
.ready_outer:
    xor cx, cx
.ready:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x80
    jz .program_packet
    loop .ready
    dec bp
    jnz .ready_outer
    jmp .fail

.program_packet:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_FEATURE
    xor al, al                 ; PIO, no DMA, no overlap
    out dx, al
    inc dx                     ; interrupt reason / sector-count register
    out dx, al
    inc dx                     ; LBA low
    out dx, al
    inc dx                     ; byte-count low (LBA mid)
    mov ax, [atapi_transfer_bytes]
    out dx, al
    inc dx                     ; byte-count high (LBA high)
    mov al, ah
    out dx, al
    inc dx                     ; device/head
    inc dx                     ; command/status
    mov al, ATA_CMD_PACKET
    out dx, al
    mov dx, [atapi_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    mov bp, 0x0020
.packet_outer:
    xor cx, cx
.packet_wait:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x80
    jnz .packet_loop
    test al, 0x01
    jnz .fail
    test al, 0x08
    jnz .send_packet
.packet_loop:
    loop .packet_wait
    dec bp
    jnz .packet_outer
    jmp .fail

.send_packet:
    mov dx, [atapi_cmd_base]
    mov si, atapi_packet
    mov cx, 6
    rep outsw
    mov di, [atapi_buffer_ptr]

.data_phase:
    mov bp, 0x0040
.data_outer:
    xor cx, cx
.data_wait:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x80
    jnz .data_loop
    test al, 0x01
    jnz .fail
    test al, 0x08
    jnz .transfer_data
    cmp word [atapi_bytes_remaining], 0
    je .success
.data_loop:
    loop .data_wait
    dec bp
    jnz .data_outer
    jmp .fail

.transfer_data:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_LBAM
    in al, dx
    mov bl, al
    inc dx
    in al, dx
    mov bh, al
    or bx, bx
    jz .protocol_fail
    test bl, 1
    jnz .protocol_fail
    cmp bx, [atapi_bytes_remaining]
    ja .protocol_fail
    sub [atapi_bytes_remaining], bx
    mov cx, bx
    shr cx, 1
    mov dx, [atapi_cmd_base]
    rep insw
    jmp .data_phase

.bad_alignment:
    mov al, 0xF5
    jmp .terminal_fail
.protocol_fail:
    mov al, 0xF6
    jmp .terminal_fail
.fail:
    ; Status 51h is the normal ATAPI CHECK CONDITION completion.  Preserve
    ; both the status and the Error register (whose high nibble is the sense
    ; key), then retry the exact same CD block.  Real optical drives may raise
    ; transient UNIT ATTENTION / NOT READY / recovered-media conditions.
    mov [raw_last_status], al
    mov [atapi_last_status], al
    test al, 0x01
    jz .retry_decide
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_FEATURE
    in al, dx
    mov [raw_last_detail], al
    mov [atapi_last_error], al
.retry_decide:
    ; CHECK CONDITION is followed by REQUEST SENSE before another READ(10).
    ; Besides producing useful ASC/ASCQ diagnostics, this clears the packet
    ; device's contingent error state on older ATAPI mechanisms.
    call raw_atapi_request_sense
    call raw_report_atapi_retry
    dec byte [atapi_retries_left]
    jz .retry_exhausted
    call raw_atapi_retry_pause
    ; Periodically reset only the selected packet device.  DEVICE RESET is
    ; mandatory for ATAPI and, unlike channel SRST, does not disturb the HDD.
    mov al, [atapi_retries_left]
    and al, 3
    jnz .retry_read
    call raw_atapi_device_reset
    jmp .retry_read
.retry_exhausted:
%if ATAPI_MIRROR_BLOCKS > 0
    ; The direct CD image contains a second 2048-byte-aligned copy of the
    ; entire emulated disk.  A persistent medium error in the primary extent
    ; therefore gets a fresh physical location before installation is failed.
    cmp byte [atapi_using_mirror], 0
    jne .mirror_exhausted
    mov byte [atapi_using_mirror], 1
    mov byte [raw_last_path], 'M'
    add word [atapi_cd_lba_lo], (ATAPI_MIRROR_BLOCKS & 0xFFFF)
    adc word [atapi_cd_lba_hi], ((ATAPI_MIRROR_BLOCKS >> 16) & 0xFFFF)
    mov ax, [atapi_cd_lba_lo]
    mov dx, [atapi_cd_lba_hi]
    mov byte [atapi_packet + 2], dh
    mov byte [atapi_packet + 3], dl
    mov byte [atapi_packet + 4], ah
    mov byte [atapi_packet + 5], al
    mov byte [atapi_retries_left], ATAPI_READ_RETRIES + 1
    mov dx, msg_serial_cd_mirror
    call serial_write_z
    call serial_write_crlf
    jmp .retry_read
.mirror_exhausted:
%endif
    mov al, [atapi_last_status]
.terminal_fail:
    mov [raw_last_status], al
    stc
    jmp .out
.success:
    clc
.out:
    pushf
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    mov dx, [atapi_ctrl_base]
    xor al, al
    out dx, al
    popf
    pop es
    pop ds
    pop bp
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Fetch fixed-format sense data after an ATAPI CHECK CONDITION.  The original
; Status/Error bytes remain in raw_last_* while these three fields capture the
; command-set-specific reason (sense key, ASC and ASCQ).
raw_atapi_request_sense:
    pusha
    push es
    push cs
    pop es
    xor ax, ax
    mov di, atapi_sense_data
    mov cx, 9
    rep stosw

    mov dx, [atapi_ctrl_base]
    mov al, 0x02
    out dx, al
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_DEV
    mov al, [atapi_dev_select]
    out dx, al
    mov dx, [atapi_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    mov cx, 0
.ready:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x80
    jz .program
    loop .ready
    jmp .fail
.program:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_FEATURE
    xor al, al
    out dx, al
    inc dx
    out dx, al
    inc dx
    out dx, al
    inc dx
    mov al, 18
    out dx, al
    inc dx
    xor al, al
    out dx, al
    inc dx
    mov al, [atapi_dev_select]
    out dx, al
    inc dx
    mov al, ATA_CMD_PACKET
    out dx, al

    mov cx, 0
.packet_wait:
    in al, dx
    test al, 0x80
    jnz .packet_loop
    test al, 0x01
    jnz .fail
    test al, 0x08
    jnz .send_packet
.packet_loop:
    loop .packet_wait
    jmp .fail
.send_packet:
    mov dx, [atapi_cmd_base]
    mov si, atapi_sense_packet
    mov cx, 6
    rep outsw

    mov cx, 0
.data_wait:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x80
    jnz .data_loop
    test al, 0x01
    jnz .fail
    test al, 0x08
    jnz .transfer
.data_loop:
    loop .data_wait
    jmp .fail
.transfer:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_LBAM
    in al, dx
    mov bl, al
    inc dx
    in al, dx
    mov bh, al
    or bx, bx
    jz .fail
    test bl, 1
    jnz .fail
    mov cx, bx
    shr cx, 1
    mov dx, [atapi_cmd_base]
    mov di, atapi_sense_data
.read_word:
    in ax, dx
    cmp di, atapi_sense_data + 18
    jae .discard
    stosw
.discard:
    loop .read_word
    mov al, [atapi_sense_data + 2]
    and al, 0x0F
    mov [atapi_sense_key], al
    mov al, [atapi_sense_data + 12]
    mov [atapi_sense_asc], al
    mov al, [atapi_sense_data + 13]
    mov [atapi_sense_ascq], al
    clc
    jmp .out
.fail:
    stc
.out:
    pop es
    popa
    ret

; Pause for two BIOS timer ticks (about 110 ms) between optical retries.  This
; gives a slow mechanism time to finish seek/error recovery without depending
; on CPU-speed-sensitive empty polling loops.
raw_atapi_retry_pause:
    push ax
    push bx
    push dx
    mov ah, 0x00
    call setup_bios_time
    mov bx, dx
.wait:
    mov ah, 0x00
    call setup_bios_time
    mov ax, dx
    sub ax, bx
    cmp ax, 2
    jb .wait
    pop dx
    pop bx
    pop ax
    ret

; DEVICE RESET (08h) targets only the selected ATAPI device.  It is used
; after repeated completed/failed packet commands; channel-wide SRST remains
; deliberately prohibited because an HDD may share the channel.
raw_atapi_device_reset:
    push ax
    push cx
    push dx
    push di
    mov dx, [atapi_ctrl_base]
    mov al, 0x02
    out dx, al
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_DEV
    mov al, [atapi_dev_select]
    out dx, al
    mov dx, [atapi_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    mov al, ATA_CMD_DEVICE_RESET
    out dx, al
    mov dx, [atapi_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    mov di, 0x0100
.outer:
    xor cx, cx
.wait:
    mov dx, [atapi_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x80
    jz .done
    loop .wait
    dec di
    jnz .outer
.done:
    pop di
    pop dx
    pop cx
    pop ax
    ret

raw_report_atapi_retry:
    push ax
    push dx
    mov dx, msg_serial_cd_retry
    call serial_write_z
    mov ax, [raw_clone_lba_hi]
    call serial_write_hex_word
    mov al, ':'
    call serial_write_char
    mov ax, [raw_clone_lba_lo]
    call serial_write_hex_word
    mov dx, msg_serial_cd_retry_status
    call serial_write_z
    mov al, [raw_last_status]
    call serial_write_hex_byte
    mov dx, msg_serial_cd_retry_error
    call serial_write_z
    mov al, [raw_last_detail]
    call serial_write_hex_byte
    mov dx, msg_serial_cd_retry_left
    call serial_write_z
    mov al, [atapi_retries_left]
    dec al
    call serial_write_hex_byte
    mov dx, msg_serial_cd_retry_sense
    call serial_write_z
    mov al, [atapi_sense_key]
    call serial_write_hex_byte
    mov al, '/'
    call serial_write_char
    mov al, [atapi_sense_asc]
    call serial_write_hex_byte
    mov al, '/'
    call serial_write_char
    mov al, [atapi_sense_ascq]
    call serial_write_hex_byte
    call serial_write_crlf
    pop dx
    pop ax
    ret

; Resolve BIOS drive 81h to a legacy ATA command block through the EDD 3.0
; device-path extension or the older EDD DPTE.  The DPTE is preferred because
; it is available on the ThinkPad-era EDD 1.x/2.x BIOSes and publishes the
; exact task-file ports and ATA DEV bit selected by INT 13h.
raw_configure_ata_target:
    push ax
    push bx
    push cx
    push dx
    push di
    push si
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es

    mov byte [raw_last_stage], 'M'
    mov byte [raw_last_path], 'E'
    ; Older BIOSes may return less than the requested EDD 3.0 structure.
    ; Clear it first so a second FORMAT/INSTALL run cannot reuse stale DPTE
    ; or BEDD bytes left by an earlier invocation.
    xor ax, ax
    mov di, edd_drive_params
    mov cx, 0x004A / 2
    rep stosw
    mov word [edd_drive_params], 0x004A
    mov si, edd_drive_params
    mov dl, RAW_HDD_TARGET_DRIVE
    mov ah, 0x48
    call setup_bios_disk
    push cs
    pop ds
    jc .mapping_fail

    ; EDD 1.x/2.x Drive Parameter Table Extension.  This is the most direct
    ; BIOS-to-hardware mapping and works even when the BEDD device path below
    ; is absent, as on some IBM/Phoenix notebook BIOS revisions.
    call raw_map_ata_from_dpte
    jnc .mapping_ready

    mov byte [raw_last_path], '3'
    cmp word [edd_drive_params + 0x1E], 0xBEDD
    jne .mapping_fail
    cmp byte [edd_drive_params + 0x28], 'A'
    jne .mapping_fail
    cmp byte [edd_drive_params + 0x29], 'T'
    jne .mapping_fail
    cmp byte [edd_drive_params + 0x2A], 'A'
    jne .mapping_fail

    ; The EDD ISA interface path publishes the command-block base only.
    ; For an ATA compatibility task file, the alternate-status/device-control
    ; register is command base + 206h (1F0h -> 3F6h, 170h -> 376h).
    cmp byte [edd_drive_params + 0x24], 'I'
    jne .try_pci
    cmp byte [edd_drive_params + 0x25], 'S'
    jne .mapping_fail
    cmp byte [edd_drive_params + 0x26], 'A'
    jne .mapping_fail
    mov ax, [edd_drive_params + 0x30]
    or ax, ax
    jz .mapping_fail
    cmp ax, 0xFFFF
    je .mapping_fail
    mov [ata_cmd_base], ax
    add ax, 0x0206
    jc .mapping_fail
    mov [ata_ctrl_base], ax
    jmp .device

.try_pci:
    ; Resolve the exact PCI function named by EDD. Compatibility-mode channels
    ; use the ISA task files; native-mode channels use their assigned I/O BARs.
    cmp byte [edd_drive_params + 0x24], 'P'
    jne .mapping_fail
    cmp byte [edd_drive_params + 0x25], 'C'
    jne .mapping_fail
    cmp byte [edd_drive_params + 0x26], 'I'
    jne .mapping_fail
    mov bh, [edd_drive_params + 0x30]
    mov bl, [edd_drive_params + 0x31]
    cmp bl, 31
    ja .mapping_fail
    shl bl, 3
    mov al, [edd_drive_params + 0x32]
    cmp al, 7
    ja .mapping_fail
    or bl, al
    mov [ata_pci_bdf], bx

    mov ax, 0xB108              ; PCI BIOS: read configuration byte
    mov di, 0x0009              ; programming interface
    stc
    call setup_bios_pci
    jc .mapping_fail
    push cs
    pop ds
    mov al, [edd_drive_params + 0x33]
    cmp al, 0
    je .pci_primary
    cmp al, 1
    jne .mapping_fail
    test cl, 0x04               ; secondary channel native-mode bit
    jnz .pci_secondary_native
    mov word [ata_cmd_base], 0x0170
    mov word [ata_ctrl_base], 0x0376
    jmp .device
.pci_primary:
    test cl, 0x01               ; primary channel native-mode bit
    jnz .pci_primary_native
    mov word [ata_cmd_base], 0x01F0
    mov word [ata_ctrl_base], 0x03F6
    jmp .device

.pci_primary_native:
    mov di, 0x0010              ; BAR0 command, BAR1 control
    mov si, 0x0014
    jmp .pci_native
.pci_secondary_native:
    mov di, 0x0018              ; BAR2 command, BAR3 control
    mov si, 0x001C
.pci_native:
    mov bx, [ata_pci_bdf]
    mov ax, 0xB10A              ; PCI BIOS: read configuration dword
    stc
    push si
    call setup_bios_pci
    pop si
    jc .mapping_fail
    push cs
    pop ds
    test cl, 1                  ; only I/O BARs are usable in real mode
    jz .mapping_fail
    test ecx, 0xFFFF0000
    jnz .mapping_fail
    and cx, 0xFFFC
    jz .mapping_fail
    mov [ata_cmd_base], cx

    mov bx, [ata_pci_bdf]
    mov di, si
    mov ax, 0xB10A
    stc
    call setup_bios_pci
    jc .mapping_fail
    push cs
    pop ds
    test cl, 1
    jz .mapping_fail
    test ecx, 0xFFFF0000
    jnz .mapping_fail
    and cx, 0xFFFC
    jz .mapping_fail
    add cx, 2                   ; BAR is control block; alt-status is +2
    jc .mapping_fail
    mov [ata_ctrl_base], cx

.device:
    mov al, [edd_drive_params + 0x38]
    cmp al, 1
    ja .mapping_fail
    mov byte [ata_dev_select], 0xE0
    or al, al
    jz .mapped
    or byte [ata_dev_select], 0x10
.mapped:
    clc
    jmp .mapping_ready

.mapping_fail:
    ; Last-resort compatibility path for pre-EDD-DPTE firmware: accept a
    ; legacy IDE mapping only when exactly one real ATA disk responds across
    ; the two standard channels.  ATAPI optical devices are rejected by the
    ; IDENTIFY DEVICE handshake, so an ambiguous multi-HDD system is never
    ; written blindly.
    mov byte [raw_last_path], 'S'
    ; One ATA disk does not imply one BIOS target (another disk can be SCSI
    ; or USB). Without an authoritative mapping, only the sole displayed BIOS
    ; target is eligible; an ambiguous selection must remain read-only.
    cmp byte [gui_disk_count], 1
    jne .mapping_rejected
    cmp byte [raw_target_bios], 0x81
    jne .mapping_rejected
    call raw_map_single_legacy_ata
    jnc .mapping_ready
.mapping_rejected:
    mov byte [raw_last_status], 0xF0
    stc
    jmp .mapping_out
.mapping_ready:
    call raw_report_ata_mapping
    clc
.mapping_out:
    pop es
    pop ds
    pop si
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Map the EDD drive through the 16-byte Device Parameter Table Extension.
; The far pointer is returned by INT 13h/AH=48h at parameter offset 1Ah.
raw_map_ata_from_dpte:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    mov byte [raw_last_path], 'D'
    mov di, [edd_drive_params + 0x1A]
    mov ax, [edd_drive_params + 0x1C]
    or ax, ax
    jz .fail
    cmp ax, 0xFFFF
    je .fail
    cmp di, 0xFFF0
    ja .fail
    mov es, ax

    ; Bytes 0..15, including the two's-complement checksum, must sum to zero.
    xor bx, bx
    mov cx, 16
.checksum:
    mov al, [es:di]
    add bl, al
    inc di
    loop .checksum
    or bl, bl
    jnz .fail
    sub di, 16

    ; Reject an ATAPI device and malformed task-file addresses.
    test byte [es:di + 10], 0x40
    jnz .fail
    mov ax, [es:di]
    mov dx, [es:di + 2]
    or ax, ax
    jz .fail
    cmp ax, 0xFFFF
    je .fail
    or dx, dx
    jz .fail
    cmp dx, 0xFFFF
    je .fail
    mov [ata_cmd_base], ax
    mov [ata_ctrl_base], dx

    mov al, [es:di + 4]
    and al, 0x10
    or al, 0xE0
    mov [ata_dev_select], al
    clc
    jmp .out
.fail:
    stc
.out:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Probe standard primary/secondary IDE channels.  Success is deliberately
; limited to the unambiguous case of one ATA disk; ATAPI CD/DVD devices do
; not complete command ECh as ATA IDENTIFY DEVICE.
raw_map_single_legacy_ata:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es

    mov byte [ata_probe_count], 0
    mov si, ata_legacy_candidates
    mov cx, 4
.next:
    mov ax, [si]
    mov [ata_probe_cmd], ax
    mov ax, [si + 2]
    mov [ata_probe_ctrl], ax
    mov al, [si + 4]
    mov [ata_probe_dev], al
    push cx
    call raw_probe_legacy_ata
    pop cx
    jc .advance
    inc byte [ata_probe_count]
    cmp byte [ata_probe_count], 1
    jne .advance
    mov ax, [ata_probe_cmd]
    mov [ata_probe_saved_cmd], ax
    mov ax, [ata_probe_ctrl]
    mov [ata_probe_saved_ctrl], ax
    mov al, [ata_probe_dev]
    or al, 0x40
    mov [ata_probe_saved_dev], al
.advance:
    add si, 6
    loop .next

    cmp byte [ata_probe_count], 1
    jne .fail
    mov ax, [ata_probe_saved_cmd]
    mov [ata_cmd_base], ax
    mov ax, [ata_probe_saved_ctrl]
    mov [ata_ctrl_base], ax
    mov al, [ata_probe_saved_dev]
    mov [ata_dev_select], al

    ; The scan may have left the optical device with an aborted ATA command.
    ; Reset the BIOS source once, before cloning starts (never mid-copy).
    xor ax, ax
    mov dl, RAW_HDD_SOURCE_DRIVE
    call setup_bios_disk
    push cs
    pop ds
    clc
    jmp .out
.fail:
    stc
.out:
    pop es
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

raw_probe_legacy_ata:
    push ax
    push cx
    push dx
    push di

    mov dx, [ata_probe_ctrl]
    mov al, 0x02                ; nIEN: no IRQ into an unowned BIOS handler
    out dx, al
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    mov dx, [ata_probe_cmd]
    add dx, ATA_REG_DEV
    mov al, [ata_probe_dev]
    out dx, al
    mov dx, [ata_probe_ctrl]
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    mov dx, [ata_probe_cmd]
    add dx, ATA_REG_NSECT
    xor al, al
    out dx, al
    inc dx
    out dx, al
    inc dx
    out dx, al
    inc dx
    out dx, al
    mov dx, [ata_probe_cmd]
    add dx, ATA_REG_STATUS
    mov al, 0xEC
    out dx, al
    in al, dx
    or al, al
    jz .fail
    cmp al, 0xFF
    je .fail

    xor cx, cx
.wait:
    in al, dx
    test al, 0x80
    jnz .continue
    test al, 0x01
    jnz .fail
    test al, 0x08
    jnz .read_identify
.continue:
    loop .wait
    jmp .fail

.read_identify:
    mov dx, [ata_probe_cmd]
    mov di, io_buffer
    mov cx, 256
    rep insw
    clc
    jmp .out
.fail:
    stc
.out:
    ; Acknowledge pending INTRQ before restoring the interrupt-enabled state
    ; expected by BIOS-owned CD reads.  Preserve the probe result in FLAGS.
    pushf
    mov dx, [ata_probe_cmd]
    add dx, ATA_REG_STATUS
    in al, dx
    mov dx, [ata_probe_ctrl]
    xor al, al
    out dx, al
    popf
    pop di
    pop dx
    pop cx
    pop ax
    ret

raw_report_ata_mapping:
    push ax
    push dx
    mov dx, msg_serial_ata_map
    call serial_write_z
    mov al, [raw_last_path]
    call serial_write_char
    mov dx, msg_serial_ata_cmd
    call serial_write_z
    mov ax, [ata_cmd_base]
    call serial_write_hex_word
    mov dx, msg_serial_ata_ctrl
    call serial_write_z
    mov ax, [ata_ctrl_base]
    call serial_write_hex_word
    mov dx, msg_serial_ata_dev
    call serial_write_z
    mov al, [ata_dev_select]
    call serial_write_hex_byte
    call serial_write_crlf
    pop dx
    pop ax
    ret

; raw_ata_write_n: write CX sectors from CS:BX to the EDD-mapped ATA HDD
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
    mov byte [ata_transfer_command], 0x30
    jmp raw_ata_transfer_n
raw_ata_read_n:
    mov byte [ata_transfer_command], 0x20
raw_ata_transfer_n:
    push es
    pushad
    push ds
    push cs
    pop ds                      ; DS=CS so rep outsw addresses CS:SI
    cld
    push cs
    pop es
    ; Reject malformed requests before touching the controller. Every public
    ; installer operation is limited to the displayed system region, and the
    ; private buffers contain at most eight sectors without segment wrapping.
    test cx, cx
    jz .ata_bad_request
    cmp cx, 8
    ja .ata_bad_request
    movzx eax, cx
    add eax, [raw_clone_lba_lo]
    jc .ata_bad_request
    cmp eax, RAW_HDD_CLONE_SECTORS
    ja .ata_bad_request
    cmp eax, [setup_disk_sectors]
    ja .ata_bad_request
    mov ax, cx
    shl ax, 9
    add ax, bx
    jc .ata_bad_request
    mov si, bx                  ; CS:SI = transfer buffer
    mov ax, [raw_clone_lba_lo]
    mov [ata_cur_lba_lo], ax
    mov ax, [raw_clone_lba_hi]
    mov [ata_cur_lba_hi], ax
    mov [ata_sectors_left], cx

.ata_next_sector:
    ; Own the polling transaction without generating IRQ14/15 into a BIOS
    ; handler which did not start it.  On a real PIIX4, repeated unsolicited
    ; disk IRQs can corrupt the following BIOS ATAPI read.
    mov dx, [ata_ctrl_base]
    mov al, 0x02                ; Device Control nIEN
    out dx, al
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    ; Device/Head is itself a task-file register: the previously selected
    ; device must finish BSY/DRQ before selection changes. An absent device
    ; (floating FFh or 00h) is allowed here, before selecting our known HDD.
    mov di, 0x0020
.ata_channel_outer:
    xor cx, cx
.ata_channel_wait:
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    cmp al, 0xFF
    je .ata_select
    test al, 0x88
    jz .ata_select
    loop .ata_channel_wait
    dec di
    jnz .ata_channel_outer
    jmp .ata_write_fail
.ata_select:
    ; A BIOS source read may have left the CD/DVD selected on this channel.
    ; Select the HDD, wait 400 ns, then poll it before programming the LBA.
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_DEV
    mov al, [ata_dev_select]
    or  al, [ata_cur_lba_hi + 1]
    out dx, al
    mov dx, [ata_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    ; --- Wait for selected drive ready (BSY=0) ---
    mov di, 0x0020              ; bounded long timeout for old/slow disks
.ata_bsy0_outer:
    xor cx, cx                  ; 65536 iterations ≈ 33 ms at 1 GHz
.ata_bsy0:
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x88               ; BSY or pending data phase (DRQ)?
    jnz .ata_ready_loop
    test al, 0x40               ; selected ATA device must also be DRDY
    jnz .ata_bsy0_ok
.ata_ready_loop:
    loop .ata_bsy0
    dec di
    jnz .ata_bsy0_outer
    jmp .ata_write_fail

.ata_bsy0_ok:
    ; --- Program LBA and device registers ---
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_NSECT
    mov al, 1
    out dx, al

    mov ax, [ata_cur_lba_lo]
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_LBAL
    out dx, al                  ; bits [7:0]
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_LBAM
    mov al, ah
    out dx, al                  ; bits [15:8]

    mov ax, [ata_cur_lba_hi]
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_LBAH
    out dx, al                  ; bits [23:16]

    ; --- Issue WRITE SECTORS (0x30) ---
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS      ; same port address as command register
    mov al, [ata_transfer_command]
    out dx, al

    ; 400 ns settling delay: read alt-status 4× (each I/O ≈ 100 ns)
    mov dx, [ata_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    ; --- Wait for DRQ=1, BSY=0 (drive ready for data) ---
    mov di, 0x0020
.ata_drq_outer:
    xor cx, cx
.ata_drq_wait:
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x80               ; BSY still set?
    jnz .ata_drq_loop
    test al, 0x21               ; DF or ERR?
    jnz .ata_write_fail
    test al, 0x08               ; DRQ?
    jnz .ata_do_write
.ata_drq_loop:
    loop .ata_drq_wait
    dec di
    jnz .ata_drq_outer
    jmp .ata_write_fail

.ata_do_write:
    ; --- Transfer 256 words (512 bytes) to ATA data register ---
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_DATA
    mov cx, 256
    cmp byte [ata_transfer_command], 0x20
    je .ata_do_read
    rep outsw
    jmp .ata_data_done
.ata_do_read:
    mov di, si
    rep insw
    mov si, di
.ata_data_done:
    ; Status can still describe the data phase for 400 ns after its last word.
    ; Wait before interpreting it; completion requires BOTH BSY and DRQ clear.
    mov dx, [ata_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    mov di, 0x0020
.ata_bsy1_outer:
    xor cx, cx
.ata_bsy1:
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x88
    jnz .ata_bsy1_loop
    test al, 0x21               ; DF or ERR after the data phase
    jnz .ata_write_fail
    jmp .ata_sector_ok
.ata_bsy1_loop:
    loop .ata_bsy1
    dec di
    jnz .ata_bsy1_outer
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
    mov [raw_last_status], al
    stc
    jmp .ata_write_out
.ata_bad_request:
    mov byte [raw_last_status], 0xF6
    stc
    jmp .ata_restore

.ata_write_out:
    ; A regular-status read acknowledges pending INTRQ before nIEN is
    ; cleared.  Never soft-reset here: the live CD may share this controller,
    ; and SRST invalidates the BIOS' El-Torito source state.
    pushf
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    mov dx, [ata_ctrl_base]
    xor al, al
    out dx, al
    popf
.ata_restore:
    pop ds
    popad
    pop es
    ret

; Commit the target drive's volatile write cache before reporting FORMAT or
; INSTALL complete.  ATA FLUSH CACHE (E7h) is the LBA28 command supported by
; the ThinkPad-era disks targeted by this installer.
raw_ata_flush_cache:
    push ax
    push cx
    push dx
    push di

    mov byte [raw_last_stage], 'F'
    mov byte [raw_last_path], 'A'
    mov dx, [ata_ctrl_base]
    mov al, 0x02                ; nIEN while command is polled directly
    out dx, al
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    mov dx, [ata_cmd_base]
    add dx, ATA_REG_DEV
    mov al, [ata_dev_select]
    out dx, al
    mov dx, [ata_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx

    mov di, 0x0020
.ready_outer:
    xor cx, cx
.ready:
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x88
    jz .issue
    loop .ready
    dec di
    jnz .ready_outer
    jmp .fail

.issue:
    mov al, 0xE7
    out dx, al
    mov dx, [ata_ctrl_base]
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    mov di, 0x0040              ; cache flush may take longer than one write
.flush_outer:
    xor cx, cx
.flush_wait:
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    test al, 0x88
    jz .flush_status
    loop .flush_wait
    dec di
    jnz .flush_outer
    jmp .fail
.flush_status:
    test al, 0x21               ; DF or ERR
    jnz .fail
    clc
    jmp .out
.fail:
    mov [raw_last_status], al
    stc
.out:
    pushf
    mov dx, [ata_cmd_base]
    add dx, ATA_REG_STATUS
    in al, dx
    mov dx, [ata_ctrl_base]
    xor al, al
    out dx, al
    popf
    pop di
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
    call setup_bios_disk
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

; Disk status/geometry and timer ticks return AX/CX/DX low words; PCI
; configuration reads return AX and full ECX. All other caller state and the
; high halves of 16-bit outputs survive firmware. Preserve entry FLAGS except
; returned CF, then establish the clear direction flag required by copy loops.
%macro SETUP_BIOS_SAVE 0
    pushf
    pushad
    push ds
    push es
    push fs
    push gs
%endmacro
setup_bios_disk:
    SETUP_BIOS_SAVE
    stc
    int 0x13
    jmp setup_bios_low_results
setup_bios_time:
    SETUP_BIOS_SAVE
    int 0x1A
setup_bios_low_results:
    pushf
    push bp
    mov bp,sp
    mov [ss:bp+40],ax
    mov [ss:bp+36],cx
    mov [ss:bp+32],dx
    jmp setup_bios_return
setup_bios_pci:
    SETUP_BIOS_SAVE
    stc
    int 0x1A
    pushf
    push bp
    mov bp,sp
    mov [ss:bp+40],ax
    mov [ss:bp+36],ecx
setup_bios_return:
    ; BP+2: BIOS flags; BP+12: PUSHAD; BP+44: entry flags.
    mov ax,[ss:bp+2]
    and ax,1
    and word [ss:bp+44],0xFFFE
    or [ss:bp+44],ax
    pop bp
    add sp,2
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popf
    cld
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
    call setup_bios_disk
    jnc .success

    mov dl, [raw_edd_retry_drive]
    xor ax, ax
    call setup_bios_disk

    push cs
    pop ds
    mov word [bios_probe_dap + 0], 0x0010
    mov cx, [raw_edd_retry_count]
    mov [bios_probe_dap + 2], cx
    mov si, bios_probe_dap
    mov dl, [raw_edd_retry_drive]
    mov ah, [raw_edd_retry_op]
    xor al, al
    call setup_bios_disk
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
    pushad
    push ds
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

    mov eax, [raw_clone_lba_lo]
    xor edx, edx
    movzx ebx, word [raw_chs_spc]
    test ebx,ebx
    jz .bad_geometry
    div ebx
    cmp eax,1023               ; CHS encodes only ten cylinder bits
    ja .bad_geometry
    mov [raw_chs_cylinder], ax

    mov eax, edx
    xor edx, edx
    movzx ebx, word [raw_chs_spt]
    test ebx,ebx
    jz .bad_geometry
    div ebx
    cmp eax,255
    ja .bad_geometry

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
    call setup_bios_disk
    jc .fail

    pop es
    pop ds
    popad
    clc
    ret
.bad_geometry:
    mov ah,0x04                ; requested sector is not addressable by CHS
.fail:
    mov [raw_last_status], ah
    mov [raw_chs_status], ah
    pop es
    pop ds
    popad
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
    push di
    push es
    push ds
    push cs
    pop ds

    mov [bios_probe_drive], dl
    mov [bios_probe_bit], bl

    mov word [bios_probe_dap + 0],0x0010
    mov word [bios_probe_dap + 2],1 ; never reuse a previous transfer count
    call setup_bios_bounce_address
    mov word [bios_probe_dap + 4],bx
    mov ax, cs
    mov [bios_probe_dap + 6], ax
    mov word [bios_probe_dap + 8], 0
    mov word [bios_probe_dap + 10], 0
    mov word [bios_probe_dap + 12], 0
    mov word [bios_probe_dap + 14], 0

    mov si, bios_probe_dap
    mov dl, [bios_probe_drive]
    mov ah, 0x42
    call setup_bios_disk
    jnc .read_ok

    push cs
    pop es
    call setup_bios_bounce_address
    mov ax, 0x0201
    mov cx, 0x0001
    xor dh, dh
    mov dl, [bios_probe_drive]
    call setup_bios_disk
    jc .done

.read_ok:
    call setup_bios_bounce_address
    mov si,bx
    mov di,io_buffer
    push cs
    pop es
    mov cx,256
    cld
    rep movsw
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
    pop di
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

print_hex_byte:
    push ax
    push dx
    mov ah, al
    shr al, 4
    call .nibble
    mov al, ah
    and al, 0x0F
    call .nibble
    pop dx
    pop ax
    ret
.nibble:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 7
.digit:
    add al, '0'
    mov dl, al
    call print_char_dl
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
    jc .fail

    mov dx, path_sanity
    mov ah, 0x41
    int 0x21
    jc .fail

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
    push di
    push es
    push ds

    mov di,bx                    ; caller destination survives the BIOS call
    call setup_bios_bounce_address
    mov si,bx

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
    call setup_bios_disk
    jc .done
    push cs
    pop ds
    mov cx,256
    cld
    rep movsw
    clc

.done:
    pop ds
    pop es
    pop di
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
    jc .close_fail
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

    jmp .copy_fail
.close_fail:
    mov word [fail_code],0x0408 ; delayed target/file-close write failure
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
    jc .write_fail
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
    push dx
    xor dx,dx                  ; preserve either close's delayed-write failure
    mov bx, [dst_handle]
    cmp bx, 0xFFFF
    je .skip_dst
    mov ah, 0x3E
    int 0x21
    jnc .skip_dst
    inc dx
.skip_dst:
    mov bx, [src_handle]
    cmp bx, 0xFFFF
    je .skip_src
    mov ah, 0x3E
    int 0x21
    jnc .skip_src
    inc dx
.skip_src:
    mov word [src_handle], 0xFFFF
    mov word [dst_handle], 0xFFFF
    test dx,dx
    jz .ok
    stc
    jmp .out
.ok:
    clc
.out:
    pop dx
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
    call setup_bios_time
    mov [prompt_tick_start], dx

.poll:
    mov ah, 0x01
    int 0x16
    jnz .have_key

    mov ah, 0x00
    call setup_bios_time
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
    cmp byte [gui_active], 0
    je .text
    call serial_write_z
    call serial_write_crlf
    ret
.text:
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
msg_serial_hdd_install_progress db '[SETUP-HDD-INSTALL] PROGRESS ', 0
msg_serial_hdd_install_fail db '[SETUP-HDD-INSTALL] FAIL S=', 0
msg_serial_hdd_install_path db ' P=', 0
msg_serial_hdd_install_lba db ' L=', 0
msg_serial_hdd_install_status db ' AH=', 0
msg_serial_hdd_install_detail db ' D=', 0
msg_serial_hdd_install_edd db ' E=', 0
msg_serial_hdd_install_chs db ' C=', 0
msg_serial_ata_map db '[SETUP-ATA-MAP] P=', 0
msg_serial_ata_cmd db ' CMD=', 0
msg_serial_ata_ctrl db ' CTRL=', 0
msg_serial_ata_dev db ' DEV=', 0
msg_serial_cd_map db '[SETUP-CD-MAP] P=', 0
msg_serial_cd_image db ' IMG=', 0
msg_serial_cd_retry db '[SETUP-CD-RETRY] L=', 0
msg_serial_cd_retry_status db ' ST=', 0
msg_serial_cd_retry_error db ' ER=', 0
msg_serial_cd_retry_left db ' LEFT=', 0
msg_serial_cd_retry_sense db ' SK/ASC/Q=', 0
msg_serial_cd_mirror db '[SETUP-CD-MIRROR] primary unreadable; using redundant extent', 0
msg_hdd_format_screen_start db 'Formatting target HDD...', 0
msg_serial_hdd_format_start db '[SETUP-HDD-FORMAT] START', 0
msg_serial_hdd_format_progress db '[SETUP-HDD-FORMAT] PROGRESS ', 0
msg_serial_hdd_format_done db '[SETUP-HDD-FORMAT] DONE', 0
msg_serial_hdd_format_fail db '[SETUP-HDD-FORMAT] FAIL S=', 0
msg_screen_hdd_install_fail db 'Install I/O failed. ST=0x', 0
msg_screen_hdd_install_detail db ' ER=0x', 0
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
msg_vis_install_fail_banner db 'INSTALLATION STOPPED. HDD left non-bootable.', 0
msg_vis_install_fail_hint   db 'Press any key to return to the Live CD shell.', 0

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

format_sectors_total    dd RAW_HDD_PARTITION_SECTORS
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
raw_last_detail         db 0
raw_edd_status          db 0
raw_chs_status          db 0
raw_chs_count           db 1
raw_edd_retry_op        db 0
raw_edd_retry_drive     db 0
raw_edd_retry_count     dw 1
batch_count             dw 0
ata_cmd_base            dw 0
ata_ctrl_base           dw 0
ata_dev_select          db 0
ata_pci_bdf             dw 0
ata_probe_count         db 0
ata_probe_cmd           dw 0
ata_probe_ctrl          dw 0
ata_probe_dev           db 0
ata_probe_saved_cmd     dw 0
ata_probe_saved_ctrl    dw 0
ata_probe_saved_dev     db 0
raw_default_drive_patched db 0
raw_clone_mbr_saved     db 0
raw_atapi_ready         db 0
atapi_cmd_base          dw 0
atapi_ctrl_base         dw 0
atapi_dev_select        db 0
atapi_block_count       db 0
atapi_image_lba_lo      dw 0
atapi_image_lba_hi      dw 0
atapi_cd_lba_lo         dw 0
atapi_cd_lba_hi         dw 0
atapi_buffer_ptr        dw 0
atapi_transfer_bytes    dw 0
atapi_bytes_remaining   dw 0
atapi_retries_left      db 0
atapi_last_status       db 0
atapi_last_error        db 0
atapi_using_mirror      db 0
atapi_sense_key         db 0
atapi_sense_asc         db 0
atapi_sense_ascq        db 0
prompt_tick_start       dw 0

; command base, control port, ATA device/head prefix, padding
ata_legacy_candidates:
    dw 0x01F0, 0x03F6
    db 0xA0, 0
    dw 0x01F0, 0x03F6
    db 0xB0, 0
    dw 0x0170, 0x0376
    db 0xA0, 0
    dw 0x0170, 0x0376
    db 0xB0, 0

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
; INT 13h/AH=48h EDD 3.0 drive-parameter buffer, including host/interface
; and device paths through the checksum byte.
edd_drive_params       times 0x4A db 0

align 16
eltorito_spec_packet   times 20 db 0
atapi_packet           times 12 db 0
atapi_sense_packet     db ATAPI_CMD_REQUEST_SENSE, 0, 0, 0, 18, 0
                       times 6 db 0
atapi_sense_data       times 18 db 0
raw_clone_mbr          times 512 db 0

align 16
; Multi-sector I/O buffer: 8 sectors (4 KB) so the install/format loops can
; batch INT 13h transfers and reduce the call count by 8x. Larger batches
; mean fewer chances for a real-HW BIOS (e.g. ThinkPad T23) to wedge.
io_buffer               times 4096 db 0

raw_target_bios db 0x81
ata_transfer_command db 0x30
align 16
setup_verify_buffer_data times 4096 db 0
; Runtime alignment needs up to 511 padding bytes plus a complete sector.
setup_bios_bounce_storage times 1023 db 0
align 16
setup_stack times 4096 db 0
setup_stack_top:
%if ($-$$) > 0xEF00
%error SETUP.COM exceeds safe COM code/data/stack size
%endif
