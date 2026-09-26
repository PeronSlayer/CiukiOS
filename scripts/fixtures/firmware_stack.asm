; Test-only option ROM: model the documented 1024-byte PCI BIOS stack budget
; and a disk BIOS using the same budget. The real SeaBIOS routine is called
; with the original stack and registers after the scratch footprint is used.
; This firmware fixture is never added to the release CD.
bits 16
cpu 386
org 0
%ifndef STACK_BYTES
%define STACK_BYTES 1024
%endif
%ifndef STACK_DISK
%define STACK_DISK 1
%endif
%ifndef STACK_PCI
%define STACK_PCI 1
%endif
    db 0x55,0xAA,4
    jmp init

%macro BIOS_SCRATCH 0
    ; Six bytes already belong to the CPU interrupt frame, twelve below.
    pushf
    push ax
    push cx
    push di
    push es
    push bp
    mov bp,sp
    push ss
    pop es
    sub sp,STACK_BYTES-18
    mov di,sp
    mov cx,(STACK_BYTES-18)/2
    mov ax,0xA55A
    cld
    rep stosw
    mov sp,bp
    pop bp
    pop es
    pop di
    pop cx
    pop ax
    popf
%endmacro

blob:
    dd 0                       ; original INT 13h
    dd 0                       ; original INT 1Ah
    dw 0,0                     ; actual disk/PCI calls through the fixture
disk:
    pushf
    inc word [cs:8]
    popf
    BIOS_SCRATCH
    jmp far [cs:0]
pci:
    pushf
    cmp ah,0xB1
    jne .other
    inc word [cs:10]
    popf
    BIOS_SCRATCH
    jmp far [cs:4]
.other:
    popf
    jmp far [cs:4]
blob_end:

init:
    pushf
    pushad
    push ds
    push es
    cli
    xor ax,ax
    mov ds,ax
    dec word [0x413]            ; reserve 1 KiB below the firmware RAM ceiling
    mov ax,[0x413]
    shl ax,6
    mov es,ax
    push cs
    pop ds
    mov si,blob
    xor di,di
    mov cx,blob_end-blob
    cld
    rep movsb
    xor ax,ax
    mov ds,ax
    mov eax,[0x13*4]
    mov [es:0],eax
    mov eax,[0x1A*4]
    mov [es:4],eax
%if STACK_DISK
    mov word [0x13*4],disk-blob
    mov [0x13*4+2],es
%endif
%if STACK_PCI
    mov word [0x1A*4],pci-blob
    mov [0x1A*4+2],es
%endif
    mov al,'S'
    out 0xE9,al
    pop es
    pop ds
    popad
    popf
    retf
    times 2047-($-$$) db 0
    db 0                       ; patched to the ROM checksum by the runner
