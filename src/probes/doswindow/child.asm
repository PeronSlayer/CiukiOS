bits 16
org 0x100
%ifndef BIOS_ONLY
%define BIOS_ONLY 0
%endif
start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    cld
    mov si, msg_start
    call serial_string
    mov ah, 0x51
    int 0x21
    mov ax, bx
    call serial_hex
    call serial_newline
    call read_ticks
    mov [start_tick], eax
.loop:
    call read_ticks
    cmp eax, [start_tick]
    jae .same_day
    add eax, 0x001800B0
.same_day:
    sub eax, [start_tick]
    cmp ax, 37
    jae .done
    cmp ax, [last_tick]
    je .wait
    mov [last_tick], ax
    mov di, 10*160+4
    call screen_hex
.wait:
    sti
    hlt
    jmp .loop
.done:
    mov si, msg_end
    call serial_string
    call serial_newline
    mov ax, 0x4C5A
    int 0x21
%include "src/probes/doswindow/common.inc"
%if BIOS_ONLY
msg_start db '[DOSWIN:BIOS-CHILD] START psp=',0
msg_end db '[DOSWIN:BIOS-CHILD] END status=005A',0
%else
msg_start db '[DOSWIN:DIRECT-CHILD] START psp=',0
msg_end db '[DOSWIN:DIRECT-CHILD] END status=005A',0
%endif
start_tick dd 0
last_tick dw 0xFFFF
    times 768 db 0
stack_top:
