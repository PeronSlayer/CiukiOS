; G29D: read-only view of the converter's committed redo journal. No BIOS
; writes or false durability claim: the kernel validates and replays it.
; All record headers/payloads are checked before the view is made visible.
cv_init:
    cmp dword [sector+3], 'CIUK'
    jne .done
    cmp dword [sector+7], 'ICVT'
    jne .done
    mov ax, 0x3000
    mov gs, ax
    cmp dword [sector+96], 'CVT1'
    jne fat_error
    mov si, sector+96
    mov cx, 28
    call cv_crc
    cmp eax, [sector+124]
    jne fat_error
    mov eax, [sector+112]
    cmp eax, [fs:0x7ca]
    jne fat_error
    mov [cv_sectors], eax
    mov eax, [sector+116]
    cmp eax, [B(partition_lba)]
    jne fat_error
    cmp dword [sector+108], 0 ; native boot is primary partition 1
    jne fat_error
    cmp dword [sector+120], 0
    jne fat_error
    mov eax, [sector+100]
    cmp eax, 32
    jb fat_error
    cmp eax, [cv_sectors]
    jae fat_error
    mov [cv_journal], eax
    mov edx, [cv_sectors]
    sub edx, eax
    shr edx, 1
    add eax, [B(partition_lba)]
    jc fat_error
    mov [cv_base], eax
    mov eax, [sector+104]
    cmp eax, 2
    jb fat_error
    cmp eax, 8192
    ja fat_error
    cmp eax, edx
    ja fat_error
    mov [cv_count], ax
    xor ecx, ecx
.record:
    mov eax, ecx
    shl eax, 1
    add eax, [cv_base]
    mov bx, cv_header
    call disk_raw
    inc eax
    mov bx, cv_data
    call disk_raw
    cmp dword [cv_header], 'CVR1'
    jne fat_error
    cmp ecx, [cv_header+4]
    jne fat_error
    push cx
    mov si, cv_header
    mov cx, 16
    call cv_crc
    cmp eax, [cv_header+16]
    jne fat_error
    mov si, cv_data
    mov cx, 512
    call cv_crc
    cmp eax, [cv_header+12]
    jne fat_error
    pop cx
    mov eax, [cv_header+8]
    mov si, cx
    shl si, 2
    mov [gs:cv_targets+si], eax
    mov dx, cx
    inc dx
    cmp dx, [cv_count]
    jne .nonfinal
    test eax, eax
    jnz fat_error
    jmp .next
.nonfinal:
    test eax, eax
    jz fat_error
    cmp eax, -1
    je .table
    cmp eax, [cv_sectors]
    jae fat_error
    cmp eax, [cv_journal]
    jb .next
    movzx edx, word [cv_count]
    shl edx, 1
    add edx, [cv_journal]
    cmp eax, edx
    jb fat_error
    jmp .next
.table:
    mov dx, cx
    add dx, 2
    cmp dx, [cv_count]
    jne fat_error
    cmp word [cv_data+510], 0xaa55
    jne fat_error
    cmp byte [cv_data+450], 0x0b
    je .compare
    cmp byte [cv_data+450], 0x0c
    jne fat_error
.compare:
    xor eax, eax
    mov bx, sector
    call disk_raw
    xor si, si
.compare_byte:
    cmp si, 450
    je .same
    mov al, [cv_data+si]
    cmp al, [sector+si]
    jne fat_error
.same:
    inc si
    cmp si, 512
    jb .compare_byte
.next:
    inc cx
    cmp cx, [cv_count]
    jb .record
    mov byte [cv_active], 1
    mov eax, [B(partition_lba)]
    mov bx, sector
    call disk_read ; final FAT32 BPB, from the validated journal
.done:
    ret

; CRC32/IEEE, DS:SI and CX bytes; preserve the outer record index.
cv_crc:
    push edx
    push si
    push cx
    mov eax, -1
.byte:
    xor al, [si]
    inc si
    mov dl, 8
.bit:
    shr eax, 1
    jnc .plain
    xor eax, 0xedb88320
.plain:
    dec dl
    jnz .bit
    loop .byte
    not eax
    pop cx
    pop si
    pop edx
    ret

cv_active db 0
cv_count dw 0
cv_sectors dd 0
cv_journal dd 0
cv_base dd 0
