; Canonical system FAT32 layout; checked against the image builder on host.
%define CIUKI_FAT_RESERVED_SECTORS 160
; BIOS sector access. EAX=LBA, ES:BX=512-byte buffer. Reads unless
; disk_op is 1: G14's boot-status write (INT 13h AH=43h AL=0 with EDD, AH=03h
; otherwise; IBM/Phoenix EDD 3.0 and the PC/AT BIOS). A failed write is not
; fatal: the Setup CD's emulated disk is write-protected (SeaBIOS cdemu).
disk_read:
    cmp byte [cv_active], 0
    je disk_raw
    cmp byte [disk_op], 0
    jne disk_raw
    pushad
    mov edx, eax
    sub edx, [B(partition_lba)]
    jc .raw
    cmp edx, [cv_sectors]
    jae .raw
    xor ecx, ecx
.search:
    mov si, cx
    shl si, 2
    cmp edx, [gs:cv_targets+si]
    jne .next
    mov eax, ecx
    shl eax, 1
    add eax, [cv_base]
    inc eax
.next:
    inc cx
    cmp cx, [cv_count]
    jb .search
.raw:
    call disk_raw
    popad
    ret
disk_raw:
    call progress_require
    pushad
    mov [disk_dap+8], eax
    mov [disk_dap+4], bx
    mov [disk_dap+6], es
    mov bp, 3
.retry:
    mov dl, [B(boot_drive)]
    cmp byte [disk_edd], 0
    je .chs
    mov si, disk_dap
    mov word [disk_dap+2], 1
    mov ax, 0x4200
    add ah, [disk_op]
    stc
    int 0x13
    jmp .result
.chs:
    mov eax, [disk_dap+8]
    xor edx, edx
    movzx ecx, word [disk_spt]
    div ecx
    inc dl
    mov cl, dl
    push cx
    xor edx, edx
    movzx ecx, word [disk_heads]
    div ecx
    pop cx
    cmp eax, 1024
    jae disk_error
    mov dh, dl
    mov ch, al
    shl ah, 6
    or cl, ah
    mov dl, [B(boot_drive)]
    mov bx, [disk_dap+4]
    mov ax, 0x0201
    add ah, [disk_op]
    stc
    int 0x13
.result:
    jnc .ok
    dec bp
    jz .failed
    mov dl, [B(boot_drive)]
    xor ah, ah
    int 0x13
    jmp .retry
.ok:
    popad
    call progress_require
    ret
.failed:
    cmp byte [disk_op], 0
    jne .ok
disk_error:
    mov si, disk_msg
    jmp fatal
disk_msg db 'L:DISK',0
disk_edd db 0
disk_op db 0
disk_spt dw 0
disk_heads dw 0
align 4, db 0
disk_dap db 16,0
    dw 1,0,0
    dq 0

fat_mount:
    mov dl, [B(boot_drive)]
    mov ah, 0x41
    mov bx, 0x55aa
    stc
    int 0x13
    jc .geometry
    cmp bx, 0xaa55
    jne .geometry
    test cx, 1
    jz .geometry
    mov byte [disk_edd], 1
    jmp .bpb
.geometry:
    mov ah, 8
    mov dl, [B(boot_drive)]
    int 0x13
    jc disk_error
    and cx, 63
    jz disk_error
    mov [disk_spt], cx
    movzx ax, dh
    inc ax
    mov [disk_heads], ax
.bpb:
    mov eax, [B(partition_lba)]
    cmp eax, 2048
    jne fat_error
    mov bx, sector
    call disk_read
    call cv_init
    cmp word [sector+510], 0xaa55
    jne fat_error
    cmp word [sector+11], 512
    jne fat_error
    mov al, [sector+13]
    test al, al
    jz fat_error
    cmp al, 64
    ja fat_error
    mov ah, al
    dec ah
    test al, ah
    jnz fat_error
    mov [cluster_sectors], al
    movzx ax, al
    shl ax, 9
    mov [cluster_bytes], ax
    cmp byte [sector+16], 2
    jne fat_error
    movzx eax, word [sector+14]
    test eax, eax
    jz fat_error
    mov [reserved_sectors], eax
    movzx eax, word [sector+19]
    test eax, eax
    jnz .total
    mov eax, [sector+32]
.total:
    mov [volume_sectors], eax
    cmp eax, [fs:0x7ca]
    ja fat_error
    add eax, [B(partition_lba)]
    jc fat_error
    mov [volume_end], eax
    movzx eax, word [sector+22]
    mov byte [fat_type], 16
    test eax, eax
    jnz .fat16
    mov byte [fat_type], 32
    cmp word [sector+17], 0
    jne fat_error
    cmp word [sector+40], 0
    jne fat_error
    cmp word [sector+42], 0
    jne fat_error
    cmp word [sector+48], 1
    jne fat_error
    cmp word [sector+50], 6
    jne fat_error
    mov eax, [sector+36]
    test eax, eax
    jz fat_error
    cmp dword [reserved_sectors], 32
    jb fat_error
    mov edx, [sector+44]
    mov [root_cluster], edx
    xor edx, edx
    jmp .layout
.fat16:
    movzx edx, word [sector+17]
    test edx, edx
    jz fat_error
    add edx, 15
    shr edx, 4
    mov [root_sectors], edx
    mov dword [root_cluster], 0
.layout:
    mov [fat_sectors], eax
    add eax, eax
    jc fat_error
    add eax, [reserved_sectors]
    jc fat_error
    add eax, [B(partition_lba)]
    jc fat_error
    mov [root_lba], eax
    add eax, edx
    jc fat_error
    cmp eax, [volume_end]
    jae fat_error
    mov [data_lba], eax
    mov edx, [volume_end]
    sub edx, eax
    mov eax, edx
    xor edx, edx
    movzx ecx, byte [cluster_sectors]
    div ecx
    cmp byte [fat_type], 32
    je .fat32_count
    cmp eax, 4085
    jb fat_error
    cmp eax, 65525
    jae fat_error
    mov cl, 8
    jmp .count
.fat32_count:
    cmp eax, 65525
    jb fat_error
    cmp eax, 0x0ffffff5
    ja fat_error
    mov cl, 7
.count:
    mov [cluster_count], eax
    add eax, 2
    mov [cluster_limit], eax
    mov edx, [fat_sectors]
    shl edx, cl
    cmp edx, eax
    jb fat_error
    mov eax, [B(partition_lba)]
    add eax, [reserved_sectors]
    mov [fat_lba], eax
    mov eax, [root_cluster]
    cmp byte [fat_type], 16
    je .root_ok
    call cluster_validate
.root_ok:
    ; Locate SYSTEM using its short entry and require a directory.
    mov si, system_name
    call directory_find
    jc fat_error
    test byte [found_attr], 0x10
    jz fat_error
    mov [system_cluster], eax
    ret
fat_error:
    mov si, fat_msg
    jmp fatal
fat_msg db 'L:FAT32',0
system_name db 'SYSTEM     '
kernel_name db 'VMM     ELF'
config_name db 'BOOT    CFG'
volume_sectors dd 0
volume_end dd 0
fat_sectors dd 0
fat_lba dd 0
data_lba dd 0
cluster_count dd 0
cluster_limit dd 0
root_cluster dd 0
system_cluster dd 0
fat_type db 32
cluster_sectors db 8
cluster_bytes dw 4096
reserved_sectors dd 0
root_sectors dd 0
root_lba dd 0
found_attr db 0
found_size dd 0

cluster_validate:
    cmp eax, 2
    jb fat_error
    cmp eax, [cluster_limit]
    jae fat_error
    ret
; EAX=cluster. Read one cluster (at most 32 KiB), preserve EAX and SI.
cluster_read:
    pushad
    call cluster_validate
    sub eax, 2
    movzx ecx, byte [cluster_sectors]
    mul ecx
    add eax, [data_lba]
    jc fat_error
    mov edx, eax
    add edx, ecx
    cmp edx, [volume_end]
    ja fat_error
    mov bx, cluster
    movzx cx, byte [cluster_sectors]
.sector:
    call disk_read
    inc eax
    add bx, 512
    loop .sector
    popad
    ret
; EAX=current -> EAX=next, CF=end; bad/reserved links are fatal.
fat_next:
    push ebx
    push edx
    push esi
    mov edx, eax
    cmp byte [fat_type], 16
    je .fat16
    shr eax, 7
    jmp .sector
.fat16:
    shr eax, 8
.sector:
    add eax, [fat_lba]
    mov bx, sector
    call disk_read
    cmp byte [fat_type], 16
    je .entry16
    and dx, 127
    shl dx, 2
    mov si, dx
    mov eax, [sector+si]
    and eax, 0x0fffffff
    cmp eax, 0x0ffffff8
    jae .end
    jmp .validate
.entry16:
    and dx, 255
    shl dx, 1
    mov si, dx
    movzx eax, word [sector+si]
    cmp eax, 0xfff8
    jae .end
.validate:
    call cluster_validate
    clc
    jmp .return
.end:
    stc
.return:
    pop esi
    pop edx
    pop ebx
    ret

; EAX=directory cluster, SI=11-byte short name -> EAX=start, size/attr globals.
; Walk bounded by the volume cluster count, so cyclic chains cannot hang.
directory_find:
    mov byte [dir_fixed], 0
    mov [dir_name], si
    mov edx, [cluster_count]
    test eax, eax
    jz .fixed_root
.cluster:
    dec edx
    jz fat_error
    call cluster_read
    mov di, cluster
    mov bp, [cluster_bytes]
    shr bp, 5
    jmp .entry
.fixed_root:
    mov byte [dir_fixed], 1
    mov eax, [root_lba]
    mov edx, [root_sectors]
.root_sector:
    mov bx, cluster
    call disk_read
    mov di, cluster
    mov bp, 16
.entry:
    cmp byte [di], 0
    je .missing
    cmp byte [di], 0xe5
    je .next
    test byte [di+11], 8 ; Skip volume labels and LFNs.
    jnz .next
    push di
    push eax
    mov si, [dir_name]
    mov cx, 11
    repe cmpsb
    pop eax
    pop di
    je .found
.next:
    add di, 32
    dec bp
    jnz .entry
    cmp byte [dir_fixed], 0
    je .chain
    inc eax
    dec edx
    jnz .root_sector
    jmp .missing
.chain:
    call fat_next
    jnc .cluster
.missing:
    stc
    ret
.found:
    mov al, [di+11]
    mov [found_attr], al
    mov eax, [di+28]
    mov [found_size], eax
    xor eax, eax
    cmp byte [fat_type], 16
    je .low
    movzx eax, word [di+20]
    shl eax, 16
.low:
    mov ax, [di+26]
    and eax, 0x0fffffff
    clc
    ret
dir_fixed db 0
dir_name dw 0

; File cursor supports seeking/re-reading a FAT chain without a whole-file
; staging buffer. Reads are clipped by the directory's byte length.
file_open:
    mov [file_start], eax
    mov [file_cluster], eax
    mov eax, [found_size]
    mov [file_size], eax
    mov [file_left], eax
    mov ax, [cluster_bytes]
    mov [file_offset], ax
    mov byte [file_loaded], 0
    mov eax, [cluster_count]
    mov [file_budget], eax
    ret
file_rewind:
    mov eax, [file_start]
    mov [file_cluster], eax
    mov eax, [file_size]
    mov [file_left], eax
    mov ax, [cluster_bytes]
    mov [file_offset], ax
    mov byte [file_loaded], 0
    mov eax, [cluster_count]
    mov [file_budget], eax
    ret
file_getc:
    cmp dword [file_left], 0
    je .end
    mov ax, [cluster_bytes]
    cmp [file_offset], ax
    jb .byte
    mov eax, [file_cluster]
    cmp byte [file_loaded], 0
    je .read
    call fat_next
    jc fat_error
    mov [file_cluster], eax
.read:
    dec dword [file_budget]
    jz fat_error
    call cluster_read
    mov byte [file_loaded], 1
    mov word [file_offset], 0
.byte:
    push bx
    mov bx, [file_offset]
    mov al, [cluster+bx]
    inc word [file_offset]
    dec dword [file_left]
    pop bx
    clc
    ret
.end:
    stc
    ret
; EAX=file offset. Bounded by file size, skip whole clusters.
file_seek:
    cmp eax, [file_size]
    ja elf_error
    push eax
    call file_rewind
    pop edx
    cmp edx, [file_size]
    jne .skip
    mov dword [file_left], 0
    ret
.skip:
    movzx ecx, word [cluster_bytes]
    cmp edx, ecx
    jb .tail
    mov eax, [file_cluster]
    call fat_next
    jc fat_error
    mov [file_cluster], eax
    sub edx, ecx
    sub [file_left], ecx
    dec dword [file_budget]
    jz fat_error
    jmp .skip
.tail:
    cmp dword [file_left], 0
    je .done
    mov eax, [file_cluster]
    call cluster_read
    mov byte [file_loaded], 1
    mov [file_offset], dx
    sub [file_left], edx
.done:
    ret
; CX=count, ES:DI=low buffer. Count must be present in file.
file_read:
    push cx
    push edi
    push ecx
    push edx
.byte:
    push cx
    call file_getc
    pop cx
    jc elf_error
    stosb
    loop .byte
    pop edx
    pop ecx
    pop edi
    pop cx
    ret
file_start dd 0
file_cluster dd 0
file_size dd 0
file_left dd 0
file_budget dd 0
file_offset dw 0
file_loaded db 0

%include "src/boot/ciukldr/convert.inc"
