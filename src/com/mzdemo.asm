bits 16
org 0x0000

; Minimal MZ executable for Stage1 loader validation.
; Header size: 0x20 bytes (2 paragraphs)
; Entry point: CS:IP = 0000:0000 (first byte after header)
; Stack:       SS:SP = 0000:FFFE (relative to image segment)

mz_header:
    dw 0x5A4D                ; e_magic = 'MZ'
    dw file_size_mod_512     ; e_cblp
    dw file_size_pages        ; e_cp
    dw 0x0001                ; e_crlc (1 relocation entry)
    dw 0x0002                ; e_cparhdr (0x20 / 16)
    dw 0x0000                ; e_minalloc
    dw 0xFFFF                ; e_maxalloc
    dw 0x0000                ; e_ss
    dw 0xFFFE                ; e_sp
    dw 0x0000                ; e_csum
    dw 0x0000                ; e_ip
    dw 0x0000                ; e_cs
    dw mz_reloc_table - mz_header ; e_lfarlc
    dw 0x0000                ; e_ovno

mz_reloc_table:
    ; Relocate segment word of term_ptr (load-module relative).
    dw (term_ptr + 2) - image_start
    dw 0x0000

times 0x20 - ($ - mz_header) db 0

image_start:
start:
    push cs
    pop ds

    mov dx, msg - image_start
    mov ah, 0x09
    int 0x21

    ; Force relocation use through a far pointer segment fixup.
    call far [term_ptr - image_start]

    retf

term_exit:
    mov ax, 0x4C55
    int 0x21
    retf

term_ptr:
    dw term_exit - image_start
    dw 0x0000

msg db "MZ demo via INT21h", 13, 10, '$'

file_end:

file_size equ file_end - mz_header
file_size_mod_512 equ file_size & 0x1FF
file_size_pages equ (file_size + 511) / 512
