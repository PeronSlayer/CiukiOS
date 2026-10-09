; Read-only BIOS sector access. EAX=LBA, ES:BX=512-byte destination.
disk_read:
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
    mov ah, 0x42
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
    stc
    int 0x13
.result:
    jnc .ok
    dec bp
    jz disk_error
    mov dl, [B(boot_drive)]
    xor ah, ah
    int 0x13
    jmp .retry
.ok:
    popad
    ret
disk_error:
    mov si, disk_msg
    jmp fatal
disk_msg db 'L:DISK',0
disk_edd db 0
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
    cmp word [sector+510], 0xaa55
    jne fat_error
    cmp word [sector+11], 512
    jne fat_error
    cmp byte [sector+13], 8
    jne fat_error
    cmp word [sector+14], 32
    jne fat_error
    cmp byte [sector+16], 2
    jne fat_error
    cmp word [sector+17], 0
    jne fat_error
    cmp word [sector+19], 0
    jne fat_error
    cmp word [sector+22], 0
    jne fat_error
    cmp word [sector+40], 0 ; Canonical volume mirrors both FATs.
    jne fat_error
    cmp word [sector+42], 0
    jne fat_error
    cmp word [sector+48], 1
    jne fat_error
    cmp word [sector+50], 6
    jne fat_error
    mov eax, [sector+32]
    mov [volume_sectors], eax
    xor edx, edx
    mov fs, dx
    cmp eax, [fs:0x7ca] ; MBR partition sectors.
    ja fat_error
    add eax, [B(partition_lba)]
    jc fat_error
    mov [volume_end], eax
    mov eax, [sector+36]
    test eax, eax
    jz fat_error
    mov [fat_sectors], eax
    add eax, eax
    jc fat_error
    add eax, 32
    jc fat_error
    cmp eax, [volume_sectors]
    jae fat_error
    mov edx, [volume_sectors]
    sub edx, eax
    shr edx, 3
    cmp edx, 65525
    jb fat_error
    cmp edx, 0x0ffffff5
    ja fat_error
    mov [cluster_count], edx
    add edx, 2
    mov [cluster_limit], edx
    mov ecx, [fat_sectors]
    shl ecx, 7
    cmp ecx, edx
    jb fat_error
    add eax, [B(partition_lba)]
    mov [data_lba], eax
    mov eax, [B(partition_lba)]
    add eax, 32
    mov [fat_lba], eax
    mov eax, [sector+44]
    mov [root_cluster], eax
    call cluster_validate
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
found_attr db 0
found_size dd 0

cluster_validate:
    cmp eax, 2
    jb fat_error
    cmp eax, [cluster_limit]
    jae fat_error
    ret
; EAX=cluster. Read entire 4 KiB cluster, preserve EAX and SI.
cluster_read:
    pushad
    call cluster_validate
    sub eax, 2
    shl eax, 3
    add eax, [data_lba]
    jc fat_error
    mov edx, eax
    add edx, 8
    cmp edx, [volume_end]
    ja fat_error
    mov bx, cluster
    mov cx, 8
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
    shr eax, 7
    add eax, [fat_lba]
    mov bx, sector
    call disk_read
    and dx, 127
    shl dx, 2
    mov si, dx
    mov eax, [sector+si]
    and eax, 0x0fffffff
    cmp eax, 0x0ffffff8
    jae .end
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
    mov [dir_name], si
    mov edx, [cluster_count]
.cluster:
    dec edx
    jz fat_error
    call cluster_read
    mov di, cluster
    mov bp, 128
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
    movzx eax, word [di+20]
    shl eax, 16
    mov ax, [di+26]
    and eax, 0x0fffffff
    clc
    ret
dir_name dw 0

; File cursor supports seeking/re-reading a FAT chain without a whole-file
; staging buffer. Reads are clipped by the directory's byte length.
file_open:
    mov [file_start], eax
    mov [file_cluster], eax
    mov eax, [found_size]
    mov [file_size], eax
    mov [file_left], eax
    mov word [file_offset], 4096
    mov byte [file_loaded], 0
    mov eax, [cluster_count]
    mov [file_budget], eax
    ret
file_rewind:
    mov eax, [file_start]
    mov [file_cluster], eax
    mov eax, [file_size]
    mov [file_left], eax
    mov word [file_offset], 4096
    mov byte [file_loaded], 0
    mov eax, [cluster_count]
    mov [file_budget], eax
    ret
file_getc:
    cmp dword [file_left], 0
    je .end
    cmp word [file_offset], 4096
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
.skip:
    cmp edx, 4096
    jb .tail
    mov eax, [file_cluster]
    call fat_next
    jc fat_error
    mov [file_cluster], eax
    sub edx, 4096
    sub dword [file_left], 4096
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
