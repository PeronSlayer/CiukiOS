; Disposable QEMU-only BIOS fixture; never packaged on the release disk.
; The genuine BIOS still describes/sets/maps every mode. Hide depths other
; than TEST_DEPTH to exercise the fallback renderer on the same VGA hardware.
bits 16
cpu 386
org 0x100
%ifndef TEST_DEPTH
%define TEST_DEPTH 16
%endif
    jmp start
old_video dd 0
video:
    cmp ax,0x4F01
    je .info
    jmp far [cs:old_video]
.info:
    pushf
    call far [cs:old_video]
    cmp ax,0x004F
    jne .done
    cmp byte [es:di+25],TEST_DEPTH
    je .done
    and word [es:di],0xFFFE
.done:
    iret
start:
    mov ax,0x3510
    int 0x21
    mov [old_video],bx
    mov [old_video+2],es
    mov ax,0x2510
    mov dx,video
    int 0x21
    mov dx,message
    mov ah,9
    int 0x21
    mov dx,((image_end-$$+0x100)+15)/16
    mov ax,0x3100
    int 0x21
message db '[VBE-FIXTURE] installed',13,10,'$'
image_end:
