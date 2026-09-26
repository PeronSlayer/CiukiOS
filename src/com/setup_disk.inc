; Target validation, readback, and real quick/full FAT16 preparation.
; All destructive addresses are limited to the explicitly displayed system
; partition plus its MBR/gap. The rest of the disk is left unallocated.
%define SETUP_RESERVED 73
%define SETUP_FAT1 (RAW_HDD_PARTITION_LBA + SETUP_RESERVED)
%define SETUP_FAT2 (SETUP_FAT1 + 128)
%define SETUP_ROOT (SETUP_FAT2 + 128)
%define SETUP_DATA (SETUP_ROOT + 32)

; Resolve selected BIOS disk to one ATA task file, then IDENTIFY that device.
; This is read-only. Capacity checks happen again immediately before writing.
setup_preflight:
    pushad
    push es
    push cs
    pop es
    cmp byte [source_drive], 3
    jne .source_bad
    cmp byte [raw_target_bios], 0x81
    jb .source_bad
    cmp byte [raw_target_bios], 0x87
    ja .source_bad
    mov byte [setup_error], 2
    call raw_configure_ata_target
    jc .fail
    mov ax, [ata_cmd_base]
    mov [ata_probe_cmd], ax
    mov ax, [ata_ctrl_base]
    mov [ata_probe_ctrl], ax
    mov al, [ata_dev_select]
    and al, 0xB0
    mov [ata_probe_dev], al
    call raw_probe_legacy_ata
    jc .identify_bad
    mov dx, setup_marker_identify
    call serial_write_z
    mov ax, [io_buffer + 98]
    call serial_write_hex_word
    mov ax, [io_buffer + 212]
    call serial_write_hex_word
    mov ax, [io_buffer + 122]
    call serial_write_hex_word
    mov ax, [io_buffer + 120]
    call serial_write_hex_word
    call serial_write_crlf
    ; IDENTIFY data was read into io_buffer by the existing bounded probe.
    test word [io_buffer + 98], 0x0200
    jz .identify_bad
    ; Reject non-512-byte logical sectors when the ATA size fields are valid.
    mov ax, [io_buffer + 212]
    and ax, 0xD000
    cmp ax, 0x5000
    je .identify_bad
    mov eax, [io_buffer + 120]
    cmp eax, RAW_HDD_CLONE_SECTORS
    jb .small
    cmp eax, 0x0FFFFFFF
    ja .identify_bad
    mov [setup_disk_sectors], eax
    shr eax, 11
    mov [setup_disk_mib], eax
    ; ATA model strings store the first character in each word's high byte.
    mov si, io_buffer + 54
    mov di, setup_disk_model
    mov cx, 20
.model:
    lodsw
    xchg al, ah
    stosw
    loop .model
    mov byte [di], 0
    ; Confirm that the source has the exact disk layout packaged by this build
    ; before a single target sector can be invalidated.
    mov dword [raw_clone_lba_lo], 0
    mov dl, RAW_HDD_SOURCE_DRIVE
    mov bx, io_buffer
    mov cx, 1
    call raw_edd_read_n
    jc .source_bad
    cmp word [io_buffer + 510], 0xAA55
    jne .source_bad
    cmp dword [io_buffer + 454], RAW_HDD_PARTITION_LBA
    jne .source_bad
    cmp dword [io_buffer + 458], RAW_HDD_PARTITION_SECTORS
    jne .source_bad
    ; A correct MBR does not establish the cluster addresses used by DOS.
    ; The installed kernel expects this build's exact BPB, so reject a stale
    ; or damaged volume before invalidating any target sector.
    mov dword [raw_clone_lba_lo], RAW_HDD_PARTITION_LBA
    mov dl, RAW_HDD_SOURCE_DRIVE
    mov bx, io_buffer
    mov cx, 1
    call raw_edd_read_n
    jc .source_bad
    cmp word [io_buffer + 510], 0xAA55
    jne .source_bad
    cmp word [io_buffer + 11], 512
    jne .source_bad
    cmp byte [io_buffer + 13], 8
    jne .source_bad
    cmp word [io_buffer + 14], SETUP_RESERVED
    jne .source_bad
    cmp byte [io_buffer + 16], 2
    jne .source_bad
    cmp word [io_buffer + 17], 512
    jne .source_bad
    cmp word [io_buffer + 19], 0
    jne .source_bad
    cmp word [io_buffer + 22], 128
    jne .source_bad
    cmp dword [io_buffer + 28], RAW_HDD_PARTITION_LBA
    jne .source_bad
    cmp dword [io_buffer + 32], RAW_HDD_PARTITION_SECTORS
    jne .source_bad
    mov byte [setup_error], 0
    clc
    jmp .out
.source_bad:
    mov byte [setup_error], 1
    jmp .fail
.identify_bad:
    mov byte [setup_error], 2
    jmp .fail
.small:
    mov byte [setup_error], 3
.fail:
    stc
.out:
    pop es
    popad
    ret

; A failed final MBR write/flush/readback must not leave an installation
; advertised as bootable. Retain the original failure location and ATA status;
; separately report whether the rollback was itself confirmed on the device.
setup_rollback_commit:
    pushad
    push dword [raw_clone_lba_lo]
    push word [raw_last_stage]
    push word [raw_last_status]
    call raw_invalidate_target_mbr
    mov dx, setup_marker_rollback_ok
    jnc .report
    mov dx, setup_marker_rollback_failed
.report:
    call serial_write_z
    call serial_write_crlf
    pop word [raw_last_status]
    pop word [raw_last_stage]
    pop dword [raw_clone_lba_lo]
    popad
    stc
    ret

; CRC-32/ISO-HDLC (reflected polynomial EDB88320h). The source checksum is
; accumulated from the actual patched copy buffers. After FLUSH CACHE, read
; every installed sector through BIOS, independently of the ATA writer, while
; the MBR signature is still invalid. The same bytes must produce the same CRC.
setup_crc_buffer:
    push ebx
    push edx
    push si
    push cx
    shl cx, 9
.byte:
    movzx ebx, byte [si]
    xor bl, al
    shr eax, 8
    xor eax, [setup_crc_table + ebx * 4]
    inc si
    loop .byte
    pop cx
    pop si
    pop edx
    pop ebx
    ret

setup_verify_installed_bios:
    pushad
    push es
    push cs
    pop es
    mov dx, setup_marker_bios_verify
    call serial_write_z
    call serial_write_crlf
    mov dword [raw_clone_lba_lo], 0
    mov dword [setup_surface_left], RAW_HDD_CLONE_SECTORS
    mov dword [setup_read_crc], 0xFFFFFFFF
    mov byte [setup_last_pct], 255
    mov byte [gui_phase], 4
.next:
    mov cx, 8
    cmp dword [setup_surface_left], 8
    jae .track
    mov cx, [setup_surface_left]
.track:
    mov eax, [raw_clone_lba_lo]
    xor edx, edx
    movzx ebx, word [raw_target_spt]
    div ebx
    sub bx, dx
    cmp cx, bx
    jbe .read
    mov cx, bx
.read:
    mov dl, RAW_HDD_TARGET_DRIVE
    mov bx, setup_verify_buffer_data
    call raw_edd_read_n
    jc .fail
    mov si, setup_verify_buffer_data
    mov eax, [setup_read_crc]
    call setup_crc_buffer
    mov [setup_read_crc], eax
    call gui_cancel_poll
    jc .fail
    movzx eax, cx
    add [raw_clone_lba_lo], eax
    sub [setup_surface_left], eax
    mov eax, [raw_clone_lba_lo]
    imul eax, 100
    xor edx, edx
    mov ebx, RAW_HDD_CLONE_SECTORS
    div ebx
    cmp al, [setup_last_pct]
    je .more
    mov [setup_last_pct], al
    call gui_progress_update
.more:
    cmp dword [setup_surface_left], 0
    jne .next
    mov eax, [setup_read_crc]
    cmp eax, [setup_source_crc]
    jne .mismatch
    mov dx, setup_marker_bios_ok
    call serial_write_z
    call serial_write_crlf
    clc
    jmp .out
.mismatch:
    mov byte [raw_last_stage], 'V'
    mov byte [raw_last_path], 'B'
    mov byte [raw_last_status], 0xF7
.fail:
    mov byte [setup_error], 4
    stc
.out:
    pop es
    popad
    ret

setup_source_crc dd 0xFFFFFFFF
setup_read_crc dd 0xFFFFFFFF
setup_marker_bios_verify db '[SETUP-VERIFY] BIOS READ AFTER FLUSH',0
setup_marker_bios_ok db '[SETUP-VERIFY] ALL SECTORS CRC MATCH',0
align 4
setup_crc_table:
%assign setup_crc_i 0
%rep 256
    %assign setup_crc_value setup_crc_i
    %rep 8
        %if setup_crc_value & 1
            %assign setup_crc_value (setup_crc_value >> 1) ^ 0xEDB88320
        %else
            %assign setup_crc_value setup_crc_value >> 1
        %endif
    %endrep
    dd setup_crc_value
    %assign setup_crc_i setup_crc_i + 1
%endrep

; Read the just-written sectors back through the exact same selected ATA
; device, and compare with the expected bytes (including in-flight D: -> C:).
setup_verify_buffer:
    mov bx, io_buffer
setup_verify_expected:
    pushad
    push es
    push cs
    pop es
    mov [setup_expected_ptr], bx
    mov [setup_verify_count], cx
    mov bx, setup_verify_buffer_data
    call raw_ata_read_n
    jc .fail
    mov si, [setup_expected_ptr]
    mov di, setup_verify_buffer_data
    mov cx, [setup_verify_count]
    shl cx, 8
    repe cmpsw
    jne .mismatch
    clc
    jmp .out
.mismatch:
    mov byte [raw_last_stage], 'V'
    mov byte [raw_last_status], 0xF5
.fail:
    mov byte [setup_error], 4
    stc
.out:
    pop es
    popad
    ret

; A full format clears and reads back every sector in the displayed system
; region. Sector zero is invalid until FORMAT/INSTALL commits successfully.
setup_full_surface:
    pushad
    push es
    push cs
    pop es
    call setup_preflight
    jc .fail
    call raw_invalidate_target_mbr
    jc .fail
    mov dword [raw_clone_lba_lo], 1
    mov dword [setup_surface_left], RAW_HDD_CLONE_SECTORS - 1
    mov dx, setup_marker_full_start
    call serial_write_z
    call serial_write_crlf
    xor eax, eax
    mov di, io_buffer
    mov cx, 1024
    rep stosd
    mov byte [gui_phase], 1
    mov byte [setup_last_pct], 255
    mov byte [setup_surface_pass], 0
.next:
    mov cx, 8
    cmp dword [setup_surface_left], 8
    jae .count
    mov cx, [setup_surface_left]
.count:
    mov bx, io_buffer
    cmp byte [setup_surface_pass], 1
    je .read_pass
    call raw_ata_write_n
    jc .fail
    jmp .transferred
.read_pass:
    call setup_verify_buffer
    jc .fail
.transferred:
    call gui_cancel_poll
    jc .fail
    movzx eax, cx
    add [raw_clone_lba_lo], eax
    sub [setup_surface_left], eax
    mov eax, [raw_clone_lba_lo]
    imul eax, 50
    xor edx, edx
    mov ebx, RAW_HDD_CLONE_SECTORS
    div ebx
    cmp byte [setup_surface_pass], 0
    je .pct
    add al, 50
.pct:
    cmp al, [setup_last_pct]
    je .more
    mov [setup_last_pct], al
    call gui_progress_update
.more:
    cmp dword [setup_surface_left], 0
    jne .next
    cmp byte [setup_surface_pass], 1
    je .verified
    call raw_ata_flush_cache
    jc .fail
    mov byte [setup_surface_pass], 1
    mov dword [raw_clone_lba_lo], 1
    mov dword [setup_surface_left], RAW_HDD_CLONE_SECTORS - 1
    mov dx, setup_marker_full_verify
    call serial_write_z
    call serial_write_crlf
    jmp .next
.verified:
    mov dx, setup_marker_full_done
    call serial_write_z
    call serial_write_crlf
    clc
    jmp .out
.fail:
    stc
.out:
    pop es
    popad
    ret

; Fresh empty FAT16 volume. Quick format writes metadata only; full formatting
; runs setup_full_surface first. No old boot code is kept in reserved sectors.
format_target_hdd:
    pushad
    push es
    push cs
    pop es
    call setup_preflight
    jc .fail
    call raw_invalidate_target_mbr
    jc .fail
    mov dx, msg_serial_hdd_format_start
    call serial_write_z
    call serial_write_crlf
    xor eax, eax
    mov di, io_buffer
    mov cx, 1024
    rep stosd
    mov dword [raw_clone_lba_lo], RAW_HDD_PARTITION_LBA + 1
    mov word [setup_format_left], SETUP_DATA - RAW_HDD_PARTITION_LBA - 1
    mov byte [gui_phase], 2
.zero_loop:
    mov cx, 8
    cmp [setup_format_left], cx
    jae .count
    mov cx, [setup_format_left]
.count:
    mov bx, io_buffer
    call raw_ata_write_n
    jc .fail
    call setup_verify_buffer
    jc .fail
    call gui_cancel_poll
    jc .fail
    sub [setup_format_left], cx
    movzx eax, cx
    add [raw_clone_lba_lo], eax
    mov eax, SETUP_DATA - RAW_HDD_PARTITION_LBA - 1
    movzx ebx, word [setup_format_left]
    sub eax, ebx
    imul eax, 90
    xor edx, edx
    mov ebx, SETUP_DATA - RAW_HDD_PARTITION_LBA - 1
    div ebx
    call gui_progress_update
    cmp word [setup_format_left], 0
    jne .zero_loop
    ; Initialize both FAT reserved entries.
    mov dword [io_buffer], 0xFFFFFFF8
    mov dword [raw_clone_lba_lo], SETUP_FAT1
    mov cx, 1
    call .write_check
    jc .fail
    mov dword [raw_clone_lba_lo], SETUP_FAT2
    call .write_check
    jc .fail
    ; A root volume-label entry keeps DOS tools and the BPB label consistent.
    mov dword [io_buffer], 0
    mov si, setup_volume_label
    mov di, io_buffer
    mov cx, 11
    rep movsb
    mov byte [io_buffer + 11], 8
    mov dword [raw_clone_lba_lo], SETUP_ROOT
    mov cx, 1
    call .write_check
    jc .fail
    ; Build a consistent empty-volume BPB, with no executable OS advertised.
    xor eax, eax
    mov di, io_buffer
    mov cx, 128
    rep stosd
    mov word [io_buffer], 0x3CEB
    mov byte [io_buffer + 2], 0x90
    mov dword [io_buffer + 3], 'CIUK'
    mov dword [io_buffer + 7], 'IOS '
    mov word [io_buffer + 11], 512
    mov byte [io_buffer + 13], 8
    mov word [io_buffer + 14], SETUP_RESERVED
    mov byte [io_buffer + 16], 2
    mov word [io_buffer + 17], 512
    mov byte [io_buffer + 21], 0xF8
    mov word [io_buffer + 22], 128
    mov word [io_buffer + 24], 63
    mov word [io_buffer + 26], 16
    mov dword [io_buffer + 28], RAW_HDD_PARTITION_LBA
    mov dword [io_buffer + 32], RAW_HDD_PARTITION_SECTORS
    mov byte [io_buffer + 36], 0x80
    mov byte [io_buffer + 38], 0x29
    mov dword [io_buffer + 39], 0x4B49554B
    mov si, setup_volume_label
    mov di, io_buffer + 43
    mov cx, 19
    rep movsb
    mov word [io_buffer + 62], 0x18CD ; INT 18h: non-system volume
    mov word [io_buffer + 64], 0xFEEB ; loop if BIOS returns
    mov word [io_buffer + 510], 0xAA55
    mov dword [raw_clone_lba_lo], RAW_HDD_PARTITION_LBA
    mov cx, 1
    call .write_check
    jc .fail
    call raw_ata_flush_cache
    jc .fail
    ; Publish only an inactive data partition after all metadata was verified.
    xor eax, eax
    mov di, io_buffer
    mov cx, 128
    rep stosd
    mov word [io_buffer], 0x18CD
    mov word [io_buffer + 2], 0xFEEB
    mov byte [io_buffer + 447], 1
    mov byte [io_buffer + 448], 1
    mov byte [io_buffer + 450], 6
    mov byte [io_buffer + 451], 0xFE
    mov word [io_buffer + 452], 0xFFFF
    mov dword [io_buffer + 454], RAW_HDD_PARTITION_LBA
    mov dword [io_buffer + 458], RAW_HDD_PARTITION_SECTORS
    mov word [io_buffer + 510], 0xAA55
    mov dword [raw_clone_lba_lo], 0
    mov cx, 1
    call .write_check
    jc .commit_failed
    call raw_ata_flush_cache
    jc .commit_failed
    mov dx, msg_serial_hdd_format_done
    call serial_write_z
    call serial_write_crlf
    mov al, 100
    call gui_progress_update
    clc
    jmp .out
.write_check:
    mov bx, io_buffer
    call raw_ata_write_n
    jc .write_out
    call setup_verify_buffer
.write_out:
    ret
.commit_failed:
    call setup_rollback_commit
.fail:
    mov dx, msg_serial_hdd_format_fail
    call serial_write_z
    call serial_write_crlf
    stc
.out:
    pop es
    popad
    ret

setup_error db 0
setup_disk_sectors dd 0
setup_disk_mib dd 0
setup_disk_model times 41 db 0
setup_expected_ptr dw 0
setup_verify_count dw 0
setup_surface_left dd 0
setup_format_left dw 0
setup_last_pct db 255
setup_volume_label db 'CIUKIOS    FAT16   '
setup_marker_full_start db '[SETUP-FULL] START',0
setup_marker_full_done db '[SETUP-FULL] DONE',0

setup_marker_identify db '[SETUP-TARGET] IDENTIFY ',0

setup_surface_pass db 0
setup_marker_full_verify db '[SETUP-FULL] VERIFY AFTER FLUSH',0
setup_marker_rollback_ok db '[SETUP-COMMIT] ROLLBACK VERIFIED',0
setup_marker_rollback_failed db '[SETUP-COMMIT] ROLLBACK FAILED',0
