; MEMMAP.COM - capture the firmware's E820 map before JemmEx starts.
; SHELL.COM runs this once per boot. The versioned binary is intentionally
; read-only evidence: no RAM is claimed, mapped, or reclassified here.
bits 16
cpu 386
org 100h

MAP_LIMIT equ 64
MAP_ENTRY_BYTES equ 24

start:
    cld
    push cs
    pop ds
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc .exit

    int 12h
    mov [conventional_kib],ax
    mov ax,40h
    mov es,ax
    mov ax,[es:0Eh]
    mov [ebda_segment],ax
    xor ebx,ebx
.next:
    cmp word [entry_count],MAP_LIMIT
    jae .truncated
    mov ax,[entry_count]
    mov cx,MAP_ENTRY_BYTES
    mul cx
    mov di,map_entries
    add di,ax
    mov dword [di+20],1       ; ACPI extended-attribute validity bit
    push cs
    pop es
    mov eax,0E820h
    mov edx,534D4150h         ; 'SMAP'
    mov ecx,MAP_ENTRY_BYTES
    push ds
    int 15h
    pop ds
    jc .bios_end
    cmp eax,534D4150h
    jne .write
    cmp ecx,20
    jb .write
    cmp ecx,MAP_ENTRY_BYTES
    ja .write
    inc word [entry_count]
    test ebx,ebx
    jnz .next
    mov byte [map_status],0
    jmp .write
.bios_end:
    cmp word [entry_count],0
    je .write
    mov byte [map_status],0   ; BIOS may end a sequence with CF set
    jmp .write
.truncated:
    mov byte [map_status],2
.write:
    mov dx,file_path
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc .exit
    mov bx,ax
    mov ax,[entry_count]
    mov cx,MAP_ENTRY_BYTES
    mul cx
    add ax,map_entries-map_header
    mov cx,ax
    mov [write_count],cx
    mov dx,map_header
    mov ah,40h
    int 21h
    pushf
    push ax
    mov ah,3Eh
    int 21h
    pop ax
    popf
    jc .exit
    cmp ax,[write_count]
    jne .exit
    cmp byte [map_status],0
    jne .exit
    mov ax,4C00h
    int 21h
.exit:
    mov ax,4C01h
    int 21h

file_path db '\SYSTEM\MEMMAP.BIN',0
write_count dw 0
map_header:
    db 'CMAP'
    db 1                    ; version
map_status db 1             ; 0 complete, 1 unavailable/invalid, 2 truncated
entry_count dw 0
conventional_kib dw 0
ebda_segment dw 0
    dw MAP_ENTRY_BYTES
    dw 0                    ; reserved
map_entries times MAP_LIMIT*MAP_ENTRY_BYTES db 0
image_end:
