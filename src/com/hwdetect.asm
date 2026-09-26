; CiukiOS hardware profile selection, read-only hardware discovery.
; HWDETECT [/Q] [/SAVE]: /SAVE publishes \DRIVERS\ACTIVE.CFG.
; Contract and provenance: docs/driver-catalog.md. No PCI, PS/2 or audio writes.
bits 16
cpu 386
org 0x100

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    cld
    call parse_args
    jc usage
    call detect_pci
    call detect_model
    call detect_vbe
    call build_report
    cmp byte [quiet], 0
    jne .save
    mov bx, 1
    mov dx, report
    mov cx, [report_length]
    mov ah, 0x40
    int 0x21
.save:
    cmp byte [save], 0
    je .ok
    call save_report
    jc .write_error
.ok:
    mov ax, 0x4c00
    int 0x21
.write_error:
    cmp byte [quiet], 0
    jne .error_exit
    mov dx, write_error
    mov ah, 9
    int 0x21
.error_exit:
    mov ax, 0x4c05
    int 0x21

usage:
    mov dx, usage_text
    mov ah, 9
    int 0x21
    mov ax, 0x4c01
    int 0x21

; Accept complete /Q and /SAVE tokens, case insensitive, not prefix matches.
parse_args:
    mov si, 0x81
    xor cx, cx
    mov cl, [0x80]
.space:
    jcxz .ok
    lodsb
    dec cx
    cmp al, ' '
    je .space
    cmp al, 9
    je .space
    cmp al, '/'
    jne .bad
    jcxz .bad
    lodsb
    dec cx
    and al, 0xdf
    cmp al, 'Q'
    je .quiet
    cmp al, 'S'
    jne .bad
    cmp cx, 3
    jb .bad
    lodsb
    and al, 0xdf
    cmp al, 'A'
    jne .bad
    lodsb
    and al, 0xdf
    cmp al, 'V'
    jne .bad
    lodsb
    and al, 0xdf
    cmp al, 'E'
    jne .bad
    sub cx, 3
    mov byte [save], 1
    jmp .delimiter
.quiet:
    mov byte [quiet], 1
.delimiter:
    jcxz .ok
    cmp byte [si], ' '
    je .space
    cmp byte [si], 9
    je .space
.bad:
    stc
    ret
.ok:
    clc
    ret

; Preserve every caller register and segment around PCI firmware. Outputs are
; copied with CS overrides before recovering segments. Configuration is read
; only; there is deliberately no direct CF8/CFC fallback and no BAR sizing.
pci_call:
    pushf
    pushad
    push ds
    push es
    push fs
    push gs
    mov byte [cs:pci_failed], 1
    stc
    int 0x1a
    jc .restore
    test ah, ah
    jnz .restore
    mov [cs:pci_bx], bx
    mov [cs:pci_cx], ecx
    mov [cs:pci_dx], edx
    mov byte [cs:pci_failed], 0
.restore:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popf
    ret

detect_pci:
    mov ax, 0xb101
    xor edi, edi
    call pci_call
    cmp byte [pci_failed], 0
    jne .done
    cmp dword [pci_dx], 0x20494350 ; "PCI "
    jne .done
    mov byte [pci_present], 1

    mov ecx, 0x030000            ; VGA-compatible PCI display controller
    xor si, si
    mov ax, 0xb103
    call pci_call
    cmp byte [pci_failed], 0
    jne .audio
    mov bx, [pci_bx]
    mov di, 0
    call pci_read_dword
    jc .audio
    mov eax, [pci_cx]
    mov [video_pci], eax
.audio:
    mov dword [find_class], 0x040100
    call scan_audio_class
    mov dword [find_class], 0x040300
    call scan_audio_class
.done:
    ret

pci_read_dword:
    mov ax, 0xb10a
    call pci_call
    cmp byte [pci_failed], 0
    jne .bad
    clc
    ret
.bad:
    stc
    ret

; At most 16 devices per class. Select native ICH before supported VSBHDA;
; retain the first unsupported controller for a useful, honest diagnosis.
scan_audio_class:
    mov word [find_index], 0
.next:
    mov si, [find_index]
    mov ecx, [find_class]
    mov ax, 0xb103
    call pci_call
    cmp byte [pci_failed], 0
    jne .done
    mov bx, [pci_bx]
    mov [candidate_bdf], bx
    mov di, 0
    call pci_read_dword
    jc .advance
    mov eax, [pci_cx]
    cmp ax, 0xffff
    je .advance
    test ax, ax
    jz .advance
    mov [candidate_id], eax
    mov byte [candidate_kind], 1 ; unknown PCI audio; never guess Intel layout
    cmp dword [find_class], 0x040300
    je .vsbhda
    mov si, native_ids
.native:
    mov edx, [si]
    test edx, edx
    jz .other
    cmp eax, edx
    je .ich
    add si, 4
    jmp .native
.other:
    mov si, vsbhda_ids
.supported:
    mov edx, [si]
    test edx, edx
    jz .select
    cmp eax, edx
    je .vsbhda
    add si, 4
    jmp .supported
.vsbhda:
    mov byte [candidate_kind], 2
    jmp .select
.ich:
    mov byte [candidate_kind], 3
.select:
    mov al, [candidate_kind]
    cmp al, [audio_kind]
    jbe .advance
    mov [audio_kind], al
    mov eax, [candidate_id]
    mov [audio_pci], eax
    mov bx, [candidate_bdf]
    mov [audio_bdf], bx
    mov dword [audio_subsystem], 0xffffffff
    mov di, 0x2c
    call pci_read_dword
    jc .advance
    mov eax, [pci_cx]
    mov [audio_subsystem], eax
.advance:
    inc word [find_index]
    cmp word [find_index], 16
    jb .next
.done:
    ret

detect_vbe:
    pushf
    pushad
    push ds
    push es
    push fs
    push gs
    push cs
    pop es
    mov di, vbe_info
    mov ax, 0x4f00
    int 0x10
    cmp ax, 0x004f
    jne .done
    cmp dword [cs:vbe_info], 0x41534556 ; VESA
    jne .done
    cmp word [cs:vbe_info+4], 0x0102
    jb .done
    mov byte [cs:vbe_present], 1
.done:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popf
    ret

; SMBIOS2's legacy _DMI_ entry point is paragraph-aligned in F0000..FFFFF.
; Validate checksum, lower-memory physical interval, length, structure count,
; formatted headers and every string terminator before following it. Tables
; above 1 MiB are intentionally ignored instead of entering unreal mode.
detect_model:
    cmp dword [audio_pci], 0x1978125d
    jne .scan
    cmp word [audio_subsystem], 0x0e11
    jne .scan
    mov byte [model], 1          ; Compaq ES1978 family, not proof of E500
.scan:
    push es
    mov ax, 0xf000
    mov es, ax
    xor bx, bx
.entry:
    cmp dword [es:bx], 0x494d445f ; _DMI
    jne .next_entry
    cmp byte [es:bx+4], '_'
    jne .next_entry
    mov si, bx
    mov cx, 15
    xor al, al
.checksum:
    add al, [es:si]
    inc si
    loop .checksum
    test al, al
    jnz .next_entry
    movzx edx, word [es:bx+6]
    cmp edx, 4
    jb .next_entry
    cmp edx, 0x8000
    ja .next_entry
    mov eax, [es:bx+8]
    cmp eax, 0x1000
    jb .next_entry
    mov ecx, eax
    add ecx, edx
    jc .next_entry
    cmp ecx, 0x100000
    ja .next_entry
    mov cx, [es:bx+12]
    test cx, cx
    jz .next_entry
    mov [dmi_count], cx
    mov si, ax
    and si, 15
    add dx, si
    mov [dmi_end], dx
    shr eax, 4
    mov es, ax
    call dmi_walk
    jmp .done
.next_entry:
    add bx, 16
    jnc .entry
.done:
    pop es
    ret

dmi_walk:
    cmp word [dmi_count], 0
    je .done
    dec word [dmi_count]
    mov ax, si
    add ax, 4
    cmp ax, [dmi_end]
    ja .done
    movzx ax, byte [es:si+1]
    cmp ax, 4
    jb .done
    mov di, si
    add di, ax
    cmp di, [dmi_end]
    jae .done
    cmp byte [es:si], 127
    je .done
    cmp byte [es:si], 1
    jne .skip_strings
    cmp ax, 8
    jb .done
    mov bl, [es:si+5]            ; product-name string index, 1 based
    test bl, bl
    jz .done
    mov si, di
.string:
    cmp si, [dmi_end]
    jae .done
    cmp byte [es:si], 0
    je .done                    ; index outside string set
    dec bl
    jz .product
.skip_string:
    cmp si, [dmi_end]
    jae .done
    cmp byte [es:si], 0
    lea si, [si+1]
    jne .skip_string
    jmp .string
.product:
    mov di, product_name
    mov cx, 63
.copy:
    cmp si, [dmi_end]
    jae .done
    mov al, [es:si]
    inc si
    test al, al
    jz .classify
    cmp al, ' '
    jb .done
    cmp al, 126
    ja .done
    cmp al, 'a'
    jb .store
    cmp al, 'z'
    ja .store
    sub al, 32
.store:
    mov [di], al
    inc di
    loop .copy
    ret                         ; truncated names aren't exact model evidence
.classify:
    mov byte [di], 0
    mov si, product_name
    mov di, name_e500
    call contains
    jc .t23
    mov byte [model], 2
    ret
.t23:
    mov si, product_name
    mov di, name_t23
    call contains
    jc .done
    mov byte [model], 3
    ret
.skip_strings:
    mov si, di
.skip_pair:
    mov ax, si
    add ax, 2
    cmp ax, [dmi_end]
    ja .done
    cmp word [es:si], 0
    lea si, [si+1]
    jne .skip_pair
    inc si
    jmp dmi_walk
.done:
    ret

; DS:SI zero-terminated haystack, DS:DI needle; CF clear when found.
contains:
    push bx
    push dx
.start:
    cmp byte [si], 0
    je .missing
    mov bx, si
    mov dx, di
.compare:
    mov al, [di]
    test al, al
    jz .found
    cmp al, [si]
    jne .mismatch
    inc si
    inc di
    jmp .compare
.mismatch:
    mov si, bx
    inc si
    mov di, dx
    jmp .start
.found:
    pop dx
    pop bx
    clc
    ret
.missing:
    pop dx
    pop bx
    stc
    ret

build_report:
    mov di, report
    mov si, header
    call append
    movzx bx, byte [model]
    shl bx, 1
    mov si, [model_names+bx]
    call append
    mov si, input_line
    call append
    mov si, value_vga
    cmp byte [vbe_present], 0
    je .video
    mov si, value_vbe
.video:
    call append
    mov si, audio_key
    call append
    movzx bx, byte [audio_kind]
    shl bx, 1
    mov si, [audio_names+bx]
    call append
    mov si, fallback_line
    call append
    mov eax, [audio_pci]
    call append_pci
    mov si, subsystem_key
    call append
    mov eax, [audio_subsystem]
    call append_pci
    mov si, video_key
    call append
    mov eax, [video_pci]
    call append_pci
    mov si, pci_key
    call append
    mov al, [pci_present]
    add al, '0'
    stosb
    mov si, end_line
    call append
    sub di, report
    mov [report_length], di
    ret

append:
    lodsb
    test al, al
    jz .done
    stosb
    jmp append
.done:
    ret

append_pci:
    push eax
    call append_hex
    mov al, ':'
    stosb
    pop eax
    shr eax, 16
    call append_hex
    ret

append_hex:
    mov dx, ax
    mov cx, 4
.digit:
    rol dx, 4
    mov al, dl
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .put
    add al, 7
.put:
    stosb
    loop .digit
    ret

; Publish only after a full write and successful close. The shell must also
; require a successful HWDETECT child exit before consuming ACTIVE.CFG, so a
; stale report is never authoritative after a failed/read-only save.
save_report:
    mov dx, report_new
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    jc .fail
    mov [report_handle], ax
    mov bx, ax
    mov dx, report
    mov cx, [report_length]
    mov ah, 0x40
    int 0x21
    jc .write_failed
    cmp ax, [report_length]
    jne .write_failed
    mov bx, [report_handle]
    mov ah, 0x3e
    int 0x21
    jc .remove_new
    mov dx, report_path
    mov ah, 0x41
    int 0x21
    jnc .rename
    cmp ax, 2                   ; absence is normal on first boot
    jne .remove_new
.rename:
    push cs
    pop es
    mov dx, report_new
    mov di, report_path
    mov ah, 0x56
    int 0x21
    jc .remove_new
    clc
    ret
.write_failed:
    mov bx, [report_handle]
    mov ah, 0x3e
    int 0x21
.remove_new:
    mov dx, report_new
    mov ah, 0x41
    int 0x21
.fail:
    stc
    ret

; Values are device:vendor packed as the PCI DWORD at offset 0.
native_ids:
    dd 0x24158086,0x24258086,0x24458086,0x24858086,0x24c58086
    dd 0x24d58086,0x25a68086,0x266e8086,0x26988086,0x27de8086,0
; Only IDs in shipped VSBHDA2.0 backend tables. ESS1978 is NOT Allegro1988.
vsbhda_ids:
    dd 0x70121039,0x01b110de,0x003a10de,0x006a10de,0x005910de
    dd 0x008a10de,0x00da10de,0x00ea10de,0x746d1022,0x74451022
    dd 0x30581106,0x30591106,0x00021102,0x00041102,0x00081102
    dd 0x00071102,0x13711274,0x58801274,0x89381102,0

quiet db 0
save db 0
pci_failed db 1
pci_present db 0
vbe_present db 0
model db 0
audio_kind db 0                 ; 0 SB legacy only, 1 unknown, 2 VSBHDA, 3 ICH
candidate_kind db 0
pci_bx dw 0
pci_cx dd 0
pci_dx dd 0
find_class dd 0
find_index dw 0
candidate_bdf dw 0
candidate_id dd 0
audio_bdf dw 0xffff
audio_pci dd 0xffffffff
audio_subsystem dd 0xffffffff
video_pci dd 0xffffffff
dmi_count dw 0
dmi_end dw 0
product_name times 64 db 0
name_e500 db 'ARMADA E500',0
name_t23 db 'THINKPAD T23',0
model_names dw model_generic,model_compaq,model_e500,model_t23
model_generic db 'GENERIC_PC',0
model_compaq db 'COMPAQ_ES1978',0
model_e500 db 'COMPAQ_E500',0
model_t23 db 'THINKPAD_T23',0
audio_names dw value_sb,value_unsupported,value_vsbhda,value_ich
value_sb db 'SB_NATIVE',0
value_unsupported db 'UNSUPPORTED',0
value_vsbhda db 'VSBHDA',0
value_ich db 'ICH_AC97',0
value_vga db 'VGA',0
value_vbe db 'VBE',0
header db 'SCHEMA=1',13,10,'MODEL=',0
input_line db 13,10,'INPUT=I8042_BIOS',13,10,'VIDEO=',0
audio_key db 13,10,'AUDIO=',0
fallback_line db 13,10,'AUDIO_FALLBACK=SB_NATIVE',13,10,'AUDIO_PCI=',0
subsystem_key db 13,10,'AUDIO_SUBSYS=',0
video_key db 13,10,'VIDEO_PCI=',0
pci_key db 13,10,'PCI_BIOS=',0
end_line db 13,10,'END=1',13,10,0
report_path db '\DRIVERS\ACTIVE.CFG',0
report_new db '\DRIVERS\ACTIVE.NEW',0
report_handle dw 0
report_length dw 0
usage_text db 'HWDETECT [/Q] [/SAVE]',13,10,'Read-only hardware detection; /SAVE writes \DRIVERS\ACTIVE.CFG.',13,10,'$'
write_error db '[HWDETECT] Cannot publish \DRIVERS\ACTIVE.CFG.',13,10,'$'
align 4
vbe_info db 'VBE2'
    times 508 db 0
report times 512 db 0
align 16
stack_bottom:
    times 2048 db 0
stack_top:
%if ($-$$+0x100) > 0xff00
    %error "HWDETECT exceeds COM segment"
%endif
