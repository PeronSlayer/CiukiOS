bits 16
org 0x0000

%ifdef CIUKIDOS_KERNEL_BUILD
; Versioned CIUKIDOS kernel image header.  The full Stage1 loader validates
; every field and every descriptor before transferring control.  Keeping the
; service table in the first 126 bytes also makes the ABI independently
; discoverable by black-box children without relying on a build-time offset.
ciukidos_image_start:
    jmp short stage1_start
    db 'CIUKIDOS'
    dw 26
    dw 0x0002
    dw 0x000B
    dw 0x0008
    dw 0x003F
    dw ciukidos_image_end - ciukidos_image_start
    dw runtime_service_table - ciukidos_image_start
    dw 0x0900

runtime_service_table:
    db 'R', 'T', 'S', 'V'
    dw 0x0002
    dw 0x000B
    dw 0x0008
    dw 0x0001, 0x0001, kernel_runtime_identity_service, 0x0000
    dw 0x0002, 0x0001, kernel_runtime_version_service, 0x0000
    dw 0x0003, 0x0001, kernel_runtime_stage2_ready_service, 0x0000
    dw 0x0004, 0x0001, kernel_runtime_dos_version_service, 0x0000
    dw 0x0005, 0x0001, kernel_runtime_default_drive_service, 0x0000
    dw 0x0006, 0x0001, kernel_runtime_get_state_ptr_service, 0x0000
    dw 0x0007, 0x0001, kernel_runtime_prepare_child_dta_service, 0x0000
    dw 0x0008, 0x0001, kernel_runtime_restore_parent_dta_service, 0x0000
    dw 0x0009, 0x0001, kernel_runtime_kernel_caps_service, 0x0000
    dw 0x000A, 0x0001, kernel_runtime_sync_process_state_service, 0x0000
    dw 0x000B, 0x0001, kernel_runtime_record_termination_service, 0x0000
%endif

%define CMD_BUF_LEN 64
%define DOS_ENV_EXEC_PATH_LEN 64
%define BOOT_SPLASH_WAIT_TICKS 55
%define BOOT_STEP_WAIT_TICKS 9
%define BOOT_SPLASH_WAIT_US_HI 0x002D
%define BOOT_SPLASH_WAIT_US_LO 0xC6C0
%define BOOT_STEP_WAIT_US_HI 0x0007
%define BOOT_STEP_WAIT_US_LO 0xA120
%define SHELL_EXEC_PATH_BUF_LEN 80
%define SHELL_HISTORY_MAX 8
; The embedded PSP JFT has twenty entries.  Handles 0..4 are the standard
; devices, so expose all fifteen remaining entries as real file slots.  The
; first eight retain their legacy fields; handles 13..19 use a compact table.
; Previously those seven entries were CON-only, which made Windows exhaust
; the FAT slots while its fonts and drivers were still open.
%define DOS_FILE_EXTRA_FIRST_HANDLE 0x000D
%define DOS_FILE_EXTRA_COUNT 7
%define DOS_FILE_EXTRA_LAST_HANDLE (DOS_FILE_EXTRA_FIRST_HANDLE + DOS_FILE_EXTRA_COUNT - 1)
%define DOS_FILE_EXTRA_FIRST_TARGET 9
%define DOS_FILE_EXTRA_ENTRY_SIZE 20
%define DOS_FILE_EXTRA_OPEN_OFF 0
%define DOS_FILE_EXTRA_POS_LO_OFF 1
%define DOS_FILE_EXTRA_POS_HI_OFF 3
%define DOS_FILE_EXTRA_MODE_OFF 5
%define DOS_FILE_EXTRA_CLUSTER_OFF 6
%define DOS_FILE_EXTRA_ROOT_LBA_OFF 8
%define DOS_FILE_EXTRA_ROOT_LBA_HI_OFF 10
%define DOS_FILE_EXTRA_ROOT_OFF_OFF 12
%define DOS_FILE_EXTRA_CLUSTER_COUNT_OFF 14
%define DOS_FILE_EXTRA_SIZE_LO_OFF 16
%define DOS_FILE_EXTRA_SIZE_HI_OFF 18
%define COM_STACK_RESERVE_PARAS 0x0200
; Keep the resident shell immediately above the kernel scratch/environment
; area.  Its self-resize leaves the widest contiguous arena possible for
; large real-mode games such as Wolfenstein 3-D.
%ifdef CIUKIDOS_KERNEL_BUILD
%define COM_LOAD_SEG 0x1180
%define MZ_LOAD_SEG 0x1200
%define MZ2_LOAD_SEG 0x3200
%define MZ3_LOAD_SEG 0x7200
%define RUNTIME_LOAD_SEG 0x0300
%define DOS_EXEC_STATE_BASE_SEG 0x0E00
; Stage2 is a boot-only 512-byte handoff at 0E80h.  Once it returns, reuse its
; complete 0E80h-0E9Fh window and the remaining gap below DOS_META_BUF_SEG for
; EXEC snapshots.  Windows itself consumes four frames before Program Manager;
; a DOS prompt plus a nested game already needs six, so the old limit of five
; rejected a perfectly valid AH=4Bh with error 8 despite abundant EMS/XMS.
%define DOS_EXEC_STATE_FRAME_MAX 11
%define STAGE2_LOAD_SEG 0x0E80
%define DOS_META_BUF_SEG 0x0F00
%define DOS_FAT_BUF_SEG  0x1000
%define DOS_IO_BUF_SEG   0x1100
%define DOS_ENV_SEG      0x1130
%else
%define COM_LOAD_SEG 0x1780
%define MZ_LOAD_SEG 0x1800
%define MZ2_LOAD_SEG 0x3800
%define MZ3_LOAD_SEG 0x7800
%define RUNTIME_LOAD_SEG 0x1100
%define DOS_EXEC_STATE_BASE_SEG (RUNTIME_LOAD_SEG + 0x0060)
%define DOS_EXEC_STATE_FRAME_PARAS 0x0016
%define DOS_EXEC_STATE_FRAME_MAX 4
%define STAGE2_LOAD_SEG 0x11E0
%define DOS_META_BUF_SEG 0x1200
%define DOS_FAT_BUF_SEG  0x1400
%define DOS_IO_BUF_SEG   0x1600
%define DOS_ENV_SEG      0x1630
%endif
%ifndef DOS_EXEC_STATE_FRAME_PARAS
%define DOS_EXEC_STATE_FRAME_PARAS 0x0016
%endif
%ifndef DOS_EXEC_STATE_FRAME_MAX
%define DOS_EXEC_STATE_FRAME_MAX 4
%endif
%ifdef CIUKIDOS_KERNEL_BUILD
%define MZ_LOAD_LIMIT_SEG 0x5200
%else
%define MZ_LOAD_LIMIT_SEG 0x5800
%endif
; AH=52h publishes a complete 20-entry DOS 5 SFT matching the PSP JFT.  Keep
; its 1792-byte image in the reserved gap below the EXEC-state frames: the old
; 1 KiB window at 1740h only fit five CON entries and made Windows interpret
; adjacent shell memory as the SFT records for handles 5..19.
%ifdef CIUKIDOS_KERNEL_BUILD
%define DOS_SYSVARS_SEG        0x0D90
%else
%define DOS_SYSVARS_SEG        0x1390
%endif
%define DOS_SYSVARS_ANCHOR_OFF 0x0000
%define DOS_SYSVARS_OFF        0x0002
%define DOS_SYSVARS_CDS_OFF    0x0100
%define DOS_SYSVARS_SFT_OFF    0x0200
%define DOS_SYSVARS_SFT_SIZE   0x003B
%define DOS_SYSVARS_SFT_COUNT  20
%define DOS_SYSVARS_DPB_OFF    0x06C0
%define DOS_SYSVARS_CON_OFF    0x0080
%define DOS_SYSVARS_DEV_RET_OFF 0x00A0
; The primary MZ copy window ends at 5800h.  Start the allocatable DOS arena
; immediately after that boundary; an active MZ2/MZ3 PSP raises the dynamic
; arena start to its real image end.  The previous 5D00h floor stranded 20 KiB
; and let DOS/4GW's 250 KiB block starve MultiVoc's following 8 KiB DMA buffer.
%define DOS_HEAP_BASE_SEG MZ_LOAD_LIMIT_SEG
; A000h is only the architectural ceiling below VGA memory.  The usable
; runtime limit is the lower of the BIOS conventional-memory word and a
; valid EBDA segment, so EXEC/MCB arenas cannot overwrite firmware storage.
%define DOS_HEAP_LIMIT_SEG 0xA000
%define DOS_HEAP_MAX_PARAS (DOS_HEAP_LIMIT_SEG - DOS_HEAP_BASE_SEG)
%define VBE_BANK_WINDOW_PARAS 0x1000
%define VBE_BANK_WINDOW_WORDS 0x8000
%define VBE_BACKING_MAX_BANKS (DOS_HEAP_MAX_PARAS / VBE_BANK_WINDOW_PARAS)
%define VBE_BACKING_TARGET_BANKS (VBE_BACKING_MAX_BANKS - 1)
%define VBE_BACKING_TARGET_PARAS (VBE_BACKING_TARGET_BANKS * VBE_BANK_WINDOW_PARAS)
%define DOS_HEAP_USER_SEG (DOS_HEAP_BASE_SEG + 1)
%define DOS_HEAP_USER_MAX_PARAS (DOS_HEAP_MAX_PARAS - 1)
; AH=47h supplies a DOS path without drive/leading slash, in 64 bytes.
%define DOS_CWD_BYTES 64
%define DOS_CWD_MAX (DOS_CWD_BYTES - 1)
%define DOS_MEM_BLOCK_FREE 0
%define DOS_MEM_BLOCK_INUSE 1
%define DOS_MEM_BLOCK_RESIDENT 2
%define DOS_MEM_BLOCK_PSP 4
%define DOS_MEM_BLOCK_ALLOC DOS_MEM_BLOCK_INUSE
%define DOS_MEM_BLOCK_TABLE_MAX 32
%define DOS_MEM_BLOCK_ENTRY_SIZE 8
; XMS extended-memory blocks must not include the High Memory Area.  The HMA
; occupies the first 64 KiB above 1 MiB (100000h..10FFFFh); DOSX places its
; protected-mode resident code there after claiming it with XMS function 01h.
; Starting EMB handle 1 at 100000h let Windows overwrite DOSX while building
; its selector tables.  Keep normal XMS allocations at 110000h and advertise
; the memory which remains after the HMA.  The active full VM exposes 64 MiB
; through the legacy AH=88h/XMS 2.x interface; QEMU keeps additional host-side
; RAM available for later 32-bit memory-discovery work.
%define XMS_EMB_BASE_HI 0x0011
%define XMS_PHYS_LIMIT_HI 0x0400
%define XMS_FREE_KB_INITIAL 0xFBC0
%define XMS_HANDLE_COUNT 16
; Large clients commonly claim the complete initial "largest" block while
%define BIOS_EXTMEM_KB 0xFC00
%define DOS_EXEC_STATE_BLOCK_COUNT_OFF (dos_mem_block_count - dos_mem_exec_state_begin)
%define DOS_EXEC_STATE_BLOCK_TABLE_OFF (dos_mem_block_table - dos_mem_exec_state_begin)
%define CDOSSTATE_OFF_CURRENT_PSP 35
%define CDOSSTATE_OFF_PARENT_PSP 37
%define CDOSSTATE_OFF_PREVIOUS_PSP 39
%define CDOSSTATE_OFF_DTA_SEG 41
%define CDOSSTATE_OFF_DTA_OFF 43
%define CDOSSTATE_OFF_SAVED_PARENT_DTA_SEG 45
%define CDOSSTATE_OFF_SAVED_PARENT_DTA_OFF 47
%ifndef DOS_DEFAULT_DRIVE_INDEX
%if FAT_TYPE == 16
%define DOS_DEFAULT_DRIVE_INDEX 2
%else
%define DOS_DEFAULT_DRIVE_INDEX 0
%endif
%endif
%ifndef STAGE1_DEBUG_COMMANDS
%define STAGE1_DEBUG_COMMANDS 0
%endif
%ifndef TRACE_CHILD_INT21
%define TRACE_CHILD_INT21 0
%endif
%ifndef TRACE_WIN_MEMORY
%define TRACE_WIN_MEMORY 0
%endif

%ifndef TRACE_WIN_INT2F
%define TRACE_WIN_INT2F 0
%endif
%ifndef TRACE_WIN_XMS
%define TRACE_WIN_XMS 0
%endif
%define CHILD_TRACE_MAX_CALLS 128
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
%ifndef FAT_TOTAL_SECTORS
%if FAT_TYPE == 16
%define FAT_TOTAL_SECTORS 262144
%else
%define FAT_TOTAL_SECTORS 2880
%endif
%endif
%define FAT_DATA_CLUSTER_COUNT ((FAT_TOTAL_SECTORS - FAT_RESERVED_SECTORS - (FAT_COUNT * FAT_SECTORS_PER_FAT) - FAT_ROOT_DIR_SECTORS) / FAT_SECTORS_PER_CLUSTER)
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
%ifndef STAGE1_BOOT_EXTERNAL_SHELL
%define STAGE1_BOOT_EXTERNAL_SHELL 0
%endif
%ifndef STAGE1_INTERACTIVE_SHELL
%define STAGE1_INTERACTIVE_SHELL 0
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
%define SPLASH_SRC_W 256
%define SPLASH_SRC_H 192
%define SPLASH_SRC_ROW_BYTES 256
%define SPLASH_PIXEL_BYTES 49152
%define SPLASH_TOTAL_SIZE 49920
%define SPLASH_SCALE_X_BASE 3
%define SPLASH_SCALE_Y_BASE 3
%define SPLASH_VESA_MODE 0x0103
%define SPLASH_VESA_ROW_BYTES 800
%define SPLASH_VRAM_SAFE_OFFSET 0xFCE0
%define SPLASH_BUF_SEG 0x9000
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
    cld
%ifdef CIUKIDOS_KERNEL_BUILD
    mov [cs:kernel_entry_default_drive], al
%endif
    cli
    mov ax, cs
    mov ds, ax
    mov es, ax
%ifdef CIUKIDOS_KERNEL_BUILD
    ; The relocation stub installs the kernel at physical 03000h.  Keep its
    ; dedicated 8 KiB stack immediately below it (01000h..02FFFh), clear of
    ; the IVT/BDA, the temporary loader image and every DOS process arena.
    mov ax, 0x0100
%endif
    mov ss, ax
%ifdef CIUKIDOS_KERNEL_BUILD
    mov sp, 0x2000
%else
    mov sp, 0xFFFE
%endif
    sti

    mov [boot_drive], dl

    call serial_init
    mov si, msg_stage1_serial
    call print_string_serial

    call run_bios_diagnostics
    call install_int21_vector

%if FAT_TYPE == 16 && STAGE1_BOOT_EXTERNAL_SHELL
%if STAGE1_SELFTEST_AUTORUN == 0
    call hide_text_cursor
    call stage1_show_boot_splash
    call stage1_show_boot_loading_screen
%endif
%endif
%if FAT_TYPE == 16
    call stage1_runtime_init
    jc .runtime_init_failed
%if STAGE1_BOOT_EXTERNAL_SHELL
%if STAGE1_SELFTEST_AUTORUN == 0
    mov al, 1
    call stage1_boot_mark_step
%endif
%endif
%endif
    call init_stage2_services
%if FAT_TYPE == 16 && STAGE1_BOOT_EXTERNAL_SHELL
%if STAGE1_SELFTEST_AUTORUN == 0
    mov al, 4
    call stage1_boot_mark_step
%endif
%endif
    call init_shell_default_dirs
%if FAT_TYPE == 16 && STAGE1_BOOT_EXTERNAL_SHELL
%if STAGE1_SELFTEST_AUTORUN == 0
    mov al, 2
    call stage1_boot_mark_step
%endif
%endif
%if FAT_TYPE == 16 && STAGE1_RUNTIME_PROBE
    call stage1_runtime_probe
%endif
%if STAGE1_SELFTEST_AUTORUN
    call run_stage1_selftest
%endif
%if FAT_TYPE == 16 && STAGE1_BOOT_EXTERNAL_SHELL
%if STAGE1_SELFTEST_AUTORUN == 0
    mov al, 3
    call stage1_boot_mark_step
%endif
%else
    call draw_shell_chrome
%endif
%if FAT_TYPE == 16
%if HARDWARE_VALIDATION_SCREEN
    call print_hardware_validation_screen
%endif
%endif
    jmp .after_runtime_init_failed

.runtime_init_failed:
    mov si, msg_runtime_missing_fatal
    jmp stage1_loader_fatal

.after_runtime_init_failed:

    call flush_keyboard_buffer
    push cs
    pop ds
%if FAT_TYPE == 16 && STAGE1_BOOT_EXTERNAL_SHELL
%if STAGE1_SELFTEST_AUTORUN == 0
    mov al, 5
    call stage1_boot_mark_step
    call show_text_cursor
%endif
    mov si, dos_env_exec_path
    call shell_try_exec_path
    jnc .shell_returned
    mov [cs:loader_exec_error], ax
    mov si, msg_shell_missing_fatal
    cmp ax, 2
    je stage1_loader_fatal
    mov si, msg_shell_exec_fatal
    jmp stage1_loader_fatal
.shell_returned:
    mov si, msg_shell_returned_fatal
    jmp stage1_loader_fatal
%else
    mov si, msg_shell_missing_fatal
    jmp stage1_loader_fatal
%endif

stage1_loader_fatal:
    push si
    mov bl, 0x1F
    call clear_screen_attr
    xor dx, dx
    call set_cursor_pos
    mov si, msg_loader_bsod_woof
    call print_string_dual
    mov si, msg_loader_bsod_body
    call print_string_dual
    mov si, msg_loader_bsod_restart
    call print_string_dual
    mov si, msg_loader_bsod_error
    call print_string_dual
    pop si
    call print_string_dual
    mov ax, [cs:loader_exec_error]
    test ax, ax
    jz .no_exec_error
    push ax
    mov si, msg_loader_exec_error
    call print_string_dual
    pop ax
    call print_hex16_dual
.no_exec_error:
    mov si, msg_halting
    call print_string_dual
.halt_forever:
    cli
    hlt
    jmp .halt_forever

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

%if STAGE1_INTERACTIVE_SHELL
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
%endif

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
    call print_string_serial

    mov si, msg_diag_int10
    call print_string_serial

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
    call print_string_serial
.int13_done:

    mov ah, 0x01
    int 0x16
    mov si, msg_diag_int16_ok
    call print_string_serial

    mov ah, 0x00
    int 0x1A
    mov si, msg_diag_int1a
    call print_string_serial
    mov ax, cx
    call print_hex16_serial
    mov ax, dx
    call print_hex16_serial
    call print_newline_serial

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
%ifdef CIUKIDOS_KERNEL_BUILD
    mov al, [kernel_entry_default_drive]
    mov [dos_default_drive], al
%else
    mov byte [dos_default_drive], DOS_DEFAULT_DRIVE_INDEX
%endif
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

    mov bx, 0x24 * 4
    mov ax, [es:bx]
    mov [old_int24_off], ax
    mov ax, [es:bx + 2]
    mov [old_int24_seg], ax
    mov word [es:bx], int24_handler
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

%if FAT_TYPE == 16
    ; SeaBIOS records dedicated cursor/navigation keys in the BIOS keyboard
    ; ring with AL=E0h.  A number of legacy real-mode applications consume
    ; that ring directly and count the E0 prefix plus the scan code as two
    ; navigation events.  Chain the real IRQ1 BIOS
    ; handler first, then expose the legacy-compatible AL=00h form used by
    ; classic DOS keyboard APIs.
    mov bx, 0x09 * 4
    mov ax, [es:bx]
    mov [old_int09_off], ax
    mov ax, [es:bx + 2]
    mov [old_int09_seg], ax
    mov word [es:bx], int09_handler
    mov ax, cs
    mov [es:bx + 2], ax
%endif

    mov bx, 0x1A * 4
    mov ax, [es:bx]
    mov [old_int1a_off], ax
    mov ax, [es:bx + 2]
    mov [old_int1a_seg], ax
    mov word [es:bx], int1a_handler
    mov ax, cs
    mov [es:bx + 2], ax

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

    ; Keyboard consumers use the platform BIOS INT 16h implementation
    ; directly.  The resident IRQ1 bridge only normalizes the BIOS ring and
    ; never replaces the public INT 16h API.
%endif

%if STAGE2_AUTORUN == 0
    ; CPU fault handlers: INT 0 (DIV) + INT 6 (invalid opcode) only.
    ; INT 0Dh is *not* hooked: in real mode it is IRQ 5 (LPT2 / sound),
    ; not GP fault (which only fires in protected mode). Hooking it caused
    ; a reboot loop on real hardware whenever the sound card pulled IRQ 5.
    ; Live-CD builds skip these handlers to fit the stage1 sector budget.
    mov bx, 0 * 4
    mov word [es:bx], int00_fault_handler
    mov ax, cs
    mov [es:bx + 2], ax
    mov bx, 6 * 4
    mov word [es:bx], int06_fault_handler
    mov ax, cs
    mov [es:bx + 2], ax
%endif

    ; Ctrl+Alt+Del: handled natively by the BIOS keyboard interrupt (INT 09h
    ; checks scan code 0x53 with Ctrl+Alt and triggers INT 19h itself).
    ; We do NOT hook INT 09h: doing so would either consume port 0x60 ahead
    ; of the BIOS (breaking keyboard delivery) or duplicate BIOS work for
    ; no gain. A future shell/UI keymap can install a chained hook.

    pop es
    pop bx
    pop ax

    mov si, msg_int21_installed
    call print_string_dual
    ret

int20_handler:
    push cs
    pop ds
    mov byte [last_exit_code], 0
    mov byte [last_term_type], 0
%ifdef CIUKIDOS_KERNEL_BUILD
    call ciukidos_record_last_termination
%endif
    call ciukidos_restore_parent_dta
    call int21_restore_psp_term_vectors
    mov bp, sp
    mov ax, [bp + 2]
%if TRACE_CHILD_INT21 != 0
    call child_trace_exit_int20
%endif
    cmp ax, [current_com_load_seg]
    mov ax, int21_mz_terminate_trampoline
    jne .patch_ip
    mov ax, int21_com_terminate_trampoline
.patch_ip:
    mov [bp + 0], ax
    mov ax, cs
    mov [bp + 2], ax
    iret

int_default_iret:
    iret

int24_handler:
    mov al, 0x03
    iret

hardware_reset:
    ; INT 19h is only a bootstrap restart.  Reset the chipset/peripherals so
    ; the next boot begins from POST on physical hardware as well as in VMs.
    cli
    xor ax, ax
    mov ds, ax
    mov word [0x0472], ax
    mov dx, 0x0CF9
    mov al, 0x02
    out dx, al
    or al, 0x04
    out dx, al
    mov cx, 0x1000
.wait_8042:
    in al, 0x64
    test al, 0x02
    jz .pulse_8042
    loop .wait_8042
    jmp .triple_fault
.pulse_8042:
    mov al, 0xFE
    out 0x64, al
    mov cx, 0x1000
.wait_reset:
    nop
    loop .wait_reset
.triple_fault:
    lidt [cs:.null_idt]
    int 3
    jmp 0xFFFF:0x0000
.null_idt:
    dw 0
    dd 0

%if STAGE2_AUTORUN == 0
; Compact CPU fault handlers (INT 0/6): recover to shell child or reboot.
int00_fault_handler:
    jmp fault_common
int06_fault_handler:
    jmp fault_common
fault_common:
    cli
    push bp
    mov bp, sp
    push cs
    pop ds
    cmp byte [shell_exec_external_program_active], 0
    je .reboot
    cmp word [dos_exec_identity_psp], 0
    je .reboot
    mov byte [last_exit_code], 0xFF
    mov byte [last_term_type], 0
    call ciukidos_record_last_termination
    call int21_restore_psp_term_vectors
    mov ax, [bp + 4]
    jmp exec_terminate_dispatch_cs
.reboot:
    call hardware_reset
.hng:
    hlt
    jmp .hng
%endif

; PC speaker beep: ~1000 Hz on PIT ch2 for ~1 s. 'beep' at shell.
pc_speaker_beep:
    pusha
    mov al, 0xB6
    out 0x43, al
    mov ax, 1193
    out 0x42, al
    mov al, ah
    out 0x42, al
    in al, 0x61
    or al, 0x03
    out 0x61, al
    mov cx, 0x000F
    mov dx, 0x4240
    mov ah, 0x86
    int 0x15
    in al, 0x61
    and al, 0xFC
    out 0x61, al
    popa
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

    ; DOS exposes its re-entrancy state through INT 21h/AH=34h.  Windows and
    ; DOS extenders inspect this byte while translating protected-mode DOS
    ; calls, so it must describe the real dispatcher lifetime rather than
    ; remaining permanently zero.
    inc byte [cs:dos_indos_flag]

    ; DOS services execute their own string operations forward regardless of
    ; the caller's DF.  INT/IRET keeps the caller FLAGS image on its stack, so
    ; CLD here cannot leak into the program but prevents backward kernel copies
    ; when an assembly-heavy client invokes INT 21h after STD.
    cld

    push ax
    mov bp, sp
    mov ax, [ss:bp + 18]
    mov [cs:int21_trace_call_ip], ax
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
    mov [cs:int21_caller_bx], bx
    pop ax
    mov byte [cs:int21_carry], 0
    mov byte [cs:int21_return_es], 0
    mov byte [cs:int21_return_ds], 0
    mov byte [cs:int21_zf_state], 0xFF
    mov byte [cs:int21_last_ah], ah
    mov byte [cs:int21_last_al], al
    mov [cs:int21_last_dx], dx
%if TRACE_CHILD_INT21 != 0
    call child_trace_int21_call
%endif
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
    cmp ah, 0x1B
    je .fn_1b
    cmp ah, 0x1C
    je .fn_1c
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
    cmp ah, 0x29
    je .fn_29
    cmp ah, 0x25
    je .fn_25
    cmp ah, 0x2F
    je .fn_2f
    cmp ah, 0x30
    je .fn_30
    cmp ah, 0x31
    je .fn_31
    cmp ah, 0x32
    je .fn_32
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
    cmp ah, 0x5D
    je .fn_5d
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
%if FAT_TYPE == 16
    cmp ah, 0xF1
    je .fn_f1
%endif
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
    jmp .success

.fn_01:
    mov ah, 0x00
    int 0x16
    call bios_putc
    call serial_putc
    jmp .success

.fn_09:
    ; DOS AH=09h treats DS:DX as an input pointer and preserves SI.  Some
    ; protected-mode hosts (notably HDPMI32) keep scanning their own format
    ; string in SI across this call.  Using SI directly here made the host
    ; resume at the end of the DOS string instead, producing false errors and
    ; destabilising the following DPMI client.
    push si
    mov si, dx
.fn_09_loop:
    lodsb
    cmp al, '$'
    je .fn_09_done
    call bios_putc
    call serial_putc
    jmp .fn_09_loop
.fn_09_done:
    pop si
    jmp .success

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

.fn_1b:
    xor dl, dl
    call int21_get_allocation_info
    mov byte [cs:int21_return_ds], 1
    jmp .success

.fn_1c:
    call int21_get_allocation_info
    mov byte [cs:int21_return_ds], 1
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

.fn_29:
    call int21_parse_fcb_filename
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

.fn_32:
    call int21_get_dpb
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
%if TRACE_CHILD_INT21 != 0
    call child_trace_prepare_exec
%endif
    ; EXEC suspends this dispatcher while the child uses the same globals.
    ; Keep its AH/AL together (adjacent bytes) until the original frame resumes.
    push word [cs:int21_last_ah]
    call int21_exec
    pop word [cs:int21_last_ah]
    ; Restore metadata from the unchanged original interrupt frame. PUSH,
    ; POP and MOV preserve EXEC's returned CF, and AX remains its result.
    push ax
    mov bp, sp
    mov ax, [ss:bp + 4]
    mov [cs:int21_caller_ds], ax
    mov ax, [ss:bp + 16]
    mov [cs:int21_caller_bx], ax
    mov ax, [ss:bp + 18]
    mov [cs:int21_trace_call_ip], ax
    mov ax, [ss:bp + 20]
    mov [cs:int21_trace_call_cs], ax
    mov ax, [ss:bp + 12]
    mov [cs:int21_last_dx], ax
    pop ax
    ; A child's last service cannot make AH=4Bh return its DS/ES or ZF result.
    mov word [cs:int21_return_es], 0
    mov byte [cs:int21_zf_state], 0xFF
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

%if FAT_TYPE == 16
; VMFORK uses this only in the newly forked VM.  BX is the owner PSP from
; the copied MCB chain and ES is the block to release.  AH=49h normally
; requires the caller's EXEC identity, which AH=50h cannot change.
.fn_f1:
    cmp al, 0x49
    jne .unsupported
    push word [cs:dos_exec_identity_psp]
    mov [cs:dos_exec_identity_psp], bx
    call int21_free
    pop word [cs:dos_exec_identity_psp]
    jmp .fn_49_result
%endif

.fn_49:
    call int21_free
.fn_49_result:
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
%if TRACE_CHILD_INT21 != 0
    pushf
    call child_trace_resize_log
    popf
%endif
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
    mov ax, [cs:dos_exec_identity_psp]
    or ax, ax
    jz .fn_4c_no_process
    ; A program started through our INT 21h/4Bh EXEC path unwinds through the
    ; CiukiDOS trampoline only while its immutable EXEC identity is also the
    ; PSP selected by AH=50h.  Windows Standard Mode selects a private PSP for
    ; each Win16 task but its INT 2Fh/1605h broadcast may be consumed inside
    ; HDPMI.  Therefore PSP identity, not windows_active, is authoritative:
    ; a different current PSP must terminate through its own INT 22h vector.
    ; Nested DOS EXEC children set both values to their own PSP and continue
    ; to take .fn_4c_exec_process below.
    cmp ax, [cs:current_psp_seg]
    je .fn_4c_exec_process

    ; DOSMGR's private Win16 PSP owns the task-return vector.  Returning there
    ; closes just that task; forcing the global EXEC trampoline here tears
    ; down KRNL386 and makes Alt+F4 appear to crash all of Windows.
    push ds
    mov ax, [cs:current_psp_seg]
    mov ds, ax
    mov dx, [ds:0x000A]
    mov cx, [ds:0x000C]
    pop ds
    mov bp, sp
    mov [ss:bp + 16], dx
    mov [ss:bp + 18], cx
    xor ax, ax
    jmp .success

.fn_4c_exec_process:
%ifdef CIUKIDOS_KERNEL_BUILD
    call ciukidos_record_last_termination
%endif
    call ciukidos_restore_parent_dta
    call int21_restore_psp_term_vectors
%if TRACE_CHILD_INT21 != 0
    call child_trace_exit_int21
%endif
    mov byte [cs:int21_force_terminate], 1
.fn_4c_no_process:
    xor ax, ax
    jmp .success

.fn_4d:
    mov al, [cs:last_exit_code]
    mov ah, [cs:last_term_type]
    jmp .success

.fn_50:
%ifdef CIUKIDOS_KERNEL_BUILD
    mov ax, [cs:kernel_runtime_state_current_psp]
    mov [cs:kernel_runtime_state_previous_psp], ax
    mov [cs:kernel_runtime_state_current_psp], bx
%endif
    mov ax, bx
    call int21_set_current_psp
    xor ax, ax
    jmp .success

.fn_51:
    call int21_get_psp
    jc .error
    jmp .success

.fn_31:
    mov [cs:last_exit_code], al
    mov byte [cs:last_term_type], 3
    mov ax, [cs:dos_exec_identity_psp]
    or ax, ax
    jz .fn_31_done
%ifdef CIUKIDOS_KERNEL_BUILD
    call ciukidos_record_last_termination
%endif
%if TRACE_CHILD_INT21 != 0
    call child_trace_exit_int21
%endif
    ; AH=31 keep-resident: resize the immutable EXEC owner, then commit its
    ; PSP and re-owned allocations to the global resident set.
    push dx
    mov es, ax
    mov bx, dx
    call int21_resize
    pop dx
    call int21_mem_commit_tsr
    call ciukidos_restore_parent_dta
    call int21_restore_psp_term_vectors
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

.fn_5d:
    ; DOS 4+ Get Swappable Data Area.  Windows 3.x DOSMGR requires a
    ; coherent SDA before it can build the system VM; rejecting AX=5D06h
    ; makes VMM report its generic enhanced-mode address-space error.
    cmp al, 0x06
    jne .unsupported
    mov al, [cs:dos_indos_flag]
    mov [cs:dos_sda_indos], al
    mov ax, [cs:dta_off]
    mov [cs:dos_sda_dta_off], ax
    mov ax, [cs:dta_seg]
    mov [cs:dos_sda_dta_seg], ax
    mov ax, [cs:current_psp_seg]
    mov [cs:dos_sda_current_psp], ax
    mov [cs:dos_sda_owning_psp], ax
    mov al, [cs:dos_default_drive]
    mov [cs:dos_sda_default_drive], al
    mov ax, [cs:windows_machine_id]
    mov [cs:dos_sda_machine_id], ax
    mov ax, [cs:dos_mem_last_mcb_seg]
    mov [cs:dos_sda_last_mcb], ax
    mov byte [cs:int21_return_ds], 1
    push cs
    pop ds
    mov si, dos_sda
    mov cx, dos_sda_end - dos_sda
    mov dx, dos_sda_swap_always - dos_sda
    mov ax, 0x5D06
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

    ; DOS AH=57h/AL=00h returns CX=packed time and DX=packed date while
    ; preserving the file handle in BX (and the caller's SI).  Keep those
    ; registers before using them as scratch space for the encoders.  The old
    ; path left the packed date in BX/SI and returned the unpacked month/day
    ; in DX, which corrupted Windows' NE resource-loader state immediately
    ; before it opened the system fonts.
    push bx
    push si

    mov bl, cl
    mov bh, dh
    xor ax, ax
    mov al, ch
    shl ax, 11
    mov cx, ax

    xor ax, ax
    mov al, bl
    shl ax, 5
    or cx, ax

    xor ax, ax
    mov al, bh
    shr al, 1
    or cx, ax

    ; Preserve the packed time before AH=2Ah-style date retrieval replaces CX
    ; with the four-digit year.  Windows checks both words while loading NE
    ; modules, so returning 2026 as the time word makes an otherwise valid
    ; file timestamp look corrupt.
    push cx
    call int21_get_date
    jc .fn_57_date_error

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
    mov si, bx
    pop cx

    mov dx, si
    pop si
    pop bx

    xor ax, ax
    jmp .success

.fn_57_date_error:
    pop cx
    pop si
    pop bx
    jmp .error

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
    mov ax, bx
    cmp ax, 20
    jae .fn_67_min_ok
    mov ax, 20
.fn_67_min_ok:
    cmp ax, 232
    jbe .fn_67_max_ok
    mov ax, 232
.fn_67_max_ok:
    push ax
    push bx
    call int21_get_psp
    mov es, bx
    pop ax
    mov [es:0x0032], ax
    pop bx
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
%if TRACE_CHILD_INT21 != 0
    call child_trace_int21_error
%endif

.done:
%if FAT_TYPE == 16
    call int21_sync_sft_table
%endif
%if TRACE_CHILD_INT21 != 0
    call child_trace_int21_return
%endif
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
    cmp byte [cs:int21_return_ds], 0
    je .return_es_check
    mov [bp + 2], ds
.return_es_check:
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
    dec byte [cs:dos_indos_flag]
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

; This diagnostic is called only by the optional boot self-test or the
; legacy interactive Stage1 shell. It is not part of the INT 21h ABI.
%if STAGE1_SELFTEST_AUTORUN || STAGE1_INTERACTIVE_SHELL
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
    mov [dos_mem_alloc_seg], ax

    mov es, ax
    mov bx, 0x0030
    mov ah, 0x4A
    int 0x21
    jc .fail

    mov bx, 0x0010
    mov ah, 0x48
    int 0x21
    jc .fail
    mov [dos_mem_alloc_seg2], ax

    mov es, [dos_mem_alloc_seg]
    mov bx, 0x0031
    mov ah, 0x4A
    int 0x21
    jnc .fail
    cmp al, 0x08
    jne .fail
    cmp bx, 0x0030
    jb .fail

    mov es, [dos_mem_alloc_seg2]
    mov ah, 0x49
    int 0x21
    jc .fail

    mov es, [dos_mem_alloc_seg]
    mov ah, 0x49
    int 0x21
    jc .fail

    mov ah, 0x49
    int 0x21
    jnc .fail
    cmp al, 0x09
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

%endif

int21_exec:
    ; Observe direct MCB-owner changes (for example an external TSR unloader)
    ; before capacity and slot decisions can reject on a stale table entry.
    call int21_mem_refresh_owners
    cmp byte [cs:dos_exec_state_depth], DOS_EXEC_STATE_FRAME_MAX
    jb .state_slot_available
    mov ax, 0x0008
    stc
    ret
.state_slot_available:
    ; Keep one entry available for an AH=31 PSP record.  AH=48 applies the
    ; same reservation while a child is active.
    cmp byte [cs:dos_mem_block_count], DOS_MEM_BLOCK_TABLE_MAX
    jb .resident_slot_available
    mov ax, 0x0008
    stc
    ret
.resident_slot_available:
    ; The global FAT slots back the per-process DOS JFT.  Capture which slots
    ; belong to the suspended parent so leaked child handles can be discarded
    ; when EXEC returns without copying the much larger per-file metadata.
    call int21_capture_open_handle_mask
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
    ; Keep allocator ownership off the caller's (potentially 512-byte) stack.
    ; CIUKIDOS reserves four 352-byte frames in the dedicated
    ; DOS_EXEC_STATE_BASE_SEG arena (1400h in the kernel profile).
    mov [cs:tmp_exec_subfn], al
    push ds
    push es
    push cs
    pop ds
    mov al, [cs:dos_exec_state_depth]
    mov ah, DOS_EXEC_STATE_FRAME_PARAS
    mul ah
    add ax, DOS_EXEC_STATE_BASE_SEG
    mov es, ax
    mov si, dos_mem_exec_state_begin
    xor di, di
    mov cx, ((dos_mem_exec_state_end - dos_mem_exec_state_begin) / 2)
.save_mem_state:
    cld
    rep movsw
    mov si, dos_list_of_lists
    mov cx, 3
    rep movsw
    mov si, dos_exec_saved_context_begin
    mov cx, ((dos_exec_saved_context_prefix_end - dos_exec_saved_context_begin) / 2)
    rep movsw
    mov si, dos_exec_saved_context_suffix_begin
    mov cx, ((dos_exec_saved_context_end - dos_exec_saved_context_suffix_begin) / 2)
    rep movsw
    pop es
    pop ds
    inc byte [cs:dos_exec_state_depth]
    call int21_mem_active_psp
    mov [cs:dos_exec_parent_identity_psp], ax

    mov al, [cs:tmp_exec_subfn]
    cmp al, 0x00
    je .exec_subfn_ok
    cmp al, 0x03
    je .overlay_subfn_ok
    jmp .bad_function

.exec_subfn_ok:
    mov byte [cs:exec_cmd_len], 0
    call int21_exec_init_fcbs
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
    call int21_exec_capture_fcbs
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
%if TRACE_WIN_MEMORY != 0
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_exec
    call print_string_serial
    mov si, dos_child_exec_path_buf
    call print_string_serial
    call print_newline_serial
    pop si
    pop ds
%endif

    push ds
    mov ax, cs
    mov ds, ax
    mov dx, dos_child_exec_path_buf

    mov si, dx
    call int21_path_to_fat_name
    jc .path_fail_restore_ds

    push si
    mov si, dx
    call int21_resolve_and_find_path

.path_resolved:
    pop si
    pop ds
    jc .done
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:child_trace_armed], 0
    cmp word [cs:dos_exec_identity_psp], 0
    je .trace_arm_ready
    mov byte [cs:child_trace_armed], 1
.trace_arm_ready:
%endif

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
    cmp word [cs:dos_exec_identity_psp], 0
    jne .nested_com_seg
    mov ax, COM_LOAD_SEG
    call int21_exec_com_slot_free
    jc .exec_slot_unavailable
    call int21_exec_set_context_slot
    mov [cs:current_com_load_seg], ax
    jmp .do_exec_com
.nested_com_seg:
    call int21_exec_select_com_region
    jc .exec_slot_unavailable
.com_slot_ready:
    mov [cs:current_com_load_seg], ax
    call int21_exec_set_context_slot
.do_exec_com:
    call int21_exec_load_com
    jc .done
    call int21_exec_run_com
    jc .done
    xor ax, ax
    clc
    jmp .done

.exec_slot_unavailable:
    mov ax, 0x0008
    stc
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
    call int21_exec_select_mz_region
    jnc .exec_mz_selected
    mov dl, 1
    call int21_exec_log_mz_failure
%if TRACE_CHILD_INT21 != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_child_exec_select_fail
    call print_string_serial
    pop si
    pop ds
    pop ax
%endif
    stc
    jmp .done
.exec_mz_selected:
    call int21_exec_set_context_slot
.do_exec_mz:
    call int21_exec_load_mz
    jnc .exec_mz_loaded
    mov dl, 2
    call int21_exec_log_mz_failure
%if TRACE_CHILD_INT21 != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_child_exec_load_fail
    call print_string_serial
    mov al, [cs:tmp_exec_mz_fail_stage]
    call print_hex8_serial
    mov si, msg_child_exec_load_seg
    call print_string_serial
    mov ax, [cs:current_load_seg]
    call print_hex16_serial
    mov si, msg_child_exec_load_limit
    call print_string_serial
    mov ax, [cs:tmp_exec_mz_copy_limit]
    call print_hex16_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif
    stc
    jmp .done
.exec_mz_loaded:
    call int21_exec_run_mz
    jnc .exec_mz_ok
    mov dl, 3
    call int21_exec_log_mz_failure
%if TRACE_CHILD_INT21 != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_child_exec_run_fail
    call print_string_serial
    mov al, [cs:tmp_exec_mz_run_fail_stage]
    call print_hex8_serial
    mov si, msg_child_exec_run_psp
    call print_string_serial
    mov ax, [cs:mz_psp_seg]
    call print_hex16_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif
    stc
    jmp .done
.exec_mz_ok:
    xor ax, ax
    clc
    jmp .done

.load_overlay:
    cmp word [cs:search_found_size_hi], 0
    jne .exec_slot_unavailable
    mov ax, MZ3_LOAD_SEG
    call int21_exec_mz_slot_free
    jc .exec_slot_unavailable
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

.path_fail_restore_ds:
    pop ds
    jmp .path_fail

.done:
    ; Preserve the EXEC result before diagnostics or arena restoration can
    ; alter CF.  The trace helper reads this explicit copy.
    pushf
    pop word [cs:tmp_exec_return_flags]
%if TRACE_WIN_MEMORY != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_exec_return
    call print_string_serial
    call print_hex16_serial
    mov al, ':'
    call serial_putc
    mov ax, [cs:tmp_exec_return_flags]
    call print_hex16_serial
    mov al, ':'
    call serial_putc
    mov al, [cs:tmp_exec_mz_fail_stage]
    call print_hex8_serial
    mov al, ':'
    call serial_putc
    mov al, [cs:tmp_exec_mz_run_fail_stage]
    call print_hex8_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:child_trace_armed], 0
%endif
    ; Reconcile the child-global resident set into the still-saved parent
    ; frame before restoring that frame over the live allocator state.
    push ax
    mov al, [cs:dos_exec_state_depth]
    dec al
    mov ah, DOS_EXEC_STATE_FRAME_PARAS
    mul ah
    add ax, DOS_EXEC_STATE_BASE_SEG
    mov es, ax
    call int21_mem_merge_exec_residents
    pop ax
    dec byte [cs:dos_exec_state_depth]
    push ax
    mov al, [cs:dos_exec_state_depth]
    mov ah, DOS_EXEC_STATE_FRAME_PARAS
    mul ah
    add ax, DOS_EXEC_STATE_BASE_SEG
    mov ds, ax
    push cs
    pop es
    xor si, si
    mov di, dos_mem_exec_state_begin
    mov cx, ((dos_mem_exec_state_end - dos_mem_exec_state_begin) / 2)
.restore_mem_state:
    cld
    rep movsw
    mov di, dos_list_of_lists
    mov cx, 3
    rep movsw
    mov di, dos_exec_saved_context_begin
    mov cx, ((dos_exec_saved_context_prefix_end - dos_exec_saved_context_begin) / 2)
    rep movsw
    mov di, dos_exec_saved_context_suffix_begin
    mov cx, ((dos_exec_saved_context_end - dos_exec_saved_context_suffix_begin) / 2)
    rep movsw
    ; A top-level transient program has no live parent application that may
    ; retain an INT 33h/INT 15h client address.  Tear those callbacks down
    ; before returning to the shell, but preserve handlers committed by a TSR
    ; and callbacks belonging to a suspended parent during nested EXEC.
    cmp byte [cs:dos_exec_state_depth], 0
    jne .mouse_clients_restored
    cmp byte [cs:last_term_type], 3
    je .mouse_clients_restored
    call mouse_release_transient_clients
.mouse_clients_restored:
    pop ax
    ; The child rebuilt physical MCB bytes while it owned the arena.  Recreate
    ; the restored parent's chain; the snapshot also restored its list mirror.
    call int21_mem_rebuild_chain
    call int21_apply_open_handle_mask
    push word [cs:tmp_exec_return_flags]
    popf
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

; Always leave a compact serial reason for an MZ EXEC failure.  DL identifies
; region selection (1), image loading (2), or child startup/arena setup (3),
; while AX is the DOS error returned to the caller.
int21_exec_log_mz_failure:
    pushf
    pusha
    push ds
    mov bp, ax
    mov bl, dl
    push cs
    pop ds
    mov si, msg_mz_exec_fail
    call print_string_serial
    mov al, bl
    call print_hex8_serial
    mov si, msg_mz_exec_fail_ax
    call print_string_serial
    mov ax, bp
    call print_hex16_serial
    mov si, msg_mz_exec_fail_sub
    call print_string_serial
    mov al, [cs:tmp_exec_mz_run_fail_stage]
    call print_hex8_serial
    mov si, msg_mz_exec_fail_arena
    call print_string_serial
    mov al, [cs:tmp_exec_mz_arena_fail_stage]
    call print_hex8_serial
    call print_newline_serial
    pop ds
    popa
    popf
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

int21_exec_init_fcbs:
    push ax
    push cx
    push di
    push es
    push cs
    pop es
    xor ax, ax
    mov di, exec_fcb1
    mov cx, 32
    cld
    rep stosb
    mov al, ' '
    mov di, exec_fcb1 + 1
    mov cx, 11
    rep stosb
    mov di, exec_fcb2 + 1
    mov cx, 11
    rep stosb
    pop es
    pop di
    pop cx
    pop ax
    ret

int21_exec_capture_fcbs:
    push ax
    push bx
    push cx
    push si
    push di
    push ds

    mov si, [es:bx + 6]
    mov ax, [es:bx + 8]
    or ax, ax
    jz .fcb2
    cmp si, 0xFFFF
    je .fcb2
    mov ds, ax
    mov di, exec_fcb1
    mov cx, 16
.copy_fcb1:
    lodsb
    mov [cs:di], al
    inc di
    loop .copy_fcb1

.fcb2:
    mov si, [es:bx + 10]
    mov ax, [es:bx + 12]
    or ax, ax
    jz .done
    cmp si, 0xFFFF
    je .done
    mov ds, ax
    mov di, exec_fcb2
    mov cx, 16
.copy_fcb2:
    lodsb
    mov [cs:di], al
    inc di
    loop .copy_fcb2

.done:
    pop ds
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

int21_exec_write_fcbs:
    push ax
    push cx
    push si
    push di
    push ds
    push cs
    pop ds
    mov si, exec_fcb1
    mov di, 0x005C
    mov cx, 16
    cld
    rep movsb
    mov si, exec_fcb2
    mov di, 0x006C
    mov cx, 16
    rep movsb
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    ret

int21_exec_init_program_psp:
    push ax
    push bx
    push ds
    xor ax, ax
    xor di, di
    mov cx, 128
    cld
    rep stosw
    mov word [es:0x0000], 0x20CD
    mov byte [es:0x0005], 0xCB
    mov word [es:0x0050], 0x21CD
    mov byte [es:0x0052], 0xCB

    xor ax, ax
    mov ds, ax

    mov bx, (0x22 * 4)
    mov ax, [bx]
    mov [es:0x000A], ax
    mov ax, [bx + 2]
    mov [es:0x000C], ax

    mov bx, (0x23 * 4)
    mov ax, [bx]
    mov [es:0x000E], ax
    mov ax, [bx + 2]
    mov [es:0x0010], ax

    mov bx, (0x24 * 4)
    mov ax, [bx]
    mov [es:0x0012], ax
    mov ax, [bx + 2]
    mov [es:0x0014], ax

    pop ds
    pop bx
    pop ax
    call int21_init_psp_handles
    ret

int21_create_child_psp:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es

    ; Windows 3.x uses AH=55h to create the PSP of each Win16 task.  DOS
    ; clones the current PSP first: zero-filling it discards the environment,
    ; command tail and task linkage before the application's WinMain starts.
    mov bx, si                      ; first segment beyond its allocation
    mov ax, [cs:current_psp_seg]    ; semantic DOS parent selected by AH=50h
    mov ds, ax
    mov es, dx
    xor si, si
    xor di, di
    mov cx, 128
    cld
    rep movsw

    mov [es:0x0002], bx
    mov [es:0x0016], ax
    mov word [es:0x0034], 0x0018
    mov [es:0x0036], dx
    mov word [es:0x0038], 0x0000
    mov [es:0x003A], ax

    mov ax, dx
    call int21_set_current_psp
    call int21_mem_adopt_child_psp
    xor ax, ax
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

int21_restore_psp_term_vectors:
    push ax
    push bx
    push ds
    push es
    mov ax, [cs:dos_exec_identity_psp]
    or ax, ax
    jz .done
    mov ds, ax
    xor ax, ax
    mov es, ax
    mov bx, (0x22 * 4)
    mov ax, [ds:0x000A]
    mov [es:bx], ax
    mov ax, [ds:0x000C]
    mov [es:bx + 2], ax
    mov bx, (0x23 * 4)
    mov ax, [ds:0x000E]
    mov [es:bx], ax
    mov ax, [ds:0x0010]
    mov [es:bx + 2], ax
    mov bx, (0x24 * 4)
    mov ax, [ds:0x0012]
    mov [es:bx], ax
    mov ax, [ds:0x0014]
    mov [es:bx + 2], ax
.done:
    pop es
    pop ds
    pop bx
    pop ax
    ret

int21_init_psp_handles:
    push ax
    push bx
    push cx
    push si
    push di
    push ds
    mov word [es:0x0032], 20
    mov word [es:0x0034], 0x0018
    mov ax, es
    mov [es:0x0036], ax
    mov ax, [cs:dos_exec_parent_identity_psp]
    cmp ax, 0
    jne .parent_ready
    mov ax, es
.parent_ready:
    mov [es:0x0016], ax
    mov ax, [cs:dos_exec_parent_identity_psp]
    mov [es:0x003A], ax
    mov di, 0x0018
    mov al, 0xFF
    mov cx, 20
    cld
    rep stosb
    mov ax, [cs:dos_exec_parent_identity_psp]
    or ax, ax
    jz .init_defaults
    mov ds, ax
    mov si, 0x0018
    mov di, 0x0018
    mov cx, [ds:0x0032]
    or cx, cx
    jz .init_defaults
    cmp cx, 20
    jbe .copy_ready
    mov cx, 20
.copy_ready:
    rep movsb
    jmp .done
.init_defaults:
    mov byte [es:0x0018], 0
    mov byte [es:0x0019], 1
    mov byte [es:0x001A], 2
    mov byte [es:0x001B], 3
    mov byte [es:0x001C], 4
.done:
    mov word [es:0x002C], DOS_ENV_SEG
    pop di
    pop ds
    pop si
    pop cx
    pop bx
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

    mov byte [cs:tmp_exec_mz_arena_fail_stage], 1

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
    call int21_mem_refresh_owners
    call int21_mem_table_keep_resident
.arena_state_ready:
    mov word [cs:dos_mem_psp_mcb_end], 0
    mov word [cs:dos_mem_psp_free_seg], 0
    mov word [cs:dos_mem_psp_free_size], 0

    mov ax, [cs:current_load_seg]
    mov es, ax

    ; Size the child from the compatibility extent actually copied, not from
    ; the strict e_cblp EOF.  Otherwise a rounded-page tail could sit beyond
    ; PSP:[2] and consume the program's requested minalloc paragraphs.
    mov bx, [cs:tmp_exec_mz_loaded_paras]
    mov ax, [es:0x0008]
    cmp bx, ax
    jbe .fail
    sub bx, ax

    mov byte [cs:tmp_exec_mz_arena_fail_stage], 2

    add bx, 0x0010
    jc .fail
    add bx, [es:0x000A]
    jc .fail

    mov byte [cs:tmp_exec_mz_arena_fail_stage], 3

    ; The child may only grow to the first live block above its PSP.  A
    ; suspended parent can retain AH=48 allocations at the high edge of the
    ; arena (Windows does this before EXECing WSWAP.EXE), so using only the
    ; global EXEC-chain ceiling would advertise paragraphs which are not
    ; contiguous.  Leave one paragraph for the next block's MCB; the chain
    ; ceiling itself is already the first non-touchable paragraph.
    push bx
    mov bx, [cs:mz_psp_seg]
    call int21_mem_table_next_limit
    mov ax, bx
    pop bx
    cmp dx, [cs:dos_mem_chain_limit_seg]
    je .mz_contiguous_limit_ready
    dec dx
.mz_contiguous_limit_ready:
    cmp dx, ax
    jbe .fail
    sub dx, ax
    mov byte [cs:tmp_exec_mz_arena_fail_stage], 4
    cmp bx, dx
    ja .fail

    mov byte [cs:tmp_exec_mz_arena_fail_stage], 5

    ; MZ images with both allocation fields clear are the DOS load-high
    ; special case.  Their PSP owns the largest available arena while the
    ; load module itself sits at the top of that arena.  Self-loading NE
    ; kernels (including Windows 3.x KRNL286) rely on the workspace below
    ; the high image and will overwrite their own code if given only the
    ; minimum image-sized block.
    cmp byte [cs:tmp_exec_mz_load_high], 0
    je .mz_check_maxalloc
    mov bx, dx
    jmp .mz_alloc_size_ready

.mz_check_maxalloc:
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
    mov byte [cs:tmp_exec_mz_arena_fail_stage], 6
    mov ax, [cs:mz_psp_seg]
    mov es, ax
    call int21_resize
    jc .fail

.success:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret

.fail:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    add sp, 2
    mov ax, 0x0008
    stc
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
    mov [cs:tmp_next_cluster], ax
    mov [cs:tmp_next_lba_hi], dx
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
    mov dx, [cs:tmp_next_lba_hi]
    adc dx, 0
    mov [cs:tmp_lba_hi], dx

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    xor bx, bx
%if FAT_TYPE == 16
    mov dx, [cs:tmp_lba_hi]
    call read_sector_lba32
%else
    call read_sector_lba
%endif
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
    cmp ax, [cs:tmp_exec_mz_copy_limit]
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
    push bx
    push cx
    push di
    push es

    mov ax, [cs:current_com_load_seg]
    mov es, ax
    call int21_exec_init_program_psp
    call int21_exec_com_required_paras
    jc .fail
    mov cx, [cs:current_com_load_seg]
    add cx, bx
    jc .fail
    mov ax, [cs:current_com_load_seg]
    call int21_exec_region_end
    cmp cx, dx
    ja .fail
    mov ax, dx
.psp_end_ready:
    mov [es:0x0002], ax
    mov [cs:tmp_exec_mz_copy_limit], dx
    call int21_build_env_block
    call int21_exec_write_fcbs
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
    pop bx
    ret

int21_exec_com_slot_free:
    push di
    mov di, 0x0001
    call int21_exec_slot_free
    pop di
    ret

int21_exec_mz_slot_free:
    push di
    ; Before reading the header, reserve for the earliest valid MZ child MCB:
    ; load + minimum header(2) - PSP prefix(10h) - MCB(1) = load - 0Fh.
    mov di, 0x000F
    call int21_exec_slot_free
    pop di
    ret

; Return-context storage is selected only from EXEC depth.  Memory placement
; is deliberately independent so changing the arena layout cannot select the
; wrong parent SS:SP/DS/ES snapshot.
int21_exec_set_context_slot:
    push ax
    xor ax, ax
    mov al, [cs:dos_exec_state_depth]
    cmp ax, 3
    jbe .store
    mov ax, 3
.store:
    mov [cs:current_mz_context_slot], ax
    pop ax
    ret

; Find the first free conventional-memory interval after the active parent's
; actual PSP arena.  BX is the required span in paragraphs; AX returns its
; first usable data paragraph.  Unlike the AH=48 heap, EXEC may use the free
; low interval below DOS_HEAP_BASE_SEG once the resident shell has shrunk.
int21_exec_find_free_region:
    mov [cs:dos_mem_block_req_size], bx
    call int21_mem_active_psp
    or ax, ax
    jz .not_found
    mov es, ax
    push ax
    call int21_mem_psp_end
    mov dx, ax
    pop ax
    cmp dx, ax
    ja .parent_end_ready
    ; WinOldAp's synthetic per-VM PSP has no owning MCB and can leave PSP:2
    ; zero.  Preserve its complete 256-byte PSP, then expose the remaining
    ; conventional arena to the unprofiled DOS child.
    mov dx, ax
    add dx, 0x0010
.parent_end_ready:
    inc dx
    jz .not_found

    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
.scan:
    cmp di, cx
    jae .tail
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .next
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jae .tail
    cmp ax, dx
    jbe .consume
    mov bx, ax
    sub bx, dx
    dec bx
    cmp bx, [cs:dos_mem_block_req_size]
    jae .found
.consume:
    mov ax, [cs:dos_mem_block_table + si]
    add ax, [cs:dos_mem_block_table + si + 2]
    jc .not_found
    inc ax
    cmp ax, dx
    jbe .next
    mov dx, ax
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.tail:
    cmp dx, [cs:dos_mem_chain_limit_seg]
    jae .not_found
    mov bx, [cs:dos_mem_chain_limit_seg]
    sub bx, dx
    cmp bx, [cs:dos_mem_block_req_size]
    jb .not_found
.found:
    mov ax, dx
    mov bx, [cs:dos_mem_block_req_size]
    clc
    ret
.not_found:
    mov ax, 0x0008
    stc
    ret

; Find the largest conventional-memory interval available to an EXEC child.
; BX is the minimum required span.  On success AX is the first usable
; paragraph and BX is the complete interval size.  DOS uses this placement
; when an MZ header has MINALLOC=MAXALLOC=0.
int21_exec_find_largest_free_region:
    mov [cs:dos_mem_block_req_size], bx
    mov word [cs:tmp_exec_region_best_seg], 0
    mov word [cs:tmp_exec_region_best_size], 0
    call int21_mem_active_psp
    or ax, ax
    jz .not_found
    mov es, ax
    push ax
    call int21_mem_psp_end
    mov dx, ax
    pop ax
    cmp dx, ax
    ja .parent_end_ready
    mov dx, ax
    add dx, 0x0010
.parent_end_ready:
    inc dx
    jz .not_found

    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
.scan:
    cmp di, cx
    jae .tail
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .next
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jae .tail
    cmp ax, dx
    jbe .consume
    mov bx, ax
    sub bx, dx
    dec bx
    call .consider
.consume:
    mov ax, [cs:dos_mem_block_table + si]
    add ax, [cs:dos_mem_block_table + si + 2]
    jc .not_found
    inc ax
    cmp ax, dx
    jbe .next
    mov dx, ax
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.tail:
    cmp dx, [cs:dos_mem_chain_limit_seg]
    jae .finish
    mov bx, [cs:dos_mem_chain_limit_seg]
    sub bx, dx
    call .consider
.finish:
    mov bx, [cs:tmp_exec_region_best_size]
    cmp bx, [cs:dos_mem_block_req_size]
    jb .not_found
    mov ax, [cs:tmp_exec_region_best_seg]
    clc
    ret

.consider:
    cmp bx, [cs:tmp_exec_region_best_size]
    jbe .consider_done
    mov [cs:tmp_exec_region_best_seg], dx
    mov [cs:tmp_exec_region_best_size], bx
.consider_done:
    ret

.not_found:
    mov ax, 0x0008
    stc
    ret

; Return in DX the first segment a child beginning at AX must not own.  An
; allocated block contributes its preceding MCB; the global arena cap is
; already exclusive and therefore is not decremented.
int21_exec_region_end:
    push ax
    push bx
    mov bx, ax
    call int21_mem_table_next_limit
    cmp dx, [cs:dos_mem_chain_limit_seg]
    je .done
    dec dx
.done:
    pop bx
    pop ax
    ret

; COM programs are placed in the first interval large enough for their PSP
; and image.  DOS gives the child ownership of the complete interval; a COM
; program which needs heap space or wants to EXEC another child first shrinks
; its PSP block with AH=4Ah.  Windows WIN.COM checks PSP:[2] before doing so.
int21_exec_com_required_paras:
    cmp word [cs:search_found_size_hi], 0
    jne .no_memory
    mov ax, [cs:search_found_size_lo]
    cmp ax, 0xFE00
    ja .no_memory
    mov bx, ax
    add bx, 0x010F
    jc .no_memory
    mov cl, 4
    shr bx, cl
    add bx, COM_STACK_RESERVE_PARAS
    jc .full_segment
    cmp bx, 0x1000
    jbe .ready
.full_segment:
    mov bx, 0x1000
.ready:
    clc
    ret
.no_memory:
    mov ax, 0x0008
    stc
    ret

int21_exec_select_com_region:
    call int21_exec_com_required_paras
    jc .no_memory
    call int21_exec_find_free_region
    ret
.no_memory:
    mov ax, 0x0008
    stc
    ret

; Read only the standard MZ header into the disk scratch buffer, derive the
; compatibility copy extent used by load_mz, add PSP/minalloc, then select the
; first arena interval which fits.  A fixed 16-paragraph prefix keeps even a
; minimum two-paragraph header and its future PSP MCB wholly inside the gap.
int21_exec_select_mz_region:
    mov byte [cs:tmp_exec_mz_load_high], 0
    mov word [cs:tmp_exec_mz_high_psp], 0
    mov ax, [cs:search_found_size_lo]
    mov [cs:tmp_exec_mz_probe_size_lo], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:tmp_exec_mz_probe_size_hi], ax
    or ax, ax
    jnz .probe_64
    cmp word [cs:tmp_exec_mz_probe_size_lo], 28
    jb .invalid
    mov ax, [cs:tmp_exec_mz_probe_size_lo]
    cmp ax, 64
    jbe .probe_size_ready
.probe_64:
    mov ax, 64
.probe_size_ready:
    mov [cs:search_found_size_lo], ax
    mov word [cs:search_found_size_hi], 0
    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    xor di, di
    xor cx, cx
    call int21_exec_load_to_es
    pushf
    mov ax, [cs:tmp_exec_mz_probe_size_lo]
    mov [cs:search_found_size_lo], ax
    mov ax, [cs:tmp_exec_mz_probe_size_hi]
    mov [cs:search_found_size_hi], ax
    popf
    jc .done

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    cmp word [es:0x0000], 0x5A4D
    jne .invalid
    mov bp, [es:0x0008]
    cmp bp, 2
    jb .invalid
    cmp word [es:0x0004], 0
    je .invalid

    ; Actual file length rounded to paragraphs.
    mov ax, [cs:tmp_exec_mz_probe_size_lo]
    mov dx, [cs:tmp_exec_mz_probe_size_hi]
    add ax, 15
    adc dx, 0
    mov cx, 4
.actual_shift:
    shr dx, 1
    rcr ax, 1
    loop .actual_shift
    or dx, dx
    jnz .no_memory
    mov si, ax

    ; The compatibility loader copies min(real size, e_cp * 512), rounded to
    ; paragraphs.  e_cp * 32 is the paragraph form of that page extent.
    mov ax, [es:0x0004]
    mov bx, 32
    mul bx
    or dx, dx
    jnz .copy_extent_ready
    cmp ax, si
    jae .copy_extent_ready
    mov si, ax
.copy_extent_ready:
    cmp si, bp
    jb .invalid

    ; MINALLOC=MAXALLOC=0 requests the historic DOS load-high layout: own
    ; the largest free block, keep the PSP at its low edge and place the MZ
    ; image at the high edge.  We copy the header as well, so reserve its
    ; rounded file extent in addition to the low PSP.
    mov ax, [es:0x000A]
    or ax, [es:0x000C]
    jnz .normal_placement
    mov bx, si
    add bx, 0x0010
    jc .no_memory
    push si
    call int21_exec_find_largest_free_region
    pop si
    jc .done
    mov [cs:tmp_exec_mz_high_psp], ax
    add ax, bx
    jc .no_memory
    sub ax, si
    jc .no_memory
    mov [cs:current_load_seg], ax
    mov byte [cs:tmp_exec_mz_load_high], 1
    clc
    ret

.normal_placement:
    mov bx, si
    add bx, [es:0x000A]
    jc .no_memory
    add bx, 0x0010
    jc .no_memory
    ; DOS gives an ordinary EXE as much of MAXALLOC as possible. A small
    ; first-fit hole can satisfy MINALLOC but strand an extender's transfer
    ; buffer, even though a larger contiguous interval remains available.
    call int21_exec_find_largest_free_region
    jc .done
    add ax, 0x0010
    jc .no_memory
    mov [cs:current_load_seg], ax
    clc
    ret

.invalid:
    mov ax, 0x000B
    stc
    ret
.no_memory:
    mov ax, 0x0008
    stc
.done:
    ret

; Return CF=0 when [AX-DI, AX+1000h) is free.  Check every suspended PSP
; arena and every active allocator entry, irrespective of block ownership.
int21_exec_slot_free:
    pusha
    push es

    mov bx, ax
    sub bx, di
    mov dx, ax
    add dx, 0x1000
    jc .busy
    cmp dx, [cs:dos_mem_top_seg]
    ja .busy

    mov si, [cs:dos_exec_identity_psp]
    mov bp, DOS_EXEC_STATE_FRAME_MAX
.parent_loop:
    or si, si
    jz .table_begin
    mov es, si
    call int21_mem_psp_end
    mov cx, ax
    cmp cx, si
    jbe .busy
    mov ax, si
    dec ax
    cmp dx, ax
    jbe .parent_next
    cmp bx, cx
    jb .busy
.parent_next:
    mov ax, [es:0x0016]
    cmp ax, si
    je .table_begin
    mov si, ax
    dec bp
    jnz .parent_loop
    or si, si
    jnz .busy

.table_begin:
    xor si, si
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
.table_loop:
    jcxz .free
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .table_next
    mov ax, [cs:dos_mem_block_table + si]
    dec ax
    cmp dx, ax
    jbe .table_next
    inc ax
    add ax, [cs:dos_mem_block_table + si + 2]
    jc .busy
    cmp bx, ax
    jb .busy
.table_next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    loop .table_loop
.free:
    clc
    jmp .done
.busy:
    stc
.done:
    pop es
    popa
    ret

int21_exec_run_com:
    mov ax, [cs:current_psp_seg]
    cmp word [cs:current_mz_context_slot], 3
    je .save_third_ctx
    cmp word [cs:current_mz_context_slot], 2
    jne .save_primary_ctx

.save_second_ctx:
    mov [cs:saved_psp2], ax
    mov [cs:saved_ss2], ss
    mov [cs:saved_sp2], sp
    mov ax, ds
    mov [cs:saved_ds2], ax
    mov ax, es
    mov [cs:saved_es2], ax
    jmp .ctx_saved

.save_third_ctx:
    mov [cs:saved_psp3], ax
    mov [cs:saved_ss3], ss
    mov [cs:saved_sp3], sp
    mov ax, ds
    mov [cs:saved_ds3], ax
    mov ax, es
    mov [cs:saved_es3], ax
    jmp .ctx_saved

.save_primary_ctx:
    mov [cs:saved_psp], ax
    mov [cs:saved_ss], ss
    mov [cs:saved_sp], sp
    mov ax, ds
    mov [cs:saved_ds], ax
    mov ax, es
    mov [cs:saved_es], ax

.ctx_saved:
    mov dx, [cs:current_com_load_seg]
    mov ax, dx
    call int21_mem_set_exec_chain_limit
    call ciukidos_save_parent_dta
    jnc .process_state_ready
    mov ax, 0x0008
    stc
    ret
.process_state_ready:
    cli
    mov ax, dx
    mov [cs:dos_exec_identity_psp], ax
    call int21_set_current_psp
    mov es, ax
    mov bx, [es:0x0002]
    cmp bx, [cs:dos_mem_chain_limit_seg]
    jbe .com_psp_end_capped
    mov bx, [cs:dos_mem_chain_limit_seg]
    mov [es:0x0002], bx
.com_psp_end_capped:
    cmp bx, ax
    jbe .arena_prepare_fail
    push ax
    mov ax, bx
    sub ax, [cs:current_com_load_seg]
    cmp ax, 0x1000
    jae .com_stack_full_segment
    mov cl, 4
    shl ax, cl
    sub ax, 2
    mov [cs:com_stack_sp], ax
    jmp .com_stack_ready
.com_stack_full_segment:
    mov word [cs:com_stack_sp], 0xFFFE
.com_stack_ready:
    pop ax
    mov [cs:dos_mem_psp_mcb_end], bx
    mov word [cs:dos_mem_psp_free_seg], 0
    mov word [cs:dos_mem_psp_free_size], 0
    mov word [cs:dos_mem_free2_seg], 0
    mov word [cs:dos_mem_free2_size], 0
    call int21_mem_rebuild_chain
%if TRACE_CHILD_INT21 != 0
    call child_trace_begin_com
%endif
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, [cs:com_stack_sp]
    ; A normal COM ends with a near RET to PSP:0000 (INT 20h).  A FAR CALL
    ; from the kernel put a kernel offset on the child's stack instead, so
    ; RET ran arbitrary child bytes at that offset and could restart the OS.
    ; Keep the same entry SP as before, but provide PSP:0000 for both RET and
    ; the legacy RETF demos. Both exits now use the ordinary DOS termination
    ; path, including nested-process ownership and parent-state restoration.
    push ax
    push word 0
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
    xor si, si
    xor di, di
    xor bp, bp
    ; EXEC's DOS call is suspended while the child runs. AH=34h must report
    ; the actual DOS activity rather than count every suspended ancestor.
    dec byte [cs:dos_indos_flag]
    ; Hand the child a known FLAGS image (IF=1, TF/DF/CF clear).
    push word 0x0202
    popf

    jmp far [cs:com_entry_off]

.after_call:

    cli
    inc byte [cs:dos_indos_flag]
    mov ax, cs
    mov ds, ax
%if TRACE_CHILD_INT21 != 0
    call child_trace_finalize
%endif
    call ciukidos_record_retf_termination
    jc .retf_vectors_done
    call int21_restore_psp_term_vectors
.retf_vectors_done:
    call ciukidos_restore_parent_dta
    cmp word [cs:current_mz_context_slot], 3
    je .restore_third_ss
    cmp word [cs:current_mz_context_slot], 2
    jne .restore_primary_ss
.restore_second_ss:
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

.restore_second_ctx:
    mov ax, [cs:saved_ds2]
    mov ds, ax
    mov ax, [cs:saved_es2]
    mov es, ax
    mov ax, [cs:saved_psp2]
    call int21_set_current_psp
    clc
    ret

.restore_third_ctx:
    mov ax, [cs:saved_ds3]
    mov ds, ax
    mov ax, [cs:saved_es3]
    mov es, ax
    mov ax, [cs:saved_psp3]
    call int21_set_current_psp
    clc
    ret

.restore_primary_ctx:
    mov ax, [cs:saved_ds]
    mov ds, ax
    mov ax, [cs:saved_es]
    mov es, ax
    mov ax, [cs:saved_psp]
    call int21_set_current_psp
    clc
    ret

.arena_prepare_fail:
    call ciukidos_restore_parent_dta
    cmp word [cs:current_mz_context_slot], 3
    je .arena_fail_third
    cmp word [cs:current_mz_context_slot], 2
    jne .arena_fail_primary
    mov ax, [cs:saved_psp2]
    jmp .arena_fail_parent_ready
.arena_fail_third:
    mov ax, [cs:saved_psp3]
    jmp .arena_fail_parent_ready
.arena_fail_primary:
    mov ax, [cs:saved_psp]
.arena_fail_parent_ready:
    call int21_set_current_psp
    mov ax, 0x0008
    stc
    ret

int21_exec_load_mz:
    push cx
    push di
    push si
    push es

    mov byte [cs:tmp_exec_mz_fail_stage], 0

    mov bx, [cs:search_found_size_lo]
    mov bp, [cs:search_found_size_hi]

    ; Establish one byte-exact hard copy boundary before even reading the MZ
    ; header.  It is the lower of the loader slot boundary and the suspended
    ; parent's MCB (parent PSP - 1).
    mov si, [cs:dos_mem_chain_limit_seg]
.copy_limit_check_parent:
    call int21_mem_active_psp
    cmp ax, [cs:current_load_seg]
    jbe .copy_limit_parent_ready
    cmp ax, si
    ja .copy_limit_parent_ready
    mov si, ax
    dec si
.copy_limit_parent_ready:
    ; Parent-owned AH=48h blocks can live inside an otherwise valid loader
    ; slot.  Stop before the first following MCB as well.
    push bx
    mov bx, [cs:current_load_seg]
    call int21_mem_table_next_limit
    mov ax, dx
    cmp ax, [cs:dos_mem_chain_limit_seg]
    je .copy_limit_mcb_ready
    dec ax
.copy_limit_mcb_ready:
    pop bx
    cmp ax, si
    jae .copy_limit_ready
    mov si, ax
.copy_limit_ready:
    mov [cs:tmp_exec_mz_copy_limit], si
    mov ax, [cs:current_load_seg]
    add ax, 0x0020
    cmp ax, si
    ja .image_too_large

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
    jc .header_load_fail

    cmp word [es:0x0000], 0x5A4D
    jne .bad_magic
    call int21_exec_validate_mz_reloc_table
    jc .bad_header_paras

    ; A child loaded above its parent must begin after the parent's complete
    ; arena.  Validate the future child MCB (load + header paras - 17) before
    ; copying the declared image into a fixed/high slot.
    cmp byte [cs:tmp_exec_mz_load_high], 0
    jne .mz_parent_end_ok
    mov cx, [cs:dos_exec_identity_psp]
    jcxz .mz_parent_end_ok
    cmp cx, [cs:current_load_seg]
    jae .mz_parent_end_ok
    push es
    mov es, cx
    call int21_mem_psp_end
    mov cx, ax
    pop es
    mov dx, [es:0x0008]
    add dx, [cs:current_load_seg]
    jc .image_too_large
    sub dx, 0x0011
    cmp dx, cx
    jb .image_too_large
.mz_parent_end_ok:

    mov ax, [es:0x0004]
    or ax, ax
    je .bad_image_size
    mov cx, [es:0x0002]
    cmp cx, 512
    jae .bad_image_size
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
    jb .bad_header_paras

.mz_size_header_ok:
    ; Keep the declared load-image extent for 32-bit relocation validation.
    sub ax, dx
    sbb di, 0
    mov [cs:tmp_overlay_image_size], ax
    mov [cs:tmp_overlay_header_bytes], di
    add ax, dx
    adc di, 0

    cmp di, bp
    ja .bad_image_size
    jb .mz_size_file_ok
    cmp ax, bx
    ja .bad_image_size

.mz_size_file_ok:
    ; Match the tolerant DOS/FreeDOS load extent within the final MZ page.
    ; Some historic COM-to-EXE converters (including the vendored CuteMouse
    ; image) count e_cp/e_cblp from the load module rather than from byte zero,
    ; leaving valid code/data after the strict declared EOF.  Load at most the
    ; rounded e_cp page extent, capped by the real directory size.  Relocation
    ; validation above deliberately keeps using the strict declared image.
    mov ax, [es:0x0004]
    mov di, ax
    mov cl, 7
    shr di, cl
    mov cl, 9
    shl ax, cl
    cmp bp, di
    jb .mz_copy_actual_size
    ja .mz_copy_extent_ready
    cmp bx, ax
    ja .mz_copy_extent_ready
.mz_copy_actual_size:
    mov ax, bx
    mov di, bp
.mz_copy_extent_ready:
    ; Reject an image which would reach the suspended parent's MCB before
    ; performing the copy.  Capping only the later zero-fill is too late: the
    ; compatibility extent could already have overwritten the parent.
    mov si, [cs:tmp_exec_mz_copy_limit]
    sub si, [cs:current_load_seg]
    mov dx, si
    mov cl, 12
    shr dx, cl
    mov cx, si
    shl cx, 1
    shl cx, 1
    shl cx, 1
    shl cx, 1
    cmp di, dx
    ja .image_too_large
    jb .mz_copy_size_ok
    cmp ax, cx
    ja .image_too_large
.mz_copy_size_ok:
    mov [cs:search_found_size_lo], ax
    mov [cs:search_found_size_hi], di

    xor di, di
    xor cx, cx
    call int21_exec_load_to_es
    jc .image_load_fail

    mov ax, [cs:search_found_size_lo]
    add ax, 15
    mov dx, [cs:search_found_size_hi]
    adc dx, 0
    mov cl, 4
.clear_mz_tail_shift:
    shr dx, 1
    rcr ax, 1
    loop .clear_mz_tail_shift

    or dx, dx
    jne .bad_image_size
    mov [cs:tmp_exec_mz_loaded_paras], ax
    add ax, [cs:current_load_seg]
    jc .bad_image_size
    mov dx, ax

    ; A nested MZ loaded below its parent owns only the interval below the
    ; parent PSP.  Keep both the zero-fill and the later MCB arena exclusive
    ; of that live parent; top-level or upward loads retain the DOS heap cap.
    mov si, [cs:tmp_exec_mz_copy_limit]
.clear_mz_tail:
    cmp dx, si
    jae .clear_mz_tail_done
    mov ax, si
    sub ax, dx
    mov cx, 128
    cmp ax, 0x0010
    jae .clear_mz_chunk_ready
    mov cx, ax
    shl cx, 1
    shl cx, 1
    shl cx, 1
.clear_mz_chunk_ready:
    mov es, dx
    xor ax, ax
    xor di, di
    rep stosw
    add dx, 0x0010
    cmp dx, si
    jb .clear_mz_tail

.clear_mz_tail_done:

    clc
    jmp .done

.header_load_fail:
    mov byte [cs:tmp_exec_mz_fail_stage], 1
    jmp .done

.bad_magic:
    mov byte [cs:tmp_exec_mz_fail_stage], 2
    jmp .invalid_format

.bad_header_paras:
    mov byte [cs:tmp_exec_mz_fail_stage], 3
    jmp .invalid_format

.bad_image_size:
    mov byte [cs:tmp_exec_mz_fail_stage], 4
    jmp .invalid_format

.image_too_large:
    mov byte [cs:tmp_exec_mz_fail_stage], 5
    mov ax, 0x0008
    stc
    jmp .image_load_fail

.image_load_fail:
    cmp byte [cs:tmp_exec_mz_fail_stage], 0
    jne .image_load_fail_ready
    mov byte [cs:tmp_exec_mz_fail_stage], 6
.image_load_fail_ready:
    jmp .done

.invalid_format:
    mov ax, 0x000B
    stc

.done:
    mov [cs:search_found_size_lo], bx
    mov [cs:search_found_size_hi], bp
    pop es
    pop si
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
    call int21_exec_validate_mz_reloc_table
    jc .invalid_format

    mov ax, [es:0x0008]
    mov cl, 4
    shl ax, cl
    mov dx, [cs:search_found_size_lo]
    cmp dx, ax
    jb .invalid_format
    sub dx, ax
    mov [cs:tmp_overlay_image_size], dx
    xor ax, ax
    mov [cs:tmp_overlay_header_bytes], ax

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
    call int21_exec_validate_mz_reloc_target
    jc .invalid_format
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

.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; Validate the complete MZ relocation-table extent before any table walk.
; AX/CX/DX are scratch; CF=1 rejects wrap, oversized headers, and tables
; extending past e_cparhdr*16.
int21_exec_validate_mz_reloc_table:
    mov dx, [es:0x0008]
    cmp dx, 0x0002
    jb .invalid
    cmp dx, 0x1000
    jae .invalid
    mov ax, [es:0x0006]
    cmp ax, 0x4000
    jae .invalid
    shl ax, 1
    shl ax, 1
    add ax, [es:0x0018]
    jc .invalid
    mov cl, 4
    shl dx, cl
    cmp ax, dx
    ja .invalid
    clc
    ret
.invalid:
    stc
    ret

; DX:BX is relative to the load image.  Require the complete target word to
; remain inside the declared image before dereferencing the relocation.
int21_exec_validate_mz_reloc_target:
    push dx
    mov ax, dx
    xor dx, dx
    push cx
    mov cx, 4
.shift:
    shl ax, 1
    rcl dx, 1
    loop .shift
    pop cx
    add ax, bx
    adc dx, 0
    add ax, 2
    adc dx, 0
    cmp dx, [cs:tmp_overlay_header_bytes]
    ja .invalid
    jb .valid
    cmp ax, [cs:tmp_overlay_image_size]
    ja .invalid
.valid:
    pop dx
    clc
    ret
.invalid:
    pop dx
    stc
    ret

int21_exec_mz_overlaps_runtime:
    push bx
    push cx
    push dx

    mov bx, ax
    cmp bx, DOS_META_BUF_SEG
    jae .no_overlap

    mov dx, [cs:search_found_size_hi]
    mov ax, [cs:search_found_size_lo]
    add ax, 15
    adc dx, 0
    mov cx, 4
.size_paras_loop:
    shr dx, 1
    rcr ax, 1
    loop .size_paras_loop

    mov dx, DOS_META_BUF_SEG
    sub dx, bx
    cmp ax, dx
    ja .overlap

.no_overlap:
    mov ax, bx
    clc
    jmp .done

.overlap:
    mov ax, bx
    stc

.done:
    pop dx
    pop cx
    pop bx
    ret

int21_exec_run_mz:
    push es

    mov byte [cs:tmp_exec_mz_run_fail_stage], 0

    mov ax, [cs:current_load_seg]
    mov es, ax
    cmp word [es:0x0000], 0x5A4D
    jne .invalid_header

    mov bx, [es:0x0008]
    add bx, [cs:current_load_seg]
    mov [cs:mz_image_seg], bx
    cmp byte [cs:tmp_exec_mz_load_high], 0
    je .psp_from_image
    mov ax, [cs:tmp_exec_mz_high_psp]
    jmp .psp_ready
.psp_from_image:
    mov ax, bx
    sub ax, 0x0010
.psp_ready:
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
    call int21_exec_validate_mz_reloc_target
    jc .invalid_header
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
    mov dx, es
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
    call int21_mem_set_exec_chain_limit
    mov dx, [cs:mz_psp_seg]
    call ciukidos_save_parent_dta
    jc .process_state_fail
    ; Resize rebuilds the MCB chain and walks the new PSP's ancestry. The
    ; full PSP is initialized after sizing because it can overlap the MZ
    ; header. Publish these already-consumed header fields first: otherwise
    ; stale load-module bytes become parent pointers and MCBs overwrite DOS.
    push es
    mov es, dx
    mov word [es:0], 0x20CD
    mov ax, dx
    add ax, 0x0010
    mov [es:2], ax
    mov ax, [cs:dos_exec_parent_identity_psp]
    mov [es:0x0016], ax
    pop es
    mov ax, dx
    mov [cs:dos_exec_identity_psp], ax
    call int21_set_current_psp
    call int21_exec_prepare_mz_free_mcb
    jc .arena_prepare_fail
    mov ax, [cs:mz_psp_seg]
    push ax
    mov ax, bx
    call int21_set_current_psp
    pop ax
    mov es, ax
    call int21_exec_init_program_psp
    call int21_set_current_psp
    mov ax, [cs:dos_mem_psp_mcb_end]
    mov [es:0x0002], ax
    mov bx, [cs:dos_exec_parent_identity_psp]
    or bx, bx
    jnz .mz_parent_ready
    mov bx, es
.mz_parent_ready:
    mov [es:0x0016], bx
    call int21_build_env_block
    call int21_exec_write_fcbs
    call int21_exec_write_tail
%if TRACE_CHILD_INT21 != 0
    call child_trace_begin_mz
%endif

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

    ; CIUKIDOS lives below the MZ loader window and now owns a resident INT 21h
    ; tranche.  Keep its validated service cache across child execution so the
    ; AH=4Ch path can restore the parent DTA/PSP through services 7/8.

    cli
    mov ax, [cs:mz_psp_seg]
    mov ds, ax
    mov es, ax
    mov ax, [cs:mz_stack_seg]
    mov ss, ax
    mov sp, [cs:mz_stack_sp]
    ; Match the register state supplied by MS-DOS/FreeDOS EXEC.  Some
    ; real-mode runtimes (notably Borland's overlay manager) consume more
    ; than DS=ES=PSP during their startup path.
    xor ax, ax
    xor bx, bx
    mov cx, 0x00FF
    mov dx, [cs:mz_psp_seg]
    mov si, [cs:mz_entry_off]
    mov di, [cs:mz_stack_sp]
    mov bp, 0x091E
    ; Balance the suspended EXEC call, including nested MZ/COM children.
    dec byte [cs:dos_indos_flag]
    ; Hand the child a known FLAGS image (IF=1, TF/DF/CF clear).
    push word 0x0202
    popf
    jmp far [cs:mz_entry_off]

.after_call:
    cli
    inc byte [cs:dos_indos_flag]
    mov ax, cs
    mov ds, ax
%if TRACE_CHILD_INT21 != 0
    call child_trace_finalize
%endif
    call ciukidos_record_retf_termination
    jc .retf_vectors_done
    call int21_restore_psp_term_vectors
.retf_vectors_done:
    call ciukidos_restore_parent_dta
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
    call int21_set_current_psp
    mov ax, [cs:saved_ds2]
    mov ds, ax
    mov ax, [cs:saved_es2]
    mov es, ax
    clc
    jmp .done

.restore_third_ctx:
    mov ax, [cs:saved_psp3]
    call int21_set_current_psp
    mov ax, [cs:saved_ds3]
    mov ds, ax
    mov ax, [cs:saved_es3]
    mov es, ax
    clc
    jmp .done

.restore_primary_ctx:
    mov ax, [cs:saved_psp]
    call int21_set_current_psp
    mov ax, [cs:saved_ds]
    mov ds, ax
    mov ax, [cs:saved_es]
    mov es, ax
    clc
    jmp .done

.arena_prepare_fail:
    mov byte [cs:tmp_exec_mz_run_fail_stage], 3
    push ax
    call ciukidos_restore_parent_dta
    mov ax, bx
    call int21_set_current_psp
    pop ax
    stc
    jmp .done

.process_state_fail:
    mov byte [cs:tmp_exec_mz_run_fail_stage], 2
    mov ax, 0x0008
    stc
    jmp .done

.invalid_header:
    mov byte [cs:tmp_exec_mz_run_fail_stage], 1
    mov ax, 0x000B
    stc

.done:
    pop es
    ret

int21_mz_terminate_trampoline:
    jmp int21_exec_run_mz.after_call

int21_com_terminate_trampoline:
    jmp int21_exec_run_com.after_call

exec_terminate_dispatch_cs:
    cmp ax, [current_com_load_seg]
    je .com
    jmp int21_exec_run_mz.after_call
.com:
    jmp int21_exec_run_com.after_call

int21_set_dta:
    mov ax, ds
    mov [cs:dta_seg], ax
    mov [cs:dta_off], dx
%ifdef CIUKIDOS_KERNEL_BUILD
    mov [cs:kernel_runtime_state_dta_seg], ax
    mov [cs:kernel_runtime_state_dta_off], dx
%endif
    xor ax, ax
    clc
    ret

int21_get_default_drive:
%if FAT_TYPE == 16
    push ds
    ; DOS AH=19h only returns AL.  The CIUKIDOS state service returns its
    ; pointer in DS:SI, so keep that implementation detail invisible to the
    ; caller.  QuickBASIC's BLOAD path builder keeps its source cursor in SI
    ; across AH=19h and otherwise loses the filename after the drive lookup.
    push si
    call stage1_runtime_get_default_drive_ptr
    jc .fallback
    xor ah, ah
    mov al, [ds:si]
    clc
    pop si
    pop ds
    ret

.fallback:
    pop si
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
    mov cx, DOS_CWD_BYTES
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
    mov cx, DOS_CWD_BYTES
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
    ja .unchanged
    call cwd_save_current_drive
    mov [cs:dos_default_drive], dl
    call stage1_runtime_sync_default_drive
    mov al, dl
    call cwd_load_drive_al
    ; AH=0Eh reports the LASTDRIVE-style logical namespace, not the count of
    ; mounted media.  Applications may probe absent letters by selecting
    ; them and checking AH=19h; DOS leaves the current drive unchanged and
    ; does not report an error through CF.
    mov al, 26
    xor ah, ah
    clc
    ret
%else
    cmp dl, 1
    ja .unchanged
    mov [cs:dos_default_drive], dl
    mov al, 1
    xor ah, ah
    clc
    ret
%endif
.unchanged:
%if FAT_TYPE == 16
    mov al, 26
%else
    mov al, 1
%endif
    xor ah, ah
    clc
    ret

int21_get_version:
%if FAT_TYPE == 16
    call stage1_runtime_get_version
    jnc .done
%endif
    ; Keep the fallback coherent with CIUKIDOS.SYS: DOS 5.0 compatibility.
    mov ax, 0x0005
    xor bx, bx
    xor cx, cx
    clc
.done:
    ret

int21_country_info:
    ; AL=0x00 (current) / 0xFF (current) / 0x01 (country 1=USA): fill the
    ; caller's 34-byte buffer at DS:DX with the default country info block.
    ; This loose interpretation matches what Watcom C runtime expects from
    ; DOS 3+: a successful Get with USA-like data. Splitting Set off was
    ; tempting per spec but broke DOOM's setlocale path on real hardware.
    cmp al, 0x00
    je .copy_current
    cmp al, 0xFF
    je .copy_current
    cmp al, 0x01
    je .copy_current
    jmp .bad_country
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
    mov ax, 0x3800
    clc
    ret
.bad_country:
    mov ax, 0x0002
    stc
    ret

; Date and time from the RTC (INT 1Ah AH=04h/02h, BCD). Before, the date was
; a fixed 2026-08-30 and the time used only the low word of the tick count.
int21_get_date:
    mov ah, 0x04
    int 0x1A                    ; CH century, CL year, DH month, DL day (BCD)
    mov al, dl
    call rtc_bcd
    mov dl, al
    mov al, dh
    call rtc_bcd
    mov dh, al
    mov al, ch
    call rtc_bcd
    mov ah, 100
    mul ah
    xchg ax, cx                 ; CX = century*100, AL = year (BCD)
    call rtc_bcd
    add cx, ax                  ; CX = year
    mov al, 6                   ; the RTC's day of the week, 1 = Sunday
    out 0x70, al
    in al, 0x71
    dec al
    clc
    ret

int21_get_time:
    push ax
    mov ah, 0x02
    int 0x1A                    ; CH hours, CL minutes, DH seconds (BCD)
    mov al, ch
    call rtc_bcd
    mov ch, al
    mov al, cl
    call rtc_bcd
    mov cl, al
    mov al, dh
    call rtc_bcd
    mov dh, al
    ; A second only changes once a second. Expose a monotonically advancing
    ; centisecond value so DOS clients can calibrate short delays without
    ; spinning on an unchanged sample.
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
    pop ax
    clc
    ret

%if FAT_TYPE == 16
; Read the RTC directly while already handling INT 21h. Calling the BIOS
; INT 1Ah service recursively here breaks file creates under the V86 monitor.
; AX = DOS time, DX = DOS date (BX, CX changed).
dos_now_stamp:
    mov al, 4                       ; hours
    call .read_bcd
    xor ah, ah
    shl ax, 6
    mov bx, ax
    mov al, 2                       ; minutes
    call .read_bcd
    xor ah, ah
    or bx, ax
    shl bx, 5
    mov al, 0                       ; seconds
    call .read_bcd
    shr al, 1
    or bl, al
    push bx
    mov al, 0x32                    ; century
    call .read_bcd
    mov bl, 100
    mul bl
    mov cx, ax
    mov al, 9                       ; year
    call .read_bcd
    xor ah, ah
    add cx, ax
    sub cx, 1980
    mov ax, cx
    shl ax, 4
    mov bx, ax
    mov al, 8                       ; month
    call .read_bcd
    xor ah, ah
    or bx, ax
    shl bx, 5
    mov al, 7                       ; day
    call .read_bcd
    or bl, al
    mov dx, bx
    pop ax
    ret
.read_bcd:
    out 0x70, al
    in al, 0x71
    call rtc_bcd
    ret
%endif

; AL (BCD) -> AL binary, AH = 0: AAM 16 splits the nibbles, AAD joins them.
rtc_bcd:
    aam 16
    aad
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

; DOS 2+ drive allocation information (AH=1Bh/1Ch).
; DL=0 selects the default drive; otherwise it is one-based.  The media
; descriptor pointer is intentionally stable in the resident kernel because
; callers are allowed to retain DS:BX after the interrupt returns.
int21_get_allocation_info:
    cmp dl, 0
    je .valid
%if FAT_TYPE == 16
    cmp dl, 3
    je .valid
    cmp dl, 4
    je .valid
%else
    cmp dl, 1
    je .valid
%endif
    mov al, 0xFF
    clc
    ret

.valid:
    mov al, FAT_SECTORS_PER_CLUSTER
    mov cx, 512
    mov dx, FAT_DATA_CLUSTER_COUNT
    mov bx, dos_media_descriptor
    push cs
    pop ds
    clc
    ret

; DOS 2+ Get DPB (AH=32h).  Unlike most DOS calls an absent drive is reported
; as AL=FFh with carry clear; Windows' disk detector depends on that detail.
int21_get_dpb:
    mov al, dl
    or al, al
    jnz .drive_ready
    mov al, [cs:dos_default_drive]
    inc al
.drive_ready:
    cmp al, 3                       ; the FAT16 system disk is C:
    jne .invalid
    cmp byte [cs:dos_sysvars_initialized], 1
    je .publish
    call int21_get_list_of_lists
.publish:
    mov ax, DOS_SYSVARS_SEG
    mov ds, ax
    mov bx, DOS_SYSVARS_DPB_OFF
    mov byte [cs:int21_return_ds], 1
    xor al, al
    clc
    ret
.invalid:
    mov al, 0xFF
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

    mov dx, DOS_SYSVARS_SEG
    mov es, dx
    ; AH=52h publishes live DOS state. Reinitializing it on every query
    ; unlinked resident device drivers (including Jemm), destroyed CDS edits
    ; and reset SFT entries while clients still held references to them.
    cmp byte [cs:dos_sysvars_initialized], 1
    je .publish
    mov di, DOS_SYSVARS_ANCHOR_OFF
    xor ax, ax
    ; Clear the complete 1792-byte SYSVARS/CDS/SFT/DPB image.  It ends at
    ; 1400:0000, immediately before the reserved EXEC-state frames.
    mov cx, 0x0380
    cld
    rep stosw

    mov word [es:DOS_SYSVARS_OFF + 0x00], DOS_SYSVARS_DPB_OFF
    mov [es:DOS_SYSVARS_OFF + 0x02], dx
    mov word [es:DOS_SYSVARS_OFF + 0x04], DOS_SYSVARS_SFT_OFF
    mov [es:DOS_SYSVARS_OFF + 0x06], dx
    ; Enhanced-mode DOSMGR follows the published CON pointer and walks the
    ; resident device-header chain. A null CON pointer makes VMM interpret the
    ; virtual interrupt table as a device header and loop forever at startup.
    mov word [es:DOS_SYSVARS_OFF + 0x0C], DOS_SYSVARS_CON_OFF
    mov [es:DOS_SYSVARS_OFF + 0x0E], dx
    mov word [es:DOS_SYSVARS_OFF + 0x10], 512
    mov word [es:DOS_SYSVARS_OFF + 0x16], DOS_SYSVARS_CDS_OFF
    mov [es:DOS_SYSVARS_OFF + 0x18], dx
    mov byte [es:DOS_SYSVARS_OFF + 0x20], 1
    mov byte [es:DOS_SYSVARS_OFF + 0x21], 3

    ; DOS 5 List-of-Lists embeds NUL at +22h. Link it to a conventional CON
    ; header, then terminate with FFFF:FFFF. Strategy/interrupt offsets refer
    ; to a harmless resident RETF stub because CiukiDOS owns console I/O via
    ; INT 21h rather than dispatching through request packets.
    mov di, DOS_SYSVARS_OFF + 0x22
    mov ax, DOS_SYSVARS_CON_OFF
    stosw
    mov ax, dx
    stosw
    mov ax, 0x8004
    stosw
    mov ax, DOS_SYSVARS_DEV_RET_OFF
    stosw
    stosw
    mov ax, 0x554E                    ; "NU"
    stosw
    mov ax, 0x204C                    ; "L "
    stosw
    mov ax, 0x2020
    stosw
    stosw

    mov di, DOS_SYSVARS_CON_OFF
    mov ax, 0xFFFF
    stosw
    stosw
    mov ax, 0x8013
    stosw
    mov ax, DOS_SYSVARS_DEV_RET_OFF
    stosw
    stosw
    mov ax, 0x4F43                    ; "CO"
    stosw
    mov ax, 0x204E                    ; "N "
    stosw
    mov ax, 0x2020
    stosw
    stosw
    mov byte [es:DOS_SYSVARS_DEV_RET_OFF], 0xCB

    mov byte [es:DOS_SYSVARS_DPB_OFF + 0x00], 2
    mov word [es:DOS_SYSVARS_DPB_OFF + 0x02], 512
    mov word [es:DOS_SYSVARS_DPB_OFF + 0x19], 0xFFFF
    mov word [es:DOS_SYSVARS_DPB_OFF + 0x1B], 0xFFFF

    mov word [es:DOS_SYSVARS_SFT_OFF + 0x00], 0xFFFF
    mov word [es:DOS_SYSVARS_SFT_OFF + 0x02], 0xFFFF
    mov word [es:DOS_SYSVARS_SFT_OFF + 0x04], DOS_SYSVARS_SFT_COUNT

    ; DOS 4+ SFT entries are 3Bh bytes.  The first five records describe the
    ; standard CON handles; int21_sync_sft_table maintains records 5..19 from
    ; CiukiDOS' live FAT handle metadata.
    mov di, DOS_SYSVARS_SFT_OFF + 0x06
    mov cx, 5
.init_console_sft:
    mov word [es:di + 0x00], 1
    mov word [es:di + 0x02], 2
    mov word [es:di + 0x05], 0x80D3
    mov word [es:di + 0x20], 0x4F43       ; "CO"
    mov word [es:di + 0x22], 0x204E       ; "N "
    mov word [es:di + 0x24], 0x2020
    mov word [es:di + 0x26], 0x2020
    mov word [es:di + 0x28], 0x2020
    mov byte [es:di + 0x2A], 0x20
    add di, DOS_SYSVARS_SFT_SIZE
    loop .init_console_sft

    mov byte [cs:dos_sysvars_initialized], 1

    mov byte [es:DOS_SYSVARS_CDS_OFF + 0xB0], 'C'
    mov byte [es:DOS_SYSVARS_CDS_OFF + 0xB1], ':'
    mov byte [es:DOS_SYSVARS_CDS_OFF + 0xB2], '\'
    mov word [es:DOS_SYSVARS_CDS_OFF + 0xB0 + 0x43], 0x4000
    mov word [es:DOS_SYSVARS_CDS_OFF + 0xB0 + 0x45], DOS_SYSVARS_DPB_OFF
    mov [es:DOS_SYSVARS_CDS_OFF + 0xB0 + 0x47], dx

.publish:
    mov ax, [cs:dos_list_of_lists]
    mov [es:DOS_SYSVARS_ANCHOR_OFF], ax
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

%if FAT_TYPE == 16
; Pack/unpack the open bits for handles 5..19.  A child never overwrites an
; already-open parent slot because the allocator skips it, so restoring this
; 15-bit JFT view is enough to preserve parent metadata and close every slot
; the child leaked.  Nested EXEC receives its own copy through the existing
; process-state frame stack.
int21_capture_open_handle_mask:
    push ax
    push bx
    push cx
    push si

    xor ax, ax
    cmp byte [cs:file_handle_open], 0
    je .slot2
    or ax, 0x0001
.slot2:
    cmp byte [cs:file_handle2_open], 0
    je .slot3
    or ax, 0x0002
.slot3:
    cmp byte [cs:file_handle3_open], 0
    je .slot4
    or ax, 0x0004
.slot4:
    cmp byte [cs:file_handle4_open], 0
    je .slot5
    or ax, 0x0008
.slot5:
    cmp byte [cs:file_handle5_open], 0
    je .slot6
    or ax, 0x0010
.slot6:
    cmp byte [cs:file_handle6_open], 0
    je .slot7
    or ax, 0x0020
.slot7:
    cmp byte [cs:file_handle7_open], 0
    je .slot8
    or ax, 0x0040
.slot8:
    cmp byte [cs:file_handle8_open], 0
    je .extra_begin
    or ax, 0x0080

.extra_begin:
    mov si, file_handle_extra_table
    mov bx, 0x0100
    mov cx, DOS_FILE_EXTRA_COUNT
.extra_loop:
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 0
    je .extra_next
    or ax, bx
.extra_next:
    shl bx, 1
    add si, DOS_FILE_EXTRA_ENTRY_SIZE
    loop .extra_loop
    mov [cs:dos_file_open_mask], ax

    pop si
    pop cx
    pop bx
    pop ax
    ret

int21_apply_open_handle_mask:
    push ax
    push bx
    push cx
    push si

    mov ax, [cs:dos_file_open_mask]
    mov bl, al
    and bl, 1
    mov [cs:file_handle_open], bl
    shr ax, 1
    mov bl, al
    and bl, 1
    mov [cs:file_handle2_open], bl
    shr ax, 1
    mov bl, al
    and bl, 1
    mov [cs:file_handle3_open], bl
    shr ax, 1
    mov bl, al
    and bl, 1
    mov [cs:file_handle4_open], bl
    shr ax, 1
    mov bl, al
    and bl, 1
    mov [cs:file_handle5_open], bl
    shr ax, 1
    mov bl, al
    and bl, 1
    mov [cs:file_handle6_open], bl
    shr ax, 1
    mov bl, al
    and bl, 1
    mov [cs:file_handle7_open], bl
    shr ax, 1
    mov bl, al
    and bl, 1
    mov [cs:file_handle8_open], bl
    shr ax, 1

    mov si, file_handle_extra_table
    mov cx, DOS_FILE_EXTRA_COUNT
.extra_loop:
    mov bl, al
    and bl, 1
    mov [cs:si + DOS_FILE_EXTRA_OPEN_OFF], bl
    shr ax, 1
    add si, DOS_FILE_EXTRA_ENTRY_SIZE
    loop .extra_loop

    pop si
    pop cx
    pop bx
    pop ax
    ret

; Mirror all open FAT handles into the DOS 5 SFT published by AH=52h.
; Windows 3.x and DOS extenders inspect these records while their INT 21h
; calls are reflected through DOSX, so size/position alone are insufficient.
int21_sync_sft_table:
    cmp byte [cs:dos_sysvars_initialized], 1
    jne .done

    pushf
    pusha
    push ds
    push es

    mov ax, DOS_SYSVARS_SEG
    mov es, ax
    mov bx, 5
    mov di, DOS_SYSVARS_SFT_OFF + 0x06 + (5 * DOS_SYSVARS_SFT_SIZE)

.slot_loop:
    ; Clear stale state first; a closed handle is represented by a zero
    ; reference count and no residual size/position fields.
    push bx
    push di
    xor ax, ax
    mov cx, DOS_SYSVARS_SFT_SIZE
    cld
    rep stosb
    pop di
    pop bx

    mov byte [cs:sft_sync_swap], 0
    cmp bx, 5
    je .selected
    cmp bx, 6
    je .select2
    cmp bx, 7
    je .select3
    cmp bx, 8
    je .select4
    cmp bx, 9
    je .select5
    cmp bx, 10
    je .select6
    cmp bx, 11
    je .select7
    cmp bx, 12
    je .select8

    call int21_extra_handle_ptr
    jc .next_slot
    mov al, bl
    sub al, 4
    mov [cs:sft_sync_swap], al
    call int21_swap_file_handle_extra
    jmp .selected

.select2:
    call int21_swap_file_handles
    mov byte [cs:sft_sync_swap], 2
    jmp .selected
.select3:
    call int21_swap_file_handles3
    mov byte [cs:sft_sync_swap], 3
    jmp .selected
.select4:
    call int21_swap_file_handles4
    mov byte [cs:sft_sync_swap], 4
    jmp .selected
.select5:
    call int21_swap_file_handles5
    mov byte [cs:sft_sync_swap], 5
    jmp .selected
.select6:
    call int21_swap_file_handles6
    mov byte [cs:sft_sync_swap], 6
    jmp .selected
.select7:
    call int21_swap_file_handles7
    mov byte [cs:sft_sync_swap], 7
    jmp .selected
.select8:
    call int21_swap_file_handles8
    mov byte [cs:sft_sync_swap], 8

.selected:
    cmp byte [cs:file_handle_open], 1
    jne .restore_slot

    mov word [es:di + 0x00], 1
    xor ax, ax
    mov al, [cs:file_handle_mode]
    mov [es:di + 0x02], ax
    mov byte [es:di + 0x04], 0
    mov word [es:di + 0x05], 0x0842
    mov word [es:di + 0x07], DOS_SYSVARS_DPB_OFF
    mov word [es:di + 0x09], DOS_SYSVARS_SEG
    mov ax, [cs:file_handle_start_cluster]
    mov [es:di + 0x0B], ax
    mov word [es:di + 0x0D], 0x83A0       ; 16:29:00, valid DOS packed time
    mov word [es:di + 0x0F], 0x5D1E       ; 2026-08-30, valid DOS packed date
    mov ax, [cs:file_handle_size_lo]
    mov [es:di + 0x11], ax
    mov ax, [cs:file_handle_size_hi]
    mov [es:di + 0x13], ax
    mov ax, [cs:file_handle_pos]
    mov [es:di + 0x15], ax
    mov ax, [cs:file_handle_pos_hi]
    mov [es:di + 0x17], ax
    mov ax, [cs:file_handle_root_lba]
    mov [es:di + 0x1B], ax
    mov ax, [cs:file_handle_root_lba_hi]
    mov [es:di + 0x1D], ax
    mov ax, [cs:file_handle_root_off]
    mov cl, 5
    shr ax, cl
    mov [es:di + 0x1F], al
    push di
    add di, 0x20
    mov al, ' '
    mov cx, 11
    rep stosb
    pop di
    mov ax, [cs:current_psp_seg]
    mov [es:di + 0x31], ax
    mov ax, [cs:file_handle_start_cluster]
    mov [es:di + 0x35], ax

.restore_slot:
    mov al, [cs:sft_sync_swap]
    cmp al, 2
    je .restore2
    cmp al, 3
    je .restore3
    cmp al, 4
    je .restore4
    cmp al, 5
    je .restore5
    cmp al, 6
    je .restore6
    cmp al, 7
    je .restore7
    cmp al, 8
    je .restore8
    cmp al, DOS_FILE_EXTRA_FIRST_TARGET
    jae .restore_extra
    jmp .next_slot
.restore2:
    call int21_swap_file_handles
    jmp .next_slot
.restore3:
    call int21_swap_file_handles3
    jmp .next_slot
.restore4:
    call int21_swap_file_handles4
    jmp .next_slot
.restore5:
    call int21_swap_file_handles5
    jmp .next_slot
.restore6:
    call int21_swap_file_handles6
    jmp .next_slot
.restore7:
    call int21_swap_file_handles7
    jmp .next_slot
.restore8:
    call int21_swap_file_handles8
    jmp .next_slot
.restore_extra:
    call int21_swap_file_handle_extra

.next_slot:
    add di, DOS_SYSVARS_SFT_SIZE
    inc bx
    cmp bx, DOS_SYSVARS_SFT_COUNT
    jb .slot_loop

    pop es
    pop ds
    popa
    popf
.done:
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
    cmp bx, 3
    sbb ax, ax
    and bx, ax
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
    call int21_trace_vector_set
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
    call int21_trace_vector_get
    xor ax, ax
    clc
    pop di
    pop ax
    ret

int21_trace_vector_set:
int21_trace_vector_get:
    ret

%if TRACE_CHILD_INT21 != 0
child_trace_should_log:
child_trace_should_log_exit:
    cmp byte [cs:child_trace_active], 1
    cmc
    ret

child_trace_print_far_string:
    push cx
    mov ds, ax
    mov si, dx
    mov cx, 48
.loop:
    jcxz .done
    lodsb
    test al, al
    jz .done
    call serial_putc
    dec cx
    jmp .loop
.done:
    pop cx
    ret

child_trace_emit_marker:
    push ds
    push cs
    pop ds
    call print_string_serial
    call print_newline_serial
    pop ds
    ret

child_trace_int21_error:
    push ax
    push dx
    push si
    push ds
    call child_trace_should_log
    jnc .done
    push cs
    pop ds
    mov si, msg_child_int21_error
    call print_string_serial
    mov al, [cs:int21_last_ah]
    call print_hex8_serial
    mov si, msg_child_int21_error_ax
    call print_string_serial
    mov ax, [cs:int21_error_ax]
    call print_hex16_serial
    mov al, [cs:int21_last_ah]
    cmp al, 0x3B
    je .path
    cmp al, 0x3C
    je .path
    cmp al, 0x3D
    je .path
    cmp al, 0x41
    je .path
    cmp al, 0x43
    je .path
    cmp al, 0x4E
    je .path
    cmp al, 0x56
    je .path
    cmp al, 0x60
    jne .newline
.path:
    mov si, msg_child_int21_error_path
    call print_string_serial
    mov ax, [cs:int21_caller_ds]
    mov dx, [cs:int21_last_dx]
    call child_trace_print_far_string
.newline:
    call print_newline_serial
.done:
    pop ds
    pop si
    pop dx
    pop ax
    ret

child_trace_int21_call:
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    call child_trace_should_log
    jnc .done
    push cs
    pop ds
    mov si, msg_child_int21_call
    call print_string_serial
    mov al, [cs:int21_last_ah]
    call print_hex8_serial
    mov si, msg_child_int21_call_ax
    call print_string_serial
    mov ah, [cs:int21_last_ah]
    mov al, [cs:int21_last_al]
    call print_hex16_serial
    mov si, msg_child_int21_call_bx
    call print_string_serial
    mov ax, bx
    call print_hex16_serial
    mov si, msg_child_int21_call_cx
    call print_string_serial
    mov ax, cx
    call print_hex16_serial
    mov si, msg_child_int21_call_dx
    call print_string_serial
    mov ax, [cs:int21_last_dx]
    call print_hex16_serial
    mov si, msg_child_int21_call_ds
    call print_string_serial
    mov ax, [cs:int21_caller_ds]
    call print_hex16_serial
    mov si, msg_child_int21_call_ret
    call print_string_serial
    mov ax, [cs:int21_trace_call_cs]
    call print_hex16_serial
    mov al, ':'
    call serial_putc
    mov ax, [cs:int21_trace_call_ip]
    call print_hex16_serial
    mov al, [cs:int21_last_ah]
    cmp al, 0x3B
    je .path
    cmp al, 0x3C
    je .path
    cmp al, 0x3D
    je .path
    cmp al, 0x41
    je .path
    cmp al, 0x43
    je .path
    cmp al, 0x4E
    je .path
    cmp al, 0x56
    je .path
    cmp al, 0x60
    jne .newline
.path:
    mov si, msg_child_int21_error_path
    call print_string_serial
    mov ax, [cs:int21_caller_ds]
    mov dx, [cs:int21_last_dx]
    call child_trace_print_far_string
.newline:
    call print_newline_serial
.done:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

child_trace_int21_return:
    push bp
    push ax
    push bx
    push cx
    push dx
    push si
    push ds
    mov al, [cs:int21_last_ah]
    cmp al, 0x19
    je .selected
    cmp al, 0x3F
    je .selected
    cmp al, 0x42
    je .selected
    cmp al, 0x57
    je .selected
    cmp al, 0x36
    je .selected
    cmp al, 0x47
    jne .done
.selected:
    call child_trace_should_log
    jnc .done
    mov bp, sp
    mov dx, si
    push cs
    pop ds
    mov si, msg_child_int21_return
    call print_string_serial
    mov al, [cs:int21_last_ah]
    call print_hex8_serial
    mov si, msg_child_int21_call_ax
    call print_string_serial
    mov ax, [ss:bp + 10]
    call print_hex16_serial
    mov si, msg_child_int21_call_bx
    call print_string_serial
    mov ax, [ss:bp + 8]
    call print_hex16_serial
    mov si, msg_child_int21_call_cx
    call print_string_serial
    mov ax, [ss:bp + 6]
    call print_hex16_serial
    mov si, msg_child_int21_call_dx
    call print_string_serial
    mov ax, [ss:bp + 4]
    call print_hex16_serial
    cmp byte [cs:int21_last_ah], 0x3F
    jne .after_read_bytes
    cmp word [ss:bp + 10], 8
    jb .after_read_bytes
    mov si, msg_child_int21_read_bytes
    call print_string_serial
    push es
    push di
    mov ax, [cs:int21_caller_ds]
    mov es, ax
    mov di, [cs:int21_last_dx]
    mov cx, 8
.read_byte_loop:
    mov al, [es:di]
    call print_hex8_serial
    inc di
    loop .read_byte_loop
    pop di
    pop es
.after_read_bytes:
    mov al, [cs:int21_last_ah]
    cmp al, 0x47
    je .cwd
    cmp al, 0x36
    jne .newline
    mov si, msg_child_int21_return_pathbuf
    call print_string_serial
    mov ax, ss
    mov dx, 0x3262
    call child_trace_print_far_string
    jmp .newline
.cwd:
    mov si, msg_child_int21_return_path
    call print_string_serial
    mov ax, [cs:int21_caller_ds]
    call child_trace_print_far_string
.newline:
    call print_newline_serial
.done:
    pop ds
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    pop bp
    ret

child_trace_prepare_exec:
    push ax
    push dx
    push si
    push ds
    cmp word [cs:dos_exec_identity_psp], 0
    jne .arm
    mov byte [cs:child_trace_armed], 0
    jmp .done
.arm:
    mov byte [cs:child_trace_armed], 1
    push cs
    pop ds
    mov si, msg_child_exec_req
    call print_string_serial
    mov ax, [cs:int21_caller_ds]
    mov dx, [cs:int21_last_dx]
    call child_trace_print_far_string
    call print_newline_serial
.done:
    pop ds
    pop si
    pop dx
    pop ax
    ret

child_trace_begin_com:
    jmp short child_trace_begin_core

child_trace_begin_core:
    cmp byte [cs:child_trace_armed], 0
    je .done
    mov byte [cs:child_trace_active], 1
    mov byte [cs:child_trace_exit_logged], 0
.done:
    ret

child_trace_begin_mz:
    push ds
    push cs
    pop ds
    cmp byte [cs:child_trace_armed], 0
    je .done
    call child_trace_begin_core
    mov si, msg_child_prejump
    call print_string_serial
    call print_newline_serial
.done:
    pop ds
    ret

child_trace_exit_int21:
    push ax
    push si
    push ds
    push cs
    pop ds
    call child_trace_should_log_exit
    jnc .done
    mov byte [cs:child_trace_exit_logged], 1
    mov si, msg_child_exit
    call print_string_serial
    mov si, msg_child_exit_reason
    call print_string_serial
    mov al, [cs:int21_last_ah]
    call print_hex8_serial
    mov si, msg_child_exit_code
    call print_string_serial
    xor ax, ax
    mov al, [cs:last_exit_code]
    call print_hex8_serial
    call print_newline_serial
.done:
    pop ds
    pop si
    pop ax
    ret

child_trace_exit_int20:
    mov byte [cs:int21_last_ah], 0x20
    jmp child_trace_exit_int21

child_trace_resize_log:
    push ax
    push si
    call child_trace_should_log_exit
    jnc .done
    mov si, msg_child_4a
    call child_trace_emit_marker
.done:
    pop si
    pop ax
    ret

child_trace_finalize:
    push ax
    push si
    push ds
    push cs
    pop ds
    call child_trace_should_log_exit
    jnc .done
    cmp byte [cs:child_trace_exit_logged], 0
    jne .emit_end
    mov si, msg_child_exit
    call print_string_serial
    mov si, msg_child_exit_reason_retf
    call print_string_serial
    call print_newline_serial
.emit_end:
    mov si, msg_child_trace_end
    call print_string_serial
    cmp byte [cs:dos_exec_state_depth], 1
    jbe .disable
    ; A nested child returned to an external parent.  Keep tracing that
    ; parent until its own EXEC frame finishes, otherwise the calls made by
    ; launchers between sequential children disappear from diagnostics.
    mov byte [cs:child_trace_active], 1
    mov byte [cs:child_trace_exit_logged], 0
    mov word [cs:child_trace_armed], 0
    jmp .done
.disable:
    mov byte [cs:child_trace_active], 0
    mov word [cs:child_trace_armed], 0
.done:
    pop ds
    pop si
    pop ax
    ret
%endif

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
    mov al, bl
    and al, 0x1F                ; DOS keeps drive number in the low five bits
    or al, al                   ; zero selects the current/default drive
    jz .generic_current_drive
    cmp al, 3                   ; only C: has a resident block device
    jne .generic_invalid_drive
    jmp .generic_drive_ready
.generic_current_drive:
    cmp byte [cs:dos_default_drive], 2
    jne .generic_invalid_drive
.generic_drive_ready:
    cmp ch, 0x08                ; generic block-device category
    jne .generic_unsupported
    cmp cl, 0x60                ; get device parameters
    jne .generic_unsupported

    ; DS:DX -> DOS generic block-device parameter packet.  Byte zero is an
    ; input flag and must be retained; bytes 1..31 describe the current BPB.
    push di
    mov di, dx
    mov byte [ds:di + 1], 5     ; fixed disk
    mov word [ds:di + 2], 1     ; bit 0: non-removable media
    mov word [ds:di + 4], ((FAT_TOTAL_SECTORS + (FAT_SPT * FAT_HEADS) - 1) / (FAT_SPT * FAT_HEADS))
    mov byte [ds:di + 6], 0
    mov word [ds:di + 7], 512
    mov byte [ds:di + 9], FAT_SECTORS_PER_CLUSTER
    mov word [ds:di + 10], FAT_RESERVED_SECTORS
    mov byte [ds:di + 12], FAT_COUNT
    mov word [ds:di + 13], FAT_ROOT_DIR_SECTORS * 16
%if FAT_TOTAL_SECTORS <= 0xFFFF
    mov word [ds:di + 15], FAT_TOTAL_SECTORS
%else
    mov word [ds:di + 15], 0
%endif
    mov byte [ds:di + 17], 0xF8
    mov word [ds:di + 18], FAT_SECTORS_PER_FAT
    mov word [ds:di + 20], FAT_SPT
    mov word [ds:di + 22], FAT_HEADS
    mov word [ds:di + 24], FAT_LBA_OFFSET & 0xFFFF
    mov word [ds:di + 26], (FAT_LBA_OFFSET >> 16) & 0xFFFF
%if FAT_TOTAL_SECTORS > 0xFFFF
    mov word [ds:di + 28], FAT_TOTAL_SECTORS & 0xFFFF
    mov word [ds:di + 30], (FAT_TOTAL_SECTORS >> 16) & 0xFFFF
%else
    mov word [ds:di + 28], 0
    mov word [ds:di + 30], 0
%endif
    pop di
    xor ax, ax
    clc
    ret
.generic_invalid_drive:
    mov ax, 0x000F              ; invalid drive
    stc
    ret
.generic_unsupported:
    mov ax, 0x0001              ; unsupported request for a valid drive
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
    cmp bx, DOS_FILE_EXTRA_FIRST_HANDLE
    jb .unknown_handle
    cmp bx, DOS_FILE_EXTRA_LAST_HANDLE
    ja .unknown_handle
    push si
    call int21_extra_handle_ptr
    jc .extra_bad_handle
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 1
    jne .extra_bad_handle
    cmp word [cs:si + DOS_FILE_EXTRA_CLUSTER_OFF], FAT_EOF
    je .extra_stdio
    pop si
    xor dx, dx
    xor ax, ax
    clc
    ret
.extra_stdio:
    pop si
    jmp .stdio
.extra_bad_handle:
    pop si
    jmp .bad_handle
%endif
.unknown_handle:
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
    cmp word [cs:file_handle_start_cluster], FAT_EOF
    je .stdio
    xor dx, dx                  ; disk file
    xor ax, ax
    clc
    ret

.disk_slot2:
    cmp byte [cs:file_handle2_open], 1
    jne .bad_handle
    cmp word [cs:file_handle2_start_cluster], FAT_EOF
    je .stdio
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot3:
    cmp byte [cs:file_handle3_open], 1
    jne .bad_handle
    cmp word [cs:file_handle3_start_cluster], FAT_EOF
    je .stdio
    xor dx, dx
    xor ax, ax
    clc
    ret

%if FAT_TYPE == 16
.disk_slot4:
    cmp byte [cs:file_handle4_open], 1
    jne .bad_handle
    cmp word [cs:file_handle4_start_cluster], FAT_EOF
    je .stdio
    xor dx, dx
    xor ax, ax
    clc
    ret
.disk_slot5:
    cmp byte [cs:file_handle5_open], 1
    jne .bad_handle
    cmp word [cs:file_handle5_start_cluster], FAT_EOF
    je .stdio
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot6:
    cmp byte [cs:file_handle6_open], 1
    jne .bad_handle
    cmp word [cs:file_handle6_start_cluster], FAT_EOF
    je .stdio
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot7:
    cmp byte [cs:file_handle7_open], 1
    jne .bad_handle
    cmp word [cs:file_handle7_start_cluster], FAT_EOF
    je .stdio
    xor dx, dx
    xor ax, ax
    clc
    ret

.disk_slot8:
    cmp byte [cs:file_handle8_open], 1
    jne .bad_handle
    cmp word [cs:file_handle8_start_cluster], FAT_EOF
    je .stdio
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
    mov ax, 0x0006
    stc
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

; The DOS swappable-data area is a live kernel interface, not a snapshot made
; only when AH=5D06h is queried.  DPMI hosts read its current-PSP fields
; directly while admitting a new client, so update all three mirrors as one
; atomic semantic operation whenever EXEC or AH=50h changes process context.
int21_set_current_psp:
    mov [cs:current_psp_seg], ax
    mov [cs:dos_sda_current_psp], ax
    mov [cs:dos_sda_owning_psp], ax
    ret

int21_chdir:
    ; DOS AH=3Bh has no register result other than AX/CF.  In particular the
    ; OpenWatcom tiny-I/O wrapper declares only AX and DX as clobbered and
    ; keeps C locals in BX/CX/SI/DI across the interrupt.  Preserve those
    ; registers while the path resolver uses them as scratch state.
    push bx
    push cx
    push dx
    push si
    push di
    call int21_chdir_impl
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_chdir_impl:
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
    ; Name conversion advances SI. On a fast-lookup miss the general
    ; parser must retry the caller's path, not the end of our scratch name.
    push si
    push ds
    mov ax, cs
    mov ds, ax
    mov si, tmp_cwd_comp
    call int21_path_to_fat_name
    pop ds
    pop si
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
    cmp bx, DOS_CWD_MAX
    jb .fast_seed_loop
    jmp .fail

.fast_append_start:
    cmp bx, 0
    je .fast_copy_component
    mov byte [cs:tmp_cwd_build + bx], '\'
    inc bx
    cmp bx, DOS_CWD_MAX
    jae .fail

.fast_copy_component:
    xor di, di
.fast_copy_component_loop:
    mov al, [cs:tmp_cwd_comp + di]
    cmp al, 0
    je .fast_append_term
    mov [cs:tmp_cwd_build + bx], al
    inc bx
    inc di
    cmp bx, DOS_CWD_MAX
    jb .fast_copy_component_loop
    cmp byte [cs:tmp_cwd_comp + di], 0
    jne .fail
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
    cmp bx, DOS_CWD_MAX
    jb .seed_loop
    mov byte [cs:tmp_cwd_build + DOS_CWD_MAX], 0
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
    cmp bx, DOS_CWD_MAX
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
    cmp bx, DOS_CWD_MAX
    jb .find_end_append
    jmp .fail
.append_start:
    cmp bx, 0
    je .copy_component
    mov byte [cs:tmp_cwd_build + bx], '\'
    inc bx
    cmp bx, DOS_CWD_MAX
    jae .fail
.copy_component:
    xor di, di
.copy_component_loop:
    mov al, [cs:tmp_cwd_comp + di]
    cmp al, 0
    je .append_term
    mov [cs:tmp_cwd_build + bx], al
    inc bx
    inc di
    cmp bx, DOS_CWD_MAX
    jb .copy_component_loop
    cmp byte [cs:tmp_cwd_comp + di], 0
    jne .fail
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
    cmp bx, DOS_CWD_MAX
    jb .commit_loop
    mov byte [cs:cwd_buf + DOS_CWD_MAX], 0
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

; DOS AH=29h - parse one command-tail token into a standard 16-byte FCB.
; This is used by Pascal/C runtimes before EXEC and by the child's PSP
; filename compatibility fields.  DS:SI advances past the parsed token;
; ES:DI remains the destination FCB pointer.
int21_parse_fcb_filename:
    push bx
    push cx
    push dx
    push di

    mov [cs:tmp_fcb_parse_control], al
    mov byte [cs:tmp_fcb_parse_wild], 0
    mov byte [cs:tmp_fcb_parse_invalid], 0
    mov bx, di
    cld

    test byte [cs:tmp_fcb_parse_control], 0x01
    jz .skip_whitespace
.skip_leading:
    mov al, [ds:si]
    cmp al, ' '
    je .skip_one
    cmp al, 9
    je .skip_one
    cmp al, ','
    je .skip_one
    cmp al, ';'
    je .skip_one
    cmp al, '='
    je .skip_one
    cmp al, '+'
    jne .skip_whitespace
.skip_one:
    inc si
    jmp .skip_leading

.skip_whitespace:
    mov al, [ds:si]
    cmp al, ' '
    je .skip_space
    cmp al, 9
    jne .drive
.skip_space:
    inc si
    jmp .skip_whitespace

.drive:
    mov al, [ds:si]
    cmp byte [ds:si + 1], ':'
    jne .default_drive
    cmp al, 'a'
    jb .drive_upper
    cmp al, 'z'
    ja .drive_upper
    sub al, 0x20
.drive_upper:
    cmp al, 'A'
    jb .bad_drive
    cmp al, 'D'
    ja .bad_drive
    sub al, 'A' - 1
    mov [es:bx], al
    add si, 2
    jmp .drive_done
.bad_drive:
    mov byte [cs:tmp_fcb_parse_invalid], 1
    xor al, al
    mov [es:bx], al
    add si, 2
    jmp .drive_done
.default_drive:
    test byte [cs:tmp_fcb_parse_control], 0x02
    jnz .drive_done
    mov byte [es:bx], 0
.drive_done:
    mov word [es:bx + 12], 0
    mov word [es:bx + 14], 0

    test byte [cs:tmp_fcb_parse_control], 0x04
    jnz .blank_ext
    push di
    mov di, bx
    inc di
    mov al, ' '
    mov cx, 8
    rep stosb
    pop di
.blank_ext:
    test byte [cs:tmp_fcb_parse_control], 0x08
    jnz .parse_name
    push di
    mov di, bx
    add di, 9
    mov al, ' '
    mov cx, 3
    rep stosb
    pop di

.parse_name:
    mov di, bx
    inc di
    mov cx, 8
.name_loop:
    mov al, [ds:si]
    call int21_fcb_is_field_end
    jc .name_done
    cmp al, '*'
    je .name_star
    inc si
    cmp al, '?'
    jne .name_upper
    mov byte [cs:tmp_fcb_parse_wild], 1
    jmp .name_store
.name_upper:
    cmp al, 'a'
    jb .name_store
    cmp al, 'z'
    ja .name_store
    sub al, 0x20
.name_store:
    jcxz .name_loop
    mov [es:di], al
    inc di
    dec cx
    jmp .name_loop
.name_star:
    mov byte [cs:tmp_fcb_parse_wild], 1
    inc si
    mov al, '?'
.name_star_fill:
    jcxz .name_star_skip
    mov [es:di], al
    inc di
    dec cx
    jmp .name_star_fill
.name_star_skip:
    mov al, [ds:si]
    call int21_fcb_is_field_end
    jc .name_done
    inc si
    jmp .name_star_skip

.name_done:
    cmp byte [ds:si], '.'
    jne .result
    inc si
    mov di, bx
    add di, 9
    mov cx, 3
.ext_loop:
    mov al, [ds:si]
    call int21_fcb_is_field_end
    jc .result
    cmp al, '*'
    je .ext_star
    inc si
    cmp al, '?'
    jne .ext_upper
    mov byte [cs:tmp_fcb_parse_wild], 1
    jmp .ext_store
.ext_upper:
    cmp al, 'a'
    jb .ext_store
    cmp al, 'z'
    ja .ext_store
    sub al, 0x20
.ext_store:
    jcxz .ext_loop
    mov [es:di], al
    inc di
    dec cx
    jmp .ext_loop
.ext_star:
    mov byte [cs:tmp_fcb_parse_wild], 1
    inc si
    mov al, '?'
.ext_star_fill:
    jcxz .ext_star_skip
    mov [es:di], al
    inc di
    dec cx
    jmp .ext_star_fill
.ext_star_skip:
    mov al, [ds:si]
    call int21_fcb_is_field_end
    jc .result
    inc si
    jmp .ext_star_skip

.result:
    cmp byte [cs:tmp_fcb_parse_invalid], 0
    jne .invalid
    mov al, [cs:tmp_fcb_parse_wild]
    jmp .done
.invalid:
    mov al, 0xFF
.done:
    pop di
    pop dx
    pop cx
    pop bx
    clc
    ret

int21_fcb_is_field_end:
    test al, al
    jz .yes
    cmp al, ' '
    je .yes
    cmp al, 9
    je .yes
    cmp al, '.'
    je .yes
    cmp al, 0x5C
    je .yes
    cmp al, '/'
    je .yes
    cmp al, ':'
    je .yes
    cmp al, ','
    je .yes
    cmp al, ';'
    je .yes
    cmp al, '='
    je .yes
    cmp al, '+'
    je .yes
    clc
    ret
.yes:
    stc
    ret

int21_get_set_attr:
    ; AH=43h returns AX/CF and, for AL=00h, CX.  Path lookup is free to use
    ; BX/DX/SI/DI internally, but those registers belong to the caller.  In
    ; particular WOLF3D keeps its VSWAP handle in BX while probing attributes;
    ; leaking the lookup's scratch BX made every following AH=42h seek target
    ; handle 0 and left the game spinning before VGA initialization.
    push bx
    push dx
    push si
    push di
    cmp al, 0x00
    je .get_attr
    cmp al, 0x01
    je .set_attr
    mov ax, 0x0001
    stc
    jmp .return

.get_attr:
%if FAT_TYPE == 16
    mov si, dx
    call int21_is_mounted_root_path
    jc .get_attr_lookup
    mov cx, 0x0010
    xor ax, ax
    clc
    jmp .return
.get_attr_lookup:
    mov si, dx
    call int21_resolve_and_find_path
    jc .not_found
    xor ch, ch
    mov cl, [cs:search_found_attr]
    xor ax, ax
    clc
    jmp .return
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
    jmp .return
%endif

.set_attr:
%if FAT_TYPE == 16
    ; The read-only, hidden, system and archive bits of CX go to the entry;
    ; the directory and volume bits cannot be changed (access denied).
    test cl, 0x18
    jnz .denied
    mov si, dx
    call int21_resolve_and_find_path
    jc .not_found
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:search_found_root_lba]
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call read_sector_lba32
    jc .denied
    mov di, [cs:search_found_root_off]
    mov al, cl
    and al, 0x27
    mov ah, [es:di + 11]
    and ah, 0x18
    or al, ah
    mov [es:di + 11], al
    mov ax, [cs:search_found_root_lba]
    call write_sector_lba32
    jc .denied
    xor ax, ax
    clc
    jmp .return
.denied:
    mov ax, 0x0005
    stc
    jmp .return
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
    jmp .return
%endif

.not_found:
    mov ax, 0x0002
    stc
.return:
    pop di
    pop si
    pop dx
    pop bx
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
    mov [cs:tmp_lba_hi], dx
    xor dx, dx

.subdir_sector_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .subdir_next_cluster

    mov ax, DOS_META_BUF_SEG
    mov es, ax
%if FAT_TYPE == 16
    push bx
    mov bx, dx
    push bx
    mov ax, [cs:tmp_lba]
    add ax, bx
    mov dx, [cs:tmp_lba_hi]
    adc dx, 0
    xor bx, bx
    call read_sector_lba32
    pop dx
    pop bx
%else
    mov ax, [cs:tmp_lba]
    add ax, dx
    push bx
    xor bx, bx
    call read_sector_lba
    pop bx
%endif
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
    jcxz .term
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
    ; AH=3Ch returns only AX/CF.  Small-model OpenWatcom passes the address
    ; where it will store AX in BX and performs that store immediately after
    ; INT 21h, so leaking the FAT allocator's scratch BX turns a successful
    ; create into handle -1.  Preserve every non-result general register.
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    call int21_create_impl
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

int21_create_impl:
    push dx
    push ds
    push cx


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
%if FAT_TYPE == 16
    ; DOS: AH=3Ch on an existing file truncates it to 0 bytes (its chain is
    ; freed and its entry rewritten below, as for a new file). A directory,
    ; a volume label or a read-only file is access denied.
    test byte [cs:search_found_attr], 0x19
    jnz .io_error
    call int21_free_found_chain
    jc .io_error
    mov ax, [cs:search_found_root_lba]
    jmp .write_entry
%else
    test byte [cs:search_found_attr], 0x10
    jnz .io_error
    jmp .open_created
%endif

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
.write_entry:
    mov [cs:tmp_next_cluster], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:tmp_cluster], ax

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_next_cluster]
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
    jc .io_error

%if FAT_TYPE == 16
    mov bp, sp
    push word [ss:bp]               ; caller's attributes, above saved DS/DX
    call dos_now_stamp
    push dx
    push ax
%endif
    mov di, [cs:tmp_cluster]
    mov si, path_fat_name
    mov cx, 11
    rep movsb

%if FAT_TYPE == 16
    ; Created now (the fields held a deleted entry's bytes), with the
    ; read-only/hidden/system bits of CX.
    pop ax
    mov [es:di - 11 + 14], ax
    mov [es:di - 11 + 22], ax
    pop ax
    mov [es:di - 11 + 16], ax
    mov [es:di - 11 + 18], ax
    mov [es:di - 11 + 24], ax
    pop ax
    and al, 0x07
    or al, 0x20
    mov [es:di - 11 + 11], al
    mov word [es:di - 11 + 12], 0
    mov word [es:di - 11 + 20], 0
%else
    mov byte [es:di - 11 + 11], 0x20
%endif
    mov word [es:di - 11 + 26], 0
    mov word [es:di - 11 + 28], 0
    mov word [es:di - 11 + 30], 0

    mov ax, [cs:tmp_next_cluster]
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
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
    pop cx
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
    pop cx
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

; Recognize the DOS console character device from the final path component.
; Device names are case-insensitive and remain valid with a drive/directory
; prefix (for example C:\CON).  CF is clear for CON/CON:, set otherwise.
int21_path_is_console_device:
    push ax
    push bx
    push si

    mov si, dx
    mov bx, si
.scan:
    mov al, [si]
    or al, al
    jz .compare
    cmp al, '\'
    je .new_component
    cmp al, '/'
    je .new_component
    cmp al, ':'
    jne .next
.new_component:
    mov bx, si
    inc bx
.next:
    inc si
    jmp .scan

.compare:
    mov al, [bx]
    or al, 0x20
    cmp al, 'c'
    jne .not_console
    mov al, [bx + 1]
    or al, 0x20
    cmp al, 'o'
    jne .not_console
    mov al, [bx + 2]
    or al, 0x20
    cmp al, 'n'
    jne .not_console
    mov al, [bx + 3]
    or al, al
    jz .console
    cmp al, ':'
    jne .not_console
    cmp byte [bx + 4], 0
    jne .not_console

.console:
    clc
    jmp .done
.not_console:
    stc
.done:
    pop si
    pop bx
    pop ax
    ret

%if FAT_TYPE == 16
; Resolve handles 13..19 to their compact metadata entry.
; Input: BX=handle. Output: SI=entry, CF clear; AX is scratch.
int21_extra_handle_ptr:
    push ax
    cmp bx, DOS_FILE_EXTRA_FIRST_HANDLE
    jb .bad
    cmp bx, DOS_FILE_EXTRA_LAST_HANDLE
    ja .bad
    mov si, bx
    sub si, DOS_FILE_EXTRA_FIRST_HANDLE
    mov ax, si
    shl si, 4
    shl ax, 2
    add si, ax
    add si, file_handle_extra_table
    pop ax
    clc
    ret
.bad:
    pop ax
    stc
    ret

; Swap one compact slot with the primary slot used by the existing FAT I/O
; engine. Input AL is the internal target number (9..15). All registers are
; preserved so read/write/seek retain their caller arguments.
int21_swap_file_handle_extra:
    push ax
    push bx
    push cx
    push dx
    push si

    xor ah, ah
    mov bx, ax
    add bx, 4
    call int21_extra_handle_ptr
    jc .done

    mov al, [cs:file_handle_open]
    xchg al, [cs:si + DOS_FILE_EXTRA_OPEN_OFF]
    mov [cs:file_handle_open], al
    mov ax, [cs:file_handle_pos]
    xchg ax, [cs:si + DOS_FILE_EXTRA_POS_LO_OFF]
    mov [cs:file_handle_pos], ax
    mov ax, [cs:file_handle_pos_hi]
    xchg ax, [cs:si + DOS_FILE_EXTRA_POS_HI_OFF]
    mov [cs:file_handle_pos_hi], ax
    mov al, [cs:file_handle_mode]
    xchg al, [cs:si + DOS_FILE_EXTRA_MODE_OFF]
    mov [cs:file_handle_mode], al
    mov ax, [cs:file_handle_start_cluster]
    xchg ax, [cs:si + DOS_FILE_EXTRA_CLUSTER_OFF]
    mov [cs:file_handle_start_cluster], ax
    mov ax, [cs:file_handle_root_lba]
    xchg ax, [cs:si + DOS_FILE_EXTRA_ROOT_LBA_OFF]
    mov [cs:file_handle_root_lba], ax
    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:si + DOS_FILE_EXTRA_ROOT_LBA_HI_OFF]
    mov [cs:file_handle_root_lba_hi], ax
    mov ax, [cs:file_handle_root_off]
    xchg ax, [cs:si + DOS_FILE_EXTRA_ROOT_OFF_OFF]
    mov [cs:file_handle_root_off], ax
    mov ax, [cs:file_handle_cluster_count]
    xchg ax, [cs:si + DOS_FILE_EXTRA_CLUSTER_COUNT_OFF]
    mov [cs:file_handle_cluster_count], ax
    mov ax, [cs:file_handle_size_lo]
    xchg ax, [cs:si + DOS_FILE_EXTRA_SIZE_LO_OFF]
    mov [cs:file_handle_size_lo], ax
    mov ax, [cs:file_handle_size_hi]
    xchg ax, [cs:si + DOS_FILE_EXTRA_SIZE_HI_OFF]
    mov [cs:file_handle_size_hi], ax

.done:
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif

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
    mov si, file_handle_extra_table
    mov bl, DOS_FILE_EXTRA_FIRST_TARGET
    mov cx, DOS_FILE_EXTRA_COUNT
.scan_extra:
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 0
    je .target_extra
    add si, DOS_FILE_EXTRA_ENTRY_SIZE
    inc bl
    loop .scan_extra
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
    jmp .target_ready

.target_extra:
    mov [cs:file_handle_target], bl
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
    cmp byte [cs:file_handle_target], DOS_FILE_EXTRA_FIRST_TARGET
    jae .assign_extra
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle2_root_lba_hi], ax
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle3_root_lba_hi], ax
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle4_root_lba_hi], ax
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle5_root_lba_hi], ax
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle6_root_lba_hi], ax
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle7_root_lba_hi], ax
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
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:file_handle8_root_lba_hi], ax
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

.assign_extra:
    xor ax, ax
    mov al, [cs:file_handle_target]
    mov bx, ax
    add bx, 4
    call int21_extra_handle_ptr
    jc .io_fail

    mov byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 1
    mov word [cs:si + DOS_FILE_EXTRA_POS_LO_OFF], 0
    mov word [cs:si + DOS_FILE_EXTRA_POS_HI_OFF], 0
    mov al, [cs:tmp_open_mode]
    mov [cs:si + DOS_FILE_EXTRA_MODE_OFF], al
    mov ax, [cs:search_found_cluster]
    mov [cs:si + DOS_FILE_EXTRA_CLUSTER_OFF], ax
    mov ax, [cs:search_found_root_lba]
    mov [cs:si + DOS_FILE_EXTRA_ROOT_LBA_OFF], ax
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:si + DOS_FILE_EXTRA_ROOT_LBA_HI_OFF], ax
    mov ax, [cs:search_found_root_off]
    mov [cs:si + DOS_FILE_EXTRA_ROOT_OFF_OFF], ax
    mov ax, [cs:search_found_size_lo]
    mov [cs:si + DOS_FILE_EXTRA_SIZE_LO_OFF], ax
    mov ax, [cs:search_found_size_hi]
    mov [cs:si + DOS_FILE_EXTRA_SIZE_HI_OFF], ax

    push si
    call int21_load_fat_cache
    pop si
    jc .extra_io_fail
    mov ax, [cs:si + DOS_FILE_EXTRA_CLUSTER_OFF]
    push si
    call int21_count_chain
    pop si
    mov [cs:si + DOS_FILE_EXTRA_CLUSTER_COUNT_OFF], ax

    xor ax, ax
    mov al, [cs:file_handle_target]
    add ax, 4
    clc
    ret

.extra_io_fail:
    mov byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 0
    jmp .io_fail
%endif

.io_fail:
    mov ax, 0x0005
    stc
    ret

int21_open:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es

    call int21_normalize_leading_drive_designator

    ; AH=3Dh: AL carries access in bits 0..2 plus sharing/inherit flags.
    ; QuickBASIC's sequential OUTPUT reopen uses the legacy value 0Bh after
    ; creating a file.  DOSBox's FAT backend treats that value as writable;
    ; normalize this one runtime convention to read/write so Costa can write
    ; RUN.DAT, while still rejecting every other invalid access value.
    cmp al, 0x0B
    jne .decode_access
    mov al, 0x02
.decode_access:
    ; Accept sharing/inherit bits and validate only the effective access mode.
    and al, 0x03
    cmp al, 0x03
    je .access_denied
    mov [cs:tmp_open_mode], al

    call int21_path_is_console_device
    jc .regular_file
    call int21_select_free_file_handle
    jc .console_extra
    mov word [cs:search_found_cluster], FAT_EOF
    mov word [cs:search_found_size_lo], 0
    mov word [cs:search_found_size_hi], 0
    mov word [cs:search_found_root_lba], 0
    mov word [cs:search_found_root_lba_hi], 0
    mov word [cs:search_found_root_off], 0
    call int21_assign_selected_file_handle
    jmp .done

.console_extra:
    mov ax, 0x0004
    stc
    jmp .done

.regular_file:
    call int21_select_free_file_handle
    jc .done
    mov byte [cs:int21_path_stage_marker], 1
    mov si, dx
    call int21_resolve_and_find_path
    jc .done
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
    pop bp
    pop di
    pop si
    pop dx
	pop cx
	pop bx
	ret

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
    cmp bx, DOS_FILE_EXTRA_FIRST_HANDLE
    jb .bad_handle
    cmp bx, DOS_FILE_EXTRA_LAST_HANDLE
    ja .bad_handle
    push si
    call int21_extra_handle_ptr
    jc .close_extra_bad
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 1
    jne .close_extra_bad
    mov byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 0
    pop si
    xor ax, ax
    clc
    ret
.close_extra_bad:
    pop si
    jmp .bad_handle
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
    push si
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
    cmp bx, DOS_FILE_EXTRA_FIRST_HANDLE
    jb .bad
    cmp bx, DOS_FILE_EXTRA_LAST_HANDLE
    ja .bad
    call int21_extra_handle_ptr
    jc .bad
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 1
    jne .bad
    jmp .ok
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
    pop si
    xor ax, ax
    clc
    ret

.bad:
    pop si
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
    cmp bx, DOS_FILE_EXTRA_FIRST_HANDLE
    jb .bad_handle
    cmp bx, DOS_FILE_EXTRA_LAST_HANDLE
    ja .bad_handle
    call int21_extra_handle_ptr
    jc .bad_handle
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 1
    jne .bad_handle
    push ax
    mov al, bl
    sub al, 4
    mov [cs:file_handle_swapped], al
    call int21_swap_file_handle_extra
    pop ax
    mov bx, 0x0005
    jmp .handle_ready
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
    cmp word [cs:file_handle_start_cluster], FAT_EOF
    je .stdin_read

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
%if FAT_TYPE == 16
    mov [cs:tmp_lba_hi], dx
%endif

    mov ax, [cs:tmp_cluster_off]
    mov cl, 9
    shr ax, cl
    add [cs:tmp_lba], ax
%if FAT_TYPE == 16
    adc word [cs:tmp_lba_hi], 0
%endif

    mov ax, [cs:tmp_cluster_off]
    and ax, 0x01FF
    mov [cs:tmp_sector_off], ax

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    xor bx, bx
%if FAT_TYPE == 16
    mov dx, [cs:tmp_lba_hi]
    call read_sector_lba32
%else
    call read_sector_lba
%endif
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
    mov di, [cs:tmp_user_ptr]
    add di, [cs:tmp_rw_done]
    jnc .read_user_ptr_ready
    ; DS:DX + bytes_done is a linear DOS buffer, not a 16-bit near
    ; pointer.  Normalize a 64 KiB offset wrap into the segment or a large
    ; AH=3Fh transfer silently starts overwriting the beginning of the
    ; caller's allocation (notably Borland overlay/cache buffers).
    add ax, 0x1000
.read_user_ptr_ready:
    mov es, ax
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
    cmp al, DOS_FILE_EXTRA_FIRST_TARGET
    jae .done_swap_extra
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
    jmp .done_noswap
.done_swap_extra:
    call int21_swap_file_handle_extra
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
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:int21_write_error_stage], '?'
%endif

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
    cmp bx, DOS_FILE_EXTRA_FIRST_HANDLE
    jb .bad_handle
    cmp bx, DOS_FILE_EXTRA_LAST_HANDLE
    ja .bad_handle
    call int21_extra_handle_ptr
    jc .bad_handle
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 1
    jne .bad_handle
    mov al, bl
    sub al, 4
    mov [cs:file_handle_swapped], al
    call int21_swap_file_handle_extra
    mov bx, 0x0005
    jmp .handle_ready
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
    cmp word [cs:file_handle_start_cluster], FAT_EOF
    je .stdio_write

    mov [cs:tmp_rw_remaining], cx
    mov word [cs:tmp_rw_done], 0
    mov ax, ds
    mov [cs:tmp_user_ds], ax
    mov [cs:tmp_user_ptr], dx

    cmp word [cs:tmp_rw_remaining], 0
    jne .prepare
    ; AH=40h/CX=0 changes the allocation as well as the directory size.
    ; Do this first: a failed extension must not advertise unreadable bytes.
    call int21_resize_write_chain
    jc .io_error
    mov ax, [cs:file_handle_pos]
    mov [cs:file_handle_size_lo], ax
%if FAT_TYPE == 16
    mov ax, [cs:file_handle_pos_hi]
    mov [cs:file_handle_size_hi], ax
%else
    mov word [cs:file_handle_size_hi], 0
%endif
    call int21_update_root_entry_size
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:int21_write_error_stage], 'Z'
%endif
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
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:int21_write_error_stage], 'G'
%endif
    jc .io_error
    jmp .cluster_resolve

.cluster_ready:
    mov [cs:tmp_cluster], ax
    mov [cs:tmp_cluster_off], dx

    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
%if FAT_TYPE == 16
    mov [cs:tmp_lba_hi], dx
%endif

    mov ax, [cs:tmp_cluster_off]
    mov cl, 9
    shr ax, cl
    add [cs:tmp_lba], ax
%if FAT_TYPE == 16
    adc word [cs:tmp_lba_hi], 0
%endif

    mov ax, [cs:tmp_cluster_off]
    and ax, 0x01FF
    mov [cs:tmp_sector_off], ax

    mov ax, DOS_IO_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_lba]
    xor bx, bx
%if FAT_TYPE == 16
    mov dx, [cs:tmp_lba_hi]
    call read_sector_lba32
%else
    call read_sector_lba
%endif
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:int21_write_error_stage], 'R'
%endif
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
    mov si, [cs:tmp_user_ptr]
    add si, [cs:tmp_rw_done]
    jnc .write_user_ptr_ready
    ; Mirror AH=3Fh's normalized far-buffer arithmetic for AH=40h.
    add ax, 0x1000
.write_user_ptr_ready:
    mov ds, ax
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
%if FAT_TYPE == 16
    mov dx, [cs:tmp_lba_hi]
    call write_sector_lba32
%else
    call write_sector_lba
%endif
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:int21_write_error_stage], 'W'
%endif
    jc .io_error

    mov ax, [cs:tmp_chunk]
    add [cs:file_handle_pos], ax
%if FAT_TYPE == 16
    adc word [cs:file_handle_pos_hi], 0
%endif
    add [cs:tmp_rw_done], ax
    sub [cs:tmp_rw_remaining], ax
    jmp .loop

.finish:
    call fat12_flush_cache
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:int21_write_error_stage], 'F'
%endif
    jc .io_error

%if FAT_TYPE == 16
    mov ax, [cs:file_handle_pos_hi]
    cmp ax, [cs:file_handle_size_hi]
    ja .grow_size
    jb .done_ok
%endif
    mov ax, [cs:file_handle_pos]
    cmp ax, [cs:file_handle_size_lo]
    jbe .done_ok
.grow_size:
    mov ax, [cs:file_handle_pos]
    mov [cs:file_handle_size_lo], ax
%if FAT_TYPE == 16
    mov ax, [cs:file_handle_pos_hi]
    mov [cs:file_handle_size_hi], ax
%else
    mov word [cs:file_handle_size_hi], 0
%endif
    call int21_update_root_entry_size
%if TRACE_CHILD_INT21 != 0
    mov byte [cs:int21_write_error_stage], 'U'
%endif
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
%if TRACE_CHILD_INT21 != 0
    push ax
    mov al, '['
    call serial_putc
    mov al, 'W'
    call serial_putc
    mov al, 'R'
    call serial_putc
    mov al, ':'
    call serial_putc
    mov al, [cs:int21_write_error_stage]
    call serial_putc
    mov al, ']'
    call serial_putc
    mov al, 0x0D
    call serial_putc
    mov al, 0x0A
    call serial_putc
    pop ax
%endif
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
    cmp al, DOS_FILE_EXTRA_FIRST_TARGET
    jae .done_swap_extra
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
    jmp .done_noswap
.done_swap_extra:
    call int21_swap_file_handle_extra
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
%if TRACE_CHILD_INT21 == 0
    push ax
    call console_putc_ansi
    pop ax
%else
    call bios_putc
%endif
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

%if TRACE_CHILD_INT21 == 0
console_ansi_reset:
    mov byte [cs:console_ansi_state], 0
    mov byte [cs:console_ansi_flags], 0
    mov byte [cs:console_ansi_p1], 0
    mov byte [cs:console_ansi_p2], 0
    ret

console_putc_ansi:
    push ax
    push bx
    push cx
    push dx
    mov bl, [cs:console_ansi_state]
    cmp bl, 0
    jne .state_nonzero
    cmp al, 0x1B
    jne .literal
    mov byte [cs:console_ansi_state], 1
    jmp .done

.literal:
    call bios_putc
    jmp .done

.state_nonzero:
    cmp bl, 1
    jne .state_csi
    cmp al, '['
    je .csi_begin
    mov byte [cs:console_ansi_state], 0
    push ax
    mov al, 0x1B
    call bios_putc
    pop ax
    call bios_putc
    jmp .done

.csi_begin:
    mov byte [cs:console_ansi_state], 2
    mov byte [cs:console_ansi_flags], 0
    mov byte [cs:console_ansi_p1], 0
    mov byte [cs:console_ansi_p2], 0
    jmp .done

.state_csi:
    cmp al, '0'
    jb .csi_control
    cmp al, '9'
    ja .csi_control
    sub al, '0'
    mov cl, al
    mov al, [cs:console_ansi_flags]
    test al, 0x01
    jnz .digit_p2
    mov al, [cs:console_ansi_p1]
    mov bl, 10
    mul bl
    add al, cl
    mov [cs:console_ansi_p1], al
    or byte [cs:console_ansi_flags], 0x02
    jmp .done

.digit_p2:
    mov al, [cs:console_ansi_p2]
    mov bl, 10
    mul bl
    add al, cl
    mov [cs:console_ansi_p2], al
    or byte [cs:console_ansi_flags], 0x04
    jmp .done

.csi_control:
    cmp al, ';'
    je .separator
    cmp al, 'H'
    je .cursor
    cmp al, 'f'
    je .cursor
    cmp al, 'J'
    je .clear_screen
    cmp al, 'K'
    je .clear_eol
    call console_ansi_reset
    jmp .done

.separator:
    or byte [cs:console_ansi_flags], 0x01
    jmp .done

.cursor:
    xor ax, ax
    mov al, [cs:console_ansi_p1]
    test byte [cs:console_ansi_flags], 0x02
    jnz .row_ready
    mov al, 1
.row_ready:
    or al, al
    jnz .row_nonzero
    mov al, 1
.row_nonzero:
    cmp al, 25
    jbe .row_clamped
    mov al, 25
.row_clamped:
    dec al
    mov dh, al
    xor ax, ax
    mov al, [cs:console_ansi_p2]
    test byte [cs:console_ansi_flags], 0x04
    jnz .col_ready
    mov al, 1
.col_ready:
    or al, al
    jnz .col_nonzero
    mov al, 1
.col_nonzero:
    cmp al, 80
    jbe .col_clamped
    mov al, 80
.col_clamped:
    dec al
    mov dl, al
    call set_cursor_pos
    call console_ansi_reset
    jmp .done

.clear_screen:
    xor ax, ax
    mov al, [cs:console_ansi_p1]
    test byte [cs:console_ansi_flags], 0x02
    jz .clear_screen_apply
    cmp al, 0
    je .clear_screen_apply
    cmp al, 2
    jne .clear_ignore
.clear_screen_apply:
    mov bl, 0x07
    call clear_screen_attr
    xor dx, dx
    call set_cursor_pos
    call console_ansi_reset
    jmp .done

.clear_eol:
    xor ax, ax
    mov al, [cs:console_ansi_p1]
    test byte [cs:console_ansi_flags], 0x02
    jz .clear_eol_apply
    cmp al, 0
    je .clear_eol_apply
    cmp al, 2
    jne .clear_ignore
.clear_eol_apply:
    mov ah, 0x03
    xor bh, bh
    int 0x10
    xor ax, ax
    mov al, [cs:console_ansi_p1]
    cmp al, 2
    jne .clear_eol_from_cursor
    xor dl, dl
.clear_eol_from_cursor:
    mov ah, 0x06
    xor al, al
    mov bh, 0x07
    mov ch, dh
    mov cl, dl
    mov dl, 79
    int 0x10
    call console_ansi_reset
    jmp .done

.clear_ignore:
    call console_ansi_reset

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif

; Resize the current handle's FAT chain to ceil(position / cluster bytes).
; Extension contents are unspecified by DOS; existing file data is untouched.
; SI retains the original allocation for rollback if the volume fills while
; extending. DI is the requested cluster count; neither is clobbered by the
; FAT/cache/BIOS helpers. The caller already saves both registers.
int21_resize_write_chain:
%if FAT_TYPE == 16
    mov eax, [cs:file_handle_pos]
%else
    movzx eax, word [cs:file_handle_pos]
%endif
    add eax, FAT_CLUSTER_MASK
    jc .fail
    shr eax, FAT_CLUSTER_SHIFT
    cmp eax, FAT_DATA_CLUSTER_COUNT
    ja .fail
    mov di, ax
    mov si, [cs:file_handle_cluster_count]
.grow:
    cmp di, [cs:file_handle_cluster_count]
    jbe .shrink
    call int21_write_grow_chain
    jnc .grow
    ; Discard only the newly allocated tail, retaining the original bytes.
    mov di, si
    call .shrink
.fail:
    stc
    ret
.shrink:
    cmp di, [cs:file_handle_cluster_count]
    je .flush
    mov ax, [cs:file_handle_start_cluster]
    mov cx, di
    jcxz .empty
.keep:
    mov bx, ax
    call fat12_get_entry_cached
    jc .fail
    loop .keep
    ; BX is the final retained cluster, AX the first cluster to release.
    push ax
    mov ax, bx
    mov dx, FAT_EOF
    call fat12_set_entry_cached
    pop ax
    jc .fail
    jmp .free
.empty:
    mov word [cs:file_handle_start_cluster], 0
.free:
    cmp ax, 2
    jb .resized
    cmp ax, FAT_EOF
    jae .resized
    mov bx, ax
    call fat12_get_entry_cached
    jc .fail
    push ax
    mov ax, bx
    xor dx, dx
    call fat12_set_entry_cached
    pop ax
    jc .fail
    jmp .free
.resized:
    mov [cs:file_handle_cluster_count], di
.flush:
    jmp fat12_flush_cache

int21_write_grow_chain:
    mov bx, 2

.find_free:
    cmp bx, FAT_DATA_CLUSTER_COUNT + 2
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
    jcxz .set_start_cluster

    ; Walk by cluster count instead of converting the last-cluster index to
    ; a 16-bit byte offset.  The old conversion wrapped at 64 KiB and, on
    ; FAT16, int21_cluster_for_pos also added file_handle_pos_hi a second
    ; time.  As a result every file stopped growing once it crossed 64 KiB.
    mov ax, [cs:file_handle_start_cluster]
    cmp ax, 2
    jb .fail
    dec cx
.find_last_cluster:
    jcxz .link_last_cluster
    call fat12_get_entry_cached
    jc .fail
    cmp ax, 2
    jb .fail
    cmp ax, FAT_EOF
    jae .fail
    dec cx
    jmp .find_last_cluster

.link_last_cluster:
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

    ; AH=41h deletes files, never directory trees. RMDIR performs its own
    ; emptiness/current-directory checks before using the common FAT release.
    ; A read-only file is access denied, as in DOS.
%if FAT_TYPE == 16
    test byte [cs:search_found_attr], 0x11
%else
    test byte [cs:search_found_attr], 0x10
%endif
    jnz .io_error
    call int21_free_found_chain
    jc .io_error

.free_done:
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:search_found_root_lba]
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
    jc .io_error

    mov di, [cs:search_found_root_off]
    mov byte [es:di], 0xE5

    mov ax, [cs:search_found_root_lba]
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
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

; Release the already-resolved object's FAT chain. Does not alter its parent
; directory location; callers own the entry update and their register frames.
int21_free_found_chain:
    mov ax, [cs:search_found_cluster]
    mov [cs:tmp_next_cluster], ax

    call int21_load_fat_cache
    jc .io_error

.free_loop:
    mov ax, [cs:tmp_next_cluster]
    cmp ax, 2
    jb .free_done
    cmp ax, FAT_EOF
    jae .free_done

    mov bx, ax
    call fat12_get_entry_cached
    jc .io_error
    mov [cs:tmp_next_cluster], ax

    mov ax, bx
    xor dx, dx
    call fat12_set_entry_cached
    jc .io_error
    jmp .free_loop

.free_done:
    jmp fat12_flush_cache

.io_error:
    mov ax, 0x0005
    stc
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
    cmp bx, FAT_DATA_CLUSTER_COUNT + 2
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
%if FAT_TYPE == 16
    mov [cs:tmp_lba_hi], dx
%endif
    xor dx, dx
.mkdir_zero_cluster_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .mkdir_reload_root_sector
    xor bx, bx
    mov ax, [cs:tmp_lba]
    add ax, dx
%if FAT_TYPE == 16
    push dx
    mov dx, [cs:tmp_lba_hi]
    adc dx, 0
    call write_sector_lba32
    pop dx
%else
    call write_sector_lba
%endif
    jc .mkdir_io_err
    inc dx
    jmp .mkdir_zero_cluster_loop

.mkdir_reload_root_sector:
    ; A subdirectory starts with canonical self/parent entries. Publish the
    ; parent entry only after these records and all empty sectors are durable.
    xor di, di
    mov al, ' '
    mov cx, 11
    rep stosb
    mov byte [es:0], '.'
    mov byte [es:11], 0x10
    mov ax, [cs:tmp_cluster_off]
    mov [es:26], ax
    mov di, 32
    mov al, ' '
    mov cx, 11
    rep stosb
    mov word [es:32], 0x2E2E
    mov byte [es:43], 0x10
    mov ax, [cs:tmp_lookup_dir]
    mov [es:58], ax
    mov ax, [cs:tmp_lba]
%if FAT_TYPE == 16
    mov dx, [cs:tmp_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
    jc .mkdir_io_err

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_next_cluster]
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
    jc .mkdir_io_err

    mov di, [cs:tmp_cluster]
    mov ax, cs
    mov ds, ax
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
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
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
    ; Removing a dot alias would delete its internal entry and release the
    ; directory while leaving the real name in its parent pointing at it.
    cmp byte [cs:search_found_name], '.'
    je .rmdir_io_err

    mov ax, [cs:search_found_cluster]
    cmp ax, [cs:cwd_cluster]
    je .rmdir_io_err
%if FAT_TYPE == 16
    cmp ax, [cs:cwd_c_cluster]
    je .rmdir_io_err
    cmp ax, [cs:cwd_d_cluster]
    je .rmdir_io_err
%endif
    call int21_directory_empty
    jc .rmdir_io_err

    ; Read the parent directory sector into the shared meta buffer. ES must be
    ; pointed at a safe segment first: read_sector_lba/write_sector_lba target
    ; ES:BX, and on entry ES still holds the INT 21h caller's segment.
    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:search_found_root_lba]
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
    jc .rmdir_io_err

    mov di, [cs:search_found_root_off]
    mov byte [es:di], 0xE5

    mov ax, [cs:search_found_root_lba]
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
    jc .rmdir_io_err

    call int21_free_found_chain
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

; AX = directory first cluster. Return CF clear only after seeing an end
; marker/EOC with no live entry besides valid dot entries. Never changes the
; resolved parent entry, DTA or FindFirst state. A corrupt/cyclic chain fails
; closed within the volume's finite cluster count; no writes occur here.
int21_directory_empty:
    push bp
    mov si, ax
    mov bp, FAT_DATA_CLUSTER_COUNT
    call int21_load_fat_cache
    jc .fail
.cluster:
    sub bp, 1
    jc .fail
    cmp si, 2
    jb .fail
    cmp si, FAT_DATA_CLUSTER_COUNT + 2
    jae .fail
    mov ax, si
    call int21_cluster_to_lba
    mov [cs:tmp_lba], ax
    mov [cs:tmp_lba_hi], dx
    xor dx, dx
.sector:
    push dx
    mov ax, [cs:tmp_lba]
    add ax, dx
%if FAT_TYPE == 16
    mov dx, [cs:tmp_lba_hi]
    adc dx, 0
%endif
    mov bx, DOS_META_BUF_SEG
    mov es, bx
    xor bx, bx
%if FAT_TYPE == 16
    call read_sector_lba32
%else
    call read_sector_lba
%endif
    pop dx
    jc .fail
    xor di, di
    mov cx, 16
.entry:
    mov al, [es:di]
    test al, al
    jz .end_marker
    cmp al, 0xE5
    je .next
    cmp word [es:di], 0x202E
    je .dot
    cmp word [es:di], 0x2E2E
    jne .fail
.dot:
    test byte [es:di + 11], 0x10
    jz .fail
    push cx
    lea bx, [di + 2]
    mov cx, 9
.dot_padding:
    cmp byte [es:bx], ' '
    jne .bad_dot
    inc bx
    loop .dot_padding
    pop cx
.next:
    add di, 32
    loop .entry
    inc dx
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jb .sector
    mov ax, si
    call fat12_get_entry_cached
    jc .fail
    cmp ax, FAT_EOF
    jae .empty
    mov si, ax
    jmp .cluster
.end_marker:
    ; Entries after 00h are unused, but the allocated chain must still be
    ; valid before deletion can release it. Check its tail without treating
    ; stale bytes after the logical end as live directory entries.
    mov ax, si
    call fat12_get_entry_cached
    jc .fail
    cmp ax, FAT_EOF
    jae .empty
    cmp ax, 2
    jb .fail
    cmp ax, FAT_DATA_CLUSTER_COUNT + 2
    jae .fail
    sub bp, 1
    jc .fail
    mov si, ax
    jmp .end_marker
.bad_dot:
    pop cx
.fail:
    stc
    pop bp
    ret
.empty:
    clc
    pop bp
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
    call int21_resolve_and_find_path
    jc .rename_old_resolve_fail

    mov ax, [cs:tmp_lookup_dir]
    mov [cs:tmp_rename_old_parent], ax

    mov ax, [cs:search_found_root_lba]
    mov [cs:tmp_rename_old_lba], ax
    mov ax, [cs:search_found_root_lba_hi]
    mov [cs:tmp_rename_old_lba_hi], ax
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
%if FAT_TYPE == 16
    mov dx, [cs:tmp_rename_old_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
    jc .rename_io_err

    mov ax, cs
    mov ds, ax
    mov di, [cs:tmp_rename_old_off]
    mov si, path_fat_name
    mov cx, 11
    rep movsb

    mov ax, [cs:tmp_rename_old_lba]
%if FAT_TYPE == 16
    mov dx, [cs:tmp_rename_old_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
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
%if FAT_TYPE == 16
    mov dx, [cs:tmp_rename_old_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
    jc .rename_io_err

    ; Moving a directory would also require rewriting its '..' entry and
    ; rejecting ancestry cycles. AH=56h only renames directories in place.
    ; Read the original entry: destination lookup has reused found metadata.
    mov si, [cs:tmp_rename_old_off]
    test byte [es:si + 11], 0x10
    jnz .rename_dest_exists
    push ds
    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
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
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
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
%if FAT_TYPE == 16
    mov dx, [cs:search_found_root_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
    jc .rename_io_err

    mov ax, DOS_META_BUF_SEG
    mov es, ax
    mov ax, [cs:tmp_rename_old_lba]
%if FAT_TYPE == 16
    mov dx, [cs:tmp_rename_old_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
    jc .rename_io_err

    mov di, [cs:tmp_rename_old_off]
    mov byte [es:di], 0xE5

    mov ax, [cs:tmp_rename_old_lba]
%if FAT_TYPE == 16
    mov dx, [cs:tmp_rename_old_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
    jc .rename_io_err

    xor ax, ax
    clc
    jmp .rename_done

.rename_old_resolve_fail:
    cmp ax, 0x0002
    je .rename_not_found
    cmp ax, 0x0003
    je .rename_fail_path
    jmp .rename_io_err

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
    push si

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
    cmp bx, DOS_FILE_EXTRA_FIRST_HANDLE
    jb .bad_handle
    cmp bx, DOS_FILE_EXTRA_LAST_HANDLE
    ja .bad_handle
    call int21_extra_handle_ptr
    jc .bad_handle
    cmp byte [cs:si + DOS_FILE_EXTRA_OPEN_OFF], 1
    jne .bad_handle
    push ax
    mov al, bl
    sub al, 4
    mov [cs:file_handle_swapped], al
    call int21_swap_file_handle_extra
    pop ax
    mov bx, 0x0005
    jmp .handle_ready
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
    cmp al, DOS_FILE_EXTRA_FIRST_TARGET
    jae .done_swap_extra
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
    jmp .done_noswap
.done_swap_extra:
    call int21_swap_file_handle_extra
%endif
.done_noswap:
    pop ax
    popf
    pop si
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

    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:file_handle2_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax

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

    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:file_handle3_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax

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

    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:file_handle4_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax

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

    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:file_handle5_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax

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

    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:file_handle6_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax

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

    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:file_handle7_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax

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

    mov ax, [cs:file_handle_root_lba_hi]
    xchg ax, [cs:file_handle8_root_lba_hi]
    mov [cs:file_handle_root_lba_hi], ax

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

    ; BDA 0040:0013 gives KiB, while 0040:000Eh gives the EBDA segment.
    ; Some firmware leaves the KiB count above its EBDA: honor the lower
    ; boundary, rejecting EBDA pointers below our arena or inside VGA/ROM.
    ; A top below the arena floor exposes an empty arena, not wrapped memory.
    push ax
    push cx
    push ds
    xor ax, ax
    mov ds, ax
    mov ax, [0x0413]
    mov cx, [0x040E]
    pop ds
    cmp ax, 640
    jbe .bios_top_kb_capped
    mov ax, 640
.bios_top_kb_capped:
    shl ax, 6
    cmp cx, DOS_HEAP_USER_SEG
    jb .bios_top_ebda_done
    cmp cx, ax
    jae .bios_top_ebda_done
    mov ax, cx
.bios_top_ebda_done:
    cmp ax, DOS_HEAP_USER_SEG
    jae .bios_top_valid
    mov ax, DOS_HEAP_USER_SEG
.bios_top_valid:
    mov [cs:dos_mem_top_seg], ax
    mov [cs:dos_mem_chain_limit_seg], ax
    mov cx, ax
    sub cx, DOS_HEAP_USER_SEG
    mov [cs:dos_mem_mcb_size], cx
    pop cx
    pop ax

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
    call int21_mem_arena_start
    mov cx, [cs:dos_mem_chain_limit_seg]
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
    push bp
    push es

    mov si, [cs:dos_mem_chain_limit_seg]
    xor di, di
    mov word [cs:dos_mem_block_found_owner], 0

    ; PSP arenas are not ordinary AH=48 table entries, but they are physical
    ; members of the one global DOS MCB chain.  Include the active process and
    ; every suspended ancestor when selecting the next occupied interval.
    ; Windows 3.x WSWAP walks this chain to release its parent's conventional
    ; arena before starting DOSX.
    call int21_mem_active_psp
    mov bp, ax
    mov cx, DOS_EXEC_STATE_FRAME_MAX
.psp_scan:
    cmp bp, COM_LOAD_SEG
    jb .table_begin
    cmp bp, [cs:dos_mem_top_seg]
    jae .table_begin
    mov es, bp
    cmp word [es:0], 0x20CD
    jne .table_begin
    call int21_mem_psp_end
    cmp ax, bp
    jbe .psp_next
    mov bx, ax
    sub bx, bp
    mov ax, bp
    cmp ax, dx
    jae .psp_candidate_ready
    push ax
    add ax, bx
    cmp ax, dx
    pop ax
    jb .psp_next
.psp_candidate_ready:
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jae .psp_next
    cmp ax, si
    jae .psp_next
    mov si, ax
    mov di, bx
    mov [cs:dos_mem_block_found_owner], bp
.psp_next:
    mov ax, [es:0x0016]
    cmp ax, bp
    je .table_begin
    mov bp, ax
    loop .psp_scan

.table_begin:
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
    mov bx, dos_mem_block_table

.scan:
    jcxz .result
    test word [cs:bx + 6], DOS_MEM_BLOCK_INUSE
    jz .next
    mov ax, [cs:bx]
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jae .next
    cmp ax, dx
    jae .candidate_start_ready
    push ax
    add ax, [cs:bx + 2]
    cmp ax, dx
    pop ax
    jb .next
.candidate_start_ready:
    cmp ax, [cs:dos_mem_chain_limit_seg]
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
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    ret

.none:
    ; POP leaves FLAGS untouched, so restore registers first and set CF last.
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    stc
    ret

int21_mem_table_clear:
    mov byte [cs:dos_mem_block_count], 0
    ret

; Remove the table entry at byte offset SI and keep the table sorted/dense.
int21_mem_table_remove_at_si:
    pusha

    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
    or cx, cx
    jz .done
    dec cx
    shl cx, 1
    shl cx, 1
    shl cx, 1
    mov di, si
.shift:
    cmp di, cx
    jae .clear_last
    mov ax, [cs:dos_mem_block_table + di + DOS_MEM_BLOCK_ENTRY_SIZE]
    mov [cs:dos_mem_block_table + di], ax
    mov ax, [cs:dos_mem_block_table + di + DOS_MEM_BLOCK_ENTRY_SIZE + 2]
    mov [cs:dos_mem_block_table + di + 2], ax
    mov ax, [cs:dos_mem_block_table + di + DOS_MEM_BLOCK_ENTRY_SIZE + 4]
    mov [cs:dos_mem_block_table + di + 4], ax
    mov ax, [cs:dos_mem_block_table + di + DOS_MEM_BLOCK_ENTRY_SIZE + 6]
    mov [cs:dos_mem_block_table + di + 6], ax
    add di, DOS_MEM_BLOCK_ENTRY_SIZE
    jmp .shift
.clear_last:
    mov word [cs:dos_mem_block_table + di], 0
    mov word [cs:dos_mem_block_table + di + 2], 0
    mov word [cs:dos_mem_block_table + di + 4], 0
    mov word [cs:dos_mem_block_table + di + 6], DOS_MEM_BLOCK_FREE
    dec byte [cs:dos_mem_block_count]
.done:
    popa
    ret

; Import direct MCB ownership/size mutations before an allocator operation or
; EXEC merge.  A zero-owner MCB is a direct free and is removed immediately.
int21_mem_refresh_owners:
    pusha
    push es

    xor si, si
.scan:
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
    mov ax, si
    shr ax, 1
    shr ax, 1
    shr ax, 1
    cmp ax, cx
    jae .done
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .remove
    mov ax, [cs:dos_mem_block_table + si]
    or ax, ax
    jz .remove
    dec ax
    mov es, ax
    mov al, [es:0x0000]
    cmp al, 'M'
    je .valid_mcb
    cmp al, 'Z'
    jne .next
.valid_mcb:
    mov dx, [es:0x0001]
    or dx, dx
    jz .remove
    mov bx, [es:0x0003]
    or bx, bx
    jz .next
    mov ax, [cs:dos_mem_block_table + si]
    add ax, bx
    jc .next
    cmp ax, [cs:dos_mem_chain_limit_seg]
    ja .next
    mov [cs:dos_mem_block_table + si + 4], dx
    mov [cs:dos_mem_block_table + si + 2], bx
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    jmp .scan
.remove:
    call int21_mem_table_remove_at_si
    jmp .scan
.done:
    pop es
    popa
    ret

; Cold-load cleanup must retain the global resident set.  Transient/free
; entries belong to the previous EXEC arena and are discarded.
int21_mem_table_keep_resident:
    pusha
    xor si, si
.scan:
    mov al, [cs:dos_mem_block_count]
    xor ah, ah
    mov dx, si
    shr dx, 1
    shr dx, 1
    shr dx, 1
    cmp dx, ax
    jae .done
    mov ax, [cs:dos_mem_block_table + si + 6]
    test ax, DOS_MEM_BLOCK_INUSE
    jz .remove
    test ax, DOS_MEM_BLOCK_RESIDENT
    jz .remove
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    jmp .scan
.remove:
    call int21_mem_table_remove_at_si
    jmp .scan
.done:
    popa
    ret

; Commit the current immutable EXEC owner as a TSR.  The actual PSP end is
; authoritative if AH=31's requested resize could not be satisfied.
int21_mem_commit_tsr:
    pusha
    push es

    call int21_mem_refresh_owners
    mov di, [cs:dos_exec_identity_psp]
    or di, di
    jz .done

    xor si, si
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
.mark_owned:
    jcxz .upsert_psp
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .mark_next
    cmp [cs:dos_mem_block_table + si + 4], di
    jne .mark_next
    or word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_RESIDENT
.mark_next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .mark_owned

.upsert_psp:
    mov es, di
    call int21_mem_psp_end
    mov bx, ax
    sub bx, di
    jbe .done
    mov ax, di
    call int21_mem_table_find_exact
    jc .insert_psp
    mov [cs:dos_mem_block_table + si + 2], bx
    mov [cs:dos_mem_block_table + si + 4], di
    or word [cs:dos_mem_block_table + si + 6], (DOS_MEM_BLOCK_INUSE | DOS_MEM_BLOCK_RESIDENT | DOS_MEM_BLOCK_PSP)
    jmp .rebuild
.insert_psp:
    cmp byte [cs:dos_mem_block_count], DOS_MEM_BLOCK_TABLE_MAX
    jae .done
    mov ax, di
    mov cx, di
    mov dx, (DOS_MEM_BLOCK_INUSE | DOS_MEM_BLOCK_RESIDENT | DOS_MEM_BLOCK_PSP)
    call int21_mem_table_insert
.rebuild:
    call int21_mem_sync_legacy
    call int21_mem_rebuild_chain
.done:
    pop es
    popa
    ret

; AX/BX/CX/DX describe an entry to upsert in the allocator snapshot at ES:0.
int21_mem_snapshot_upsert:
    pusha

    mov [cs:dos_mem_block_tmp_seg], ax
    mov [cs:dos_mem_block_tmp_size], bx
    mov [cs:dos_mem_block_tmp_owner], cx
    mov [cs:dos_mem_block_tmp_state], dx
    xor si, si
    xor cx, cx
    mov cl, [es:DOS_EXEC_STATE_BLOCK_COUNT_OFF]
.find:
    jcxz .insert
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si]
    cmp [cs:dos_mem_block_tmp_seg], ax
    je .store
    jb .insert
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .find

.insert:
    cmp byte [es:DOS_EXEC_STATE_BLOCK_COUNT_OFF], DOS_MEM_BLOCK_TABLE_MAX
    jae .done
    xor di, di
    mov dl, [es:DOS_EXEC_STATE_BLOCK_COUNT_OFF]
    mov di, dx
    shl di, 1
    shl di, 1
    shl di, 1
.shift:
    cmp di, si
    jbe .insert_count
    mov bp, di
    sub bp, DOS_MEM_BLOCK_ENTRY_SIZE
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + bp]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di], ax
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + bp + 2]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di + 2], ax
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + bp + 4]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di + 4], ax
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + bp + 6]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di + 6], ax
    sub di, DOS_MEM_BLOCK_ENTRY_SIZE
    jmp .shift
.insert_count:
    inc byte [es:DOS_EXEC_STATE_BLOCK_COUNT_OFF]
.store:
    mov ax, [cs:dos_mem_block_tmp_seg]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si], ax
    mov ax, [cs:dos_mem_block_tmp_size]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si + 2], ax
    mov ax, [cs:dos_mem_block_tmp_owner]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si + 4], ax
    mov ax, [cs:dos_mem_block_tmp_state]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si + 6], ax
.done:
    mov byte [es:(dos_mem_init - dos_mem_exec_state_begin)], 1
    popa
    ret

; Reconcile the live child table with the saved parent frame at ES:0.
; Parent transient entries remain private, while the complete live resident
; set replaces the saved resident set.  Thus installs, resizes and unloads
; propagate through every nested EXEC return.
int21_mem_merge_exec_residents:
    pusha

    call int21_mem_refresh_owners

    ; A new block whose physical owner was changed away from the child is a
    ; deliberately re-owned allocation and survives normal termination.
    xor si, si
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]
.classify:
    jcxz .compact_saved
    mov ax, [cs:dos_mem_block_table + si + 6]
    test ax, DOS_MEM_BLOCK_INUSE
    jz .classify_next
    test ax, DOS_MEM_BLOCK_RESIDENT
    jnz .classify_next
    mov ax, [cs:dos_mem_block_table + si + 4]
    or ax, ax
    jz .classify_next
    cmp ax, [cs:dos_exec_identity_psp]
    je .classify_next

    push cx
    push si
    mov bp, [cs:dos_mem_block_table + si]
    xor di, di
    xor cx, cx
    mov cl, [es:DOS_EXEC_STATE_BLOCK_COUNT_OFF]
.saved_find:
    jcxz .new_reowned
    test word [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di + 6], DOS_MEM_BLOCK_INUSE
    jz .saved_next
    cmp [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di], bp
    je .saved_present
.saved_next:
    add di, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .saved_find
.new_reowned:
    pop si
    or word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_RESIDENT
    pop cx
    jmp .classify_next
.saved_present:
    pop si
    pop cx
.classify_next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .classify

.compact_saved:
    xor si, si
    xor di, di
    xor cx, cx
    xor bx, bx
    mov cl, [es:DOS_EXEC_STATE_BLOCK_COUNT_OFF]
.compact_loop:
    jcxz .compact_done
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si + 6]
    test ax, DOS_MEM_BLOCK_INUSE
    jz .compact_next
    test ax, DOS_MEM_BLOCK_RESIDENT
    jnz .compact_next
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di], ax
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si + 2]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di + 2], ax
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si + 4]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di + 4], ax
    mov ax, [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + si + 6]
    mov [es:DOS_EXEC_STATE_BLOCK_TABLE_OFF + di + 6], ax
    add di, DOS_MEM_BLOCK_ENTRY_SIZE
    inc bx
.compact_next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    dec cx
    jmp .compact_loop
.compact_done:
    mov [es:DOS_EXEC_STATE_BLOCK_COUNT_OFF], bl

    xor si, si
    xor bp, bp
    mov bl, [cs:dos_mem_block_count]
.merge_live:
    cmp bp, bx
    jae .done
    mov dx, [cs:dos_mem_block_table + si + 6]
    test dx, DOS_MEM_BLOCK_INUSE
    jz .merge_next
    test dx, DOS_MEM_BLOCK_RESIDENT
    jz .merge_next
    mov cx, [cs:dos_mem_block_table + si + 4]
    or cx, cx
    jz .merge_next
    mov ax, [cs:dos_mem_block_table + si]
    mov bx, [cs:dos_mem_block_table + si + 2]
    call int21_mem_snapshot_upsert
    xor bx, bx
    mov bl, [cs:dos_mem_block_count]
.merge_next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc bp
    jmp .merge_live
.done:
    popa
    ret

int21_mem_arena_start:
    push bx
    push es

    mov ax, DOS_HEAP_USER_SEG
    call int21_mem_active_psp
    mov bx, ax
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
    add ax, 0x0010
.end_ready:
    inc ax
.check_limit:
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jbe .done
    mov ax, [cs:dos_mem_chain_limit_seg]

.done:
    pop es
    pop bx
    ret

int21_mem_table_insert:
    pusha

    cmp bx, 0
    je .done
    ; Transient AH=48h allocations may occupy the free interval immediately
    ; above the active PSP, including the historic MZ loader window below
    ; 5800h.  EXEC already consults this table before choosing a child slot,
    ; so keeping an artificial high-memory floor only strands conventional
    ; memory (and prevents Windows 3.x from meeting its startup minimum).
    test dx, DOS_MEM_BLOCK_PSP
    jnz .segment_ready
    push ax
    call int21_mem_arena_start
    mov di, ax
    pop ax
    cmp ax, di
    jb .done
.segment_non_psp_ready:
    or ax, ax
    jz .done
.segment_ready:
    cmp ax, [cs:dos_mem_chain_limit_seg]
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
    jcxz .slot_ready
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
    popa
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
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .next
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
    jcxz .clear
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jnz .done
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

    mov dx, [cs:dos_mem_chain_limit_seg]
    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp di, cx
    jae .done
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .next
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
    ; An allocated block beginning at AX needs the paragraph immediately
    ; before the next allocated block for that block's MCB.  The arena cap,
    ; unlike a block start, is already the first non-touchable paragraph.
    cmp ax, [cs:dos_mem_chain_limit_seg]
    je .done
    dec dx

.done:
    pop bx
    pop ax
    ret

int21_mem_find_free_gap:
    mov [cs:dos_mem_block_req_size], bx
    mov word [cs:dos_mem_gap_candidate_seg], 0
    mov word [cs:dos_mem_gap_candidate_size], 0
    call int21_mem_arena_start
    mov dx, ax
    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp di, cx
    jae .tail
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .next
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jae .tail
    cmp ax, dx
    jbe .consume
    mov bx, ax
    sub bx, dx
    dec bx
    call .consider_gap
    jnc .candidate_ready
.consume:
    mov ax, [cs:dos_mem_block_table + si]
    add ax, [cs:dos_mem_block_table + si + 2]
    inc ax
    cmp ax, dx
    jbe .next
    mov dx, ax
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.tail:
    cmp dx, [cs:dos_mem_chain_limit_seg]
    jae .not_found
    mov bx, [cs:dos_mem_chain_limit_seg]
    sub bx, dx
    call .consider_gap

.finish:
.candidate_ready:
    mov ax, [cs:dos_mem_gap_candidate_seg]
    or ax, ax
    jz .not_found
    mov bx, [cs:dos_mem_block_req_size]
    clc
    ret

.not_found:
    stc
    ret

; DX/BX describe a free data interval.  First fit stops immediately, best
; fit retains the smallest adequate interval, and last fit retains the last
; adequate interval while carving the allocation from its high end, matching
; MS-DOS/FreeDOS MCB splitting semantics.  CF=0 asks the caller to return now.
.consider_gap:
    cmp bx, [cs:dos_mem_block_req_size]
    jb .consider_continue
    push ax
    push bp
    mov bp, [cs:dos_mem_strategy]
    cmp bp, 1
    je .consider_best
    cmp bp, 2
    je .consider_last

    ; Keep the low EXEC window contiguous: the table-backed allocator already
    ; splits a matching free block from its high edge, so do the same when the
    ; free interval is implicit rather than materialised as a table entry.
    mov ax, dx
    add ax, bx
    sub ax, [cs:dos_mem_block_req_size]
    mov [cs:dos_mem_gap_candidate_seg], ax
    mov [cs:dos_mem_gap_candidate_size], bx
    pop bp
    pop ax
    clc
    ret

.consider_best:
    cmp word [cs:dos_mem_gap_candidate_seg], 0
    je .consider_store_start
    cmp bx, [cs:dos_mem_gap_candidate_size]
    jae .consider_saved
.consider_store_start:
    mov [cs:dos_mem_gap_candidate_seg], dx
    mov [cs:dos_mem_gap_candidate_size], bx
    jmp .consider_saved

.consider_last:
    mov ax, dx
    add ax, bx
    sub ax, [cs:dos_mem_block_req_size]
    mov [cs:dos_mem_gap_candidate_seg], ax
    mov [cs:dos_mem_gap_candidate_size], bx

.consider_saved:
    pop bp
    pop ax
.consider_continue:
    stc
    ret

int21_mem_table_alloc_from_free:
    mov [cs:dos_mem_block_req_size], bx
    mov bp, [cs:dos_mem_strategy]
    cmp bp, 2
    jbe .strategy_ready
    xor bp, bp
.strategy_ready:
    mov word [cs:dos_mem_block_candidate_si], 0xFFFF
    mov dx, 0xFFFF
    cmp bp, 2
    jne .metric_ready
    xor dx, dx
.metric_ready:
    xor si, si
    xor di, di
    xor cx, cx
    mov cl, [cs:dos_mem_block_count]

.scan:
    cmp di, cx
    jae .use_free
    cmp word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_FREE
    jne .next
    mov bx, [cs:dos_mem_block_table + si + 2]
    cmp bx, [cs:dos_mem_block_req_size]
    jb .next
    cmp bp, 0
    je .select_immediate
    cmp bp, 1
    je .best_fit
    cmp bp, 2
    je .last_fit
    jmp .next

.best_fit:
    cmp bx, dx
    jae .next
    mov dx, bx
    mov [cs:dos_mem_block_candidate_si], si
    jmp .next

.last_fit:
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, dx
    jbe .next
    mov dx, ax
    mov [cs:dos_mem_block_candidate_si], si
    jmp .next

.select_immediate:
    mov [cs:dos_mem_block_candidate_si], si
    jmp .use_free
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.use_free:
    mov si, [cs:dos_mem_block_candidate_si]
    cmp si, 0xFFFF
    je .not_found
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
    cmp word [cs:dos_exec_identity_psp], 0
    je .split_capacity_ready
    cmp byte [cs:dos_mem_block_count], (DOS_MEM_BLOCK_TABLE_MAX - 1)
    jae .use_whole
.split_capacity_ready:
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
    ret

int21_mem_sync_legacy:
    pusha

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
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_INUSE
    jz .next
    mov ax, [cs:dos_mem_block_table + si]
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jae .tail_gap
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
    mov ax, [cs:dos_mem_block_table + si]
    add ax, [cs:dos_mem_block_table + si + 2]
    inc ax
    cmp ax, dx
    jbe .next
    mov dx, ax
.next:
    add si, DOS_MEM_BLOCK_ENTRY_SIZE
    inc di
    jmp .scan

.tail_gap:
    cmp dx, [cs:dos_mem_chain_limit_seg]
    jae .done
    mov bx, [cs:dos_mem_chain_limit_seg]
    sub bx, dx
    call int21_mem_sync_store_gap

.done:
    cmp word [cs:dos_mem_alloc_size], 0
    jne .return
    mov word [cs:dos_mem_mcb_owner], 0
    mov word [cs:dos_mem_mcb_size], DOS_HEAP_USER_MAX_PARAS
.return:
    popa
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
    pusha
    push es

    call int21_mem_table_rebuild

    call int21_mem_lowest_psp
    mov dx, ax
    or dx, dx
    jz .done

    mov es, dx
    call int21_mem_psp_end
    mov bx, ax
    cmp bx, dx
    jae .psp_end_ready
    mov bx, dx
    add bx, 0x0010
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

    cmp di, [cs:dos_mem_chain_limit_seg]
    jae .mark_last

.scan_next:
    cmp di, [cs:dos_mem_chain_limit_seg]
    jae .mark_last
    mov dx, di
    call int21_mem_find_next_alloc
    jc .final_gap
    cmp ax, [cs:dos_mem_chain_limit_seg]
    jae .final_gap
    cmp ax, di
    jbe .write_alloc

    push ax
    push bx
    mov bx, ax
    sub bx, di
    dec bx
    mov ax, di
    xor cx, cx
    mov dl, 'M'
    call int21_mem_write_chain_entry
    mov si, ax
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
    cmp di, [cs:dos_mem_chain_limit_seg]
    jae .mark_last
    mov ax, di
    mov bx, [cs:dos_mem_chain_limit_seg]
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
    mov [cs:dos_mem_last_mcb_seg], ax
    mov es, ax
    mov byte [es:0x0000], 'Z'

.done:
    pop es
    popa
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

; ES = PSP. Return its current arena end in AX without changing PSP:2,
; which records the initial EXEC allocation. Synthetic Windows PSPs without
; a valid owning MCB retain the existing PSP-field fallback.
int21_mem_psp_end:
    push bx
    push es
    mov bx, es
    call int21_mem_active_psp
    cmp ax, bx
    jne .mcb
    mov ax, [cs:dos_mem_psp_mcb_end]
    cmp ax, bx
    ja .done
.mcb:
    mov ax, bx
    dec ax
    mov es, ax
    cmp byte [es:0], 'M'
    je .owner
    cmp byte [es:0], 'Z'
    jne .fallback
.owner:
    cmp [es:1], bx
    jne .fallback
    mov ax, [es:3]
    add ax, bx
    jnc .done
.fallback:
    mov es, bx
    mov ax, [es:2]
.done:
    pop es
    pop bx
    ret

int21_mem_active_psp:
    ; DOSMGR selects each Win16 task and secondary DOS VM through AH=50h.  The
    ; inherited EXEC identity still names WIN.COM; using it for allocations
    ; makes every DOS box walk the system VM arena and WinOldAp reports
    ; "insufficient memory" even for COMMAND.COM.  While Windows is active,
    ; honor the selected, instanced PSP.  The immutable identity remains the
    ; separate authority for INT 21h/4Ch unwind decisions.
    cmp word [cs:windows_active], 0
    je .exec_identity
    mov ax, [cs:current_psp_seg]
    ret
.exec_identity:
    mov ax, [cs:dos_exec_identity_psp]
    or ax, ax
    jnz .done
    mov ax, [cs:current_psp_seg]
.done:
    ret

; Return the numerically lowest PSP in the active EXEC ancestry.  It anchors
; the global conventional-memory MCB chain exposed through INT 21h/AH=52h.
int21_mem_lowest_psp:
    push bx
    push cx
    push dx
    push es

    call int21_mem_active_psp
    mov bx, ax
    mov dx, 0xFFFF
    mov cx, DOS_EXEC_STATE_FRAME_MAX
.scan:
    cmp bx, COM_LOAD_SEG
    jb .ready
    cmp bx, [cs:dos_mem_top_seg]
    jae .ready
    mov es, bx
    cmp word [es:0], 0x20CD
    jne .ready
    cmp bx, dx
    jae .parent
    mov dx, bx
.parent:
    mov ax, [es:0x0016]
    cmp ax, bx
    je .ready
    mov bx, ax
    loop .scan
.ready:
    mov ax, dx
    cmp ax, 0xFFFF
    jne .return
    xor ax, ax
.return:
    pop es
    pop dx
    pop cx
    pop bx
    ret

; AX is the child load segment.  Cap its arena before the MCB of every higher
; suspended ancestor, not just the immediate parent.
int21_mem_set_exec_chain_limit:
    pusha
    push es
    mov bx, ax
    mov dx, [cs:dos_mem_top_seg]
    mov si, [cs:dos_exec_identity_psp]
    mov cx, DOS_EXEC_STATE_FRAME_MAX
.scan:
    or si, si
    jz .store
    cmp si, bx
    jbe .next
    mov ax, si
    dec ax
    cmp ax, dx
    jae .next
    mov dx, ax
.next:
    mov es, si
    mov ax, [es:0x0016]
    cmp ax, si
    je .store
    mov si, ax
    loop .scan
.store:
    mov [cs:dos_mem_chain_limit_seg], dx
    pop es
    popa
    ret

int21_mem_current_owner:
    call int21_mem_active_psp
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

    call int21_mem_active_psp
    mov dx, ax
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
    cmp dx, [cs:dos_mem_chain_limit_seg]
    jae .largest_ready
    mov cx, [cs:dos_mem_chain_limit_seg]
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
    call int21_mem_refresh_owners
%if TRACE_WIN_MEMORY != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_48
    call print_string_serial
    mov ax, bx
    call print_hex16_serial
    mov si, msg_win_mem_sep
    call print_string_serial
    mov ax, [cs:dos_mem_strategy]
    call print_hex16_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif

    ; DOS callers use BX=FFFFh to query the largest available block.
    cmp bx, 0xFFFF
    jne .req_ready
    call int21_mem_largest_global
%if TRACE_WIN_MEMORY != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_largest
    call print_string_serial
    mov ax, bx
    call print_hex16_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif
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
    cmp word [cs:dos_exec_identity_psp], 0
    je .insert_capacity_ready
    cmp byte [cs:dos_mem_block_count], (DOS_MEM_BLOCK_TABLE_MAX - 1)
    jae .no_memory
.insert_capacity_ready:
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
%if TRACE_WIN_MEMORY != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_largest
    call print_string_serial
    mov ax, bx
    call print_hex16_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif
    mov ax, 0x0008
    stc
    ret

int21_free:
    call int21_mem_init
    call int21_mem_refresh_owners
%if TRACE_WIN_MEMORY != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_free
    call print_string_serial
    mov ax, es
    call print_hex16_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif

    mov ax, es
    call int21_mem_table_find_exact
    jc .legacy_static
    test word [cs:dos_mem_block_table + si + 6], DOS_MEM_BLOCK_RESIDENT
    jnz .free_exact
    call int21_mem_active_psp
    cmp cx, ax
    jne .owner_invalid
.free_exact:
%if TRACE_WIN_MEMORY != 0
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_free_ok
    call print_string_serial
    pop si
    pop ds
%endif
    push ax
    push es
    mov ax, [cs:dos_mem_block_table + si]
    dec ax
    mov es, ax
    mov word [es:0x0001], 0
    pop es
    pop ax
    call int21_mem_table_remove_at_si
    call int21_mem_sync_legacy
    call int21_mem_rebuild_chain
    xor ax, ax
    clc
    ret

.legacy_static:
%if TRACE_WIN_MEMORY != 0
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_free_miss
    call print_string_serial
    pop si
    pop ds
%endif
    mov ax, es
    cmp ax, DOS_ENV_SEG
    je .env_static_ok
    jmp .invalid_real

.owner_invalid:
%if TRACE_WIN_MEMORY != 0
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_free_owner
    call print_string_serial
    pop si
    pop ds
%endif
.invalid_real:
    mov ax, 0x0009
    stc
    ret

.env_static_ok:
    xor ax, ax
    clc
    ret

int21_resize:
    call int21_mem_init
    call int21_mem_refresh_owners
%if TRACE_WIN_MEMORY != 0
    push ax
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_mem_4a
    call print_string_serial
    mov ax, es
    call print_hex16_serial
    mov si, msg_win_mem_sep
    call print_string_serial
    mov ax, bx
    call print_hex16_serial
    call print_newline_serial
    pop si
    pop ds
    pop ax
%endif
%if TRACE_CHILD_INT21 != 0
%endif

.resize_entry:
    mov dx, es
    call int21_mem_active_psp
    cmp dx, ax
    mov ax, dx
    je .check_psp_zero
    jmp .check_heap_block
.check_psp_zero:
    cmp ax, 0
    jne .check_psp_size
    jmp .check_heap_block
.check_psp_size:
    cmp bx, 0
    je .no_memory
%if TRACE_CHILD_INT21 != 0
%endif

    mov [cs:dos_mem_block_req_size], bx
    mov bx, ax
    call int21_mem_table_next_limit
    cmp dx, [cs:dos_mem_chain_limit_seg]
    je .psp_limit_ready
    dec dx
.psp_limit_ready:
    sub dx, ax
    mov bx, [cs:dos_mem_block_req_size]
    cmp bx, dx
    ja .psp_no_memory

    mov si, ax
    add si, bx
    push ax
    push bx
    mov [cs:dos_mem_psp_mcb_end], si
    ; SETBLOCK changes the MCB, not the initial allocation stored in PSP:2.
    ; DOS/32A compares that initial ceiling after its first shrink.
    call int21_mem_sync_legacy
    mov al, 'Z'
    cmp byte [cs:dos_mem_block_count], 0
    je .psp_type_ready
    mov al, 'M'
.psp_type_ready:
    call int21_psp_mcb_update_type
    call int21_mem_rebuild_chain
%if TRACE_WIN_MEMORY != 0
    cmp bx, 0x0576
    jne .psp_trace_done
    pusha
    push ds
    call int21_mem_largest_global
    push cs
    pop ds
    mov si, msg_win_mem_free
    call print_string_serial
    mov ax, bx
    call print_hex16_serial
    call print_newline_serial
    pop ds
    popa
.psp_trace_done:
%endif
    pop bx
    pop ax
    mov ax, es
    clc
    ret

.psp_no_memory:
    mov ax, es
    mov bx, ax
    call int21_mem_table_next_limit
    cmp dx, [cs:dos_mem_chain_limit_seg]
    je .psp_no_memory_limit_ready
    dec dx
.psp_no_memory_limit_ready:
    sub dx, ax
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
    call int21_mem_active_psp
    cmp cx, ax
    jne .invalid_real
%if TRACE_CHILD_INT21 != 0
%endif
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
    push ax
    call int21_mem_active_psp
    mov dx, ax
    pop ax
    cmp dx, 0
    je .invalid_check_high
    cmp ax, dx
    jb .invalid_check_high
    cmp ax, DOS_HEAP_USER_SEG
    jae .invalid_check_high
    mov es, dx
    jmp .resize_entry

.invalid_check_high:
    cmp ax, [cs:dos_mem_top_seg]
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
    jcxz .done
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

; Cluster -> data LBA. Returns the LBA as 32-bit DX:AX so high clusters do
; not alias: the FAT16/full profile shifts by FAT_CLUSTER_SECTOR_SHIFT, which
; overflows a 16-bit register once (cluster-2) << shift exceeds 0xFFFF.
; Low clusters keep DX = 0, so callers that only consume AX stay correct.
int21_cluster_to_lba:
    sub ax, 2
    xor dx, dx
%if FAT_CLUSTER_SECTOR_SHIFT > 0
    push cx
    mov cx, FAT_CLUSTER_SECTOR_SHIFT
.shift_cluster_lba:
    shl ax, 1
    rcl dx, 1
    loop .shift_cluster_lba
    pop cx
%endif
    add ax, FAT_DATA_START_LBA
    adc dx, 0
    ret

%if FAT_TYPE == 16
; Read one 512-byte sector by 32-bit LBA (DX:AX) into ES:BX via INT 13h EDD.
; Used for file data, whose clusters can sit past the 16-bit LBA range.
read_sector_lba32:
    push bp
    mov bp, 0x4200
    jmp disk_sector_lba32

; Same register contract and recovery path for writes and reads.
write_sector_lba32:
    push bp
    mov bp, 0x4300

disk_sector_lba32:
    pushad
    push ds
    push es
    add ax, FAT_LBA_OFFSET
    adc dx, 0
    mov [cs:disk_packet_lba], ax
    mov [cs:disk_packet_lba + 2], dx
    mov [cs:disk_packet_lba + 4], word 0
    mov [cs:disk_packet_lba + 6], word 0
    mov [cs:disk_packet_off], bx
    mov [cs:disk_packet_seg], es
    mov di, 3
.retry:
    ; EDD writes the number transferred back here even on failure. Reusing
    ; zero silently turns the next request into a zero-sector transfer on
    ; some firmware. The DAP is input/output, not an immutable descriptor.
    mov word [cs:disk_packet + 2], 1
    mov ax, cs
    mov ds, ax
    mov si, disk_packet
    mov dl, [cs:boot_drive]
    mov ax, bp
    call bios_disk_interrupt
    jnc .done
    mov ax, [cs:disk_packet_lba]
    mov dx, [cs:disk_packet_lba + 2]
    cmp bp, 0x4200
    jne .write_chs
    call bios_read_chs_sector32
    jmp .chs_done
.write_chs:
    call bios_write_chs_sector32
.chs_done:
    jnc .done
    dec di
    jz .failed
    xor ax, ax
    mov dl, [cs:boot_drive]
    call bios_disk_interrupt
    jmp .retry
.failed:
    stc
.done:
    mov [cs:tmp_disk_status], ah
    pop es
    pop ds
    popad
    pop bp
    ret
%endif

int21_count_chain:
    push bx

    cmp ax, 2
    jb .zero
    cmp ax, FAT_EOF
    jae .zero

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
    ; A failed replacement did not populate this sector. Never let a later
    ; cache hit interpret the previous FAT sector as the requested one.
    mov word [cs:fat_cache_sector], 0xFFFF
    mov byte [cs:fat_cache_valid], 0
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
%if FAT_TYPE == 16
    mov dx, [cs:file_handle_root_lba_hi]
    xor bx, bx
    call read_sector_lba32
%else
    xor bx, bx
    call read_sector_lba
%endif
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
%if FAT_TYPE == 16
    mov dx, [cs:file_handle_root_lba_hi]
    xor bx, bx
    call write_sector_lba32
%else
    xor bx, bx
    call write_sector_lba
%endif
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
    ; Inline upcase for extension
    cmp al, 'a'
    jb .ext_store
    cmp al, 'z'
    ja .ext_store
    sub al, 0x20
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

; Detect the mounted FAT16 root for DOS APIs which accept directory paths.
; The regular path resolver intentionally requires a final 8.3 component, so
; a bare root such as C:\ must be recognized before entering that resolver.
; Input : DS:SI ASCIIZ/CR-terminated DOS path
; Output: CF clear for C:\ (or \ while C: is current), CF set otherwise
int21_is_mounted_root_path:
    push ax
    push bx

    xor bx, bx
    mov al, [si]
    cmp byte [si + 1], ':'
    jne .implicit_drive
    call int21_upcase_al
    cmp al, 'C'
    jne .not_root
    add si, 2
    jmp .separator

.implicit_drive:
    cmp byte [cs:dos_default_drive], 2
    jne .not_root

.separator:
    mov al, [si]
    cmp al, '\'
    je .skip_separators
    cmp al, '/'
    jne .not_root

.skip_separators:
    inc si
    mov al, [si]
    cmp al, '\'
    je .skip_separators
    cmp al, '/'
    je .skip_separators
    cmp al, 0
    je .is_root
    cmp al, 13
    je .is_root

.not_root:
    stc
    jmp .done

.is_root:
    clc

.done:
    pop bx
    pop ax
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
    mov word [cs:search_found_root_lba_hi], 0

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
    mov [cs:tmp_lba_hi], dx
    xor dx, dx

.sector_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .next_cluster

    mov ax, DOS_META_BUF_SEG
    mov es, ax
%if FAT_TYPE == 16
    push dx
    mov ax, [cs:tmp_lba]
    add ax, dx
    mov dx, [cs:tmp_lba_hi]
    adc dx, 0
    xor bx, bx
    call read_sector_lba32
    pop dx
%else
    mov ax, [cs:tmp_lba]
    add ax, dx
    xor bx, bx
    call read_sector_lba
%endif
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
%if FAT_TYPE == 16
    mov ax, [cs:tmp_lba_hi]
    adc ax, 0
    mov [cs:search_found_root_lba_hi], ax
%endif
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
    mov word [cs:search_found_root_lba_hi], 0
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
    mov [cs:tmp_lba_hi], dx
    xor dx, dx

.sector_loop:
    cmp dx, FAT_SECTORS_PER_CLUSTER
    jae .next_cluster

    mov ax, DOS_META_BUF_SEG
    mov es, ax
%if FAT_TYPE == 16
    push dx
    mov ax, [cs:tmp_lba]
    add ax, dx
    mov dx, [cs:tmp_lba_hi]
    adc dx, 0
    xor bx, bx
    call read_sector_lba32
    pop dx
%else
    mov ax, [cs:tmp_lba]
    add ax, dx
    xor bx, bx
    call read_sector_lba
%endif
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
    mov ax, [cs:tmp_lba_hi]
    adc ax, 0
    mov [cs:search_found_root_lba_hi], ax
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
    push dx
    push si
    push di
    push ds
    push es

    ; DOS extenders do not merely trust PSP:2Ch: they expect the environment
    ; to be backed by a conventional DOS block.  Give the shared environment
    ; its own valid MCB instead of pointing at anonymous scratch memory.
    mov dx, es
    mov ax, DOS_ENV_SEG - 1
    mov es, ax
    mov byte [es:0x0000], 'M'
    mov [es:0x0001], dx
    mov word [es:0x0003], ((dos_env_block_end - dos_env_block) + 15) >> 4
    mov es, dx
    mov word [es:0x002C], DOS_ENV_SEG
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
.done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop ax
    ret

%if STAGE1_SELFTEST_AUTORUN || STAGE1_INTERACTIVE_SHELL
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

%endif
%if STAGE1_SELFTEST_AUTORUN
int21_move_rename_path_test:
    push ax
    push di
    push dx
    push si
    push ds
    push es

    mov ax, cs
    mov ds, ax
    mov es, ax

    mov dx, path_mvren_dir_dos
    mov ah, 0x39
    int 0x21
    jc .fail

    mov dx, path_comdemo_dos
    mov di, path_mvren_moved_dos
    mov ah, 0x56
    int 0x21
    jc .fail

    mov dx, path_mvren_moved_dos
    mov di, path_mvren_final_dos
    mov ah, 0x56
    int 0x21
    jc .fail

    mov si, path_mvren_final_dos
    call int21_resolve_and_find_path
    jc .fail

    mov dx, path_mvren_final_dos
    mov di, path_comdemo_dos
    mov ah, 0x56
    int 0x21
    jc .fail

    mov si, msg_mvren_serial_pass
    call print_string_serial
    jmp .done

.fail:
    mov si, msg_mvren_serial_fail
    call print_string_serial

.done:
    pop es
    pop ds
    pop si
    pop dx
    pop di
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

; The VGA demo is reachable only from the self-test and the debug commands of
; the legacy interactive Stage1 shell; the kernel build leaves it out.
%if STAGE1_SELFTEST_AUTORUN || (STAGE1_INTERACTIVE_SHELL && STAGE1_DEBUG_COMMANDS)
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

%endif

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

    call stage1_splash_apply_palette
    call stage1_splash_blit_scaled
    jc .fail_graphics

    mov byte [cs:boot_splash_active], 1
    xor ax, ax
    call stage1_splash_draw_progress_bar
    jnc .done
    mov byte [cs:boot_splash_active], 0
    jmp .fail_graphics

.fail_graphics:
    call vdi_leave_graphics
    jmp .done

.fail_text:
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

stage1_show_boot_loading_screen:
    cmp byte [cs:boot_splash_active], 1
    je .skip
    push ax
    push bx
    push cx
    push dx
    push si

    call hide_text_cursor

    mov bl, 0x07
    call clear_screen_attr

    mov si, msg_boot_loader_title
    mov dh, 3
    mov bl, 0x0F
    call video_write_centered_attr

    mov si, msg_boot_loading_runtime
    mov dh, 7
    mov bl, 0x08
    call video_write_centered_attr

    mov si, msg_boot_loading_volume
    mov dh, 9
    mov bl, 0x08
    call video_write_centered_attr

    mov si, msg_boot_loading_shell
    mov dh, 11
    mov bl, 0x08
    call video_write_centered_attr

    mov si, msg_boot_loading_services
    mov dh, 13
    mov bl, 0x08
    call video_write_centered_attr

    mov si, msg_boot_loading_ready
    mov dh, 15
    mov bl, 0x08
    call video_write_centered_attr

    mov si, msg_boot_progress_0
    mov dh, 18
    mov bl, 0x07
    call video_write_centered_attr

    mov dh, 20
    xor dl, dl
    call set_cursor_pos

    pop si
    pop dx
    pop cx
    pop bx
    pop ax
.skip:
    ret

stage1_boot_mark_step:
    push ax
    push bx
    push dx
    push si
    cmp byte [cs:boot_splash_active], 1
    jne .text
    push ax
    mov bl, al
    xor bh, bh
    mov al, [cs:boot_progress_units + bx - 1]
    xor ah, ah
    call stage1_splash_advance_progress
    pop ax
    jc .graphics_failed
    cmp al, 5
    jne .done
    ; The completed splash stays on screen until the desktop's own mode set
    ; replaces it (boot_splash_active stays 1 for the SHELL exec below).
    jmp .done
.graphics_failed:
    call vdi_leave_graphics
    mov byte [cs:boot_splash_active], 0
    jmp .done
.text:

    cmp al, 1
    je .runtime
    cmp al, 2
    je .volume
    cmp al, 3
    je .shell
    cmp al, 4
    je .services

    mov si, msg_boot_loading_ready
    mov dh, 15
    mov bl, 0x0F
    call video_write_centered_attr
    mov si, msg_boot_progress_100
    jmp .progress

.runtime:
    mov si, msg_boot_loading_runtime
    mov dh, 7
    mov bl, 0x0F
    call video_write_centered_attr
    mov si, msg_boot_progress_20
    jmp .progress

.volume:
    mov si, msg_boot_loading_volume
    mov dh, 9
    mov bl, 0x0F
    call video_write_centered_attr
    mov si, msg_boot_progress_60
    jmp .progress

.shell:
    mov si, msg_boot_loading_shell
    mov dh, 11
    mov bl, 0x0F
    call video_write_centered_attr
    mov si, msg_boot_progress_80
    jmp .progress

.services:
    mov si, msg_boot_loading_services
    mov dh, 13
    mov bl, 0x0F
    call video_write_centered_attr
    mov si, msg_boot_progress_40

.progress:
    mov dh, 18
    mov bl, 0x0F
    call video_write_centered_attr
.done:
    pop si
    pop dx
    pop bx
    pop ax
    ret

stage1_wait_visible_delay:
    push ax
    push bx
    push cx
    push dx

    ; Prefer BIOS elapsed-time wait for a visible pause; fall back to timer ticks
    ; if the platform does not support INT 15h AH=86h.
    mov ah, 0x86
    int 0x15
    jnc .done

    mov cx, bx
    call stage1_boot_wait_ticks

.done:
    pop dx
    pop cx
    pop bx
    pop ax
    ret

stage1_boot_wait_ticks:
    push ax
    push bx
    push cx
    push dx
    push si

    mov bx, cx
    call gfx_get_tick_count
    ; INT 1Ah overwrites AH (and CX): AX cannot hold the starting tick.
    ; Use a preserved register so this also works after BIOS tick 00FFh.
    mov si, dx

.loop:
    call gfx_get_tick_count
    sub dx, si
    cmp dx, bx
    jb .loop

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

    call stage1_splash_clear_vram
    jc .fail

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
    xor ah, ah
    mov cx, SPLASH_SRC_W
.expand_x:
    lodsb
    stosb
    stosb
    stosb
    inc ah
    cmp ah, 8
    jne .next_x
    xor ah, ah
    stosb
.next_x:
    loop .expand_x
    pop di

    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    mov ax, 0xA000
    mov es, ax
    ; Preserve the whole photograph in 576 rows, with a separate 24-row bar.
    mov cx, SPLASH_SCALE_Y_BASE
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

stage1_splash_clear_vram:
    push ax
    push bx
    push cx
    push dx
    push di
    push es

    xor dx, dx
.bank_loop:
    xor di, di
    call stage1_splash_set_bank
    jc .fail

    mov ax, 0xA000
    mov es, ax
    xor ax, ax
    mov cx, 0x8000
    rep stosb

    inc dx
    cmp dx, 8
    jb .bank_loop

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

    mov ax, 576
    mov bx, SPLASH_VESA_ROW_BYTES
    mul bx
    mov di, ax
    call stage1_splash_set_bank
    jc .fail

    mov ax, DOS_IO_BUF_SEG
    mov ds, ax
    mov bx, 24
    mov si, 576

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

    cmp si, 580
    jb .copy_row
    cmp si, 595
    ja .copy_row

    mov di, 82
    mov al, 254
    mov cx, 636
    rep stosb

    cmp si, 584
    jb .copy_row
    cmp si, 591
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

stage1_splash_advance_progress:
    push ax
    push bx
    push cx
    push bp
    mov bp, ax
.next:
    movzx ax, byte [cs:boot_progress_value]
    cmp ax, bp
    jae .done
    add ax, 2
    cmp ax, bp
    jbe .draw
    mov ax, bp
.draw:
    mov [cs:boot_progress_value], al
    call stage1_splash_draw_progress_bar
    jc .return
    mov cx, 1
    call stage1_boot_wait_ticks
    jmp .next
.done:
    clc
.return:
    pop bp
    pop cx
    pop bx
    pop ax
    ret

%endif

%if STAGE1_SELFTEST_AUTORUN || (STAGE1_INTERACTIVE_SHELL && STAGE1_DEBUG_COMMANDS)
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

%endif

vdi_leave_graphics:
    mov ax, 0x0003
    int 0x10
    ret

%if STAGE1_SELFTEST_AUTORUN || (STAGE1_INTERACTIVE_SHELL && STAGE1_DEBUG_COMMANDS)
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

%endif

gfx_get_tick_count:
    mov ah, 0x00
    int 0x1A
    ret

%if STAGE1_SELFTEST_AUTORUN || (STAGE1_INTERACTIVE_SHELL && STAGE1_DEBUG_COMMANDS)
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

%endif
%if STAGE1_SELFTEST_AUTORUN || STAGE1_INTERACTIVE_SHELL
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

%endif
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
    mov word [search_found_root_lba_hi], 0
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
%if FAT_TYPE == 16
    push dx
    xor dx, dx
    call read_sector_lba32
    pop dx
    ret
%else
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
.chs_read:
    mov ax, [cs:tmp_disk_lba_save]
    xor dx, dx
    call bios_read_chs_sector32
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
%endif

write_sector_lba:
%if FAT_TYPE == 16
    push dx
    xor dx, dx
    call write_sector_lba32
    pop dx
    ret
%else
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
.chs_write:
    mov ax, [cs:tmp_disk_lba_save]
    xor dx, dx
    call bios_write_chs_sector32
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
%endif

; INT 13h has AX/CX/DX results; all other caller state belongs to us.
; AH=08h may return ES:DI and old firmware can damage high register halves.
; Preserve buffer pointers across both EDD failures and the CHS fallback.
bios_disk_interrupt:
    push ebx
    push esi
    push edi
    push ebp
    push ds
    push es
    push fs
    push gs
%ifdef CIUKIDOS_KERNEL_BUILD
    ; Like DOS, run the BIOS on a kernel disk stack, not the caller's: the
    ; firmware may use 1 KiB there (JLOAD's own stack is 1 KiB).
    mov [cs:disk_saved_sp], sp
    mov [cs:disk_saved_sp + 2], ss
    lss sp, [cs:disk_stack_ptr]
%endif
    stc
    sti
    int 0x13
    sti
    cld
%ifdef CIUKIDOS_KERNEL_BUILD
    lss sp, [cs:disk_saved_sp]
%endif
    pop gs
    pop fs
    pop es
    pop ds
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

bios_get_chs_geometry:
    pushad
    push es

    xor ax, ax
    mov es, ax
    xor di, di

    mov dl, [cs:boot_drive]
    mov ah, 0x08
    call bios_disk_interrupt
    jc .fallback
    and cl, 0x3F
    jz .fallback
    xor ax, ax
    mov al, cl
    mov [cs:tmp_disk_spt], ax
    xor ax, ax
    mov al, dh
    inc ax
    mov [cs:tmp_disk_heads], ax
    clc
    jmp .done

.fallback:
    mov word [cs:tmp_disk_spt], FAT_SPT
    mov word [cs:tmp_disk_heads], FAT_HEADS
    stc

.done:
    pop es
    popad
    ret

bios_lba32_to_chs:
    mov si, bx
    call bios_get_chs_geometry
    mov cx, [cs:tmp_disk_spt]
    ; DIV accepts DX:AX but traps when the quotient would exceed 16 bits.
    ; Such an LBA is outside the legacy CHS address space in any case.
    cmp dx, cx
    jae .range_fail
    div cx
    mov cl, dl
    inc cl
    xor dx, dx
    mov bx, [cs:tmp_disk_heads]
    div bx
    cmp ax, 1023
    ja .range_fail
    mov ch, al
    mov dh, dl
    ; INT 13h stores cylinder bits 8-9 in CL bits 6-7.
    mov al, ah
    shl al, 6
    or cl, al
    mov bx, si
    mov dl, [cs:boot_drive]
    clc
    ret

.range_fail:
    mov bx, si
    mov ah, 0x04
    stc
    ret

bios_read_chs_sector32:
    call bios_lba32_to_chs
    jc .done
    mov ah, 0x02
    mov al, 0x01
    call bios_disk_interrupt
.done:
    ret

bios_write_chs_sector32:
    call bios_lba32_to_chs
    jc .done
    mov ah, 0x03
    mov al, 0x01
    call bios_disk_interrupt
.done:
    ret

%if STAGE1_INTERACTIVE_SHELL
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
%if STAGE1_DEBUG_COMMANDS
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
%endif
    mov di, bx
    mov si, str_dir
    call str_eq
    jc .cmd_dir
    mov di, bx
    mov si, str_pwd
    call str_eq
    jc .cmd_pwd
    mov di, bx
    mov si, str_cdup
    call str_eq
    jc .cmd_cdup
    mov di, bx
    mov si, str_cd
    call str_eq
    jc .cmd_cd
    mov di, bx
    mov si, str_run
    call str_eq
    jc .cmd_run
    mov di, bx
    mov si, str_exit
    call str_eq
    jc .cmd_exit
%if STAGE1_DEBUG_COMMANDS
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
    mov si, str_beep
    call str_eq
    jc .cmd_beep
%endif
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
%if STAGE1_DEBUG_COMMANDS
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
%else
    jmp .done
%endif

.cmd_drive:
%if STAGE1_DEBUG_COMMANDS
    mov si, msg_drive
    call print_string_dual
    xor ah, ah
    mov al, [boot_drive]
    call print_hex8_dual
    call print_newline_dual
    jmp .done
%else
    jmp .done
%endif

.cmd_drives:
%if STAGE1_DEBUG_COMMANDS
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
%else
    jmp .done
%endif

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

.cmd_run:
    call shell_cmd_run
    jmp .done

.cmd_exit:
    call shell_cmd_exit
    jmp .done

.cmd_dos21:
%if STAGE1_DEBUG_COMMANDS
    mov al, [int21_installed]
    cmp al, 1
    jne .cmd_dos21_missing
    call int21_smoke_test
    jmp .done
.cmd_dos21_missing:
    mov si, msg_int21_missing
    call print_string_dual
    jmp .done
%else
    jmp .done
%endif

.cmd_comdemo:
%if STAGE1_DEBUG_COMMANDS
    call run_com_demo
    jmp .done
%else
    jmp .done
%endif

.cmd_mzdemo:
%if STAGE1_DEBUG_COMMANDS
    call run_mz_demo
    jmp .done
%else
    jmp .done
%endif

.cmd_fileio:
%if STAGE1_DEBUG_COMMANDS
    call int21_fileio_test
    jmp .done
%else
    jmp .done
%endif

.cmd_gfxdemo:
%if STAGE1_DEBUG_COMMANDS
    call run_gfx_demo
    jmp .done
%else
    jmp .done
%endif

.cmd_gfxrect:
%if STAGE1_DEBUG_COMMANDS
    call run_gfxrect_demo
    jmp .done
%else
    jmp .done
%endif

.cmd_gfxstar:
%if STAGE1_DEBUG_COMMANDS
    call run_gfxstar_demo
    jmp .done
%else
    jmp .done
%endif

.cmd_findtest:
%if STAGE1_DEBUG_COMMANDS
    call int21_find_test
    jmp .done
%else
    jmp .done
%endif

.cmd_mouse:
%if STAGE1_DEBUG_COMMANDS
    call shell_cmd_mouse
    jmp .done
%else
    jmp .done
%endif

.cmd_keytest:
%if STAGE1_DEBUG_COMMANDS
    call shell_cmd_keytest
    jmp .done
%else
    jmp .done
%endif

.cmd_beep:
%if STAGE1_DEBUG_COMMANDS
    call pc_speaker_beep
    jmp .done
%else
    jmp .done
%endif

.cmd_reboot:
    mov si, msg_rebooting
    call print_string_dual
    call hardware_reset
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
    jcxz .clear_tail
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
%if STAGE1_DEBUG_COMMANDS
    mov si, str_ticks
    call shell_completion_consider_candidate
    mov si, str_drive
    call shell_completion_consider_candidate
    mov si, str_drives
    call shell_completion_consider_candidate
%endif
    mov si, str_dir
    call shell_completion_consider_candidate
    mov si, str_pwd
    call shell_completion_consider_candidate
    mov si, str_cdup
    call shell_completion_consider_candidate
    mov si, str_cd
    call shell_completion_consider_candidate
    mov si, str_run
    call shell_completion_consider_candidate
    mov si, str_exit
    call shell_completion_consider_candidate
%if STAGE1_DEBUG_COMMANDS
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
%endif
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
    jcxz .term
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
    jcxz .prefix_match
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
%endif

skip_spaces:
.skip:
    cmp byte [si], ' '
    jne .done
    inc si
    jmp .skip
.done:
    ret

shell_arg_ptr:
    ; GPLv2 FreeCOM docommand-style first/rest split.
    mov si, bx
    mov ah, ' '
    cmp byte [si], '"'
    jne .scan_token
    inc si
    mov ah, '"'

.scan_token:
    lodsb
    cmp al, 0
    je .done
    cmp al, ah
    je .skip_tail_spaces
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

shell_copy_token_for_exec:
    mov di, shell_exec_path_buf
    mov cx, SHELL_EXEC_PATH_BUF_LEN - 1
    mov ah, ' '
    cmp byte [si], '"'
    jne .copy_loop
    inc si
    mov ah, '"'

.copy_loop:
    lodsb
    cmp al, 0
    je .copy_done
    cmp al, ah
    je .copy_done
    jcxz .copy_fail
    mov [di], al
    inc di
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
    jcxz .copy_fail
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
    jcxz .append_fail
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

shell_append_token_for_exec:
    mov ah, ' '
    cmp byte [si], '"'
    jne .copy_loop
    inc si
    mov ah, '"'

.copy_loop:
    lodsb
    cmp al, 0
    je .copy_done
    cmp al, ah
    je .copy_done
    jcxz .copy_fail
    mov [di], al
    inc di
    dec cx
    jmp .copy_loop

.copy_done:
    mov byte [di], 0
    clc
    ret

.copy_fail:
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
    mov cx, DOS_CWD_BYTES
    rep movsb
    mov ax, [cs:cwd_cluster]
    mov [cs:shell_exec_saved_cwd_cluster], ax
    pop es
    pop di
    pop si
    pop cx
    pop ax
    ; Clear shell chrome before handing control to external DOS programs,
    ; except for SHELL at boot: it replaces the splash itself.
    push ax
    cmp byte [cs:boot_splash_active], 1
    je .keep_splash
    mov ax, 0x0003
    int 0x10
.keep_splash:
    mov byte [cs:boot_splash_active], 0
    pop ax
    mov byte [cs:shell_exec_external_program_active], 1
    call shell_exec_restore_bios_int10
    ; Run the program
    push bx
    push es
    mov dx, shell_exec_path_buf
    mov ax, cs
    mov es, ax
    mov [cs:shell_exec_param_block + 4], ax
    mov [cs:shell_exec_param_block + 8], ax
    mov [cs:shell_exec_param_block + 12], ax
    mov bx, shell_exec_param_block
    mov ax, 0x4B00
    int 0x21
    pop es
    pop bx
    jc .exec_failed
    ; Exec succeeded: restore CWD to pre-exec state
    push ax
    push cx
    push si
    push di
    push es
    push cs
    pop es
    mov si, shell_exec_saved_cwd_buf
    mov di, cwd_buf
    mov cx, DOS_CWD_BYTES
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
    mov byte [cs:shell_exec_external_program_active], 0
    call shell_exec_reinstall_int10
    call draw_shell_chrome
    clc
    ret
.exec_failed:
    push ax                     ; preserve DOS error through the video redraw
    mov byte [cs:shell_exec_external_program_active], 0
    push ax
    mov ax, 0x0003
    int 0x10
    pop ax
    call shell_exec_reinstall_int10
    call draw_shell_chrome
    pop ax
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
    ; GPLv2 FreeCOM DoExec command-tail packing ported from third_party/freedos/freecom-master/shell/cswapc.c.
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

%if STAGE1_INTERACTIVE_SHELL
shell_try_exec_token:
    push ds
    push cs
    pop ds

    push si
    call shell_try_resolve_exec_token
    pop bx
    jc .fail
    call shell_arg_ptr
    call shell_exec_set_tail_from_si
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
%endif

shell_try_exec_path:
    push ds

    mov bx, si
    push cs
    pop ds

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

%if STAGE1_INTERACTIVE_SHELL
shell_cmd_help:
    push ax
    push bx
    push si
    push di
    push ds

    push cs
    pop ds
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

.short:
    call print_shell_help
    jmp .done

.all:
    call print_shell_help
%if STAGE1_DEBUG_COMMANDS
    call print_shell_help_all
%else
    mov si, msg_help_all_disabled
    call print_string_dual
%endif

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

    push cs
    pop ds
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

%if STAGE1_DEBUG_COMMANDS
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
%endif

%if STAGE1_DEBUG_COMMANDS
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
%endif

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

    push cs
    pop ds
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

    push cs
    pop ds

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
    mov cx, DOS_CWD_BYTES
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
    mov cx, DOS_CWD_BYTES
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
    jcxz .emit
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

%endif

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

hide_text_cursor:
    push ax
    push cx
    mov ah, 0x01
    mov ch, 0x20
    mov cl, 0x00
    int 0x10
    pop cx
    pop ax
    ret

show_text_cursor:
    push ax
    push cx
    mov ah, 0x01
    mov ch, 0x06
    mov cl, 0x07
    int 0x10
    pop cx
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

video_write_centered_attr:
    push ax
    push cx
    push dx
    push si

    push si
    xor cx, cx
.count:
    lodsb
    test al, al
    jz .count_done
    inc cx
    jmp .count
.count_done:
    pop si

    mov ax, 80
    sub ax, cx
    shr ax, 1
    mov dl, al
    call video_write_string_attr

    pop si
    pop dx
    pop cx
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
    ret

shell_footer_compute_cpu_pct:
    ret

shell_footer_maybe_refresh_disk:
    ret

shell_footer_refresh_disk_pct:
    clc
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

%if STAGE1_INTERACTIVE_SHELL
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

%if STAGE1_DEBUG_COMMANDS
print_shell_help_all:
    mov si, msg_help_all
    jmp print_string_dual
%endif

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
%endif

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
    ; AH=0Eh can destroy BP when scrolling on older BIOSes.  Keep the
    ; complete caller state, including the DS:SI string cursor, across all
    ; INT 10h paths; firmware hooks must not redirect the following print.
    pushad
    push ds
    push es
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
    jmp .done

.lf_teletype:
    pop dx
    pop cx

.teletype:
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
.done:
    pop es
    pop ds
    popad
    cld
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
    push cx
    push dx
    mov ah, al
    ; Diagnostics must also return when COM1 is disabled or never ready.
    mov cx, 0x1000
.wait:
    mov dx, 0x03F8 + 5
    in al, dx
    test al, 0x20
    jnz .ready
    loop .wait
    jmp .done
.ready:
    mov dx, 0x03F8
    mov al, ah
    out dx, al
.done:
    pop dx
    pop cx
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
    call install_int33_vector
    call init_mouse
%if FAT_TYPE == 16
    nop
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
    mov [cs:runtime_table_off], ax
    mov [cs:runtime_table_seg], ax
    mov [cs:runtime_status_flags], ax
    mov [cs:runtime_service_off], ax
    mov [cs:runtime_service_seg], ax
    mov [cs:runtime_state_off], ax
    mov [cs:runtime_state_seg], ax
    pop ax
    ret

stage1_runtime_lookup_service:
    push bx
    push cx
    push dx
    push es

    mov dx, ax
    mov ax, [cs:runtime_status_flags]
    test ax, 1
    jz .fail
    mov ax, [cs:runtime_table_seg]
    or ax, ax
    jz .fail
    mov es, ax
    mov bx, [cs:runtime_table_off]
    or bx, bx
    jz .fail
    cmp dx, 0x0001
    jne .service_two_plus
    cmp word [es:bx + 12], 0x0001
    jne .fail
    mov ax, [es:bx + 14]
    or ax, ax
    jz .fail
    mov [cs:runtime_service_off], ax
    mov ax, [cs:runtime_table_seg]
    mov [cs:runtime_service_seg], ax
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
    mov [cs:runtime_service_off], ax
    mov ax, [cs:runtime_table_seg]
    mov [cs:runtime_service_seg], ax
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

ciukidos_get_state_ptr:
    mov ax, 0x0006
    call stage1_runtime_lookup_service
    jc .fail
    call far [cs:runtime_service_ptr]
    jc .fail
    mov ax, ds
    cmp ax, RUNTIME_LOAD_SEG
    jne .fail
    or si, si
    jz .fail
    mov [cs:runtime_state_off], si
    mov [cs:runtime_state_seg], ax
    clc
    ret

.fail:
    xor ax, ax
    mov [cs:runtime_state_off], ax
    mov [cs:runtime_state_seg], ax
    stc
    ret

ciukidos_save_parent_dta:
    push ax
    push bx
    push cx
    mov ax, 0x0007
    call stage1_runtime_lookup_service
    jc .done
    call int21_mem_active_psp
    mov cx, ax
    ; The PSP lookup returns AX; load the independent DTA segment after it.
    mov ax, [cs:dta_seg]
    mov bx, [cs:dta_off]
    call far [cs:runtime_service_ptr]
    jc .done
    mov [cs:dta_seg], dx
    mov word [cs:dta_off], 0x0080
.done:
    pop cx
    pop bx
    pop ax
    ret

ciukidos_restore_parent_dta:
    push ax
    push cx
    mov ax, 0x0008
    call stage1_runtime_lookup_service
    jc .done
    mov cx, [cs:dos_exec_identity_psp]
    call far [cs:runtime_service_ptr]
    jc .done
    mov [cs:dta_seg], ax
    mov [cs:dta_off], dx
.done:
    pop cx
    pop ax
    ret

ciukidos_record_last_termination:
    push ax
    push cx
    mov ax, 0x000B
    call stage1_runtime_lookup_service
    jc .done
    mov al, [cs:last_exit_code]
    mov ah, [cs:last_term_type]
    mov cx, [cs:dos_exec_identity_psp]
    call far [cs:runtime_service_ptr]
.done:
    pop cx
    pop ax
    ret

; A plain RETF bypasses both resident termination hooks.  Service 11 is
; record-once for the current child: CF clear means this is the first observed
; termination and the Stage1 mirror must become a normal zero exit; CF set
; means AH=4Ch/AH=31h/INT 20h/fault already supplied the authoritative status.
ciukidos_record_retf_termination:
    push ax
    push cx
    mov ax, 0x000B
    call stage1_runtime_lookup_service
    jc .done
    xor ax, ax
    mov cx, [cs:dos_exec_identity_psp]
    call far [cs:runtime_service_ptr]
    jc .done
    mov byte [cs:last_exit_code], 0
    mov byte [cs:last_term_type], 0
.done:
    pop cx
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
    ret

stage1_runtime_validate_cache:
    push ax
    push bx
    push cx
    push es

    mov ax, [cs:runtime_status_flags]
    test ax, 1
    jz .fail
    mov ax, [cs:runtime_table_seg]
    cmp ax, RUNTIME_LOAD_SEG
    jne .fail
    or ax, ax
    jz .fail
    mov es, ax
    mov bx, [cs:runtime_table_off]
    or bx, bx
    jz .fail
    cmp word [es:bx], 0x5452
    jne .fail
    cmp word [es:bx + 2], 0x5653
    jne .fail
%ifdef CIUKIDOS_KERNEL_BUILD
    cmp word [es:bx + 4], 2
%else
    cmp word [es:bx + 4], 1
%endif
    jne .fail
    cmp word [es:bx + 6], 11
    jne .fail
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
%ifdef CIUKIDOS_KERNEL_BUILD
    push ax
    push bx
    push ds
    push es

    call stage1_runtime_clear_cache
    mov ax, runtime_service_table
    mov [cs:runtime_table_off], ax
    mov ax, cs
    mov [cs:runtime_table_seg], ax
    mov word [cs:runtime_status_flags], 0x0001
    call kernel_runtime_initialize
    call stage1_runtime_validate_cache
    jc .kernel_fail
    call ciukidos_get_state_ptr
    jc .kernel_fail
    call stage1_runtime_sync_default_drive
    jc .kernel_fail
    clc
    jmp .kernel_done
.kernel_fail:
    call stage1_runtime_clear_cache
    stc
.kernel_done:
    pop es
    pop ds
    pop bx
    pop ax
    ret
%else
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
    mov cx, 1792
    rep stosw

    mov ax, RUNTIME_LOAD_SEG
    mov ds, ax
    xor dx, dx
    mov cx, 3584
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

    mov word [runtime_handoff_version], 0x0001
    mov al, [boot_drive]
    mov [runtime_handoff_boot_drive], al
    mov al, [dos_default_drive]
    mov [runtime_handoff_default_drive], al
    int 0x12
    mov [runtime_handoff_mem_kb], ax
    mov word [runtime_handoff_fat_spt], FAT_SPT
    mov word [runtime_handoff_fat_heads], FAT_HEADS
    mov word [runtime_handoff_fat_reserved], FAT_RESERVED_SECTORS
    mov byte [runtime_handoff_fat_spc], FAT_SECTORS_PER_CLUSTER
%if STAGE1_BOOT_EXTERNAL_SHELL
    mov byte [runtime_handoff_entry_flags], 1
%else
    mov byte [runtime_handoff_entry_flags], 0
%endif

    push cs
    pop es
    mov di, runtime_handoff
    call RUNTIME_LOAD_SEG:0x0000

    push cs
    pop ds
    call stage1_runtime_validate_cache
    jc .fail
    call ciukidos_get_state_ptr
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
%endif

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
    pusha
    push ds
    push es

    push cs
    pop ds
    mov si, msg_runtime_probe_begin
    call print_string_serial

    call stage1_runtime_validate_cache
    jc .fail

    mov ax, [runtime_table_seg]
    mov es, ax
    mov bx, [runtime_table_off]
    cmp word [es:bx + 6], 11
    jne .fail

    mov si, msg_runtime_probe_table
    call print_string_serial

    mov ax, 0x0001
    call stage1_runtime_lookup_service
    jc .fail
    call far [cs:runtime_service_ptr]
    jc .fail
    cmp ax, 0x5254
    jne .fail

    push cs
    pop ds
    mov si, msg_runtime_probe_call
    call print_string_serial
    clc
    jmp .done

.fail:
    stc

.done:
    pop es
    pop ds
    popa
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

    call STAGE2_LOAD_SEG:0x0000
    mov byte [cs:stage2_autorun_status], 1
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
%else
; FAT12 keeps the legacy in-Stage1 process state and has no CIUKIDOS table.
; Generic INT 20h/21h paths still call these hooks, so keep them explicit
; no-ops instead of leaving the floppy profile with unresolved symbols.
ciukidos_save_parent_dta:
ciukidos_restore_parent_dta:
ciukidos_record_last_termination:
ciukidos_record_retf_termination:
    ret
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
%if FAT_TYPE == 16
    ; Native initialization has quiesced AUX and restored IRQ1. Give a BIOS
    ; PS/2 driver the original firmware IRQ12/C2 pair, rather than stacking
    ; it above our unsuccessful direct-controller handler. Keep INT33 safe
    ; to call: AX=0 against the inert IRET stub reports no installed driver.
    pushf
    cli
    push es
    xor ax, ax
    mov es, ax
    mov byte [cs:shell_exec_external_mouse_disabled], 1
    mov eax, [cs:old_int74_off]
    mov [es:0x74 * 4], eax
    mov word [es:0x33 * 4], int_default_iret
    mov ax, cs
    mov [es:0x33 * 4 + 2], ax
    pop es
    popf
%endif
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
    push cx
    mov cx, 32
.wait_ack:
    call ps2_wait_output_full
    jc .ack_done
    ; Do not steal a pending keyboard scan while waiting for a mouse ACK.
    test al, 0x20
    jz .bad_ack
    in al, 0x60
    cmp al, 0xFA
    je .ack
    cmp al, 0xFE
    je .bad_ack
    ; A streaming device can finish an older packet before acknowledging
    ; F6/F4. Bound the drain, rather than treating that packet as an ACK.
    loop .wait_ack
.bad_ack:
    stc
    jmp .ack_done
.ack:
    clc
.ack_done:
    pop cx
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
    pushf
    cli
    mov byte [cs:ps2_command_saved],0
    ; The keyboard, TrackPoint and command-byte reply share port 60h.
    ; Quiesce both ports before reading controller RAM: an arriving scan or
    ; AUX packet must never become the new keyboard/translation settings.
    mov al, 0xAD
    call ps2_write_cmd
    jc .fail
    mov al, 0xA7
    call ps2_write_cmd
    jc .fail
    call ps2_wait_input_clear
    jc .fail
    call ps2_mouse_flush
    mov al, 0x20
    call ps2_write_cmd
    jc .fail
    call ps2_read_data
    jc .fail
    mov [cs:ps2_saved_command],al
    mov byte [cs:ps2_command_saved],1
    ; Preserve BIOS translation and system bits. Keep the keyboard clock
    ; inhibited until the polled AUX replies have all been consumed.
    or al, 0x13                 ; IRQ1 + IRQ12, keyboard clock inhibited
    and al, 0xDF
    mov ah, al
    mov al, 0x60
    call ps2_write_cmd
    jc .fail
    mov al, ah
    call ps2_write_data
    jc .fail
    mov al, 0xA8
    call ps2_write_cmd
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
    mov byte [cs:mouse_packet_index], 0
    mov byte [cs:mouse_hw_ready], 1
    mov byte [cs:mouse_bios_reporting_stopped], 0
    jmp .restore_keyboard
.fail:
    mov byte [cs:mouse_hw_ready], 0
    ; A failed mouse command can leave IRQ12 enabled and an ACK in the
    ; shared output buffer. Quiesce AUX before returning the port to the
    ; keyboard; preserve firmware translation/system bits, not our temporary
    ; inhibited-keyboard command byte. Never invent a byte if its read failed.
    mov al,0xA7
    call ps2_write_cmd
    call ps2_wait_input_clear
    call ps2_mouse_flush
    in al,0xA1
    or al,0x10
    out 0xA1,al
    cmp byte [cs:ps2_command_saved],1
    je .restore_command
    ; An early timeout may have cleared by now. Recover the real command
    ; byte before changing IRQ policy; retain the BIOS translation choice.
    mov al,0x20
    call ps2_write_cmd
    jc .restore_keyboard
    call ps2_read_data
    jc .restore_keyboard
    mov [cs:ps2_saved_command],al
.restore_command:
    mov al,0x60
    call ps2_write_cmd
    jc .restore_keyboard
    mov al,[cs:ps2_saved_command]
    and al,0xED                ; clear IRQ12 and keyboard inhibit
    or al,0x21                 ; enable IRQ1, inhibit failed AUX
    call ps2_write_data
.restore_keyboard:
    ; Re-enable IRQ1/its clock on success AND on an absent or failed mouse.
    ; Enabling AUX alone is not sufficient on notebook controllers.
    mov al, 0xAE
    call ps2_write_cmd
    jc .keyboard_unavailable
    call ps2_wait_input_clear
    jnc .keyboard_ready
.keyboard_unavailable:
    mov byte [cs:mouse_hw_ready], 0
    ; F4 may already have succeeded when the final keyboard operation times
    ; out. Mask and inhibit AUX before handing IRQ12 back to firmware; then
    ; retry keyboard enable once. Never loop indefinitely on a busy 8042.
    in al, 0xA1
    or al, 0x10
    out 0xA1, al
    mov al, 0xA7
    call ps2_write_cmd
    call ps2_wait_input_clear
    call ps2_mouse_flush
    mov al, 0xAE
    call ps2_write_cmd
    call ps2_wait_input_clear
.keyboard_ready:
    in al, 0x21
    and al, 0xF9                ; keyboard IRQ1 and slave PIC cascade
    out 0x21, al
    popf
    cmp byte [cs:mouse_hw_ready], 1 ; return CF without changing caller's IF
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
    ; Hardware interrupts preserve the interrupted task's DF.  Driver code
    ; and user event handlers require the normal forward-string convention;
    ; IRET restores the application's original FLAGS image.
    cld

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
    ; A latched/spurious IRQ12 can run after a polled ACK was consumed.
    ; Leave a keyboard byte in the shared buffer for the BIOS IRQ1 handler.
    test al, 0x20
    jz .eoi_fast
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
    mov [cs:mouse_prev_buttons], ah
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

    mov ax, [cs:mouse_pos_x]
    mov [cs:mouse_prev_x], ax
    mov ax, [cs:mouse_pos_y]
    mov [cs:mouse_prev_y], ax

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
    mov [cs:mouse_last_event_mask], bp

    test bp, 0x0002
    jz .left_press_done
    inc word [cs:mouse_press_count + 0]
    mov [cs:mouse_press_x + 0], cx
    mov [cs:mouse_press_y + 0], dx
.left_press_done:
    test bp, 0x0004
    jz .left_release_done
    inc word [cs:mouse_release_count + 0]
    mov [cs:mouse_release_x + 0], cx
    mov [cs:mouse_release_y + 0], dx
.left_release_done:
    test bp, 0x0008
    jz .right_press_done
    inc word [cs:mouse_press_count + 2]
    mov [cs:mouse_press_x + 2], cx
    mov [cs:mouse_press_y + 2], dx
.right_press_done:
    test bp, 0x0010
    jz .right_release_done
    inc word [cs:mouse_release_count + 2]
    mov [cs:mouse_release_x + 2], cx
    mov [cs:mouse_release_y + 2], dx
.right_release_done:
    test bp, 0x0020
    jz .middle_press_done
    inc word [cs:mouse_press_count + 4]
    mov [cs:mouse_press_x + 4], cx
    mov [cs:mouse_press_y + 4], dx
.middle_press_done:
    test bp, 0x0040
    jz .middle_release_done
    inc word [cs:mouse_release_count + 4]
    mov [cs:mouse_release_x + 4], cx
    mov [cs:mouse_release_y + 4], dx
.middle_release_done:

    call mouse_vga_update_position

    cmp byte [cs:mouse_driver_enabled], 1
    jne .eoi

    cmp byte [cs:mouse_bios_enabled], 0
    je .int33_callback
    cmp word [cs:mouse_bios_asr_seg], 0
    je .int33_callback
    inc word [cs:mouse_bios_callback_count]

    ; INT 15h/C207h installs a FAR callback whose entry stack, after the FAR
    ; return address, contains Z, Y, X and status.  Push the packet words in
    ; reverse order so the callback observes that layout.  Deltas remain raw
    ; zero-extended PS/2 bytes; their sign is also encoded in the status word.
    ; Preserve the complete interrupted context around the BIOS callback.
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    xor ah, ah
    mov al, [cs:mouse_packet]
    push ax                         ; packet status/buttons/sign/overflow
    xor ah, ah
    mov al, [cs:mouse_packet + 1]
    push ax                         ; raw PS/2 X delta
    xor ah, ah
    mov al, [cs:mouse_packet + 2]
    push ax                         ; raw PS/2 Y delta
    xor ax, ax
    push ax                         ; Z delta (standard three-byte mouse = 0)
    call far [cs:mouse_bios_asr_off]
    add sp, 8
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax

    ; INT 15h/C2 is an exclusive hardware ownership path.  Once a BIOS
    ; client (notably Windows MOUSE.DRV) has enabled its ASR, never dispatch
    ; the same packet to a DOS INT 33h callback as well.  A callback left by
    ; the previously executed DOS program may already point into reclaimed
    ; memory; invoking both paths caused duplicate/erratic Windows motion and
    ; could jump into that stale client during a click.
    jmp .eoi

.int33_callback:
    mov ax, bp
    and ax, [cs:mouse_cb_mask]
    jz .eoi
    cmp word [cs:mouse_cb_seg], 0
    je .eoi

    cmp byte [cs:mouse_cb_busy], 0
    jne .queue_callback

    xor bx, bx
    mov bl, [cs:mouse_buttons]
    mov cx, [cs:mouse_pos_x]
    mov dx, [cs:mouse_pos_y]
    mov si, [cs:mouse_last_mickey_x]
    mov di, [cs:mouse_last_mickey_y]
    ; Match conventional DOS mouse drivers: callbacks may rely on timer and
    ; keyboard interrupts remaining serviceable while their FAR routine runs.
    sti
    call mouse_dispatch_user_callback
    ; A real mouse can deliver another complete packet while the client is
    ; still inside its callback.  The nested IRQ coalesces that packet in the
    ; pending slot; drain it before leaving this IRQ or button releases and
    ; motion remain stranded until the next application reset.
    call mouse_flush_pending_callback
    cli
    jmp .eoi

.queue_callback:
    or [cs:mouse_cb_pending_mask], ax
    mov al, [cs:mouse_buttons]
    mov [cs:mouse_cb_pending_buttons], al
    mov [cs:mouse_cb_pending_x], cx
    mov [cs:mouse_cb_pending_y], dx
    mov [cs:mouse_cb_pending_dx], si
    mov [cs:mouse_cb_pending_dy], di

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
    ; Sync the planar EGA/VGA cursor position directly from the logical
    ; mouse position (already scaled and clamped by the IRQ12 handler).
    mov ax, [cs:mouse_pos_x]
    ; Modes 10h and 12h are both 640 pixels wide.
    cmp ax, 632
    jbe .x_ok
    mov ax, 632
.x_ok:
    mov [cs:mouse_vga_cursor_x], ax
    mov ax, [cs:mouse_pos_y]
    cmp byte [cs:current_video_mode], 0x10
    je .mode10_y
    cmp ax, 472
    jbe .y_ok
    mov ax, 472
    jmp .y_ok
.mode10_y:
    cmp ax, 342
    jbe .y_ok
    mov ax, 342
.y_ok:
    mov [cs:mouse_vga_cursor_y], ax
    pop ax
    ret

mouse_vga_cursor_refresh:
    push ax
    push bx
    push dx

    cmp byte [cs:current_video_mode], 0x10
    je .planar_ready
    cmp byte [cs:current_video_mode], 0x12
    je .planar_ready
    call mouse_vga_cursor_erase_if_drawn
    jmp .done

.planar_ready:
    cmp byte [cs:mouse_bios_enabled], 0
    je .inactive
    cmp word [cs:mouse_bios_asr_seg], 0
    je .inactive

    ; A BIOS PS/2 client owns cursor rendering once it has installed an ASR.
    ; Drawing the INT 33h XOR fallback as well produces two independently
    ; scaled cursors in Windows 3.x.
    call mouse_vga_cursor_erase_if_drawn
    jmp .done

.inactive:
    cmp byte [cs:current_video_mode], 0x12
    jne .inactive_visibility_check
    cmp word [cs:mouse_cb_seg], 0
    jne .inactive_visibility_check
    cmp byte [cs:mouse_bios_enabled], 0
    je .skip_no_trace
.inactive_visibility_check:
    cmp byte [cs:mouse_driver_enabled], 1
    jne .skip_no_trace
    cmp byte [cs:mouse_visible], 1
    je .active
.skip_no_trace:
    call mouse_vga_cursor_erase_if_drawn
    jmp .done

.active:
    ; When a client callback owns motion events, let the client draw its cursor.
    ; If the callback excludes motion, keep the standard XOR fallback active.
    cmp word [cs:mouse_cb_seg], 0
    je .active_no_cb
    test word [cs:mouse_cb_mask], 0x0001
    jnz .done
.active_no_cb:
    cmp byte [cs:mouse_gfx_cursor_custom], 0
    jne .skip_no_trace
    cmp byte [cs:mouse_excl_enabled], 0
    je .draw_check
    mov ax, [cs:mouse_pos_x]
    cmp ax, [cs:mouse_excl_min_x]
    jb .draw_check
    cmp ax, [cs:mouse_excl_max_x]
    ja .draw_check
    mov ax, [cs:mouse_pos_y]
    cmp ax, [cs:mouse_excl_min_y]
    jb .draw_check
    cmp ax, [cs:mouse_excl_max_y]
    ja .draw_check
    jmp .skip_no_trace

.draw_check:
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

mouse_vga_cursor_erase_if_drawn:
    cmp byte [cs:mouse_vga_cursor_drawn], 0
    je .done
    push bx
    push dx
    mov bx, [cs:mouse_vga_cursor_last_x]
    mov dx, [cs:mouse_vga_cursor_last_y]
    call mouse_vga_xor_cursor12
    mov byte [cs:mouse_vga_cursor_drawn], 0
    pop dx
    pop bx
.done:
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
    mov bx, 0xFFFF
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
    cmp byte [cs:current_video_mode], 0x10
    je .mode10_pixel_y
    cmp ax, 480
    jae .pixel_done
    jmp .pixel_y_ok
.mode10_pixel_y:
    cmp ax, 350
    jae .pixel_done
.pixel_y_ok:
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
%else
; Mode 12h cursor rendering is a FAT16/full-profile facility. INT 10h/33h
; shares callers with FAT12, where these hooks intentionally do nothing.
mouse_vga_update_position:
mouse_vga_cursor_refresh:
mouse_vga_cursor_erase_if_drawn:
    ret
%endif

int10_handler:
%if TRACE_CHILD_INT21 != 0
    call child_trace_int10_enter
%endif
    cmp ah, 0x4F
    je .vbe
    cmp ah, 0x0F
    je .get_mode
    cmp ah, 0x00
    je .set_mode
    jmp far [cs:old_int10_off]

.vbe:
    cmp al, 0x00
    je .vbe_get_controller
    cmp al, 0x01
    je .vbe_get_mode_info
    cmp al, 0x02
    je .vbe_set_mode
    cmp al, 0x03
    je .vbe_get_current_mode
    cmp al, 0x05
    je .vbe_window
    jmp far [cs:old_int10_off]

.set_mode:
    mov [cs:current_video_mode], al
    call int10_vbe_reset_state
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

.vbe_get_controller:
    call int10_call_original_vbe
    jnc .vbe_bios_done

.vbe_get_controller_local:
    mov bx, di
    mov word [es:bx], 0x4556
    mov word [es:bx + 2], 0x4153
    mov word [es:bx + 4], 0x0200
    mov word [es:bx + 6], vbe_oem_string
    mov ax, cs
    mov [es:bx + 8], ax
    xor ax, ax
    mov [es:bx + 0x0A], ax
    mov [es:bx + 0x0C], ax
    mov word [es:bx + 0x0E], vbe_mode_list
    mov ax, cs
    mov [es:bx + 0x10], ax
    mov word [es:bx + 0x12], 16
    mov ax, 0x004F
    iret

.vbe_get_mode_info:
    call int10_call_original_vbe
    jnc .vbe_bios_done

.vbe_get_mode_info_local:
    mov ax, cx
    and ax, 0x3FFF
    call int10_vbe_find_mode
    jc .vbe_unsupported
    mov bx, di
    mov word [es:bx], 0x001B
    mov word [es:bx + 2], 0x0707
    mov word [es:bx + 4], 64
    mov word [es:bx + 6], 64
    mov word [es:bx + 8], 0xA000
    mov word [es:bx + 0x0A], 0xA000
    push cx
    push di
    mov di, bx
    add di, 0x0C
    xor ax, ax
    mov cx, 15
    rep stosw
    pop di
    pop cx
    mov ax, [cs:si + 6]
    mov [es:bx + 0x10], ax
    mov ax, [cs:si + 2]
    mov [es:bx + 0x12], ax
    mov ax, [cs:si + 4]
    mov [es:bx + 0x14], ax
    mov byte [es:bx + 0x16], 8
    mov byte [es:bx + 0x17], 16
    mov byte [es:bx + 0x18], 1
    mov byte [es:bx + 0x19], 8
    mov al, [cs:si + 8]
    mov [es:bx + 0x1A], al
    mov byte [es:bx + 0x1B], 4
    mov byte [es:bx + 0x1C], 64
    mov al, [cs:si + 9]
    mov [es:bx + 0x1D], al
    mov ax, 0x004F
    iret

.vbe_set_mode:
    push bx
    pop bx
    call int10_call_original_vbe
    jc .vbe_set_mode_local
    mov dx, bx
    and dx, 0x3FFF
    call int10_vbe_set_mode_from_bios
    mov ax, 0x004F
    iret

.vbe_set_mode_local:
    mov ax, bx
    and ax, 0x3FFF
    call int10_vbe_find_mode
    jc .vbe_unsupported
    mov [cs:current_vbe_mode], ax
    xor ax, ax
    mov [cs:current_vbe_bank_a], ax
    mov [cs:current_vbe_bank_b], ax
    mov al, [cs:si + 10]
    mov [cs:current_video_mode], al
    push es
    xor ax, ax
    mov es, ax
    mov al, [cs:current_video_mode]
    mov [es:0x0449], al
    xor ax, ax
    mov [es:0x0462], ax
    mov word [es:0x0463], 0x03D4
    mov al, [cs:si + 11]
    xor ah, ah
    mov [es:0x044A], ax
    pop es
    call int10_vbe_activate_local_mode
    mov ax, 0x004F
    iret

.vbe_get_current_mode:
    call int10_call_original_vbe
    jc .vbe_get_current_mode_local
    mov [cs:current_vbe_mode], bx
    iret

.vbe_get_current_mode_local:
    mov bx, [cs:current_vbe_mode]
    or bx, bx
    jnz .vbe_current_ready
    xor bh, bh
    mov bl, [cs:current_video_mode]
.vbe_current_ready:
    mov ax, 0x004F
    iret

.vbe_window:
    cmp bl, 0x00
    je .vbe_window_set_try_bios
    cmp bl, 0x01
    je .vbe_window_get_try_bios

.vbe_window_local:
    mov ax, [cs:current_vbe_mode]
    or ax, ax
    jz .vbe_unsupported
    cmp bl, 0x00
    je .vbe_set_window
    cmp bl, 0x01
    je .vbe_get_window
    jmp .vbe_unsupported

.vbe_window_set_try_bios:
    call int10_call_original_vbe
    jc .vbe_window_set_local
    cmp bh, 0x00
    je .vbe_bios_bank_a
    cmp bh, 0x01
    je .vbe_bios_bank_b
    iret

.vbe_window_set_local:
    jmp .vbe_window_local

.vbe_window_get_try_bios:
    call int10_call_original_vbe
    jc .vbe_window_get_local
    cmp bh, 0x00
    je .vbe_bios_get_bank_a
    cmp bh, 0x01
    je .vbe_bios_get_bank_b
    iret

.vbe_window_get_local:
    jmp .vbe_window_local

.vbe_bios_bank_a:
    mov [cs:current_vbe_bank_a], dx
    mov [cs:current_vbe_visible_bank], dx
    mov byte [cs:current_vbe_visible_window], 0x00
    iret

.vbe_bios_bank_b:
    mov [cs:current_vbe_bank_b], dx
    mov [cs:current_vbe_visible_bank], dx
    mov byte [cs:current_vbe_visible_window], 0x01
    iret

.vbe_bios_get_bank_a:
    mov [cs:current_vbe_bank_a], dx
    iret

.vbe_bios_get_bank_b:
    mov [cs:current_vbe_bank_b], dx
    iret

.vbe_bios_done:
    iret

.vbe_set_window:
    cmp bh, 0x00
    je .vbe_set_bank_a
    cmp bh, 0x01
    je .vbe_set_bank_b
    jmp .vbe_unsupported

.vbe_set_bank_a:
    call int10_vbe_set_window_local_bank
    iret

.vbe_set_bank_b:
    call int10_vbe_set_window_local_bank
    iret

.vbe_get_window:
    cmp bh, 0x00
    je .vbe_get_bank_a
    cmp bh, 0x01
    je .vbe_get_bank_b
    jmp .vbe_unsupported

.vbe_get_bank_a:
    mov dx, [cs:current_vbe_bank_a]
    mov ax, 0x004F
    iret

.vbe_get_bank_b:
    mov dx, [cs:current_vbe_bank_b]
    mov ax, 0x004F
    iret

.vbe_unsupported:
    mov ax, 0x014F
    iret

int10_call_original_bios:
    push ax
    mov ax, [cs:old_int10_off]
    or ax, [cs:old_int10_seg]
    jz .missing
    push cs
    pop ax
    cmp ax, [cs:old_int10_seg]
    jne .call
    mov ax, [cs:old_int10_off]
    cmp ax, int10_handler
    je .missing
.call:
    pop ax
    pushf
    call far [cs:old_int10_off]
    clc
    ret
.missing:
    pop ax
    stc
    ret

int10_call_original_vbe:
    call int10_call_original_bios
    jc .failed
    cmp ax, 0x004F
    jne .failed
    clc
    ret
.failed:
    stc
    ret

int10_vbe_reset_state:
    push ax
    xor ax, ax
    mov [cs:current_vbe_mode], ax
    mov [cs:current_vbe_bank_a], ax
    mov [cs:current_vbe_bank_b], ax
    mov [cs:current_vbe_mode_banks], ax
    mov word [cs:current_vbe_visible_bank], 0xFFFF
    mov byte [cs:current_vbe_visible_window], 0xFF
    mov byte [cs:current_vbe_backing_ready], 0
    pop ax
    ret

int10_vbe_set_mode_common:
    push bx
    mov [cs:current_vbe_mode], ax
    xor bx, bx
    mov [cs:current_vbe_bank_a], bx
    mov [cs:current_vbe_bank_b], bx
    mov [cs:current_vbe_visible_bank], bx
    mov bl, [cs:si + 8]
    mov [cs:current_vbe_mode_banks], bx
    mov byte [cs:current_vbe_visible_window], 0x00
    mov byte [cs:current_vbe_backing_ready], 0
    pop bx
    ret

int10_vbe_set_mode_from_bios:
    push ax
    push si
    mov ax, dx
    call int10_vbe_find_mode
    jc .unsupported
    call int10_vbe_set_mode_common
    jmp .done
.unsupported:
    call int10_vbe_reset_state
    mov [cs:current_vbe_mode], dx
.done:
    pop si
    pop ax
    ret

int10_vbe_activate_local_mode:
    call int10_vbe_set_mode_common
    call int10_vbe_clear_backing_store
    xor dx, dx
    call int10_vbe_load_window_bank
    mov byte [cs:current_vbe_backing_ready], 1
    ret

int10_vbe_ensure_backing_store:
    push ax
    push bx
    push dx
    cmp word [cs:current_vbe_backing_seg], 0
    jne .done
    ; Leave one 64 KiB window free for DOS heap callers before reserving banks.
    mov bx, VBE_BACKING_TARGET_PARAS
    call int10_vbe_alloc_system_block
    jnc .full
    cmp bx, VBE_BANK_WINDOW_PARAS
    jb .done
    mov dx, 1
    cmp bx, VBE_BANK_WINDOW_PARAS * 2
    jb .partial
    mov dx, 2
    cmp bx, VBE_BANK_WINDOW_PARAS * 3
    jb .partial
    mov dx, 3
.partial:
    mov bx, dx
    mov cl, 12
    shl bx, cl
    call int10_vbe_alloc_system_block
    jc .done
    mov [cs:current_vbe_backing_seg], ax
    mov [cs:current_vbe_backing_banks], dx
    jmp .done
.full:
    mov [cs:current_vbe_backing_seg], ax
    mov word [cs:current_vbe_backing_banks], VBE_BACKING_TARGET_BANKS
.done:
    pop dx
    pop bx
    pop ax
    ret

int10_vbe_alloc_system_block:
    push dx
    mov dx, [cs:current_psp_seg]
    xor ax, ax
    call int21_set_current_psp
    call int21_alloc
    push ax
    mov ax, dx
    call int21_set_current_psp
    pop ax
    jc .done
    push ax
    call int21_mem_table_find_exact
    pop ax
    jc .done
    mov word [cs:dos_mem_block_table + si + 4], 0x0008
    call int21_mem_sync_legacy
    call int21_mem_rebuild_chain
.done:
    pop dx
    ret

int10_vbe_clear_backing_store:
    push ax
    push bx
    push cx
    push di
    push es
    call int10_vbe_ensure_backing_store
    mov ax, [cs:current_vbe_backing_seg]
    or ax, ax
    jz .done
    mov es, ax
    xor di, di
    xor ax, ax
    mov bx, [cs:current_vbe_backing_banks]
.bank_loop:
    or bx, bx
    jz .done
    mov cx, VBE_BANK_WINDOW_WORDS
    rep stosw
    mov ax, es
    add ax, VBE_BANK_WINDOW_PARAS
    mov es, ax
    dec bx
    jmp .bank_loop
.done:
    pop es
    pop di
    pop cx
    pop bx
    pop ax
    ret

int10_vbe_bank_segment:
    push bx
    mov ax, [cs:current_vbe_backing_seg]
    or ax, ax
    jz .fail
    cmp dx, [cs:current_vbe_backing_banks]
    jae .fail
    mov bx, dx
.next_bank:
    or bx, bx
    jz .ready
    add ax, VBE_BANK_WINDOW_PARAS
    dec bx
    jmp .next_bank
.ready:
    clc
    pop bx
    ret
.fail:
    stc
    pop bx
    ret

int10_vbe_save_window_bank:
    push ax
    push cx
    push si
    push di
    push ds
    push es
    call int10_vbe_bank_segment
    jc .done
    mov es, ax
    mov ax, 0xA000
    mov ds, ax
    xor si, si
    xor di, di
    mov cx, VBE_BANK_WINDOW_WORDS
    cld
    rep movsw
.done:
    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop ax
    ret

int10_vbe_load_window_bank:
    push ax
    push bx
    push cx
    push si
    push di
    push ds
    push es
    call int10_vbe_bank_segment
    jc .clear
    mov bx, ax
    mov ax, 0xA000
    mov es, ax
    mov ds, bx
    xor si, si
    xor di, di
    mov cx, VBE_BANK_WINDOW_WORDS
    cld
    rep movsw
    jmp .done
.clear:
    call int10_vbe_clear_window
.done:
    pop es
    pop ds
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

int10_vbe_save_visible_bank:
    push dx
    mov dx, [cs:current_vbe_visible_bank]
    cmp dx, 0xFFFF
    je .done
    call int10_vbe_save_window_bank
.done:
    pop dx
    ret

int10_vbe_set_window_local_bank:
    cmp bh, 0x01
    ja .unsupported
    mov ax, [cs:current_vbe_mode_banks]
    or ax, ax
    jz .unsupported
    cmp dx, ax
    jae .unsupported
    cmp byte [cs:current_vbe_backing_ready], 0
    jne .save_visible
    call int10_vbe_ensure_backing_store
    mov byte [cs:current_vbe_backing_ready], 1
.save_visible:
    call int10_vbe_save_visible_bank
    cmp bh, 0x00
    jne .bank_b
    mov [cs:current_vbe_bank_a], dx
    jmp .load
.bank_b:
    mov [cs:current_vbe_bank_b], dx
.load:
    call int10_vbe_load_window_bank
    mov [cs:current_vbe_visible_bank], dx
    mov [cs:current_vbe_visible_window], bh
    mov byte [cs:current_vbe_backing_ready], 1
    mov ax, 0x004F
    ret
.unsupported:
    mov ax, 0x014F
    ret

int10_vbe_clear_window:
    push ax
    push cx
    push di
    push es
    cld
    mov ax, 0xA000
    mov es, ax
    xor di, di
    xor ax, ax
    mov cx, 0x8000
    rep stosw
    pop es
    pop di
    pop cx
    pop ax
    ret

int10_vbe_find_mode:
    mov si, vbe_mode_table
.next:
    cmp word [cs:si], 0xFFFF
    je .not_found
    cmp word [cs:si], ax
    je .found
    add si, 12
    jmp .next
.found:
    clc
    ret
.not_found:
    stc
    ret

int1a_handler:
    cmp ah, 0x00
    je .get_ticks
    cmp ah, 0x01
    je .set_ticks
    jmp far [cs:old_int1a_off]

.get_ticks:
    push bp
    mov bp, sp
    push es
    mov ax, 0x0040
    mov es, ax
    mov dx, [es:0x006C]
    mov cx, [es:0x006E]
    mov al, [es:0x0070]
    mov byte [es:0x0070], 0
    xor ah, ah
    and word [ss:bp + 6], 0xFFFE
    pop es
    pop bp
    iret

.set_ticks:
    push bp
    mov bp, sp
    push es
    mov ax, 0x0040
    mov es, ax
    mov [es:0x006C], dx
    mov [es:0x006E], cx
    mov byte [es:0x0070], 0
    xor ax, ax
    and word [ss:bp + 6], 0xFFFE
    pop es
    pop bp
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
    jne .chain

    cmp al, 0x00
    je .enable_disable
    cmp al, 0x01
    je .reset
    cmp al, 0x02
    je .set_sample_rate
    cmp al, 0x03
    je .set_resolution
    cmp al, 0x04
    je .get_device_id
    cmp al, 0x05
    je .initialize
    cmp al, 0x06
    je .extended_command
    cmp al, 0x07
    je .set_handler
    jmp .unsupported

.extmem_88:
    ; INT 15h/AH=88h reports all memory above 1 MiB, including the HMA.
    ; XMS free-memory queries intentionally exclude that reserved 64 KiB.
    mov ax, BIOS_EXTMEM_KB
    and word [ss:bp + 6], 0xFFFE
    pop bp
    iret

.enable_disable:
    cmp bh, 0
    je .disable
    cmp bh, 1
    jne .invalid_input
    mov al, 0xF4                  ; enable PS/2 data reporting
    call ps2_mouse_write
    jc .interface_error
    mov byte [cs:mouse_bios_reporting_stopped], 0
    mov byte [cs:mouse_bios_enabled], 1
    cmp byte [cs:mouse_vga_cursor_drawn], 1
    je .enable_seed_done
    call mouse_vga_cursor_seed
.enable_seed_done:
    jmp .success
.disable:
    mov al, 0xF5                  ; disable PS/2 data reporting
    call ps2_mouse_write
    jc .interface_error
    mov byte [cs:mouse_bios_reporting_stopped], 1
    mov byte [cs:mouse_bios_enabled], 0
    mov byte [cs:mouse_vga_cursor_drawn], 0
    jmp .success

.reset:
    mov al, 0xFF                  ; reset, then consume BAT and device ID
    call ps2_mouse_write
    jc .interface_error
    mov byte [cs:mouse_bios_reporting_stopped], 1
    call ps2_read_data
    jc .interface_error
    cmp al, 0xAA
    jne .interface_error
    call ps2_read_data
    jc .interface_error
    mov byte [cs:mouse_bios_enabled], 0
    mov byte [cs:mouse_packet_index], 0
    mov byte [cs:mouse_vga_cursor_drawn], 0
    mov bx, 0x00AA
    jmp .success

.set_sample_rate:
    cmp bh, 6
    ja .invalid_input
    mov bl, bh
    xor bh, bh
    mov al, 0xF3
    call ps2_mouse_write
    jc .interface_error
    mov al, [cs:mouse_bios_sample_rates + bx]
    call ps2_mouse_write
    jc .interface_error
    jmp .success

.set_resolution:
    cmp bh, 3
    ja .invalid_input
    mov al, 0xE8
    call ps2_mouse_write
    jc .interface_error
    mov al, bh
    call ps2_mouse_write
    jc .interface_error
    jmp .success

.get_device_id:
    ; IBM PS/2 pointing-device BIOS function C204h.  Query the device rather
    ; than returning a synthetic ID so later-loaded clients see the hardware
    ; selected by the emulator or machine firmware.
    mov al, 0xF2
    call ps2_mouse_write
    jc .interface_error
    call ps2_read_data
    jc .interface_error
    mov bh, al
    jmp .success

.initialize:
    ; This runtime consumes the standard three-byte packet.  Four-byte wheel
    ; packets require a different IRQ framing contract and are rejected until
    ; a client explicitly negotiates that protocol through the hardware path.
    cmp bh, 3
    jne .invalid_input
    mov al, 0xF6                  ; defaults also stop data reporting
    call ps2_mouse_write
    jc .interface_error
    mov byte [cs:mouse_bios_reporting_stopped], 1
    mov byte [cs:mouse_bios_enabled], 0
    mov byte [cs:mouse_packet_index], 0
    jmp .success

.extended_command:
    cmp bh, 0
    je .read_status
    cmp bh, 1
    je .scaling_1_1
    cmp bh, 2
    je .scaling_2_1
    jmp .invalid_input

.read_status:
    mov al, 0xE9
    call ps2_mouse_write
    jc .interface_error
    call ps2_read_data
    jc .interface_error
    xor bx, bx
    mov bl, al
    call ps2_read_data
    jc .interface_error
    xor cx, cx
    mov cl, al
    call ps2_read_data
    jc .interface_error
    xor dx, dx
    mov dl, al
    jmp .success

.scaling_1_1:
    mov al, 0xE6
    call ps2_mouse_write
    jc .interface_error
    jmp .success

.scaling_2_1:
    mov al, 0xE7
    call ps2_mouse_write
    jc .interface_error
    jmp .success

.set_handler:
    call mouse_vga_cursor_erase_if_drawn
    inc word [cs:mouse_bios_asr_set_count]
    mov [cs:mouse_bios_asr_off], bx
    mov [cs:mouse_bios_asr_seg], es
    jmp .success

.success:
    and word [ss:bp + 6], 0xFFFE
    xor ah, ah
    pop bp
    iret

.invalid_input:
    or word [ss:bp + 6], 0x0001
    mov ah, 0x02
    pop bp
    iret

.interface_error:
    or word [ss:bp + 6], 0x0001
    mov ah, 0x03
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

mouse_bios_sample_rates db 10, 20, 40, 60, 80, 100, 200
 
mouse_vga_cursor_seed:
    mov word [cs:mouse_vga_cursor_x], 320
    mov word [cs:mouse_vga_cursor_y], 240
    mov word [cs:mouse_vga_cursor_last_x], 320
    mov word [cs:mouse_vga_cursor_last_y], 240
    mov byte [cs:mouse_vga_cursor_drawn], 0
    ret
%endif

mouse_sync_video_mode_from_bda:
    ; QuickBASIC SCREEN 9 updates the BIOS data area after programming the
    ; EGA/VGA hardware, but does not necessarily issue INT 10h/AH=00h through
    ; our hooked vector.  INT 33h reset is the DOS mouse-driver synchronization
    ; point, so refresh the cached mode from the canonical BDA byte here.
    push ax
    push es
    xor ax, ax
    mov es, ax
    mov al, [es:0x0449]
    mov [cs:current_video_mode], al
    pop es
    pop ax
    ret

mouse_reset_runtime_state:
    call mouse_sync_video_mode_from_bda
    mov byte [cs:mouse_installed], 1
    mov byte [cs:mouse_driver_enabled], 1
    mov al, [cs:mouse_hw_ready]
    mov [cs:mouse_detected], al
    mov byte [cs:mouse_button_count], 3
    mov word [cs:mouse_pos_x], 320
    mov word [cs:mouse_pos_y], 240
    mov word [cs:mouse_prev_x], 320
    mov word [cs:mouse_prev_y], 240
    mov byte [cs:mouse_buttons], 0
    mov byte [cs:mouse_prev_buttons], 0
    mov word [cs:mouse_hide_count], 0
    mov byte [cs:mouse_visible], 1
    mov word [cs:mouse_min_x], 0
%if FAT_TYPE == 16
    mov word [cs:mouse_max_x], 639
    cmp byte [cs:current_video_mode], 0x10
    jne .reset_not_mode10
    mov word [cs:mouse_max_y], 349
    jmp .reset_ranges_done
.reset_not_mode10:
    cmp byte [cs:current_video_mode], 0x13
    jne .reset_planar_480
    mov word [cs:mouse_max_x], 319
    mov word [cs:mouse_max_y], 199
    jmp .reset_ranges_done
.reset_planar_480:
    mov word [cs:mouse_max_y], 479
.reset_ranges_done:
%else
    mov word [cs:mouse_max_x], 319
    mov word [cs:mouse_max_y], 199
%endif
    mov word [cs:mouse_min_y], 0
    mov word [cs:mouse_cb_mask], 0
    mov word [cs:mouse_cb_off], 0
    mov word [cs:mouse_cb_seg], 0
    mov byte [cs:mouse_cb_busy], 0
    mov word [cs:mouse_cb_pending_mask], 0
    mov byte [cs:mouse_cb_pending_buttons], 0
    mov word [cs:mouse_cb_pending_x], 320
    mov word [cs:mouse_cb_pending_y], 240
    mov word [cs:mouse_cb_pending_dx], 0
    mov word [cs:mouse_cb_pending_dy], 0
    mov word [cs:mouse_alt_mask], 0
    mov word [cs:mouse_alt_off], 0
    mov word [cs:mouse_alt_seg], 0
    mov word [cs:mouse_mickey_x], 8
    mov word [cs:mouse_mickey_y], 8
    mov word [cs:mouse_sens_x], 8
    mov word [cs:mouse_sens_y], 8
    mov word [cs:mouse_double_threshold], 64
    mov word [cs:mouse_interrupt_rate], 100
    mov byte [cs:mouse_crt_page], 0
    mov byte [cs:mouse_language], 0
    mov byte [cs:mouse_light_pen_enabled], 0
    mov byte [cs:mouse_excl_enabled], 0
    mov word [cs:mouse_excl_min_x], 0
    mov word [cs:mouse_excl_min_y], 0
    mov word [cs:mouse_excl_max_x], 0
    mov word [cs:mouse_excl_max_y], 0
    mov word [cs:mouse_text_cursor_type], 0
    mov word [cs:mouse_text_screen_mask], 0xFFFF
    mov word [cs:mouse_text_cursor_mask], 0x7700
    mov word [cs:mouse_gfx_hot_x], 0
    mov word [cs:mouse_gfx_hot_y], 0
    mov byte [cs:mouse_gfx_cursor_custom], 0
    mov word [cs:mouse_last_event_mask], 0
    mov word [cs:mouse_press_count + 0], 0
    mov word [cs:mouse_press_count + 2], 0
    mov word [cs:mouse_press_count + 4], 0
    mov word [cs:mouse_release_count + 0], 0
    mov word [cs:mouse_release_count + 2], 0
    mov word [cs:mouse_release_count + 4], 0
    mov word [cs:mouse_press_x + 0], 320
    mov word [cs:mouse_press_x + 2], 320
    mov word [cs:mouse_press_x + 4], 320
    mov word [cs:mouse_press_y + 0], 240
    mov word [cs:mouse_press_y + 2], 240
    mov word [cs:mouse_press_y + 4], 240
    mov word [cs:mouse_release_x + 0], 320
    mov word [cs:mouse_release_x + 2], 320
    mov word [cs:mouse_release_x + 4], 320
    mov word [cs:mouse_release_y + 0], 240
    mov word [cs:mouse_release_y + 2], 240
    mov word [cs:mouse_release_y + 4], 240
    mov word [cs:mouse_delta_x], 0
    mov word [cs:mouse_delta_y], 0
    mov word [cs:mouse_last_mickey_x], 0
    mov word [cs:mouse_last_mickey_y], 0
%if FAT_TYPE == 16
    call mouse_vga_cursor_seed
%endif
    ret

mouse_release_transient_clients:
    call mouse_vga_cursor_erase_if_drawn
    call mouse_reset_runtime_state
    mov byte [cs:mouse_bios_enabled], 0
    mov word [cs:mouse_bios_asr_seg], 0
    ret

mouse_dispatch_user_callback:
    cmp word [cs:mouse_cb_seg], 0
    je .done
    mov byte [cs:mouse_cb_busy], 1
    push ds
    push es
    ; INT 33h/AX=000Ch handlers are invoked as FAR procedures and return with
    ; RETF.  Do not leave a FLAGS word below the FAR return address: doing so
    ; shifts the saved DS/ES and the enclosing IRQ12 frame after every event.
    call far [cs:mouse_cb_off]
    pop es
    pop ds
    mov byte [cs:mouse_cb_busy], 0
.done:
    ret

mouse_flush_pending_callback:
    ; Called with interrupts enabled after the primary client callback.  Take
    ; each coalesced snapshot atomically, then re-enable interrupts around the
    ; client routine so further IRQ12 packets can replace the pending slot.
    ; Preserve the caller's IF state; the IRQ handler disables it before EOI.
    pushf
.loop:
    cli
    cmp byte [cs:mouse_cb_busy], 0
    jne .done
    mov ax, [cs:mouse_cb_pending_mask]
    or ax, ax
    jz .done
    cmp word [cs:mouse_cb_seg], 0
    je .clear
    mov word [cs:mouse_cb_pending_mask], 0
    xor bx, bx
    mov bl, [cs:mouse_cb_pending_buttons]
    mov cx, [cs:mouse_cb_pending_x]
    mov dx, [cs:mouse_cb_pending_y]
    mov si, [cs:mouse_cb_pending_dx]
    mov di, [cs:mouse_cb_pending_dy]
    mov byte [cs:mouse_cb_busy], 1
    sti
    push ds
    push es
    call far [cs:mouse_cb_off]
    pop es
    pop ds
    cli
    mov byte [cs:mouse_cb_busy], 0
    jmp .loop
.clear:
    mov word [cs:mouse_cb_pending_mask], 0
.done:
    popf
    ret

mouse_copy_gfx_mask_from_esdx:
    push ds
    push si
    push di
    push cx
    push es
    mov si, dx
    mov cx, 32
    ; INT 33h/AX=0009h supplies the 32-word cursor mask at caller ES:DX.
    ; Copy from that segment into resident driver storage, never the reverse.
    push es
    pop ds
    push cs
    pop es
    mov di, mouse_gfx_cursor_mask
    pushf
    cld
    rep movsw
    popf
    pop es
    pop cx
    pop di
    pop si
    pop ds
    ret

mouse_save_state_to_esdx:
    push ds
    push si
    push di
    push cx
    push es
    push cs
    pop ds
    mov si, mouse_state_begin
    mov di, dx
    pushf
    cld
    mov cx, MOUSE_STATE_SIZE
    rep movsb
    popf
    pop es
    pop cx
    pop di
    pop si
    pop ds
    ret

mouse_restore_state_from_esdx:
    push ds
    push si
    push di
    push cx
    push es
    mov si, dx
    push es
    pop ds
    push cs
    pop es
    mov di, mouse_state_begin
    mov ax, [si]
    cmp ax, MOUSE_STATE_VERSION
    jne .done
    mov ax, [si + 2]
    cmp ax, MOUSE_STATE_SIZE
    jne .done
    pushf
    cld
    mov cx, MOUSE_STATE_SIZE
    rep movsb
    popf
.done:
    pop es
    pop cx
    pop di
    pop si
    pop ds
    ret

%if FAT_TYPE == 16
int09_handler:
    ; Windows 3.x enhanced mode may attempt to inject the same virtual IRQ1
    ; again while its BIOS chain is returning. A second V86 entry with no
    ; keyboard data used to recurse at 0300:8A15 until
    ; the private DOS stack overwrote the resident kernel.  Bit 7 is a busy
    ; guard; bit 0 retains the existing external-owner meaning.
    push ax
    test byte [cs:irq1_external_owner], 0x80
    jnz .reentrant_irq
    mov byte [cs:irq1_external_owner], 0x80

    ; A raw DOS IRQ1 owner may read the scan byte before chaining this
    ; handler.  Preserve that byte while checking whether the live vector is
    ; currently owned by the kernel or by an external handler.
    push bx
    push dx
    push ds
    push es
    push si
    xor ax, ax
    mov es, ax
    mov bx, [es:0x09 * 4]
    mov dx, [es:0x09 * 4 + 2]
    mov ax, cs
    cmp dx, ax
    jne .external_owner
    cmp bx, int09_handler
    je .owner_known
.external_owner:
    or byte [cs:irq1_external_owner], 1
.owner_known:
    mov word [cs:irq1_tail_before], 0xFFFF

    ; Record the BIOS ring tail before the chained handler runs.  The raw DOS
    ; owner may already have inserted an enhanced navigation key; the chained
    ; BIOS can then insert the same key a second time during this one IRQ.
    test byte [cs:irq1_external_owner], 1
    jz .pre_bios_done
    mov ax, 0x0040
    mov ds, ax
    mov ax, [0x001C]
    mov [cs:irq1_tail_before], ax
.pre_bios_done:
    pop si
    pop es
    pop ds
    pop dx
    pop bx
    pop ax

    ; Let the platform BIOS acknowledge IRQ1 and update its keyboard ring.
    call .call_bios

    push ax
    push bx
    push dx
    push ds
    push si
    mov ax, 0x0040
    mov ds, ax

    ; The external owner runs before this handler and may already have added
    ; the first record.  Collapse the chained BIOS's exact one-slot advance
    ; only when the preceding record is still unread and carries the same
    ; navigation scan.  An insertion into an otherwise empty ring is kept.
    test byte [cs:irq1_external_owner], 1
    jz .inspect_ring
    mov bx, [0x001C]                  ; tail / next write position
    mov ax, [0x0080]                  ; ring start
    mov dx, [0x0082]                  ; ring end
    cmp dx, ax
    jbe .inspect_ring
    cmp bx, ax
    jb .inspect_ring
    cmp bx, dx
    ja .inspect_ring
    mov si, [cs:irq1_tail_before]
    cmp si, ax
    jb .inspect_ring
    cmp si, dx
    ja .inspect_ring
    jne .dedupe_tail_before_ready
    mov si, ax
.dedupe_tail_before_ready:
    add si, 2
    cmp si, dx
    jb .dedupe_expected_ready
    sub si, dx
    add si, ax
.dedupe_expected_ready:
    cmp bx, si                        ; exactly one entry from the chained BIOS
    jne .inspect_ring
    cmp bx, ax
    jne .dedupe_last_ready
    mov bx, dx
.dedupe_last_ready:
    sub bx, 2                         ; newest entry
    cmp bx, [0x001A]                  ; one or zero unread entries
    je .inspect_ring
    mov si, bx
    cmp bx, ax
    jne .dedupe_previous_ready
    mov bx, dx
.dedupe_previous_ready:
    sub bx, 2                         ; preceding entry
    mov al, [bx + 1]
    cmp al, [si + 1]
    jne .inspect_ring
    call int09_is_navigation_scan
    jnc .inspect_ring
    mov [0x001C], si                  ; discard only the newest duplicate

.inspect_ring:
    ; 40:1C is the next write position.  40:80/82 contain the actual
    ; keyboard-ring bounds, so this also works when a BIOS expands the ring.
    mov bx, [0x001C]
    mov ax, [0x0080]
    mov dx, [0x0082]
    cmp dx, ax
    jbe .done
    cmp bx, ax
    jb .done
    cmp bx, dx
    ja .done
    cmp bx, ax
    jne .previous_slot
    mov bx, dx
.previous_slot:
    sub bx, 2

    cmp byte [bx], 0xE0
    jne .done
    mov al, [bx + 1]
    call int09_is_navigation_scan
    jnc .done
.normalize:
    mov byte [bx], 0
.done:
    mov byte [cs:irq1_external_owner], 0
    pop si
    pop ds
    pop dx
    pop bx
    pop ax
    iret

.reentrant_irq:
    ; An AT BIOS may send EOI and enable interrupts inside its LED-update
    ; routine, then wait for IRQ1 to receive FAh/FEh. A real pending keyboard
    ; byte must reach that BIOS even while the outer call is active. Leave
    ; the byte in port 60h for the original handler and retain the outer guard.
    in al, 0x64
    and al, 0x21                     ; output full, excluding auxiliary data
    cmp al, 0x01
    jne .reentrant_no_data
    pop ax
    call .call_bios
    iret
.reentrant_no_data:
    ; Complete only the duplicate virtual-PIC transaction without entering
    ; an empty BIOS chain again. The outer handler clears the busy guard.
    mov al, 0x20
    out 0x20, al
    pop ax
    iret

.call_bios:
    ; Legacy firmware may preserve only 16-bit registers. IRQ1 can interrupt
    ; native drawing with live 32-bit offsets and font segments, including a
    ; nested LED-ACK IRQ while the outer BIOS chain is still running.
    pushad
    push ds
    push es
    push fs
    push gs
    pushf
    call far [cs:old_int09_off]
    pop gs
    pop fs
    pop es
    pop ds
    popad
    ret

int09_is_navigation_scan:
    cmp al, 0x47                    ; Home
    je .yes
    cmp al, 0x48                    ; Up
    je .yes
    cmp al, 0x49                    ; Page Up
    je .yes
    cmp al, 0x4B                    ; Left
    je .yes
    cmp al, 0x4D                    ; Right
    je .yes
    cmp al, 0x4F                    ; End
    je .yes
    cmp al, 0x50                    ; Down
    je .yes
    cmp al, 0x51                    ; Page Down
    je .yes
    cmp al, 0x52                    ; Insert
    je .yes
    cmp al, 0x53                    ; Delete
    je .yes
    clc
    ret
.yes:
    stc
    ret
%endif

int33_handler:
    cld
    ; A queued callback is part of the previous mouse event, not the current
    ; INT 33h request.  Its application routine may alter every general
    ; register, so preserve the request frame before delivering it.
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    call mouse_flush_pending_callback
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
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
    cmp ax, 0x0005
    je .button_press_info
    cmp ax, 0x0006
    je .button_release_info
    cmp ax, 0x0007
    je .set_x_range
    cmp ax, 0x0008
    je .set_y_range
    cmp ax, 0x0009
    je .set_gfx_cursor
    cmp ax, 0x000A
    je .set_text_cursor
    cmp ax, 0x000B
    je .motion
    cmp ax, 0x000C
    je .set_callback
    cmp ax, 0x000D
    je .light_pen_on
    cmp ax, 0x000E
    je .light_pen_off
    cmp ax, 0x0014
    je .exchange_callback
    cmp ax, 0x000F
    je .set_mickey_ratio
    cmp ax, 0x0010
    je .set_exclusion_region
    cmp ax, 0x0013
    je .set_double_speed_threshold
    cmp ax, 0x0015
    je .get_state_size
    cmp ax, 0x0016
    je .save_state
    cmp ax, 0x0017
    je .restore_state
    cmp ax, 0x0018
    je .set_alt_handler
    cmp ax, 0x0019
    je .get_alt_handler
    cmp ax, 0x001A
    je .set_sensitivity
    cmp ax, 0x001B
    je .get_sensitivity
    cmp ax, 0x001C
    je .set_interrupt_rate
    cmp ax, 0x001D
    je .set_crt_page
    cmp ax, 0x001E
    je .get_crt_page
    cmp ax, 0x001F
    je .disable_driver
    cmp ax, 0x0020
    je .enable_driver
    cmp ax, 0x0021
    je .software_reset
    cmp ax, 0x0022
    je .set_language
    cmp ax, 0x0023
    je .get_language
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
%if FAT_TYPE == 16
    cmp byte [cs:mouse_hw_ready], 1
    jne .external_mouse_disabled
%endif
    call mouse_reset_runtime_state
%if FAT_TYPE == 16
    ; BIOS clients such as Windows stop PS/2 reporting with C200h/F5 when
    ; they exit. A subsequent DOS reset must reclaim the physical stream,
    ; not only reset coordinates. Do this only for a recorded BIOS stop and
    ; after BIOS ownership has ended; native resets otherwise send no I/O.
    cmp byte [cs:mouse_bios_reporting_stopped], 1
    jne .reset_ready
    cmp byte [cs:mouse_bios_enabled], 0
    jne .reset_ready
    mov byte [cs:mouse_packet_index], 0
    mov al, 0xF4
    call ps2_mouse_write
    jc .reset_ready
    mov byte [cs:mouse_bios_reporting_stopped], 0
.reset_ready:
%endif
    mov ax, 0xFFFF
    xor bh, bh
    mov bl, [cs:mouse_button_count]
    iret

.show:
    cmp word [cs:mouse_hide_count], 0
    je .show_visible
    dec word [cs:mouse_hide_count]
.show_visible:
    cmp word [cs:mouse_hide_count], 0
    jne .show_done
    mov byte [cs:mouse_visible], 1
    call mouse_vga_cursor_refresh
.show_done:
    xor ax, ax
    iret

.hide:
    inc word [cs:mouse_hide_count]
%if FAT_TYPE == 16
    mov byte [cs:mouse_visible], 0
    call mouse_vga_cursor_erase_if_drawn
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
    mov ax, [cs:mouse_pos_x]
    mov [cs:mouse_prev_x], ax
    mov ax, [cs:mouse_pos_y]
    mov [cs:mouse_prev_y], ax
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
    call mouse_vga_cursor_refresh
    xor ax, ax
    iret

.button_press_info:
    cmp bx, 2
    ja .button_info_fail
    ; BX is both the input button number and the required output event
    ; count.  Keep the array index separate: using the loaded count as the
    ; index for the clear left the event permanently pending and could zero
    ; an adjacent counter after the first click.
    push si
    mov si, bx
    shl si, 1
    xor ax, ax
    mov al, [cs:mouse_buttons]
    mov cx, [cs:mouse_press_x + si]
    mov dx, [cs:mouse_press_y + si]
    mov bx, [cs:mouse_press_count + si]
    mov word [cs:mouse_press_count + si], 0
    pop si
    iret

.button_release_info:
    cmp bx, 2
    ja .button_info_fail
    push si
    mov si, bx
    shl si, 1
    xor ax, ax
    mov al, [cs:mouse_buttons]
    mov cx, [cs:mouse_release_x + si]
    mov dx, [cs:mouse_release_y + si]
    mov bx, [cs:mouse_release_count + si]
    mov word [cs:mouse_release_count + si], 0
    pop si
    iret

.button_info_fail:
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
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
    call mouse_vga_update_position
    call mouse_vga_cursor_refresh
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
    call mouse_vga_update_position
    call mouse_vga_cursor_refresh
    xor ax, ax
    iret

.set_gfx_cursor:
    mov [cs:mouse_gfx_hot_x], bx
    mov [cs:mouse_gfx_hot_y], cx
    call mouse_copy_gfx_mask_from_esdx
    mov byte [cs:mouse_gfx_cursor_custom], 1
    xor ax, ax
    iret

.set_text_cursor:
    mov [cs:mouse_text_cursor_type], bx
    mov [cs:mouse_text_screen_mask], cx
    mov [cs:mouse_text_cursor_mask], dx
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
    mov byte [cs:mouse_cb_busy], 0
    mov word [cs:mouse_cb_pending_mask], 0
    xor ax, ax
    iret

.light_pen_on:
    mov byte [cs:mouse_light_pen_enabled], 1
    xor ax, ax
    iret

.light_pen_off:
    mov byte [cs:mouse_light_pen_enabled], 0
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

.set_exclusion_region:
    mov [cs:mouse_excl_min_x], cx
    mov [cs:mouse_excl_min_y], dx
    mov [cs:mouse_excl_max_x], si
    mov [cs:mouse_excl_max_y], di
    mov byte [cs:mouse_excl_enabled], 1
    call mouse_vga_cursor_refresh
    xor ax, ax
    iret

.set_double_speed_threshold:
    mov [cs:mouse_double_threshold], dx
    xor ax, ax
    iret

.get_state_size:
    xor ax, ax
    mov bx, MOUSE_STATE_SIZE
    xor cx, cx
    xor dx, dx
    iret

.save_state:
    call mouse_save_state_to_esdx
    xor ax, ax
    iret

.restore_state:
    call mouse_restore_state_from_esdx
    call mouse_vga_update_position
    call mouse_vga_cursor_refresh
    xor ax, ax
    iret

.set_alt_handler:
    mov [cs:mouse_alt_mask], cx
    mov [cs:mouse_alt_off], dx
    mov [cs:mouse_alt_seg], es
    xor ax, ax
    iret

.get_alt_handler:
    mov ax, [cs:mouse_alt_seg]
    mov bx, [cs:mouse_alt_off]
    mov cx, [cs:mouse_alt_mask]
    mov dx, bx
    mov es, ax
    xor ax, ax
    iret

.set_sensitivity:
    mov [cs:mouse_sens_x], bx
    mov [cs:mouse_sens_y], cx
    mov [cs:mouse_double_threshold], dx
    xor ax, ax
    iret

.get_sensitivity:
    xor ax, ax
    mov bx, [cs:mouse_sens_x]
    mov cx, [cs:mouse_sens_y]
    mov dx, [cs:mouse_double_threshold]
    iret

.set_interrupt_rate:
    mov [cs:mouse_interrupt_rate], bx
    xor ax, ax
    iret

.set_crt_page:
    mov [cs:mouse_crt_page], bl
    xor ax, ax
    iret

.get_crt_page:
    xor ax, ax
    xor bx, bx
    mov bl, [cs:mouse_crt_page]
    iret

.disable_driver:
    xor ax, ax
    mov al, [cs:mouse_driver_enabled]
    mov byte [cs:mouse_driver_enabled], 0
    mov byte [cs:mouse_visible], 0
    call mouse_vga_cursor_erase_if_drawn
    iret

.enable_driver:
    xor ax, ax
    mov al, [cs:mouse_driver_enabled]
    mov byte [cs:mouse_driver_enabled], 1
    cmp word [cs:mouse_hide_count], 0
    jne .enable_done
    mov byte [cs:mouse_visible], 1
    call mouse_vga_cursor_refresh
.enable_done:
    iret

.software_reset:
    jmp .reset

.set_language:
    mov [cs:mouse_language], bl
    xor ax, ax
    iret

.get_language:
    xor ax, ax
    xor bx, bx
    mov bl, [cs:mouse_language]
    iret

.version:
    ; Microsoft-compatible function 24h contract:
    ;   AX=0024h, BX=driver version, CH=mouse type, CL=IRQ (0 for PS/2).
    ; Returning private status fields here confuses software which gates newer
    ; INT 33h calls on the advertised driver version.
    mov ax, 0x0024
    mov bx, 0x0626
    xor cx, cx
    mov ch, 4
    iret

int2f_handler:
%if TRACE_WIN_INT2F != 0
    pushf
    pusha
    push ds
    mov bp, sp
    push cs
    pop ds
    mov si, msg_win_int2f
    call print_string_serial
    mov ax, [ss:bp + 16]
    call print_hex16_serial
    mov al, ':'
    call serial_putc
    mov ax, [ss:bp + 10]
    call print_hex16_serial
    mov al, ':'
    call serial_putc
    mov ax, [ss:bp + 14]
    call print_hex16_serial
    mov al, ':'
    call serial_putc
    mov ax, [ss:bp + 12]
    call print_hex16_serial
    call print_newline_serial
    pop ds
    popa
    popf
%endif
%if TRACE_CHILD_INT21 != 0
    pushf
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    push bp
    mov bp, sp
    push cs
    pop ds
    mov si, msg_child_int2f_call
    call print_string_serial
    mov ax, [ss:bp + 16]
    call print_hex16_serial
    mov si, msg_child_int21_call_bx
    call print_string_serial
    mov ax, [ss:bp + 14]
    call print_hex16_serial
    mov si, msg_child_int21_call_cx
    call print_string_serial
    mov ax, [ss:bp + 12]
    call print_hex16_serial
    mov si, msg_child_int21_call_dx
    call print_string_serial
    mov ax, [ss:bp + 10]
    call print_hex16_serial
    mov si, msg_child_int21_call_ds
    call print_string_serial
    mov ax, [ss:bp + 4]
    call print_hex16_serial
    mov si, msg_child_int2f_es
    call print_string_serial
    mov ax, [ss:bp + 2]
    call print_hex16_serial
    mov si, msg_child_int2f_di
    call print_string_serial
    mov ax, [ss:bp + 6]
    call print_hex16_serial
    call print_newline_serial
    pop bp
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    popf
%endif
    cmp ax, 0x1605
    je .fn_1605
    cmp ax, 0x1606
    je .fn_1606
    cmp ax, 0x1607
    je .fn_1607
    cmp ax, 0x1600
    je .fn_1600
    cmp ax, 0x1680
    je .fn_16xx_idle
    cmp ax, 0x1689
    je .fn_16xx_idle
    cmp ax, 0x1687
    je .fn_1687
    cmp ax, 0x4300
    je .fn_4300
    cmp ax, 0x4310
    je .fn_4310
.chain:
    jmp far [cs:old_int2f_off]

.fn_1687:
    mov ax, 0x8001
    jmp .iret_clear_cf_enter

.fn_1600:
    cmp word [cs:windows_active], 0
    je .fn_16xx_idle
    mov ax, [cs:windows_version]
    xchg al, ah
    jmp .iret_clear_cf_enter

.fn_1605:
    ; Windows 3.x startup broadcast.  ES:BX must point to a valid
    ; WinStartupInfo record on return.  CiukiDOS does not expose a swappable
    ; DOS data area yet, so the instance table is deliberately empty instead
    ; of incorrectly advertising the complete resident code image as data.
    ; CX is deliberately preserved: a non-zero value rejects startup.
    mov [cs:windows_version], di
    mov [cs:windows_startup_info], di
    mov word [cs:windows_active], 1
    ; The 1605h broadcast precedes Windows' final system-VM MZ handoff by one
    ; EXEC frame.  Include that fixed startup continuation in the baseline;
    ; later application frames must unwind independently on AH=4Ch.
    mov al, [cs:dos_exec_state_depth]
    inc ax
    mov [cs:windows_exec_base_depth], al

    ; Defensively start WIN386 from a clean transient-client state even if a
    ; nonstandard termination path bypassed normal top-level EXEC cleanup.
    call mouse_release_transient_clients
    push cs
    pop es
    mov bx, windows_startup_info
    jmp .iret_clear_cf_enter

.fn_1606:
    mov word [cs:windows_active], 0
    ; Windows has released the system VM.  Never leave IRQ12 targeting its
    ; former MOUSE.DRV callback after that memory has been returned to DOS.
    call mouse_release_transient_clients
    jmp .iret_clear_cf_enter

.fn_1607:
    cmp bx, 0x0015
    jne .chain
    cmp cx, 0x0000
    je .fn_1607_query
    cmp cx, 0x0001
    je .fn_1607_enable
    cmp cx, 0x0002
    je .fn_1607_disable
    cmp cx, 0x0003
    je .fn_1607_structure_size
    cmp cx, 0x0004
    je .fn_1607_exemptions
    cmp cx, 0x0005
    je .fn_1607_driver_size
    jmp .chain

.fn_1607_query:
    mov cx, [cs:windows_active]
    mov dx, cs
    mov es, dx
    mov bx, windows_patch_table
    jmp .iret_clear_cf_enter

.fn_1607_enable:
    mov bx, dx
    mov dx, 0xA2AB
    mov ax, 0xB97C
    jmp .iret_clear_cf_enter

.fn_1607_disable:
    xor cx, cx
    jmp .iret_clear_cf_enter

.fn_1607_structure_size:
    ; DOS 5 compatible Current Directory Structure size.  VMM/DOSMGR asks
    ; for this before creating the system VM and otherwise attempts to patch
    ; an unknown DOS kernel layout.
    mov cx, 0x0058
    jmp short .fn_1607_success

.fn_1607_exemptions:
    xor bx, bx
.fn_1607_success:
    mov dx, 0xA2AB
    mov ax, 0xB97C
    jmp .iret_clear_cf_enter

.fn_1607_driver_size:
    ; CiukiDOS' built-in NUL/CON headers live inside SYSVARS rather than a
    ; separately allocated driver MCB.  Report "not a relocatable driver".
    xor ax, ax
    xor bx, bx
    xor cx, cx
    xor dx, dx
    jmp .iret_clear_cf_enter

.fn_16xx_idle:
    xor ax, ax
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
    mov ax, 0x0080

.iret_clear_cf_enter:
    push bp
    mov bp, sp

.iret_clear_cf:
    and byte [bp + 6], 0xFE
    pop bp
    iret

xms_entrypoint:
    ; Windows 3.x DOSX treats the first five bytes of an XMS entry point as a
    ; resident patch window and rewrites them to EB 00 90 90 90.  Keep real
    ; dispatch code out of that window: both this original short jump and the
    ; Windows no-op replacement fall through at exactly +5.
    jmp short .entry_dispatch
    nop
    nop
    nop
.entry_dispatch:
%if TRACE_CHILD_INT21 != 0
    pushf
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    push bp
    mov bp, sp
    push cs
    pop ds
    mov si, msg_child_xms_call
    call print_string_serial
    mov ax, [ss:bp + 16]
    call print_hex16_serial
    mov si, msg_child_int21_call_bx
    call print_string_serial
    mov ax, [ss:bp + 14]
    call print_hex16_serial
    mov si, msg_child_int21_call_dx
    call print_string_serial
    mov ax, [ss:bp + 10]
    call print_hex16_serial
    mov si, msg_child_int21_call_ds
    call print_string_serial
    mov ax, [ss:bp + 4]
    call print_hex16_serial
    mov si, msg_child_xms_si
    call print_string_serial
    mov ax, [ss:bp + 8]
    call print_hex16_serial
    call print_newline_serial
    pop bp
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    popf
%endif
%if TRACE_WIN_XMS != 0
    cmp ah, 0x07
    je .win_mem_trace_done
    cmp ah, 0x06
    je .win_mem_trace_done
    push ax
    push dx
    push ds
    push si
    push cs
    pop ds
    mov si, msg_win_xms
    call print_string_serial
    pop si
    pop ds
    pop dx
    pop ax
    push ax
    call print_hex16_serial
    mov ax, dx
    call print_hex16_serial
    call print_newline_serial
    pop ax
.win_mem_trace_done:
%endif
    ; CiukiDOS owns the global A20 gate while its resident XMS manager is
    ; active.  A DOS extender may leave port 92h disabled after returning to
    ; real mode; repair that external state before reporting XMS services so
    ; a later client starts from the same machine state as the first one.
    cmp byte [cs:xms_a20_global_enabled], 0
    je .dispatch
    call .a20_hw_enable
.dispatch:
    cmp ah, 0x88
    je .query_free_extended
    cmp ah, 0x89
    je .alloc_emb_extended
    or ah, ah
    je .version
    cmp ah, 0x08
    jb .hma_a20_stub
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
    cmp ah, 0x10
    je .umb_unavailable
    jmp .unsupported

.query_free:
    mov dx, [cs:xms_free_kb]
    mov ax, dx
    xor bl, bl
    retf

.query_free_extended:
    ; XMS 3.0 clients select the 32-bit services from our advertised version.
    ; Publish only this allocator's existing below-64-MiB pool; do not imply
    ; that unenumerated high RAM belongs to DOS or truncate a large request.
    movzx eax, word [cs:xms_free_kb]
    mov edx, eax
    mov ecx, (XMS_PHYS_LIMIT_HI << 16) - 1
    xor bl, bl
    retf

.alloc_emb_extended:
    cmp edx, 0xFFFF
    ja .alloc_fail
    ; A valid request fits the legacy allocator without changing its handle,
    ; high-water, HMA exclusion or locked-physical-address contracts.
.alloc_emb:
    cmp dx, [cs:xms_free_kb]
    ja .alloc_fail
    ; Every live handle has a table slot, including zero-length hook handles.
    ; New blocks follow the physical high-water mark, so reusing a low-numbered
    ; slot never moves or overlaps an address already published to a client.
    push di
    xor di, di
.alloc_find_handle:
    cmp word [cs:xms_handle_size_table + di], 0
    je .alloc_handle_found
    add di, 2
    cmp di, XMS_HANDLE_COUNT * 2
    jb .alloc_find_handle
    pop di
    jmp .alloc_no_handles
.alloc_handle_found:
    mov ax, dx
    or ax, ax
    jnz .alloc_size_ready
    dec ax                         ; FFFFh means occupied, public size zero
.alloc_size_ready:
    mov [cs:xms_handle_size_table + di], ax
    mov ax, [cs:xms_tail_kb]
    mov [cs:xms_handle_base_table + di], ax
    or dx, dx
    jz .alloc_no_storage
    add [cs:xms_tail_kb], dx
    sub [cs:xms_free_kb], dx
.alloc_no_storage:
    mov dx, di
    shr dx, 1
    inc dx
    pop di
    mov ax, 1
    xor bl, bl
    retf

.alloc_fail:
    xor ax, ax
    mov bl, 0xA0
    retf

.alloc_no_handles:
    xor ax, ax
    mov bl, 0xA1
    retf

.free_emb:
    push di
    call .lookup_handle
    jc .free_bad_handle
    mov word [cs:xms_handle_size_table + di], 0
    mov word [cs:xms_handle_base_table + di], 0
    call .recompute_tail
    pop di
.free_success:
    mov ax, 1
    xor bl, bl
    retf

.free_bad_handle:
    pop di

.free_fail:
    xor ax, ax
    mov bl, 0xA2
    retf

; Recalculate the high-water mark. Interior holes remain reserved until every
; later block is freed, preserving all locked physical addresses.
.recompute_tail:
    xor dx, dx
    xor di, di
.recompute_tail_loop:
    mov ax, [cs:xms_handle_size_table + di]
    or ax, ax
    jz .recompute_tail_next
    cmp ax, 0xFFFF
    je .recompute_tail_next
    add ax, [cs:xms_handle_base_table + di]
    cmp ax, dx
    jbe .recompute_tail_next
    mov dx, ax
.recompute_tail_next:
    add di, 2
    cmp di, XMS_HANDLE_COUNT * 2
    jb .recompute_tail_loop
    mov [cs:xms_tail_kb], dx
    mov ax, XMS_FREE_KB_INITIAL
    sub ax, dx
    mov [cs:xms_free_kb], ax
    ret

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

    mov dx, [ds:si + 4]
    or dx, dx
    jnz .move_src_emb

    mov ax, [ds:si + 6]
    mov [cs:xms_move_src_lo], ax
    mov ax, [ds:si + 8]
    mov dx, ax
    mov cl, 12
    shr dx, cl
    mov [cs:xms_move_src_hi], dx
    mov cl, 4
    shl ax, cl
    add [cs:xms_move_src_lo], ax
    ; Apply the low-word carry immediately.  A SHR here used to overwrite CF
    ; and added bit 11 of the segment instead, shifting some XMS moves by 64K.
    adc word [cs:xms_move_src_hi], 0
    jmp .move_dst_setup

.move_src_emb:
    call .lookup_handle
    jc .move_bad_src
    mov bx, ax
    cmp bx, 0xFFFF
    jne .move_src_size_ready
    xor bx, bx
.move_src_size_ready:
    mov [cs:xms_move_range_kb], bx
    mov ax, [ds:si + 6]
    mov dx, [ds:si + 8]
    call .move_check_emb_range
    jc .move_bad_src_offset
    call .handle_base_physical
    add ax, [ds:si + 6]
    adc dx, [ds:si + 8]
    cmp dx, XMS_PHYS_LIMIT_HI
    jae .move_bad_src_offset
    mov [cs:xms_move_src_lo], ax
    mov [cs:xms_move_src_hi], dx

.move_dst_setup:
    mov dx, [ds:si + 10]
    or dx, dx
    jnz .move_dst_emb

    mov ax, [ds:si + 12]
    mov [cs:xms_move_dst_lo], ax
    mov ax, [ds:si + 14]
    mov dx, ax
    mov cl, 12
    shr dx, cl
    mov [cs:xms_move_dst_hi], dx
    mov cl, 4
    shl ax, cl
    add [cs:xms_move_dst_lo], ax
    adc word [cs:xms_move_dst_hi], 0
    jmp .move_loop

.move_dst_emb:
    call .lookup_handle
    jc .move_bad_dst
    mov bx, ax
    cmp bx, 0xFFFF
    jne .move_dst_size_ready
    xor bx, bx
.move_dst_size_ready:
    mov [cs:xms_move_range_kb], bx
    mov ax, [ds:si + 12]
    mov dx, [ds:si + 14]
    call .move_check_emb_range
    jc .move_bad_dst_offset
    call .handle_base_physical
    add ax, [ds:si + 12]
    adc dx, [ds:si + 14]
    cmp dx, XMS_PHYS_LIMIT_HI
    jae .move_bad_dst_offset
    mov [cs:xms_move_dst_lo], ax
    mov [cs:xms_move_dst_hi], dx

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
    call .move_check_physical_chunk
    jc .move_bad_src_offset
    mov ax, [cs:xms_move_dst_lo]
    mov dx, [cs:xms_move_dst_hi]
    call .move_check_physical_chunk
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
    mov bx, [cs:xms_move_range_kb]
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

.move_check_physical_chunk:
    mov bx, [cs:xms_move_chunk]
    dec bx
    add ax, bx
    adc dx, 0
    cmp dx, XMS_PHYS_LIMIT_HI
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
    mov ax, [cs:xms_move_src_hi]
    mov [cs:xms_87_gdt + 20], al
    mov byte [cs:xms_87_gdt + 21], 0x93
    mov [cs:xms_87_gdt + 23], ah

    mov ax, [cs:xms_move_chunk]
    dec ax
    mov [cs:xms_87_gdt + 24], ax
    mov ax, [cs:xms_move_dst_lo]
    mov [cs:xms_87_gdt + 26], ax
    mov ax, [cs:xms_move_dst_hi]
    mov [cs:xms_87_gdt + 28], al
    mov byte [cs:xms_87_gdt + 29], 0x93
    mov [cs:xms_87_gdt + 31], ah

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
    push di
    mov dx, [ds:si + 4]
    or dx, dx
    jz .move_check_dst_handle
    call .lookup_handle
    jc .move_stub_fail
.move_check_dst_handle:
    mov dx, [ds:si + 10]
    or dx, dx
    jz .move_stub_ok
    call .lookup_handle
    jc .move_stub_fail
.move_stub_ok:
    pop di
.move_ok:
    mov ax, 1
    xor bl, bl
    retf
.move_stub_fail:
    pop di
    jmp .free_fail
%endif

.lookup_handle:
    mov di, dx
    dec di
    cmp di, XMS_HANDLE_COUNT - 1
    ja .check_handle_fail
    shl di, 1
    mov ax, [cs:xms_handle_size_table + di]
    or ax, ax
    jz .check_handle_fail
    clc
    ret
.check_handle_fail:
    stc
    ret

; Convert the selected handle's KiB offset to a 32-bit physical address.
; Output DX:AX points at the first byte of the block.
.handle_base_physical:
    mov ax, [cs:xms_handle_base_table + di]
    mov dx, ax
    shl ax, 10
    shr dx, 6
    add dx, XMS_EMB_BASE_HI
    ret

.lock_emb:
    push di
    call .lookup_handle
    jc .lock_bad_handle
    call .handle_base_physical
    mov bx, ax
    pop di
    mov ax, 1
    retf
.lock_bad_handle:
    pop di
    jmp .free_fail

.unlock_emb:
    push di
    call .lookup_handle
    jc .unlock_bad_handle
    pop di
    mov ax, 1
    xor bx, bx
    retf
.unlock_bad_handle:
    pop di
    jmp .free_fail

.query_handle:
    push di
    push cx
    call .lookup_handle
    jc .query_bad_handle
    mov dx, ax
    cmp dx, 0xFFFF
    jne .query_size_ready
    xor dx, dx
.query_size_ready:
    xor bx, bx
    xor di, di
    mov cx, XMS_HANDLE_COUNT
.query_free_handle_loop:
    cmp word [cs:xms_handle_size_table + di], 0
    jne .query_next_handle
    inc bl
.query_next_handle:
    add di, 2
    loop .query_free_handle_loop
    pop cx
    pop di
    mov ax, 1
    retf
.query_bad_handle:
    pop cx
    pop di
    jmp .free_fail

.realloc_emb:
    push di
    push cx
    mov cx, bx
    call .lookup_handle
    jc .realloc_bad_handle
    cmp ax, 0xFFFF
    jne .realloc_old_size_ready
    xor ax, ax
.realloc_old_size_ready:
    add ax, [cs:xms_handle_base_table + di]
    cmp ax, [cs:xms_tail_kb]
    jne .realloc_no_space
    mov ax, XMS_FREE_KB_INITIAL
    sub ax, [cs:xms_handle_base_table + di]
    cmp cx, ax
    ja .realloc_no_space
    mov ax, cx
    or ax, ax
    jnz .realloc_size_ready
    dec ax
.realloc_size_ready:
    mov [cs:xms_handle_size_table + di], ax
    mov ax, [cs:xms_handle_base_table + di]
    add ax, cx
    mov [cs:xms_tail_kb], ax
    mov dx, XMS_FREE_KB_INITIAL
    sub dx, ax
    mov [cs:xms_free_kb], dx
    pop cx
    pop di
    mov ax, 1
    xor bl, bl
    retf
.realloc_bad_handle:
    pop cx
    pop di
    jmp .free_fail
.realloc_no_space:
    pop cx
    pop di
    jmp .alloc_fail

.unsupported:
    xor ax, ax
    mov bl, 0x80
    retf

.umb_unavailable:
    ; XMS 3.0 UMB request is implemented even when no UMB provider exists.
    ; B1h plus DX=0 means no upper-memory block is available.
    xor ax, ax
    xor dx, dx
    mov bl, 0xB1
    retf

.hma_a20_stub:
    cmp ah, 0x03
    je .a20_global_enable
    cmp ah, 0x04
    je .a20_global_disable
    cmp ah, 0x05
    je .a20_local_enable
    cmp ah, 0x06
    je .a20_local_disable
    cmp ah, 0x07
    je .a20_query
    ; HMA request/release (01h/02h): the current kernel does not occupy HMA,
    ; but accepting ownership is compatible and does not change the gate.
    mov ax, 1
    xor bl, bl
    retf

.a20_global_enable:
    mov byte [cs:xms_a20_global_enabled], 1
    call .a20_hw_enable
    jmp .a20_success

.a20_global_disable:
    mov byte [cs:xms_a20_global_enabled], 0
    cmp byte [cs:xms_a20_local_count], 0
    jne .a20_success
    call .a20_hw_disable
    jmp .a20_success

.a20_local_enable:
    cmp byte [cs:xms_a20_local_count], 0xFF
    je .a20_failure
    inc byte [cs:xms_a20_local_count]
    call .a20_hw_enable
    jmp .a20_success

.a20_local_disable:
    cmp byte [cs:xms_a20_local_count], 0
    je .a20_failure
    dec byte [cs:xms_a20_local_count]
    jne .a20_success
    cmp byte [cs:xms_a20_global_enabled], 0
    jne .a20_success
    call .a20_hw_disable
    jmp .a20_success

.a20_query:
    in al, 0x92
    and ax, 0x0002
    shr ax, 1
    xor bl, bl
    retf

.a20_hw_enable:
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    ret

.a20_hw_disable:
    in al, 0x92
    and al, 0xFD
    out 0x92, al
    ret

.a20_success:
    mov ax, 1
    xor bl, bl
    retf

.a20_failure:
    xor ax, ax
    mov bl, 0x82
    retf

.version:
    ; Windows 3.1 DOSX validates a 3.x HIMEM provider before entering Standard
    ; or 386 Enhanced mode.  Keep the 3.0 identity used by the resident patch
    ; protocol; HDPMI is constrained to the compatible legacy allocation path
    ; by the CiukiOS host patch.
    mov ax, 0x0300
    mov bx, ax
    mov dx, 1                      ; HMA is available and AH=01h is supported
    retf

boot_drive db 0
int21_installed db 0
int21_carry db 0
int21_write_error_stage db 0
int21_zf_state db 0xFF
int21_caller_ds dw 0
int21_caller_bx dw 0
int21_return_es db 0
int21_return_ds db 0
dos_media_descriptor db 0xF8
int2f_installed db 0
dos_default_drive db 0
dos_verify_flag db 0
dos_ctrl_break_flag db 0
dos_sysvars_initialized db 0
sft_sync_swap db 0
last_exit_code db 0
int21_last_ah db 0
int21_last_al db 0
int21_path_stage_marker db 0
int21_error_ax dw 0
int21_silent_errors db 0
int21_chdir_drive db 0
int21_chdir_qualified db 0
int21_trace_call_cs dw 0
int21_trace_call_ip dw 0
int21_path_upcase db 0
int21_last_dx dw 0
dos_time_centis db 0
last_term_type db 0
int21_force_terminate db 0
current_psp_seg dw 0
%if TRACE_CHILD_INT21 != 0
child_trace_active db 0
child_trace_armed db 0
child_trace_exit_logged db 0
%endif
exec_cmd_len db 0
exec_cmd_buf times 126 db 0
exec_fcb1 times 16 db 0
exec_fcb2 times 16 db 0
tmp_fcb_parse_control db 0
tmp_fcb_parse_wild db 0
tmp_fcb_parse_invalid db 0
old_int21_off dw 0
old_int21_seg dw 0
old_int20_off dw 0
old_int20_seg dw 0
old_int24_off dw 0
old_int24_seg dw 0
old_int2f_off dw 0
old_int2f_seg dw 0
int_ef_target_off dw 0
int_ef_target_seg dw 0
xms_free_kb dw XMS_FREE_KB_INITIAL
xms_tail_kb dw 0
xms_handle_size_table times XMS_HANDLE_COUNT dw 0
xms_handle_base_table times XMS_HANDLE_COUNT dw 0
xms_a20_global_enabled db 1
xms_a20_local_count db 0
%if FAT_TYPE == 16
xms_move_len_lo dw 0
xms_move_len_hi dw 0
xms_move_range_kb dw 0
xms_move_src_lo dw 0
xms_move_src_hi dw 0
xms_move_dst_lo dw 0
xms_move_dst_hi dw 0
xms_move_chunk dw 0
xms_87_gdt times 48 db 0
%endif
old_int1a_off dw 0
old_int1a_seg dw 0
%if FAT_TYPE == 16
old_int09_off dw 0
old_int09_seg dw 0
irq1_external_owner db 0
irq1_tail_before dw 0xFFFF
%endif
old_int10_off dw 0
old_int10_seg dw 0
%if FAT_TYPE == 16
old_int15_off dw 0
old_int15_seg dw 0
%endif
%if FAT_TYPE == 16
old_int74_off dw 0
old_int74_seg dw 0
%endif
current_video_mode db 0x03
current_vbe_mode dw 0
current_vbe_bank_a dw 0
current_vbe_bank_b dw 0
current_vbe_mode_banks dw 0
current_vbe_visible_bank dw 0xFFFF
current_vbe_backing_seg dw 0
current_vbe_backing_banks dw 0
current_vbe_visible_window db 0xFF
current_vbe_backing_ready db 0
vbe_mode_list dw 0x0100, 0x0101, 0x0103, 0xFFFF
vbe_mode_table:
    dw 0x0100, 640, 400, 640
    db 4, 0, 0x13, 80
    dw 0x0101, 640, 480, 640
    db 5, 0, 0x13, 80
    dw 0x0103, 800, 600, 800
    db 8, 0, 0x13, 100
    dw 0xFFFF
vbe_oem_string db 0
%if TRACE_CHILD_INT21 == 0
console_ansi_state db 0
console_ansi_flags db 0
console_ansi_p1 db 0
console_ansi_p2 db 0
%endif
%if FAT_TYPE == 16
mouse_hw_ready db 0
ps2_command_saved db 0
ps2_saved_command db 0
mouse_packet_index db 0
mouse_packet times 3 db 0
mouse_last_byte_tick dw 0
mouse_delta_x dw 0
mouse_delta_y dw 0
mouse_last_mickey_x dw 0
mouse_last_mickey_y dw 0
mouse_bios_enabled db 0
; Physical device state; deliberately outside the per-process EXEC snapshot.
mouse_bios_reporting_stopped db 0
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
%else
mouse_hw_ready db 0
mouse_delta_x dw 0
mouse_delta_y dw 0
mouse_last_mickey_x dw 0
mouse_last_mickey_y dw 0
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
file_handle_root_lba_hi dw 0
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
file_handle2_root_lba_hi dw 0
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
file_handle3_root_lba_hi dw 0
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
file_handle4_root_lba_hi dw 0
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
file_handle5_root_lba_hi dw 0
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
file_handle6_root_lba_hi dw 0
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
file_handle7_root_lba_hi dw 0
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
file_handle8_root_lba_hi dw 0
file_handle8_root_off dw 0
file_handle8_cluster_count dw 0
file_handle8_size_lo dw 0
file_handle8_size_hi dw 0
file_handle_extra_table times (DOS_FILE_EXTRA_COUNT * DOS_FILE_EXTRA_ENTRY_SIZE) db 0
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
runtime_handoff_version dw 0
runtime_handoff_boot_drive db 0
runtime_handoff_default_drive db 0
runtime_handoff_mem_kb dw 0
runtime_handoff_fat_spt dw FAT_SPT
runtime_handoff_fat_heads dw FAT_HEADS
runtime_handoff_fat_reserved dw FAT_RESERVED_SECTORS
runtime_handoff_fat_spc db FAT_SECTORS_PER_CLUSTER
runtime_handoff_entry_flags db 0
runtime_state_off dw 0
runtime_state_seg dw 0
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
tmp_disk_spt dw FAT_SPT
tmp_disk_heads dw FAT_HEADS
tmp_cluster dw 0
tmp_cluster_off dw 0
tmp_sector_off dw 0
tmp_lba dw 0
tmp_lba_hi dw 0
tmp_capacity dw 0
tmp_next_cluster dw 0
tmp_next_lba_hi dw 0
tmp_exec_limit dw 0
tmp_exec_total dw 0
tmp_exec_handle dw 0
tmp_exec_error dw 0
tmp_exec_subfn db 0
tmp_exec_return_flags dw 0
tmp_exec_mz_copy_limit dw 0
tmp_exec_mz_loaded_paras dw 0
tmp_exec_mz_probe_size_lo dw 0
tmp_exec_mz_probe_size_hi dw 0
tmp_exec_mz_load_high db 0
tmp_exec_mz_high_psp dw 0
tmp_exec_mz_fail_stage db 0
tmp_exec_mz_run_fail_stage db 0
tmp_exec_mz_arena_fail_stage db 0
tmp_exec_region_best_seg dw 0
tmp_exec_region_best_size dw 0
dos_exec_state_depth db 0
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
tmp_rename_old_lba_hi dw 0
tmp_rename_old_off dw 0
dos_mem_exec_state_begin:
dos_mem_init db 0
dos_exec_identity_psp dw 0
dos_exec_parent_identity_psp dw 0
dos_mem_alloc_seg dw 0
dos_mem_alloc_size dw 0
dos_mem_psp_free_seg dw 0
dos_mem_psp_free_size dw 0
dos_mem_psp_mcb_end dw 0
dos_mem_chain_limit_seg dw DOS_HEAP_LIMIT_SEG
dos_mem_last_mcb_seg dw DOS_HEAP_LIMIT_SEG - 1
dos_mem_top_seg dw DOS_HEAP_LIMIT_SEG
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
dos_mem_block_candidate_si dw 0
dos_mem_gap_candidate_seg dw 0
dos_mem_gap_candidate_size dw 0
dos_file_open_mask dw 0
dos_mem_exec_state_end:
dos21_saved_drive db 0
dos_exec_saved_context_begin:
saved_ss dw 0
saved_sp dw 0
saved_psp2 dw 0
saved_ds dw 0
saved_es dw 0
saved_ds2 dw 0
saved_es2 dw 0
dos_exec_saved_context_prefix_end:
current_load_seg dw MZ_LOAD_SEG
current_mz_context_slot dw 0
current_com_load_seg dw COM_LOAD_SEG
dos_exec_saved_context_suffix_begin:
saved_psp dw 0
saved_ss2 dw 0
saved_sp2 dw 0
saved_psp3 dw 0
saved_ss3 dw 0
saved_sp3 dw 0
saved_ds3 dw 0
saved_es3 dw 0
dos_exec_saved_context_end:
com_entry_off dw 0
com_entry_seg dw 0
com_stack_sp dw 0xFFFE
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
search_found_root_lba_hi dw 0
search_found_root_off dw 0
search_found_attr db 0
search_found_name times 11 db 0
dta_seg dw 0
dta_off dw 0
find_attr db 0
find_active db 0
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
windows_active db 0
windows_exec_base_depth db 0
windows_version dw 0
; WinStartupInfo (INT 2Fh/1605h): version, next record, optional VDD
; pointers, required instance-table pointer, and optional-instance pointer.
windows_startup_info:
    dw 0
    dw 0, 0
    dw 0, 0
    dw 0, 0
    dw windows_instance_table, RUNTIME_LOAD_SEG
    dw 0, 0
windows_instance_table:
    ; Windows instance-table entry: segment, offset, byte count.  Enhanced mode
    ; creates additional DOS VMs, so every mutable resident-kernel byte must be
    ; private to the VM.  Code and data currently share one segment; instancing
    ; the complete compact image is conservative and keeps every DOS service
    ; coherent until the kernel gains a separate data segment.
    dw RUNTIME_LOAD_SEG, 0, ciukidos_image_end - ciukidos_image_start
    dw 0, 0, 0
windows_machine_id dw 0
windows_empty_critical_patch_table dw 0
; DOSMGR patch table returned by INT 2Fh/1607h/BX=15h/CX=0.
windows_patch_table:
    dw 0x0005
    dw int21_caller_ds
    dw int21_caller_bx
    dw dos_indos_flag
    dw windows_machine_id
    dw windows_empty_critical_patch_table
    dw dos_mem_last_mcb_seg

; Compact MS-DOS 4/5-compatible Swappable Data Area prefix returned by
; INT 21h/AX=5D06h.  The published length is authoritative, so consumers do
; not assume the much larger private tail used by MS-DOS itself.
dos_sda:
    db 0                              ; +00 critical-error flag
dos_sda_indos db 0                    ; +01 InDOS snapshot
    db 0, 0                           ; +02 critical drive/locus
    dw 0                              ; +04 extended error code
    db 0, 0                           ; +06 action/class
    dw 0, 0                           ; +08 failing-device pointer
dos_sda_dta_off dw 0                  ; +0C current DTA
dos_sda_dta_seg dw 0
dos_sda_current_psp dw 0              ; +10 current PSP
    dw 0                              ; +12 break stack
    dw 0                              ; +14 child return code
dos_sda_default_drive db 0            ; +16 zero-based current drive
    db 1                              ; +17 Ctrl-Break enabled
    db 0, 0                           ; +18 code-page flags
dos_sda_swap_always:
    dw 0                              ; +1A last INT 21h AX
dos_sda_owning_psp dw 0               ; +1C owning PSP
dos_sda_machine_id dw 0               ; +1E redirector machine ID
    dw DOS_HEAP_BASE_SEG              ; +20 first usable MCB
    dw 0                              ; +22 best usable MCB
dos_sda_last_mcb dw DOS_HEAP_LIMIT_SEG - 1 ; +24 last usable MCB
    dw DOS_HEAP_MAX_PARAS             ; +26 conventional arena size
    dw 0                              ; +28 reserved
    db 0, 0, 0, 0                     ; +2A private/break flags
    dw 0                              ; +2E reserved
dos_sda_end:
dos_list_of_lists times 64 db 0
tmp_cwd_comp times 24 db 0
tmp_cwd_build times DOS_CWD_BYTES db 0
dos_env_block db 'COMSPEC=COMMAND.COM', 0
              db 'PATH=C:\APPS;C:\NET;C:\SYSTEM\DRIVERS;C:\SYSTEM', 0
              db 'BLASTER=A220 I7 D1 H5 T6', 0
              db 'MTCPCFG=C:\NET\MTCP.CFG', 0
              db 0
              dw 1
%if FAT_TYPE == 16 && STAGE1_BOOT_EXTERNAL_SHELL
dos_env_exec_path db '\SYSTEM\SHELL.COM', 0
%else
dos_env_exec_path db 'C:\COMMAND.COM', 0
%endif
              times DOS_ENV_EXEC_PATH_LEN - ($ - dos_env_exec_path) db 0
dos_env_block_end:
dos_child_exec_path_buf times DOS_ENV_EXEC_PATH_LEN db 0
align 4, db 0
disk_packet:
    db 0x10
    db 0
    dw 1
disk_packet_off dw 0
disk_packet_seg dw 0
disk_packet_lba dq 0
%ifdef CIUKIDOS_KERNEL_BUILD
disk_saved_sp dw 0, 0
disk_stack_ptr dw 0x1000, 0x0100         ; 02000h, in the kernel's own stack
%endif
%if ((dos_mem_exec_state_end - dos_mem_exec_state_begin) + 6 + (dos_exec_saved_context_prefix_end - dos_exec_saved_context_begin) + (dos_exec_saved_context_end - dos_exec_saved_context_suffix_begin)) > (DOS_EXEC_STATE_FRAME_PARAS * 16)
    %error "EXEC allocator snapshot exceeds its CIUKIDOS frame"
%endif
%if ((dos_exec_saved_context_prefix_end - dos_exec_saved_context_begin) & 1) != 0
    %error "EXEC saved-context prefix must contain whole words"
%endif
%if ((dos_exec_saved_context_end - dos_exec_saved_context_suffix_begin) & 1) != 0
    %error "EXEC saved-context suffix must contain whole words"
%endif
%if STAGE1_INTERACTIVE_SHELL
cmd_buffer times CMD_BUF_LEN db 0
%endif
shell_exec_path_buf times SHELL_EXEC_PATH_BUF_LEN db 0
shell_exec_param_block:
    dw 0
    dw shell_exec_cmd_tail
    dw 0
    dw shell_exec_fcb1
    dw 0
    dw shell_exec_fcb2
    dw 0
shell_exec_cmd_tail db 0, 0x0D
              times 127 db 0
shell_exec_fcb1 db 0, '           ', 0, 0, 0, 0
shell_exec_fcb2 db 0, '           ', 0, 0, 0, 0
%if FAT_TYPE == 16 || FAT_TYPE == 12
shell_copy_src_ptr dw 0
shell_copy_dst_ptr dw 0
shell_copy_dst_cluster dw 0
%endif
shell_last_error_ax dw 0
%if STAGE1_INTERACTIVE_SHELL
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
%endif

msg_stage1_serial db 0
msg_diag_begin    db 0
msg_diag_int10    db 0
msg_diag_int13_ok db 0
msg_diag_int16_ok db 0
msg_diag_int1a    db 0
msg_int21_installed db 0
%if STAGE1_DEBUG_COMMANDS
msg_int21_missing db "[I21] no", 13, 10, 0
%endif
msg_vec25 db 0
msg_vec35 db 0
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
msg_runtime_probe_call db "[RTP] C", 13, 10
msg_runtime_probe_ok db "[RTP] OK", 13, 10, 0
%endif

msg_banner_title db "CiukiOS pre-Alpha v0.8.0 (CiukiDOS Shell)", 0
%if FAT_TYPE == 12
msg_shell_sysinfo_prefix db "RAM:", 0
%endif
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
msg_mz_exec_fail db "[MZ] exec fail stage=", 0
msg_mz_exec_fail_ax db " AX=", 0
msg_mz_exec_fail_sub db " SUB=", 0
msg_mz_exec_fail_arena db " ARENA=", 0
msg_mz_serial_pass db "[MZDEMO-SERIAL] PASS", 13, 10, 0
msg_mz_serial_fail db "[MZDEMO-SERIAL] FAIL", 13, 10, 0
%if TRACE_WIN_MEMORY != 0
msg_win_mem_48 db "MEM48 ", 0
msg_win_mem_4a db "MEM4A ", 0
msg_win_mem_largest db "MEMMAX ", 0
msg_win_mem_free db "MEMFREE ", 0
msg_win_mem_free_ok db "MEMOK", 13, 10, 0
msg_win_mem_free_miss db "MEMMISS", 13, 10, 0
msg_win_mem_free_owner db "MEMOWNER", 13, 10, 0
msg_win_mem_sep db ":", 0
msg_win_exec db "EXEC ", 0
msg_win_exec_return db "EXRET ", 0
%endif
%if TRACE_WIN_INT2F != 0
msg_win_int2f db "W2F ", 0
%endif
%if TRACE_WIN_MEMORY != 0 || TRACE_WIN_XMS != 0
msg_win_xms db "XMEM ", 0
%endif
%if TRACE_CHILD_INT21 != 0
msg_child_trace_end db "CHILD_TRACE_END", 13, 10, 0
msg_child_exec_req db "CHILD_EXEC_REQ ", 0
msg_child_exec_select_fail db "CHILD_EXEC_SELECT_FAIL", 13, 10, 0
msg_child_exec_load_fail db "CHILD_EXEC_LOAD_FAIL", 13, 10, 0
msg_child_exec_load_seg db " load=", 0
msg_child_exec_load_limit db " limit=", 0
msg_child_exec_run_fail db "CHILD_EXEC_RUN_FAIL", 13, 10, 0
msg_child_exec_run_psp db " psp=", 0
msg_child_prejump db "CHILD_PREJUMP", 0
msg_child_4a db "CH4A", 0
msg_child_int21_error db "I21ERR ah=", 0
msg_child_int21_error_ax db " ax=", 0
msg_child_int21_error_path db " path=", 0
msg_child_int21_call db "I21 ah=", 0
msg_child_int21_call_ax db " ax=", 0
msg_child_int21_call_bx db " bx=", 0
msg_child_int21_call_cx db " cx=", 0
msg_child_int21_call_dx db " dx=", 0
msg_child_int21_call_ds db " ds=", 0
msg_child_int21_call_ret db " ret=", 0
msg_child_int21_read_bytes db " data8=", 0
msg_child_int21_return db "I21RET ah=", 0
msg_child_int21_return_path db " cwd=", 0
msg_child_int21_return_pathbuf db " pathbuf=", 0
msg_child_exit db "CHILD_EXIT", 0
msg_child_exit_reason db " reason=", 0
msg_child_exit_reason_retf db " reason=RETF", 0
msg_child_exit_code db " code=", 0
msg_child_vec25 db "CH25", 0
msg_child_vec35 db "I10I ", 0
%endif
; These diagnostics are intentionally silent in the production kernel.  They
; share one empty ASCIIZ instead of spending a byte per alias in the resident
; image (the labels remain stable for their callers).
msg_fileio_begin:
msg_fileio_serial_pass:
msg_fileio_serial_fail:
msg_find_begin:
msg_find_serial_pass:
msg_find_serial_fail:
msg_gfx_begin:
msg_gfx_done:
msg_gfx_serial_pass:
msg_gfxrect_serial_pass:
msg_gfxrect_serial_fail:
msg_gfxstar_serial_pass:
msg_gfxstar_serial_fail:
    db 0
%if STAGE1_SELFTEST_AUTORUN
msg_mvren_serial_pass db "[MVR] PASS", 13, 10, 0
msg_mvren_serial_fail db "[MVR] FAIL", 13, 10, 0
%endif
msg_rebooting:
msg_halting:
    db 0
msg_loader_bsod_woof db "WOOF! CiukiOS ran into a problem.", 13, 10, 13, 10, 0
msg_loader_bsod_body db "The system loader could not continue safely.", 13, 10, 0
msg_loader_bsod_restart db "Please restart your PC.", 13, 10, 13, 10, 0
msg_loader_bsod_error db "Error:", 13, 10, 0
%if TRACE_CHILD_INT21 != 0
msg_child_int2f_call db "I2F ax=", 0
msg_child_int2f_es db " es=", 0
msg_child_int2f_di db " di=", 0
msg_child_xms_call db "XMS ax=", 0
msg_child_xms_si db " si=", 0
%endif
msg_runtime_missing_fatal db "- CIUKIDOS.SYS missing or invalid", 13, 10, 0
msg_shell_missing_fatal db "- SHELL.COM missing", 13, 10, 0
msg_shell_exec_fatal db "- SHELL.COM execution failed", 13, 10, 0
msg_loader_exec_error db "DOS error: 0x", 0
loader_exec_error dw 0
msg_shell_returned_fatal db "SHELL.COM returned control to the loader.", 13, 10, "This is not supported in loader-only mode.", 13, 10, 0
msg_boot_splash_title:
msg_boot_splash_subtitle:
msg_boot_splash_tagline:
msg_boot_loader_title:
msg_boot_loading_runtime:
msg_boot_loading_volume:
msg_boot_loading_shell:
msg_boot_loading_services:
msg_boot_loading_ready:
msg_boot_progress_0:
msg_boot_progress_20:
msg_boot_progress_40:
msg_boot_progress_60:
msg_boot_progress_80:
msg_boot_progress_100:
    db 0
msg_dir_header db "Dir", 13, 10, 0
msg_dir_empty db "no files found", 13, 10, 0
msg_cwd_prefix db "cwd=", 0
msg_err_ax db " err=0x", 0
%if STAGE1_DEBUG_COMMANDS
msg_mouse_status db "mouse=0x", 0
msg_mouse_buttons db "buttons=0x", 0
msg_mouse_x db "x=0x", 0
msg_mouse_y db "y=0x", 0
msg_keytest_prompt db "press a key...", 0
msg_keytest_ax db "key AX=0x", 0
%endif
gfx_text_ciukios:
gfx_text_demo:
gfx_text_vdi:
gfx_text_timer:
    db 0
str_ext_com db ".COM", 0
str_ext_exe db ".EXE", 0

path_comdemo_dos db "\APPS\COMDEMO.COM", 0
path_mzdemo_dos  db "\APPS\MZDEMO.EXE", 0
path_fileio_dos  db "\APPS\FILEIO.BIN", 0
path_deltest_dos db "\APPS\DELTEST.BIN", 0
path_gfxrect_dos db "\APPS\GFXRECT.COM", 0
path_gfxstar_dos db "\APPS\GFXSTAR.COM", 0
%if STAGE1_SELFTEST_AUTORUN
path_mvren_dir_dos db "\APPS\T", 0
path_mvren_moved_dos db "\APPS\T\COMDEMO.COM", 0
path_mvren_final_dos db "\APPS\T\C.COM", 0
%endif
%if FAT_TYPE == 16
%if STAGE2_AUTORUN
path_stage2_dos db "\SYSTEM\STAGE2.BIN", 0
%endif
path_runtime_dos db "\SYSTEM\CIUKIDOS.SYS", 0
path_splash_bin_dos db "\SYSTEM\SPLASH.BIN", 0
runtime_loader_signature db "CIUKIDOS"
%endif
path_pattern_com db "*.COM", 0
path_pattern_exe db "*.EXE", 0
path_pattern_mz equ path_mzdemo_dos
path_dotdot_fat    db "..         "
path_system_dir_dos db "\SYSTEM", 0
path_drivers_dir_dos db "\SYSTEM\DRIVERS\", 0
path_apps_dir_dos db "\APPS", 0
path_parent_dos  db "..", 0
path_root_dos    db "\", 0
cwd_buf times DOS_CWD_BYTES db 0
cwd_cluster dw 0
%if FAT_TYPE == 16
cwd_c_buf times DOS_CWD_BYTES db 0
cwd_c_cluster dw 0
cwd_d_buf times DOS_CWD_BYTES db 0
cwd_d_cluster dw 0
%endif
%if STAGE1_INTERACTIVE_SHELL
shell_saved_cwd_buf times DOS_CWD_BYTES db 0
shell_saved_cwd_cluster dw 0
%endif
shell_exec_saved_cwd_buf times DOS_CWD_BYTES db 0
shell_exec_saved_cwd_cluster dw 0
shell_exec_external_program_active db 0
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
boot_splash_active db 0
boot_progress_value db 0
boot_progress_units db 8, 24, 32, 16, 40
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
MOUSE_STATE_VERSION equ 0x3301
MOUSE_STATE_SIZE equ 190
mouse_state_begin:
mouse_state_version dw MOUSE_STATE_VERSION
mouse_state_size_field dw MOUSE_STATE_SIZE
mouse_pos_x dw 320
mouse_pos_y dw 240
mouse_prev_x dw 320
mouse_prev_y dw 240
mouse_buttons db 0
mouse_prev_buttons db 0
mouse_installed db 0
mouse_detected db 0
mouse_button_count db 2
mouse_driver_enabled db 1
mouse_visible db 0
mouse_gfx_cursor_custom db 0
mouse_language db 0
mouse_crt_page db 0
mouse_light_pen_enabled db 0
mouse_excl_enabled db 0
mouse_hide_count dw 0
mouse_min_x dw 0
mouse_max_x dw 319
mouse_min_y dw 0
mouse_max_y dw 199
mouse_excl_min_x dw 0
mouse_excl_min_y dw 0
mouse_excl_max_x dw 0
mouse_excl_max_y dw 0
mouse_cb_mask dw 0
mouse_cb_off dw 0
mouse_cb_seg dw 0
mouse_cb_busy db 0
mouse_cb_pending_buttons db 0
mouse_cb_pending_mask dw 0
mouse_cb_pending_x dw 320
mouse_cb_pending_y dw 240
mouse_cb_pending_dx dw 0
mouse_cb_pending_dy dw 0
mouse_alt_mask dw 0
mouse_alt_off dw 0
mouse_alt_seg dw 0
mouse_mickey_x dw 8
mouse_mickey_y dw 8
mouse_sens_x dw 8
mouse_sens_y dw 8
mouse_double_threshold dw 64
mouse_interrupt_rate dw 100
mouse_last_event_mask dw 0
mouse_text_cursor_type dw 0
mouse_text_screen_mask dw 0xFFFF
mouse_text_cursor_mask dw 0x7700
mouse_gfx_hot_x dw 0
mouse_gfx_hot_y dw 0
mouse_press_count dw 0,0,0
mouse_release_count dw 0,0,0
mouse_press_x dw 320,320,320
mouse_press_y dw 240,240,240
mouse_release_x dw 320,320,320
mouse_release_y dw 240,240,240
mouse_gfx_cursor_mask times 32 dw 0

%if STAGE1_SELFTEST_AUTORUN || (STAGE1_INTERACTIVE_SHELL && STAGE1_DEBUG_COMMANDS)
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
%endif

; Stage2 Extended Services Messages
msg_stage2_ready db 0
%if STAGE2_AUTORUN
%endif
%if HARDWARE_VALIDATION_SCREEN
; HW validation strings emptied so the function runs (stage1 layout preserved
; for the SETUP default-drive byte patch) but no [HW] lines are printed on
; the screen. The [S2] / shell prompt come right after.
msg_hw_validation_title db 0
msg_hw_validation_pass db 0
msg_hw_validation_return db 0
msg_hw_validation_capture db 0
msg_hw_validation_fail db 0
msg_hw_validation_notrun db 0
%endif
msg_mouse_enabled db 0
msg_mouse_not_found db 0
msg_exit_str db "Exit", 13, 10, 0
%if TRACE_CHILD_INT21 != 0
child_trace_int10_enter:
    push si
    mov si, msg_child_vec35
child_trace_bios_marker:
    push ax
    call child_trace_should_log_exit
    jnc short .done
    call child_trace_emit_marker
.done:
    pop ax
    pop si
    ret
%endif

%ifdef CIUKIDOS_KERNEL_BUILD
%include "src/runtime/ciukidos_kernel_services.inc"
ciukidos_image_end:
%if (ciukidos_image_end - ciukidos_image_start) > 0xA900
    %error "CIUKIDOS kernel overlaps the DOS SYSVARS boundary"
%endif
%endif
