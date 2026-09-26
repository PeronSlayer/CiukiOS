; Test-only legacy BIOS variation; never part of release images.
; EDD is advertised by the original BIOS but read/write reports zero transfer.
; AH=08 supplies CHS geometry while returning its own ES:DI output pointer.
bits 16
cpu 386
org 100h
%ifndef RESIDENT_PADDING_PARAS
%define RESIDENT_PADDING_PARAS 0
%endif
    jmp start
tag db 'DBIOSFIX'
edd_reads dw 0
edd_writes dw 0
geometry_calls dw 0
chs_reads dw 0
chs_writes dw 0
resets dw 0
invalid_dap_count dw 0
old_disk dd 0
disk:
    cmp ah, 42h
    je .edd_read
    cmp ah, 43h
    je .edd_write
    cmp ah, 8
    je .geometry
    cmp ah, 2
    jne .not_read
    inc word [cs:chs_reads]
.not_read:
    cmp ah, 3
    jne .not_write
    inc word [cs:chs_writes]
.not_write:
    cmp ah, 0
    jne .chain
    inc word [cs:resets]
.chain:
    jmp far [cs:old_disk]
.edd_read:
    inc word [cs:edd_reads]
    jmp .edd_failure
.edd_write:
    inc word [cs:edd_writes]
.edd_failure:
    cmp word [si+2], 1
    je .count_valid
    inc word [cs:invalid_dap_count]
.count_valid:
    mov word [si+2], 0
    mov ah, 20h
    push bp
    mov bp, sp
    or word [ss:bp+6], 1
    pop bp
    iret
.geometry:
    inc word [cs:geometry_calls]
    pushf
    call far [cs:old_disk]
    jc .geometry_failed
    push ax
    mov ax, 0f000h
    mov es, ax
    pop ax
    mov di, 1234h
    push bp
    mov bp, sp
    and word [ss:bp+6], 0fffeh
    pop bp
    iret
.geometry_failed:
    push bp
    mov bp, sp
    or word [ss:bp+6], 1
    pop bp
    iret
start:
    mov ax, 3513h
    int 21h
    mov [old_disk], bx
    mov [old_disk+2], es
    mov ax, 2513h
    mov dx, disk
    int 21h
    mov dx, message
    mov ah, 9
    int 21h
    mov dx, (image_end-$$+100h+15)/16
    mov ax, 3100h
    int 21h
message db '[DISK-BIOS-FAULT] installed', 13, 10, '$'
; Vary the following MZ load segment without changing the INT13 behavior.
times RESIDENT_PADDING_PARAS*16 db 0
image_end:
