bits 16
org 0x0000

; The full-profile Stage1 is deliberately loader-only.  It discovers the
; CIUKIDOS kernel through FAT16, validates the versioned image header and ABI
; table, then transfers control.  No DOS interrupt, process, allocator, EXEC,
; handle or file-service implementation lives in this binary.

%include "src/runtime/ciukidos_abi.inc"
%define CIUKIDOS_RELOCATOR_SEG   0x0060
%define LOADER_BUFFER_SEG        0x0500
; The temporary window permits a 60 KiB core. SYSVARS and process scratch
; reservations follow the actual relocated image, not its maximum capacity.
%define CIUKIDOS_MAX_SIZE CIUKIDOS_KERNEL_MAX_BYTES

%ifndef FAT_SPT
%define FAT_SPT 63
%endif
%ifndef FAT_HEADS
%define FAT_HEADS 16
%endif
%ifndef FAT_RESERVED_SECTORS
%define FAT_RESERVED_SECTORS 73
%endif
%ifndef FAT_SECTORS_PER_CLUSTER
%define FAT_SECTORS_PER_CLUSTER 8
%endif
%ifndef FAT_SECTORS_PER_FAT
%define FAT_SECTORS_PER_FAT 128
%endif
%ifndef FAT_COUNT
%define FAT_COUNT 2
%endif
%ifndef FAT_ROOT_DIR_SECTORS
%define FAT_ROOT_DIR_SECTORS 32
%endif
%ifndef FAT_LBA_OFFSET
%define FAT_LBA_OFFSET 0
%endif
%ifndef DOS_DEFAULT_DRIVE_INDEX
%define DOS_DEFAULT_DRIVE_INDEX 2
%endif

%define FAT1_LBA            FAT_RESERVED_SECTORS
%define FAT_ROOT_START_LBA  (FAT_RESERVED_SECTORS + (FAT_COUNT * FAT_SECTORS_PER_FAT))
%define FAT_DATA_START_LBA  (FAT_ROOT_START_LBA + FAT_ROOT_DIR_SECTORS)
%define FAT_LBA_OFFSET_LO   (FAT_LBA_OFFSET & 0xFFFF)
%define FAT_LBA_OFFSET_HI   ((FAT_LBA_OFFSET >> 16) & 0xFFFF)
%define FAT_DATA_LBA_LO     (FAT_DATA_START_LBA & 0xFFFF)
%define FAT_DATA_LBA_HI     ((FAT_DATA_START_LBA >> 16) & 0xFFFF)

stage1_loader_start:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x7C00
    mov ax, cs
    mov ds, ax
    mov es, ax
    sti

    mov [boot_drive], dl
%ifdef BOOT_DISK_LOG
    mov al, 4                   ; Stage1 entered
    call 0x0000:0x06E0
%endif
    ; SETUP.COM patches this immediate in the raw installed Stage1 so a
    ; direct-CD D: build becomes an installed C: build without rewriting the
    ; FAT-resident CIUKIDOS image.
    mov byte [loader_default_drive], DOS_DEFAULT_DRIVE_INDEX
    cld                         ; keep Setup's immediate patch offset unchanged

    call serial_init
    mov si, msg_loader_ready
    call print_string_dual

    call load_ciukidos
    jc stage1_loader_fatal
    mov si, msg_loader_loaded
    call print_string_dual
    call validate_ciukidos
    jc stage1_loader_fatal
%ifdef BOOT_DISK_LOG
    mov al, 5                   ; kernel loaded and validated
    call 0x0000:0x06E0
%endif
    mov si, msg_loader_valid
    call print_string_dual

    ; Select the notebook input backend while this boot-only module exists.
    ; The kernel relocation overwrites the loader, so pass only the decision
    ; in DH. A magic value distinguishes it from ordinary BIOS scratch data.
    call input_platform_firmware_first
    mov dh, 0
    jnc .input_policy_ready
    mov dh, 0xE5
.input_policy_ready:

    ; The loader occupies 0800h and therefore reads/validates the kernel at
    ; 0900h.  Once validation is complete, a tiny stub below the DOS arena
    ; moves the position-independent image to its final low-memory segment.
    ; This recovers 24 KiB of conventional memory for DOS applications while
    ; keeping both the copy loop and its temporary stack outside the source,
    ; destination and loader ranges.
    mov bl, [boot_drive]
    mov bh, [loader_default_drive]
    mov ax, [found_size_lo]
    inc ax
    shr ax, 1
    mov bp, ax
    push cs
    pop ds
    mov ax, CIUKIDOS_RELOCATOR_SEG
    mov es, ax
    mov si, ciukidos_relocator
    xor di, di
    mov cx, ciukidos_relocator_end - ciukidos_relocator
    cld
    rep movsb
    mov cx, bp
    jmp CIUKIDOS_RELOCATOR_SEG:0x0000

ciukidos_relocator:
    cli
    xor ax, ax
    mov ss, ax
    mov sp, 0x2F00
    mov ax, CIUKIDOS_LOAD_SEG
    mov ds, ax
    mov ax, CIUKIDOS_RUNTIME_SEG
    mov es, ax
    xor si, si
    xor di, di
    cld
    rep movsw
    mov al, bh
    mov dl, bl
    jmp CIUKIDOS_RUNTIME_SEG:0x0000
ciukidos_relocator_end:

%include "src/boot/input_platform.inc"

stage1_loader_fatal:
%ifdef BOOT_DISK_LOG
    mov al, 0xE2
    call 0x0000:0x06E0
%endif
    mov si, msg_loader_fatal
    call print_string_dual
.halt:
    cli
    hlt
    jmp .halt

; Locate SYSTEM in the FAT16 root directory, locate CIUKIDOS.SYS inside that
; directory, and load the complete cluster chain into CIUKIDOS_LOAD_SEG.
load_ciukidos:
    mov si, fat_name_system
    call find_root_entry
    jc .fail
    test byte [found_attr], 0x10
    jz .fail
    mov si, msg_loader_system
    call print_string_dual

    mov ax, [found_cluster]
    mov si, fat_name_ciukidos
    call find_directory_entry
    jc .fail
    test byte [found_attr], 0x10
    jnz .fail
    mov si, msg_loader_file
    call print_string_dual
    cmp word [found_size_hi], 0
    jne .fail
    mov ax, [found_size_lo]
    cmp ax, CIUKIDOS_MIN_SIZE
    jb .fail
    cmp ax, CIUKIDOS_MAX_SIZE
    ja .fail

    add ax, 511
    mov cl, 9
    shr ax, cl
    or ax, ax
    jz .fail
    mov [remaining_sectors], ax
    mov word [target_offset], 0
    mov ax, [found_cluster]
    mov [current_cluster], ax

.cluster_loop:
    mov ax, [current_cluster]
    call cluster_to_lba
    mov [scan_lba_lo], ax
    mov [scan_lba_hi], dx
    mov byte [cluster_sector_left], FAT_SECTORS_PER_CLUSTER

.sector_loop:
    cmp word [remaining_sectors], 0
    je .success
    mov ax, CIUKIDOS_LOAD_SEG
    mov es, ax
    mov bx, [target_offset]
    mov ax, [scan_lba_lo]
    mov dx, [scan_lba_hi]
    call read_sector_rel32
    jc .fail
    add word [target_offset], 512
    jc .fail
    dec word [remaining_sectors]
    ; A kernel whose rounded size ends exactly on a cluster boundary is
    ; complete here.  Do not demand another FAT link after its final sector.
    jz .success
    add word [scan_lba_lo], 1
    adc word [scan_lba_hi], 0
    dec byte [cluster_sector_left]
    jnz .sector_loop

    mov ax, [current_cluster]
    call fat16_next_cluster
    jc .fail
    cmp ax, 0xFFF8
    jae .fail
    cmp ax, 2
    jb .fail
    mov [current_cluster], ax
    jmp .cluster_loop

.success:
    clc
    ret
.fail:
    stc
    ret

; Input DS:SI -> 11-byte FAT name.  Output is copied to found_*.
find_root_entry:
    mov word [scan_lba_lo], FAT_ROOT_START_LBA
    mov word [scan_lba_hi], 0
    mov cx, FAT_ROOT_DIR_SECTORS
.sector:
    call read_scan_sector
    jc .fail
    call scan_directory_sector
    jnc .found
    cmp byte [directory_end_seen], 0
    jne .fail
    add word [scan_lba_lo], 1
    adc word [scan_lba_hi], 0
    loop .sector
.fail:
    stc
    ret
.found:
    clc
    ret

; Input AX=directory start cluster, DS:SI -> 11-byte FAT name.
find_directory_entry:
    mov [current_cluster], ax
.cluster:
    mov ax, [current_cluster]
    call cluster_to_lba
    mov [scan_lba_lo], ax
    mov [scan_lba_hi], dx
    mov byte [cluster_sector_left], FAT_SECTORS_PER_CLUSTER
.sector:
    call read_scan_sector
    jc .fail
    call scan_directory_sector
    jnc .found
    cmp byte [directory_end_seen], 0
    jne .fail
    add word [scan_lba_lo], 1
    adc word [scan_lba_hi], 0
    dec byte [cluster_sector_left]
    jnz .sector

    mov ax, [current_cluster]
    call fat16_next_cluster
    jc .fail
    cmp ax, 0xFFF8
    jae .fail
    cmp ax, 2
    jb .fail
    mov [current_cluster], ax
    jmp .cluster
.fail:
    stc
    ret
.found:
    clc
    ret

read_scan_sector:
    push ax
    push bx
    push dx
    push es
    mov ax, LOADER_BUFFER_SEG
    mov es, ax
    xor bx, bx
    mov ax, [scan_lba_lo]
    mov dx, [scan_lba_hi]
    call read_sector_rel32
    pop es
    pop dx
    pop bx
    pop ax
    ret

; Scan the sector in LOADER_BUFFER_SEG for DS:SI.  CF=0 means found.
scan_directory_sector:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
    mov byte [directory_end_seen], 0
    mov ax, LOADER_BUFFER_SEG
    mov es, ax
    xor di, di
    mov cx, 16
.entry:
    mov al, [es:di]
    or al, al
    jz .end_marker
    cmp al, 0xE5
    je .next
    mov al, [es:di + 11]
    cmp al, 0x0F
    je .next
    test al, 0x08
    jnz .next

    push cx
    push si
    push di
    mov cx, 11
.compare:
    mov al, [si]
    cmp al, [es:di]
    jne .different
    inc si
    inc di
    loop .compare
    pop di
    pop si
    pop cx
    mov ax, [es:di + 26]
    mov [found_cluster], ax
    mov ax, [es:di + 28]
    mov [found_size_lo], ax
    mov ax, [es:di + 30]
    mov [found_size_hi], ax
    mov al, [es:di + 11]
    mov [found_attr], al
    clc
    jmp .done
.different:
    pop di
    pop si
    pop cx
.next:
    add di, 32
    loop .entry
    stc
    jmp .done
.end_marker:
    mov byte [directory_end_seen], 1
    stc
.done:
    pop es
    pop di
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Input AX=cluster. Output DX:AX=relative data-sector LBA.
cluster_to_lba:
    push bx
    sub ax, 2
    mov bx, FAT_SECTORS_PER_CLUSTER
    mul bx
    add ax, FAT_DATA_LBA_LO
    adc dx, FAT_DATA_LBA_HI
    pop bx
    ret

; Input AX=cluster. Output AX=next FAT16 cluster.
fat16_next_cluster:
    push bx
    push cx
    push dx
    push di
    push es
    mov cx, ax
    mov di, ax
    and di, 0x00FF
    shl di, 1
    mov cl, 8
    shr ax, cl
    xor dx, dx
    add ax, FAT1_LBA
    adc dx, 0
    mov bx, LOADER_BUFFER_SEG
    mov es, bx
    xor bx, bx
    call read_sector_rel32
    jc .fail
    mov ax, [es:di]
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
    ret

; Input DX:AX=relative LBA, ES:BX=512-byte destination.
; EDD is preferred; a geometry-derived CHS path is the bounded fallback.
; Preserve DI: the FAT caller keeps its entry offset there, while INT 13h
; AH=08h may return a firmware table through ES:DI during CHS discovery.
read_sector_rel32:
    mov [cs:io_buffer_off], bx
    mov [cs:io_buffer_seg], es
    add ax, FAT_LBA_OFFSET_LO
    adc dx, FAT_LBA_OFFSET_HI
    mov [cs:disk_packet_lba], ax
    mov [cs:disk_packet_lba + 2], dx

    pushad
    push ds
    push es
    push fs
    push gs
    ; A failed EDD request can leave a partial/zero transfer count behind.
    ; Each invocation still requests exactly one sector.
    mov word [cs:disk_packet+2], 1
    mov [cs:disk_packet_off], bx
    mov [cs:disk_packet_seg], es
    push cs
    pop ds
    mov si, disk_packet
    mov dl, [cs:boot_drive]
    mov ah, 0x42
    stc
    sti
    int 0x13
    jnc .success

    mov ax, [cs:disk_packet_lba]
    mov dx, [cs:disk_packet_lba + 2]
    call read_sector_chs32
    jc .failure
.success:
    clc
    jmp .done
.failure:
    stc
.done:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    cld
    sti
    ret

read_sector_chs32:
    mov [cs:chs_lba_lo], ax
    mov [cs:chs_lba_hi], dx
    mov dl, [cs:boot_drive]
    mov ah, 0x08
    stc
    sti
    int 0x13
    jc .fallback_geometry
    and cl, 0x3F
    jz .fallback_geometry
    xor ax, ax
    mov al, cl
    mov [cs:chs_spt], ax
    xor ax, ax
    mov al, dh
    inc ax
    mov [cs:chs_heads], ax
    jmp .convert
.fallback_geometry:
    mov word [cs:chs_spt], FAT_SPT
    mov word [cs:chs_heads], FAT_HEADS
.convert:
    mov ax, [cs:chs_lba_lo]
    mov dx, [cs:chs_lba_hi]
    mov cx, [cs:chs_spt]
    cmp dx, cx
    jae .range_fail
    div cx
    mov cl, dl
    inc cl
    xor dx, dx
    mov bx, [cs:chs_heads]
    div bx
    cmp ax, 1023
    ja .range_fail
    mov ch, al
    mov dh, dl
    mov al, ah
    shl al, 6
    or cl, al
    mov bx, [cs:io_buffer_off]
    mov es, [cs:io_buffer_seg]
    mov dl, [cs:boot_drive]
    mov ah, 0x02
    mov al, 1
    stc
    sti
    int 0x13
    ret
.range_fail:
    mov ah, 0x04
    stc
    ret

validate_ciukidos:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
    mov ax, CIUKIDOS_LOAD_SEG
    mov es, ax
    cmp word [es:2], 0x4943       ; CI
    jne .fail
    cmp word [es:4], 0x4B55       ; UK
    jne .fail
    cmp word [es:6], 0x4449       ; ID
    jne .fail
    cmp word [es:8], 0x534F       ; OS
    jne .fail
    cmp word [es:10], CIUKIDOS_HEADER_SIZE
    jne .fail
    cmp word [es:12], CIUKIDOS_ABI_VERSION
    jne .fail
    cmp word [es:14], CIUKIDOS_SERVICE_COUNT
    jne .fail
    cmp word [es:16], CIUKIDOS_DESCRIPTOR_SIZE
    jne .fail
    cmp word [es:18], CIUKIDOS_CAPABILITIES
    jne .fail
    mov ax, [found_size_lo]
    cmp [es:20], ax
    jne .fail
    cmp word [es:22], CIUKIDOS_TABLE_OFFSET
    jne .fail
    cmp word [es:24], CIUKIDOS_LOAD_SEG
    jne .fail

    mov di, [es:22]
    cmp word [es:di], 0x5452      ; RT
    jne .fail
    cmp word [es:di + 2], 0x5653  ; SV
    jne .fail
    cmp word [es:di + 4], CIUKIDOS_ABI_VERSION
    jne .fail
    cmp word [es:di + 6], CIUKIDOS_SERVICE_COUNT
    jne .fail
    cmp word [es:di + 8], CIUKIDOS_DESCRIPTOR_SIZE
    jne .fail
    add di, 10
    mov bx, 1
    mov cx, CIUKIDOS_SERVICE_COUNT
.descriptor:
    cmp [es:di], bx
    jne .fail
    cmp word [es:di + 2], 1
    jne .fail
    cmp word [es:di + 4], 0
    je .fail
    mov dx, [es:di + 4]
    cmp dx, CIUKIDOS_MIN_SIZE
    jb .fail
    cmp dx, [found_size_lo]
    jae .fail
    cmp word [es:di + 6], 0
    jne .fail
    inc bx
    add di, CIUKIDOS_DESCRIPTOR_SIZE
    loop .descriptor
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

serial_init:
    mov dx, 0x03F9
    xor al, al
    out dx, al
    mov dx, 0x03FB
    mov al, 0x80
    out dx, al
    mov dx, 0x03F8
    mov al, 0x03
    out dx, al
    mov dx, 0x03F9
    xor al, al
    out dx, al
    mov dx, 0x03FB
    mov al, 0x03
    out dx, al
    mov dx, 0x03FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x03FC
    mov al, 0x0B
    out dx, al
    ret

print_string_dual:
    cld
    lodsb
    test al, al
    jz .done
    push ax
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
    pop ax
    call serial_putc
    jmp print_string_dual
.done:
    ret

serial_putc:
    push ax
    push cx
    push dx
    mov ah, al
    mov cx, 0x0400
.wait:
    mov dx, 0x03FD
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

fat_name_system   db 'SYSTEM     '
fat_name_ciukidos db 'CIUKIDOSSYS'
msg_loader_ready  db '[STAGE1] loader-only CIUKIDOS handoff', 13, 10, 0
msg_loader_system db '[STAGE1] SYSTEM found', 13, 10, 0
msg_loader_file   db '[STAGE1] CIUKIDOS file found', 13, 10, 0
msg_loader_loaded db '[STAGE1] CIUKIDOS loaded', 13, 10, 0
msg_loader_valid  db '[STAGE1] CIUKIDOS ABI2 valid', 13, 10, 0
msg_loader_fatal  db 'WOOF! CiukiOS ran into a problem.', 13, 10
                  db '- CIUKIDOS.SYS missing or invalid', 13, 10
                  db 'System halted.', 13, 10, 0

boot_drive db 0
loader_default_drive db 0
found_attr db 0
directory_end_seen db 0
cluster_sector_left db 0
found_cluster dw 0
found_size_lo dw 0
found_size_hi dw 0
current_cluster dw 0
remaining_sectors dw 0
target_offset dw 0
scan_lba_lo dw 0
scan_lba_hi dw 0
io_buffer_off dw 0
io_buffer_seg dw 0
chs_lba_lo dw 0
chs_lba_hi dw 0
chs_spt dw FAT_SPT
chs_heads dw FAT_HEADS

align 4, db 0
disk_packet:
    db 0x10, 0
    dw 1
disk_packet_off dw 0
disk_packet_seg dw 0
disk_packet_lba dd 0
                    dd 0

%if ($ - $$) > 0x1000
    %error "full Stage1 loader overlaps CIUKIDOS load segment"
%endif
