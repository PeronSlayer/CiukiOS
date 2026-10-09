; Public far bridges for resident LFN.COM. Included only by the full kernel.
; RTSV 0Dh returns AX=version, CX=bytes, DS:SI=CLFN; CF=0.
; Each entry is {id, ABI version, offset, segment}. The segment is relocated
; from actual CS on discovery, rather than a compiled memory-map address.
%include "src/runtime/ciukidos_lfn_abi.inc"
align 2
kernel_lfn_api_table:
    db 'CLFN'
    dw CLFN_API_VERSION, CLFN_TABLE_SIZE, CLFN_ENTRY_COUNT, CLFN_ENTRY_SIZE
    dw 1, 0
    dw CLFN_READ_SECTOR, 1, kernel_lfn_read_sector, 0
    dw CLFN_WRITE_SECTOR, 1, kernel_lfn_write_sector, 0
    dw CLFN_FLUSH_FAT, 1, kernel_lfn_flush_fat, 0
    dw CLFN_INVALIDATE_FAT, 1, kernel_lfn_invalidate_fat, 0
    dw CLFN_ENTER_DOS, 1, kernel_lfn_enter_dos, 0
    dw CLFN_LEAVE_DOS, 1, kernel_lfn_leave_dos, 0

kernel_runtime_lfn_api_service:
    push bx
    mov ax, cs
    mov ds, ax
    mov bx, kernel_lfn_api_table + CLFN_HEADER_SIZE + 6
    mov cx, CLFN_ENTRY_COUNT
.relocate:
    mov [bx], ax
    add bx, CLFN_ENTRY_SIZE
    loop .relocate
    mov si, kernel_lfn_api_table
    mov ax, CLFN_API_VERSION
    mov cx, CLFN_TABLE_SIZE
    pop bx
    clc
    retf

; DX:AX = volume-relative 32-bit LBA; ES:BX = one 512-byte sector.
; Preserve every input register; return only CF (0 done, 1 I/O failure).
kernel_lfn_read_sector:
    call read_sector_lba32
    retf
kernel_lfn_write_sector:
    call write_sector_lba32
    retf

; Flush the existing kernel-owned FAT cache, preserving registers and CF.
kernel_lfn_flush_fat:
    cmp word [cs:fat_cache_sector], 0xFFFF
    je .empty
    call fat12_flush_cache
    retf
.empty:
    clc
    retf
kernel_lfn_invalidate_fat:
    mov word [cs:fat_cache_sector], 0xFFFF
    mov byte [cs:fat_cache_valid], 0
    mov byte [cs:fat_cache_dirty], 0
    clc
    retf

; The extension holds InDOS through its complete operation, as before.
; Balanced operations reject wrap/underflow instead of corrupting DOS state.
kernel_lfn_enter_dos:
    cmp byte [cs:dos_indos_flag], 0xFF
    je .invalid
    inc byte [cs:dos_indos_flag]
    clc
    retf
.invalid:
    stc
    retf
kernel_lfn_leave_dos:
    cmp byte [cs:dos_indos_flag], 0
    je .invalid
    dec byte [cs:dos_indos_flag]
    clc
    retf
.invalid:
    stc
    retf
