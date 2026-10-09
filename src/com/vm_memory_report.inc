; Persist a read-only XMS 3.0 and VCPI snapshot for physical diagnostics.
; No XMS or VCPI pages are allocated by these queries.
vm_memory_report:
    pushad
    push ds
    push es
    call dosmem_init
    call query_xms
    call query_vcpi
    or byte [cs:dosmem_flags],80h
    call save_dosmem
    call print_dosmem
    pop es
    pop ds
    popad
    ret

dosmem_init:
    mov byte [cs:dosmem_flags],0
    mov byte [cs:dosmem_errors],0
    mov word [cs:dosmem_xms_version],0
    mov byte [cs:dosmem_xms_status],0FFh
    mov byte [cs:dosmem_vcpi_presence_status],0FFh
    mov byte [cs:dosmem_vcpi_query_status],0FFh
    mov dword [cs:dosmem_xms_largest_kib],0
    mov dword [cs:dosmem_xms_total_kib],0
    mov dword [cs:dosmem_vcpi_free_pages],0
    ; The firmware ceiling is deliberately not queried here. MEMMAP.BIN is a
    ; separate pre-Jemm BIOS capture and must not be presented as live free RAM.
    mov dword [cs:dosmem_firmware_kib],0
    ret

query_xms:
    ; INT 2Fh/4300h locates the XMS manager; 4310h returns its entry point.
    mov ax,4300h
    int 2Fh
    cmp al,80h
    jne .missing
    mov ax,4310h
    int 2Fh
    mov [cs:xms_entry],bx
    mov [cs:xms_entry+2],es

    ; XMS function 00h reports the version in AX.
    mov ah,00h
    call far [cs:xms_entry]
    mov [cs:dosmem_xms_version],ax
    cmp ax,0300h
    jb .old
    or byte [cs:dosmem_flags],01h

    ; XMS 3.0 function 88h returns largest/total free KiB in EAX/EDX.
    mov ah,88h
    call far [cs:xms_entry]
    mov [cs:dosmem_xms_status],bl
    mov [cs:dosmem_xms_largest_kib],eax
    mov [cs:dosmem_xms_total_kib],edx
    test bl,bl
    jnz .query_error
    or byte [cs:dosmem_flags],02h
    ret

.missing:
    mov byte [cs:dosmem_errors],01h
    ret
.old:
    ; 80h is XMS's standard “function not implemented” status for AH=88h.
    mov byte [cs:dosmem_xms_status],80h
    mov byte [cs:dosmem_errors],01h
    ret
.query_error:
    or byte [cs:dosmem_errors],02h
    ret

query_vcpi:
    ; DE00h checks for VCPI; on success BH is the major version.
    mov ax,0DE00h
    int 67h
    mov [cs:dosmem_vcpi_presence_status],ah
    test ah,ah
    jnz .missing
    test bh,bh
    jz .missing
    or byte [cs:dosmem_flags],04h

    ; DE03h is read-only and returns the currently free 4-KiB page count in EDX.
    mov ax,0DE03h
    int 67h
    mov [cs:dosmem_vcpi_query_status],ah
    mov [cs:dosmem_vcpi_free_pages],edx
    test ah,ah
    jnz .query_error
    or byte [cs:dosmem_flags],08h
    ret

.missing:
    or byte [cs:dosmem_errors],04h
    ret
.query_error:
    mov dword [cs:dosmem_vcpi_free_pages],0
    or byte [cs:dosmem_errors],08h
    ret

save_dosmem:
    push ds
    push es
    pushad
    push cs
    pop ds

    ; INT 21h/3Ch creates or truncates; the foreground kernel writes the
    ; complete 32-byte record through its normal FAT16 file path.
    mov dx,dosmem_path
    xor cx,cx
    mov ah,3Ch
    int 21h
    jc .open_error
    mov [cs:dosmem_handle],ax

    mov bx,ax
    mov dx,dosmem_record
    mov cx,DOSMEM_RECORD_SIZE
    mov ah,40h
    int 21h
    jc .write_error
    cmp ax,DOSMEM_RECORD_SIZE
    jne .short_write

    mov bx,[cs:dosmem_handle]
    mov ah,3Eh
    int 21h
    jc .close_error
    mov byte [cs:dosmem_save_status],0
    jmp .done

.write_error:
    mov byte [cs:dosmem_save_status],02h
    jmp .close_failed_file
.short_write:
    mov byte [cs:dosmem_save_status],03h
.close_failed_file:
    mov bx,[cs:dosmem_handle]
    mov ah,3Eh
    int 21h
    jmp .done
.open_error:
    mov byte [cs:dosmem_save_status],01h
    jmp .done
.close_error:
    mov byte [cs:dosmem_save_status],04h
.done:
    popad
    pop es
    pop ds
    ret

print_dosmem:
    push ds
    push cs
    pop ds
    cmp byte [cs:dosmem_xms_status],0
    jne .xms_unavailable
    mov si,dosmem_xms_prefix
    call print
    mov eax,[cs:dosmem_xms_largest_kib]
    mov di,dosmem_hex
    call vm_format_hex32
    mov si,dosmem_hex
    call print
    mov si,dosmem_xms_middle
    call print
    mov eax,[cs:dosmem_xms_total_kib]
    mov di,dosmem_hex
    call vm_format_hex32
    mov si,dosmem_hex
    call print
    mov si,dosmem_xms_suffix
    call print

.xms_unavailable:
    test byte [cs:dosmem_flags],08h
    jz .vcpi_unavailable
    mov si,dosmem_vcpi_prefix
    call print
    mov eax,[cs:dosmem_vcpi_free_pages]
    mov di,dosmem_hex
    call vm_format_hex32
    mov si,dosmem_hex
    call print
    mov si,dosmem_vcpi_suffix
    call print
.vcpi_unavailable:
    cmp byte [cs:dosmem_save_status],0
    jne .save_failed
    mov si,dosmem_saved
    call print
    jmp .done
.save_failed:
    mov si,dosmem_save_failed
    call print
.done:
    pop ds
    ret

; EAX -> eight uppercase hexadecimal digits at CS:DI.
vm_format_hex32:
    pushad
    mov cx,8
.next:
    rol eax,4
    mov dl,al
    and dl,0Fh
    cmp dl,9
    jbe .digit
    add dl,'A'-10
    jmp .store
.digit:
    add dl,'0'
.store:
    mov [cs:di],dl
    inc di
    loop .next
    mov byte [cs:di],0
    popad
    ret

DOSMEM_RECORD_SIZE equ 32
xms_entry dd 0
dosmem_path db '\SYSTEM\DOSMEM.BIN',0
dosmem_handle dw 0
dosmem_save_status db 0FFh
dosmem_hex times 9 db 0
dosmem_xms_prefix db '[VMSTART] XMS3 largest=',0
dosmem_xms_middle db ' KiB total=',0
dosmem_xms_suffix db ' KiB',13,10,0
dosmem_vcpi_prefix db '[VMSTART] VCPI free pages=',0
dosmem_vcpi_suffix db ' (4 KiB)',13,10,0
dosmem_saved db '[VMSTART] SYSTEM/DOSMEM.BIN saved',13,10,0
dosmem_save_failed db '[VMSTART] SYSTEM/DOSMEM.BIN write failed',13,10,0

; CDMR v1, 32 bytes, all multibyte integers little-endian:
; 00 signature, 04 version, 05 size, 06 result flags, 07 error flags,
; 08 XMS version, 0A XMS AH=88 status, 0B VCPI DE00 status,
; 0C VCPI DE03 status, 0D reserved, 0E XMS largest KiB,
; 12 XMS total KiB, 16 VCPI free 4-KiB pages, 1A firmware usable KiB,
; 1E reserved. Firmware value is zero unless a future version sets flag 10h.
dosmem_record:
    db 'CDMR',1,DOSMEM_RECORD_SIZE
dosmem_flags db 0
dosmem_errors db 0
dosmem_xms_version dw 0
dosmem_xms_status db 0FFh
dosmem_vcpi_presence_status db 0FFh
dosmem_vcpi_query_status db 0FFh
    db 0
dosmem_xms_largest_kib dd 0
dosmem_xms_total_kib dd 0
dosmem_vcpi_free_pages dd 0
dosmem_firmware_kib dd 0
    dw 0
