; Validate all headers and extents before writing any kernel byte.
elf_load:
    mov eax, [system_cluster]
    mov si, kernel_name
    call directory_find
    jc .missing
    test byte [found_attr], 0x10
    jnz elf_error
    cmp dword [found_size], 52
    jb elf_error
    cmp dword [found_size], 0x1000000
    ja elf_error
    call file_open
    mov di, elf_header
    mov cx, 52
    call file_read
    cmp dword [elf_header], 0x464c457f
    jne elf_error
    cmp word [elf_header+4], 0x0101 ; ELF32, little-endian
    jne elf_error
    cmp byte [elf_header+6], 1
    jne elf_error
    cmp dword [elf_header+16], 0x00030002 ; ET_EXEC, EM_386
    jne elf_error
    cmp dword [elf_header+20], 1
    jne elf_error
    cmp word [elf_header+40], 52
    jne elf_error
    cmp word [elf_header+42], 32
    jne elf_error
    movzx ecx, word [elf_header+44]
    test ecx, ecx
    jz elf_error
    cmp ecx, MAX_PH
    ja elf_error
    mov [ph_count], cx
    shl ecx, 5
    mov eax, [elf_header+28]
    mov edx, eax
    add edx, ecx
    jc elf_error
    cmp edx, [file_size]
    ja elf_error
    push cx
    call file_seek
    pop cx
    mov di, ph_table
    call file_read
    mov dword [B(kernel_start)], 0x1000000
    mov dword [B(kernel_end)], 0
    mov dword [B(kernel_entry_phys)], 0
    mov si, ph_table
    mov cx, [ph_count]
.validate:
    push cx
    cmp dword [si], 2 ; Dynamic/interpreter kernels are unsupported.
    je elf_error
    cmp dword [si], 3
    je elf_error
    cmp dword [si], 1
    jne .next
    mov eax, [si+16]
    cmp eax, [si+20]
    ja elf_error
    add eax, [si+4]
    jc elf_error
    cmp eax, [file_size]
    ja elf_error
    mov eax, [si+20]
    test eax, eax
    jz elf_error
    add eax, [si+8]
    jc elf_error
    mov eax, [si+28]
    cmp eax, 1
    jbe .extent
    mov edx, eax
    dec eax
    test eax, edx
    jnz elf_error
    mov edx, [si+8]
    sub edx, [si+4]
    test eax, edx
    jnz elf_error
.extent:
    mov eax, [si+12]
    mov edx, eax
    add edx, [si+20]
    jc elf_error
    cmp eax, 0x100000
    jb .window
    cmp edx, 0x1000000
    ja .window
    call extent_ram
    jc .ram
    ; Check against every previously validated physical PT_LOAD extent.
    mov di, ph_table
.overlap:
    cmp di, si
    je .entry
    cmp dword [di], 1
    jne .previous
    mov ebx, [di+12]
    add ebx, [di+20]
    cmp eax, ebx
    jae .previous
    cmp edx, [di+12]
    ja .overlap_error
.previous:
    add di, 32
    jmp .overlap
.entry:
    cmp eax, [B(kernel_start)]
    jae .max
    mov [B(kernel_start)], eax
.max:
    cmp edx, [B(kernel_end)]
    jbe .entry_check
    mov [B(kernel_end)], edx
.entry_check:
    test byte [si+24], 1
    jz .next
    mov ebx, [elf_header+24]
    cmp ebx, [si+8]
    jb .next
    sub ebx, [si+8]
    cmp ebx, [si+20]
    jae .next
    cmp dword [B(kernel_entry_phys)], 0
    jne elf_error
    add ebx, eax
    mov [B(kernel_entry_phys)], ebx
.next:
    pop cx
    add si, 32
    dec cx
    jnz .validate
    cmp dword [B(kernel_entry_phys)], 0
    je .entry_error
    mov si, ph_table
    mov cx, [ph_count]
.load:
    push cx
    push si
    cmp dword [si], 1
    jne .loaded
    mov eax, [si+4]
    mov ebx, [si+12]
    mov [copy_dest], ebx
    mov ebx, [si+16]
    mov [copy_left], ebx
    mov ebx, [si+20]
    sub ebx, [si+16]
    mov [zero_left], ebx
    cmp dword [copy_left], 0
    je .bss
    call file_seek
.chunk:
    cmp dword [copy_left], 0
    je .bss
    mov ecx, [copy_left]
    cmp ecx, 512
    jbe .count
    mov cx, 512
.count:
    mov [copy_count], cx
    mov di, cfg_buffer
    call file_read
    call unreal_begin
    movzx ecx, word [copy_count]
    mov edi, [copy_dest]
    mov si, cfg_buffer
.byte:
    lodsb
    mov [fs:edi], al
    inc edi
    loop .byte
    mov [copy_dest], edi
    call unreal_end
    movzx eax, word [copy_count]
    sub [copy_left], eax
    jmp .chunk
.bss:
    ; Zero in short bursts too, retaining BIOS timer service between bursts.
    cmp dword [zero_left], 0
    je .loaded
    mov ecx, [zero_left]
    cmp ecx, 512
    jbe .zero_count
    mov ecx, 512
.zero_count:
    mov [copy_count], cx
    call unreal_begin
    movzx ecx, word [copy_count]
    mov edi, [copy_dest]
    xor al, al
.zero:
    mov [fs:edi], al
    inc edi
    dec ecx
    jnz .zero
    mov [copy_dest], edi
    call unreal_end
    movzx eax, word [copy_count]
    sub [zero_left], eax
    jmp .bss
.loaded:
    pop si
    pop cx
    add si, 32
    dec cx
    jnz .load
    ret
.missing:
    mov si, missing_msg
    jmp fatal
.window:
    mov si, window_msg
    jmp fatal
.ram:
    mov si, ram_msg
    jmp fatal
.overlap_error:
    mov si, overlap_msg
    jmp fatal
.entry_error:
    mov si, entry_msg
    jmp fatal
elf_error:
    mov si, elf_msg
    jmp fatal
elf_msg db 'L:ELF',0
missing_msg db 'L:NO_VMM',0
window_msg db 'L:ELF_WINDOW',0
ram_msg db 'L:ELF_RAM',0
overlap_msg db 'L:ELF_OVERLAP',0
entry_msg db 'L:ELF_ENTRY',0
ph_count dw 0
copy_dest dd 0
copy_left dd 0
zero_left dd 0
copy_count dw 0

; EAX start, EDX exclusive end, CF=not fully inside normalized RAM.
extent_ram:
    pushad
    mov di, B(e820)
    mov cx, [B(e820_count)]
.range:
    cmp dword [di+16], 1
    jne .next
    cmp dword [di+4], 0
    jne .next
    cmp eax, [di]
    jb .next
    mov ebx, [di]
    mov ebp, [di+4]
    add ebx, [di+8]
    adc ebp, [di+12]
    test ebp, ebp
    jnz .yes
    cmp edx, ebx
    jbe .yes
.next:
    add di, 24
    loop .range
    popad
    stc
    ret
.yes:
    popad
    clc
    ret
