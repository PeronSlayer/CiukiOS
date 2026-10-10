a20_test:
    pushf
    cli
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov ax, 0xffff
    mov es, ax
    mov al, [0x500]
    mov ah, [es:0x510]
    push ax
    mov byte [0x500], 0
    mov byte [es:0x510], 0xff
    cmp byte [0x500], 0xff
    pop ax
    mov [es:0x510], ah
    mov [0x500], al
    pop es
    pop ds
    jne .on
    popf
    stc
    ret
.on:
    popf
    clc
    ret

a20_enable:
    call a20_test
    mov bx, CBI_A20_ALREADY_ON
    jnc .ok
    mov ax, 0x2401
    int 0x15
    call a20_test
    mov bx, CBI_A20_INT15
    jnc .ok
    cmp byte [B(input_policy)], 1
    je .port92
    ; KBC output-port method. Every wait is finite; no controller reset.
    call kbc_wait_write
    jc .port92
    mov al, 0xad
    out 0x64, al
    call kbc_wait_write
    jc .reenable
    mov al, 0xd0
    out 0x64, al
    mov cx, 0xffff
.output:
    in al, 0x64
    test al, 1
    jnz .read
    loop .output
    jmp .reenable
.read:
    in al, 0x60
    mov bl, al
    call kbc_wait_write
    jc .reenable
    mov al, 0xd1
    out 0x64, al
    call kbc_wait_write
    jc .reenable
    mov al, bl
    or al, 2
    out 0x60, al
.reenable:
    call kbc_wait_write
    jc .port92
    mov al, 0xae
    out 0x64, al
    call a20_test
    mov bx, CBI_A20_KBC
    jnc .ok
.port92:
    in al, 0x92
    or al, 2 ; Preserve bit 0 (fast reset).
    out 0x92, al
    call a20_test
    mov bx, CBI_A20_PORT92
    jnc .ok
    mov si, a20_error
    jmp fatal
.ok:
    shl bx, CBI_F_A20_SHIFT
    or [B(flags)], bx
    ret
kbc_wait_write:
    mov cx, 0xffff
.poll:
    in al, 0x64
    test al, 2
    jz .ok
    loop .poll
    stc
    ret
.ok:
    clc
    ret
a20_error db 'L:A20',0

memory_collect:
    mov ax, 0xe801
    stc
    int 0x15
    jc .int88
    test cx, cx
    jz .e801_ax
    mov ax, cx
    mov bx, dx
.e801_ax:
    movzx eax, ax
    mov [B(e801_low_kib)], eax
    movzx eax, bx
    mov [B(e801_high_64k)], eax
.int88:
    mov ah, 0x88
    stc
    int 0x15
    jc .e820
    movzx eax, ax
    mov [B(int88_kib)], eax
.e820:
    xor ebx, ebx
    mov di, raw_map
    mov bp, CIUKI_E820_MAX
.call:
    mov dword [es:di+20], 1
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 24
    stc
    int 0x15
    jc memory_error
    cmp eax, 0x534d4150
    jne memory_error
    cmp ecx, 20
    je .short
    cmp ecx, 24
    jne memory_error
    jmp .record
.short:
    mov dword [di+20], 1
.record:
    mov eax, [di+8]
    mov edx, [di+12]
    mov ecx, eax
    or ecx, edx
    jz memory_error
    add eax, [di]
    adc edx, [di+4]
    jc memory_error
    cmp dword [di+16], 0
    je memory_error
    ; Internal form is [base,end,type,ext]. Unknown types become reserved.
    mov [di+8], eax
    mov [di+12], edx
    cmp dword [di+16], 5
    jbe .ext
    mov dword [di+16], 2
.ext:
    test byte [di+20], 1
    jz .continue
    inc word [raw_count]
    add di, 24
.continue:
    test ebx, ebx
    jz .normalize
    dec bp
    jnz .call
    jmp memory_error ; Continuation at the bound is a truncated map.
.normalize:
    cmp word [raw_count], 0
    je memory_error
    call map_normalize
    cmp dword [B(e820_count)], 0
    je memory_error
    mov byte [B(memmap_source)], 1
    ret
memory_error:
    mov si, map_error
    jmp minimum_halt
map_error db 'CIUKI: this PC needs a valid BIOS E820 memory map (L:E820)',0
raw_count dw 0

; Sweep every 64-bit boundary. Non-RAM wins overlapping RAM; precedence
; bad > NVS > reserved/unknown > ACPI reclaim > RAM. Inward rounding and
; coalescing happen only after overlap resolution. >128 outputs is invalid.
map_normalize:
    mov dword [sweep], 0
    mov dword [sweep+4], 0
.interval:
    mov dword [next_boundary], -1
    mov dword [next_boundary+4], -1
    mov byte [boundary_found], 0
    mov byte [winner_rank], 0
    mov si, raw_map
    mov cx, [raw_count]
.raw:
    mov eax, [si]
    mov edx, [si+4]
    call consider_boundary
    mov eax, [si+8]
    mov edx, [si+12]
    call consider_boundary
    ; Does [base,end) contain the current boundary?
    mov eax, [sweep+4]
    cmp eax, [si+4]
    jb .next_raw
    ja .base_ok
    mov eax, [sweep]
    cmp eax, [si]
    jb .next_raw
.base_ok:
    mov eax, [sweep+4]
    cmp eax, [si+12]
    ja .next_raw
    jb .contains
    mov eax, [sweep]
    cmp eax, [si+8]
    jae .next_raw
.contains:
    mov bx, [si+16]
    mov al, [type_rank+bx]
    cmp al, [winner_rank]
    jbe .next_raw
    mov [winner_rank], al
    mov [winner_type], bx
.next_raw:
    add si, 24
    loop .raw
    cmp byte [boundary_found], 0
    je .done
    cmp byte [winner_rank], 0
    je .advance
    mov eax, [sweep]
    mov edx, [sweep+4]
    mov ebx, [next_boundary]
    mov ecx, [next_boundary+4]
    cmp word [winner_type], 1
    jne .emit
    add eax, 4095
    adc edx, 0
    jc .advance
    and eax, 0xfffff000
    and ebx, 0xfffff000
.emit:
    cmp edx, ecx
    ja .advance
    jb .extent
    cmp eax, ebx
    jae .advance
.extent:
    call map_emit
.advance:
    mov eax, [next_boundary]
    mov edx, [next_boundary+4]
    mov [sweep], eax
    mov [sweep+4], edx
    jmp .interval
.done:
    ret

consider_boundary:
    cmp edx, [sweep+4]
    jb .done
    ja .after
    cmp eax, [sweep]
    jbe .done
.after:
    cmp byte [boundary_found], 0
    je .save
    cmp edx, [next_boundary+4]
    ja .done
    jb .save
    cmp eax, [next_boundary]
    jae .done
.save:
    mov [next_boundary], eax
    mov [next_boundary+4], edx
    mov byte [boundary_found], 1
.done:
    ret

; EDX:EAX base, ECX:EBX end.
map_emit:
    mov edi, [B(e820_count)]
    test edi, edi
    jz .new
    imul di, 24
    add di, B(e820)-24
    movzx esi, word [winner_type]
    cmp [di+16], esi
    jne .new
    mov esi, [di]
    mov ebp, [di+4]
    add esi, [di+8]
    adc ebp, [di+12]
    cmp eax, esi
    jne .new
    cmp edx, ebp
    jne .new
    sub ebx, [di]
    sbb ecx, [di+4]
    mov [di+8], ebx
    mov [di+12], ecx
    ret
.new:
    mov edi, [B(e820_count)]
    cmp edi, CIUKI_E820_MAX
    jae memory_error
    imul di, 24
    add di, B(e820)
    mov [di], eax
    mov [di+4], edx
    sub ebx, eax
    sbb ecx, edx
    mov [di+8], ebx
    mov [di+12], ecx
    movzx eax, word [winner_type]
    mov [di+16], eax
    mov dword [di+20], 1
    inc dword [B(e820_count)]
    ret
type_rank db 0,1,3,2,4,5
winner_rank db 0
winner_type dw 0
boundary_found db 0
sweep dq 0
next_boundary dq 0

; Refresh a flat FS cache only around above-1MiB copies. Firmware never
; runs with unreal segments. No BIOS block-move service is used.
unreal_begin:
    pushf
    pop word [unreal_flags]
    cli
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp 0x18:.pm
.pm:
    mov ax, 0x10
    mov fs, ax
    mov eax, cr0
    and al, 0xfe
    mov cr0, eax
    jmp 0x2000:.rm
.rm:
    ret
unreal_end:
    xor ax, ax
    mov fs, ax
    push word [unreal_flags]
    popf
    ret
unreal_flags dw 0
