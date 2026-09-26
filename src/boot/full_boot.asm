bits 16
org 0x7C00

%define STAGE1_SEG              0x0800
%define STAGE1_SLOT_SECTORS     72
%define STAGE1_LOAD_SECTORS      8
%ifndef BOOT_LBA_OFFSET
%define BOOT_LBA_OFFSET 0
%endif

%if STAGE1_LOAD_SECTORS < 1
%error STAGE1_LOAD_SECTORS must be positive
%endif
%if STAGE1_LOAD_SECTORS > STAGE1_SLOT_SECTORS
%error STAGE1_LOAD_SECTORS exceeds the reserved Stage1 slot
%endif
; Both legacy CHS fallbacks start at sector 2 and must stay on one 63-sector
; track.  The thin loader currently needs only eight sectors.
%if STAGE1_LOAD_SECTORS > 62
%error STAGE1_LOAD_SECTORS exceeds the single-track CHS transfer
%endif

jmp short boot_start
nop

; FAT16 BPB (release size supplied by the image builder)
%ifndef FAT_TOTAL_SECTORS
%define FAT_TOTAL_SECTORS 262144
%endif
bpb_oem_label         db "CIUKFULL"
bpb_bytes_per_sector  dw 512
bpb_sectors_per_clu   db 8
bpb_reserved_secs     dw (1 + STAGE1_SLOT_SECTORS)
bpb_fat_count         db 2
bpb_root_entries      dw 512
bpb_total_secs16      dw 0
bpb_media             db 0xF8
bpb_sectors_per_fat   dw 128
bpb_sectors_per_trk   dw 63
bpb_heads             dw 16
bpb_hidden_secs       dd BOOT_LBA_OFFSET
bpb_total_secs32      dd FAT_TOTAL_SECTORS
bs_drive_num          db 0x80
bs_reserved1          db 0
bs_boot_sig           db 0x29
bs_volume_id          dd 0x20260422
bs_volume_label       db "CIUKIOSFULL"
bs_fs_type            db "FAT16   "

boot_start:
    ; The BIOS may enter at 07C0:0000 or 0000:7C00. Normalize the code
    ; address before handing control to the next loader.
    jmp 0:.normalized
.normalized:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [boot_drive], dl
    mov [bs_drive_num], dl

    call serial_init

    mov si, msg_stage0
    call print_bios_string
    mov si, msg_stage0
    call print_serial_string

    mov byte [retry_count], 3

.read_stage1:
    ; EDD is independent of the physical drive's translated CHS geometry.
    ; Firmware can change the packet's transfer count on a failed read.
    mov word [stage1_dap+2], STAGE1_LOAD_SECTORS
    mov si, stage1_dap
    mov ah, 0x42
    mov dl, [boot_drive]
    call boot_disk_io
    jnc .stage1_ok
%if BOOT_LBA_OFFSET == 63
    mov ax, STAGE1_SEG
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, STAGE1_LOAD_SECTORS
    mov ch, 0
    mov cl, 2
    mov dh, 1
    mov dl, [boot_drive]
    call boot_disk_io
    jnc .stage1_ok
%elif BOOT_LBA_OFFSET == 0
    mov ax, STAGE1_SEG
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, STAGE1_LOAD_SECTORS
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    call boot_disk_io
    jnc .stage1_ok
%endif
    ; Other partition offsets require EDD. Never interpret an unoffset
    ; sector as Stage1 after a failed partition-relative read.

    xor ah, ah
    mov dl, [boot_drive]
    call boot_disk_io
    dec byte [retry_count]
    jnz .read_stage1

    mov si, msg_disk_err
    call print_bios_string
    mov si, msg_disk_err
    call print_serial_string
    jmp halt

.stage1_ok:
    mov si, msg_stage1_jump
    call print_bios_string
    mov si, msg_stage1_jump
    call print_serial_string
    mov dl, [boot_drive]
    jmp STAGE1_SEG:0x0000

halt:
    cli
    hlt
    jmp halt

; Only CF is an output here. A disk BIOS must not replace the loader's
; packet pointer, data segments or retry state. Boot runs with IRQs enabled.
boot_disk_io:
    pushad
    push ds
    push es
    push fs
    push gs
    stc
    sti
    int 0x13
    pop gs
    pop fs
    pop es
    pop ds
    popad
    cld
    sti
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

print_bios_string:
    cld
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    pushf
    pushad
    push ds
    push es
    push fs
    push gs
    int 0x10
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popf
    cld
    jmp print_bios_string
.done:
    ret

print_serial_string:
    cld
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp print_serial_string
.done:
    ret

serial_putc:
    push ax
    push cx
    push dx
    mov ah, al
    mov cx, 0x0400
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

boot_drive  db 0
retry_count db 0
align 4, db 0
stage1_dap:
    db 0x10
    db 0
    dw STAGE1_LOAD_SECTORS
    dw 0x0000
    dw STAGE1_SEG
    dd BOOT_LBA_OFFSET + 1
    dd 0

msg_stage0      db "[BOOT0-FULL] CiukiOS full stage0 ready", 13, 10, 0
msg_stage1_jump db "[BOOT0-FULL] Loading stage1", 13, 10, 0
msg_disk_err    db "[BOOT0-FULL] Disk read error", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
