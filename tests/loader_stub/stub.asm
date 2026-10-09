; Loader-only diagnostic stub, no kernel services.
; SPDX-License-Identifier: GPL-2.0-only
bits 32
%include "src/boot/boot_info.inc"
%define P(x) (x-0xc0000000)
%define I(field) ebp+ciuki_boot_info.%+field
global stub_entry
section .text
stub_entry:
    mov edi, eax
    mov ebp, ebx
    pushfd
    pop edx
    cli
    cld
    call serial_init
    cmp edi, CIUKI_BOOT_MAGIC_EAX
    jne invalid
    test edx, (1<<9)|(1<<10)
    jnz invalid
    cmp esp, 0x7c00
    ja invalid
    mov eax, cr0
    and eax, 0x80000001
    cmp eax, 1
    jne invalid
    cmp ebp, 0x500
    jb invalid
    cmp ebp, 0x90000-CIUKI_BOOT_INFO_SIZE
    ja invalid
    test ebp, 15
    jnz invalid
    cmp dword [I(magic)], CIUKI_BOOT_INFO_MAGIC
    jne invalid
    cmp word [I(version)], CIUKI_BOOT_INFO_VER
    jne invalid
    cmp word [I(size)], CIUKI_BOOT_INFO_SIZE
    jne invalid
    cmp dword [I(e820_count)], 1
    jb invalid
    cmp dword [I(e820_count)], CIUKI_E820_MAX
    ja invalid
    cmp word [I(test_request_len)], 64
    ja invalid
    test dword [I(flags)], ~0x7ff
    jnz invalid
    cmp byte [I(reserved0)], 0
    jne invalid
    cmp dword [I(reserved1)], 0
    jne invalid
    cmp word [I(reserved1)+4], 0
    jne invalid
    cmp byte [I(memmap_source)], 1
    jne invalid
    cmp byte [I(input_policy)], 1
    ja invalid
    mov eax, [I(flags)]
    shr eax, 8
    and eax, 7
    dec eax
    cmp eax, 3
    ja invalid
    test dword [I(flags)], CBI_F_INPUT_FORCED
    jz .extents
    mov eax, [I(flags)]
    and eax, CBI_F_SMBIOS_QEMU|CBI_F_TEST_REQUEST
    cmp eax, CBI_F_SMBIOS_QEMU|CBI_F_TEST_REQUEST
    jne invalid
    cmp byte [I(input_policy)], 1
    jne invalid
.extents:
    mov eax, [I(loader_start)]
    cmp eax, [I(loader_end)]
    jae invalid
    cmp dword [I(loader_end)], 0x90000
    ja invalid
    mov eax, [I(kernel_start)]
    cmp eax, 0x100000
    jb invalid
    cmp eax, [I(kernel_end)]
    jae invalid
    cmp dword [I(kernel_end)], 0x1000000
    ja invalid
    mov eax, [I(kernel_entry_phys)]
    cmp eax, [I(kernel_start)]
    jb invalid
    cmp eax, [I(kernel_end)]
    jae invalid
    cmp eax, P(stub_entry)
    jne invalid
    ; Explicitly exercise BSS zero-fill across the second PT_LOAD.
    mov esi, P(bss_canary)
    mov ecx, 4096
.bss:
    cmp byte [esi], 0
    jne invalid
    inc esi
    loop .bss
    lea esi, [I(e820)]
    mov ecx, [I(e820_count)]
.map:
    mov eax, [esi+8]
    mov edx, [esi+12]
    mov ebx, eax
    or ebx, edx
    jz invalid
    add eax, [esi]
    adc edx, [esi+4]
    jc invalid
    test byte [esi+20], 1
    jz invalid
    add esi, 24
    loop .map
    lea esi, [I(test_request)]
    movzx ecx, word [I(test_request_len)]
.run:
    cmp ecx, 13
    jb .records
    cmp byte [esi], ' '
    jne .run_next
    cmp dword [esi+1], 'run='
    jne .run_next
    add esi, 5
    mov edi, P(run_id)
    mov ecx, 8
    rep movsb
    jmp .records
.run_next:
    inc esi
    dec ecx
    jmp .run
.records:
    call prefix
    mov esi, P(begin)
    call puts
    call prefix
    mov esi, P(flags)
    call puts
    mov eax, [I(flags)]
    call hex8
    mov esi, P(count)
    call puts
    mov eax, [I(e820_count)]
    call decimal
    mov esi, P(policy)
    call puts
    movzx eax, byte [I(input_policy)]
    call decimal
    mov esi, P(line_end)
    call puts
    call prefix
    mov esi, P(framebuffer)
    call puts
    test dword [I(flags)], CBI_F_TEXT_MODE
    jnz .text
    mov esi, P(lfb)
    call puts
    mov esi, P(width)
    call puts
    movzx eax, word [I(fb_width)]
    call decimal
    mov esi, P(height)
    call puts
    movzx eax, word [I(fb_height)]
    call decimal
    mov esi, P(bpp)
    call puts
    movzx eax, byte [I(fb_bpp)]
    call decimal
    mov esi, P(pitch)
    call puts
    mov eax, [I(fb_pitch)]
    call decimal
    jmp .fb_end
.text:
    mov esi, P(text_mode)
    call puts
.fb_end:
    mov esi, P(line_end)
    call puts
    call prefix
    mov esi, P(request)
    call puts
    movzx eax, word [I(test_request_len)]
    call decimal
    mov esi, P(request_hex)
    call puts
    lea esi, [I(test_request)]
    movzx ecx, word [I(test_request_len)]
    jecxz .no_request
.request_byte:
    lodsb
    call hex2
    loop .request_byte
    jmp .request_end
.no_request:
    mov esi, P(absent_request)
    call puts
.request_end:
    mov esi, P(line_end)
    call puts
    call prefix
    mov esi, P(entry_phys)
    call puts
    mov eax, [I(kernel_entry_phys)]
    call hex8
    mov esi, P(line_end)
    call puts
    call prefix
    mov esi, P(pass)
    call puts
.halt:
    hlt
    jmp .halt
invalid:
    call prefix
    mov esi, P(begin)
    call puts
    call prefix
    mov esi, P(fail)
    call puts
    jmp stub_entry.halt

prefix:
    mov esi, P(record)
    call puts
    mov esi, P(run_id)
    call puts
    mov esi, P(sequence)
    call puts
    mov esi, P(seq_id)
    call puts
    mov edi, P(seq_id)+5
.increment:
    inc byte [edi]
    cmp byte [edi], '9'
    jbe .done
    mov byte [edi], '0'
    dec edi
    jmp .increment
.done:
    mov esi, P(probe)
    call puts
    ret
puts:
    lodsb
    test al, al
    jz .done
    call putc
    jmp puts
.done:
    ret
putc:
    pushad
    cmp byte [P(serial_present)], 0
    je .done
    dec dword [P(serial_budget)]
    jz .disable
    mov bl, al
    mov dx, 0x3fd
    mov ecx, 0x1000
.poll:
    in al, dx
    test al, 0x20
    jnz .send
    loop .poll
.disable:
    mov byte [P(serial_present)], 0
    jmp .done
.send:
    mov dx, 0x3f8
    mov al, bl
    out dx, al
.done:
    popad
    ret
serial_init:
    pushad
    mov dx, 0x3fd
    in al, dx
    cmp al, 0xff
    je .done
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 3
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 3
    out dx, al
    mov byte [P(serial_present)], 1
.done:
    popad
    ret
hex8:
    pushad
    mov ebx, eax
    mov ecx, 8
.digit:
    rol ebx, 4
    mov al, bl
    and al, 15
    call nibble
    loop .digit
    popad
    ret
hex2:
    push eax
    shr al, 4
    call nibble
    pop eax
    and al, 15
    jmp nibble
nibble:
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 'a'-'9'-1
.out:
    jmp putc
decimal:
    pushad
    xor ecx, ecx
    mov ebx, 10
.divide:
    xor edx, edx
    div ebx
    push edx
    inc ecx
    test eax, eax
    jnz .divide
.digit:
    pop eax
    add al, '0'
    call putc
    loop .digit
    popad
    ret
section .rodata
record db 'CIUKI_TEST v=1 run=',0
sequence db ' seq=',0
probe db ' probe=loader event=',0
begin db 'BEGIN',13,10,0
flags db 'DATA flags=',0
count db ' e820_count=',0
policy db ' input_policy=',0
framebuffer db 'DATA video=',0
lfb db 'lfb',0
text_mode db 'text',0
width db ' width=',0
height db ' height=',0
bpp db ' bpp=',0
pitch db ' pitch=',0
request db 'DATA request_len=',0
request_hex db ' request_hex=',0
absent_request db '-',0
entry_phys db 'DATA entry_phys=',0
line_end db 13,10,0
pass db 'END status=PASS',13,10,0
fail db 'END status=FAIL reason=handoff',13,10,0
section .data
run_id db '00000000',0
seq_id db '000001',0
serial_present db 0
serial_budget dd 8192
section .bss
bss_canary resb 4096
