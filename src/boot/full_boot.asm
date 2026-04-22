bits 16
org 0x7C00

%define STAGE1_SEG     0x0800
%define STAGE1_SECTORS 21

jmp short boot_start
nop

; FAT16 BPB (128MB disk)
bpb_oem_label         db "CIUKFULL"
bpb_bytes_per_sector  dw 512
bpb_sectors_per_clu   db 8
bpb_reserved_secs     dw (1 + STAGE1_SECTORS)
bpb_fat_count         db 2
bpb_root_entries      dw 512
bpb_total_secs16      dw 0
bpb_media             db 0xF8
bpb_sectors_per_fat   dw 128
bpb_sectors_per_trk   dw 63
bpb_heads             dw 16
bpb_hidden_secs       dd 0
bpb_total_secs32      dd 262144
bs_drive_num          db 0x80
bs_reserved1          db 0
bs_boot_sig           db 0x29
bs_volume_id          dd 0x20260422
bs_volume_label       db "CIUKIOSFULL"
bs_fs_type            db "FAT16   "

boot_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl
    mov [bs_drive_num], dl

    call serial_init

    mov si, msg_stage0
    call print_bios_string
    mov si, msg_stage0
    call print_serial_string

    mov byte [retry_count], 3

.read_stage1:
    mov ax, STAGE1_SEG
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, STAGE1_SECTORS
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    int 0x13
    jnc .stage1_ok

    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
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
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print_bios_string
.done:
    ret

print_serial_string:
    lodsb
    test al, al
    jz .done
    call serial_putc
    jmp print_serial_string
.done:
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

boot_drive  db 0
retry_count db 0

msg_stage0      db "[BOOT0-FULL] CiukiOS full stage0 ready", 13, 10, 0
msg_stage1_jump db "[BOOT0-FULL] Loading stage1", 13, 10, 0
msg_disk_err    db "[BOOT0-FULL] Disk read error", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
