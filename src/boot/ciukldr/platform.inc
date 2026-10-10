; Minimum checks precede disk discovery/loading and native i8042 access.
; Intel/AMD CPUID: toggle EFLAGS.ID, restore flags, then query basic leaf 1.
; No CPUID is executed until the ID test succeeds; FXSR/SSE are optional.
cpu_minimum:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    test eax, 1 << 21
    jz .bad
    xor eax, eax
    cpuid
    cmp eax, 1
    jb .bad
    mov eax, 1
    cpuid
    mov [cpu_signature], eax
    mov [cpu_features], edx
    ; Diagnostics stay outside frozen ciuki_boot_info v1 reserved fields.
    mov di, cpu_signature_text
    call cpu_hex
    mov eax, [cpu_signature]
    mov ebx, eax
    shr ebx, 8
    and ebx, 15
    mov edx, ebx
    cmp ebx, 15
    jne .family
    shr eax, 20
    and eax, 255
    add edx, eax
.family:
    mov eax, edx
    mov di, cpu_family_text
    call cpu_hex
    mov eax, [cpu_signature]
    shr eax, 4
    and eax, 15
    cmp ebx, 6
    je .extended_model
    cmp ebx, 15
    jne .model
.extended_model:
    mov edx, [cpu_signature]
    shr edx, 12
    and edx, 0xf0
    or eax, edx
.model:
    mov di, cpu_model_text
    call cpu_hex
    mov eax, [cpu_signature]
    and eax, 15
    mov di, cpu_stepping_text
    call cpu_hex
    mov si, cpu_diagnostic
    call print
    test dword [cpu_features], 1 << 15
    jz .bad
    ret
.bad:
    mov si, cpu_error
    jmp minimum_halt

; EAX -> eight hexadecimal ASCII digits at DS:DI, preserving registers.
cpu_hex:
    pushad
    mov cx, 8
.digit:
    rol eax, 4
    mov bx, ax
    and bx, 15
    mov dl, [cpu_hex_digits+bx]
    mov [di], dl
    inc di
    loop .digit
    popad
    ret
cpu_signature dd 0
cpu_features dd 0
cpu_hex_digits db '0123456789abcdef'
cpu_diagnostic db 'L:CPU signature='
cpu_signature_text db '00000000 family='
cpu_family_text db '00000000 model='
cpu_model_text db '00000000 stepping='
cpu_stepping_text db '00000000',13,10,0
cpu_error db 'CIUKI: this PC needs an i686 CPU with CMOV (Pentium Pro or later)',0

; Query only, never modeset before BOOT.CFG/menu. A responding pre-2.0
; controller is unsupported. An absent controller keeps the contract's
; text/serial recovery path (including the -vga none regression cases).
vbe_minimum:
    mov dword [B(vbe_ctrl)], 'VBE2'
    mov di, B(vbe_ctrl)
    mov ax, 0x4f00
    int 0x10
    cmp ax, 0x004f
    jne .absent
    cmp dword [B(vbe_ctrl)], 'VESA'
    jne .bad
    cmp word [B(vbe_ctrl)+4], 0x0200
    jb .bad
    ret
.absent:
    ret
.bad:
    mov si, vbe_error
    jmp minimum_halt
vbe_error db 'CIUKI: this PC needs VBE 2.0 or later for a linear framebuffer',0

; Minimum failures do not wait for a keyboard, retry firmware or access disk.
minimum_halt:
    call print
    mov si, newline
    call print
    cli
.halt:
    hlt
    jmp .halt

; Optional firmware discovery precedes all direct i8042 operations.
platform_collect:
    mov ax, 0xb101
    xor edx, edx
    stc
    int 0x1a
    jc .no_pci
    test ah, ah
    jnz .no_pci
    cmp edx, 0x20494350
    jne .no_pci
    movzx edx, al
    shl edx, 8
    mov dl, 1
    movzx eax, cl
    shl eax, 16
    or eax, edx
    mov [B(pci_bios)], eax
.no_pci:
    call smbios_scan
    mov word [B(edd)], 66
    mov si, B(edd)
    mov dl, [B(boot_drive)]
    mov ah, 0x48
    stc
    int 0x13
    jnc .edd_ok
    mov di, B(edd)
    mov cx, 66
    xor ax, ax
    rep stosb
.edd_ok:
    mov ax, 0x5300
    xor bx, bx
    stc
    int 0x15
    jc .no_apm
    cmp bx, 0x504d
    jne .no_apm
    mov [B(apm)], ax
.no_apm:
    call rsdp_scan
    ret

smbios_scan:
    mov ax, 0xf000
    mov fs, ax
    xor bx, bx
.scan:
    cmp dword [fs:bx], 0x334d535f ; _SM3_
    je .sm3
    cmp dword [fs:bx], 0x5f4d535f ; _SM_
    jne .next
    movzx cx, byte [fs:bx+5]
    cmp cx, 0x1f
    jb .next
    cmp cx, 64
    ja .next
    mov dx, bx
    add dx, cx
    jc .next
    mov si, bx
    call checksum_fs
    test al, al
    jnz .next
    cmp dword [fs:bx+16], 0x494d445f
    jne .next
    cmp byte [fs:bx+20], '_'
    jne .next
    lea si, [bx+16]
    mov cx, 15
    call checksum_fs
    test al, al
    jnz .next
    movzx eax, bx
    add eax, 0xf0000
    mov [B(smbios_entry_phys)], eax
    movzx edx, word [fs:bx+22]
    mov eax, [fs:bx+24]
    call smbios_table
    ret
.sm3:
    cmp bx, 65512
    ja .next
    cmp byte [fs:bx+4], '_'
    jne .next
    movzx cx, byte [fs:bx+6]
    cmp cx, 24
    jb .next
    cmp cx, 64
    ja .next
    mov dx, bx
    add dx, cx
    jc .next
    mov si, bx
    call checksum_fs
    test al, al
    jnz .next
    movzx eax, bx
    add eax, 0xf0000
    mov [B(smbios_entry_phys)], eax
    cmp dword [fs:bx+20], 0
    jne .absent_table
    mov edx, [fs:bx+12]
    mov eax, [fs:bx+16]
    call smbios_table
.absent_table:
    ret
.next:
    add bx, 16
    jnc .scan
    ret

; EAX physical table, EDX length. Only conventional/ROM memory is read.
smbios_table:
    test edx, edx
    jz .done
    mov ecx, eax
    add ecx, edx
    jc .done
    cmp ecx, 0x100000
    ja .done
    mov ebx, eax
    and ebx, 15
    shr eax, 4
    mov fs, ax
    add edx, ebx
    cmp edx, 65535
    ja .done
    mov [smb_end], dx
.structure:
    mov si, bx
    add si, 4
    jc .done
    cmp si, [smb_end]
    ja .done
    movzx cx, byte [fs:bx+1]
    cmp cx, 4
    jb .done
    mov si, bx
    add si, cx
    jc .done
    cmp si, [smb_end]
    jae .done
    cmp byte [fs:bx], 1
    jne .skip_strings
    cmp cx, 8
    jb .done
    mov cl, [fs:bx+4] ; Manufacturer string index.
    jcxz .done
.string:
    dec cl
    jz .manufacturer
.skip_one:
    cmp si, [smb_end]
    jae .done
    cmp byte [fs:si], 0
    je .one_end
    inc si
    jmp .skip_one
.one_end:
    inc si
    jmp .string
.manufacturer:
    mov bx, si
    add bx, 5
    jc .done
    cmp bx, [smb_end]
    ja .done
    cmp dword [fs:si], 0x554d4551
    jne .done
    cmp byte [fs:si+4], 0
    jne .done
    or dword [B(flags)], CBI_F_SMBIOS_QEMU
    ret
.skip_strings:
    mov bx, si
    inc si
    cmp si, [smb_end]
    jae .done
    cmp word [fs:bx], 0
    jne .skip_strings
    inc si
    mov bx, si
    jmp .structure
.done:
    ret
smb_end dw 0

checksum_fs:
    push si
    xor al, al
.sum:
    add al, [fs:si]
    inc si
    loop .sum
    pop si
    ret

rsdp_scan:
    xor ax, ax
    mov fs, ax
    mov ax, [fs:0x40e]
    cmp ax, 0x8000
    jb .rom
    cmp ax, 0x9fc0
    ja .rom
    mov fs, ax
    mov cx, 64
    call .range
    jc .done
.rom:
    mov ax, 0xe000
    mov fs, ax
    mov cx, 4096
    call .range
    jc .done
    mov ax, 0xf000
    mov fs, ax
    mov cx, 4096
    call .range
.done:
    ret
.range:
    xor bx, bx
.slot:
    push cx
    cmp dword [fs:bx], 0x20445352
    jne .next
    cmp dword [fs:bx+4], 0x20525450
    jne .next
    cmp bx, 65516
    ja .next
    mov si, bx
    mov cx, 20
    call checksum_fs
    test al, al
    jnz .next
    cmp byte [fs:bx+15], 2
    jb .valid
    cmp bx, 65500
    ja .next
    mov ecx, [fs:bx+20]
    cmp ecx, 36
    jb .next
    cmp ecx, 4096
    ja .next
    movzx eax, bx
    add eax, ecx
    cmp eax, 65536
    ja .next
    call checksum_fs
    test al, al
    jnz .next
.valid:
    xor eax, eax
    mov ax, fs
    shl eax, 4
    movzx edx, bx
    add eax, edx
    mov [B(acpi_rsdp_phys)], eax
    pop cx
    stc
    ret
.next:
    pop cx
    add bx, 16
    loop .slot
    clc
    ret

fwcfg_request:
    test dword [B(flags)], CBI_F_SMBIOS_QEMU
    jz .done
    mov dx, 0x510
    xor ax, ax
    out dx, ax
    inc dx
    mov di, fw_file
    mov cx, 4
    rep insb
    cmp dword [fw_file], 0x554d4551
    jne .done
    dec dx
    mov ax, 0x19
    out dx, ax
    inc dx
    mov di, fw_file
    mov cx, 4
    rep insb
    mov eax, [fw_file]
    bswap eax
    cmp eax, 256 ; Finite directory walk; no DMA.
    ja .warning
    mov bp, ax
.file:
    test bp, bp
    jz .done
    dec bp
    mov di, fw_file
    mov cx, 64
    rep insb
    mov si, fw_name
    mov di, fw_file+8
    mov cx, fw_name_end-fw_name
    repe cmpsb
    jne .file
    mov eax, [fw_file]
    bswap eax
    test eax, eax
    jz .warning
    cmp eax, 64
    ja .warning
    mov cx, ax
    mov ax, [fw_file+4]
    xchg al, ah
    cmp ax, 0x20
    jb .warning
    test ax, 0x4000
    jnz .warning
    dec dx
    out dx, ax
    inc dx
    mov di, selector_buffer
    push cx
    rep insb
    pop cx
    mov si, selector_buffer
    mov byte [selector_fw], 1
    call selector_validate
    jc .warning
    call selector_accept
.done:
    ret
.warning:
    mov si, selection_error
    call print
    ret
fw_name db 'opt/it.alcybercloud.ciukios/test',0
fw_name_end:
selection_error db 'L:SELECT_ERROR',13,10,0

; UART discovery: BDA COM1, scratch-register round-trip and LSR presence.
uart_init:
    xor ax, ax
    mov fs, ax
    cmp word [fs:0x400], 0x3f8
    jne .absent
    mov dx, 0x3ff
    in al, dx
    mov bl, al
    mov al, 0x5a
    out dx, al
    in al, dx
    mov ah, al
    mov al, bl
    out dx, al
    cmp ah, 0x5a
    jne .absent
    mov dx, 0x3fd
    in al, dx
    cmp al, 0xff
    je .absent
    mov dx, 0x3f9
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 3
    out dx, al
    in al, dx
    cmp al, 3
    jne .restore_absent
    inc dx
    xor al, al
    out dx, al
    in al, dx
    test al, al
    jnz .restore_absent
    mov dx, 0x3fb
    mov al, 3
    out dx, al
    mov dword [B(uart_base)], 0x3f8
    mov dword [B(uart_divisor)], 3
    or dword [B(flags)], CBI_F_SERIAL_LOG
.absent:
    ret
.restore_absent:
    mov dx, 0x3fb
    mov al, 3
    out dx, al
    ret

print:
    lodsb
    test al, al
    jz .end
    pushad
    push ds
    push es
    mov bl, 7
    mov bh, 0
    mov ah, 0x0e
    int 0x10
    pop es
    pop ds
    popad
    call uart_putc
    jmp print
.end:
    ret

uart_putc:
    push ax
    push cx
    push dx
    cmp dword [B(uart_base)], 0
    je .done
    cmp word [uart_budget], 0
    je .disable
    dec word [uart_budget]
    mov dx, 0x3fd
    mov cx, 0x1000
.poll:
    in al, dx
    test al, 0x20
    jnz .send
    loop .poll
.disable:
    mov dword [B(uart_base)], 0
    mov dword [B(uart_divisor)], 0
    jmp .done
.send:
    mov dx, 0x3f8
    mov al, [ss:esp+4]
    out dx, al
.done:
    pop dx
    pop cx
    pop ax
    ret
uart_budget dw 16384 ; Covers two passes over the bounded mode list plus errors.
