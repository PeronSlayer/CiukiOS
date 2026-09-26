; Real BIOS-text workload. Submitted batches are NOT displayed frames.
; RDTSC measures this guest's elapsed host TSC, including interrupt work.
bits 16
org 0x100

%ifndef PERF_TICKS
%define PERF_TICKS 182
%endif
%define BIOS_ONLY 1

start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    cld
    mov bx,(image_end-$$+0x100+15)/16
    mov ah,0x4A
    int 0x21
    jc failed
    mov ah,0x0F
    int 0x10
    and al,0x7F
    cmp al,3
    jne failed
    cmp ah,80
    jne failed
    test bh,bh
    jnz failed
    mov ah,1
    mov cx,0x2000             ; no caret workload hidden in the text batches
    int 0x10
    mov si,message_start
    call serial_string
    call serial_newline

    mov byte [phase],0
.phase:
    call print_phase
    mov si,message_begin
    call serial_string
    call serial_newline
    xor eax,eax
    mov [batches],eax
    mov [video_calls],eax
    call read_ticks
    mov [tick_start],eax
    call read_tsc
    mov [tsc_start],eax
    mov [tsc_start+4],edx
.batch:
    cmp byte [phase],0
    jne .scroll
    ; 25 actual INT10/AH13 strings: all 2,000 logical cells change per batch.
    xor dx,dx
.row:
    call prepare_row
    call write_row
    inc dh
    cmp dh,25
    jb .row
    jmp .done_batch
.scroll:
    mov ax,0x0601
    mov bh,0x17
    xor cx,cx
    mov dx,0x184F
    int 0x10
    inc dword [video_calls]
    mov dx,0x1800
    call prepare_row
    call write_row
.done_batch:
    inc dword [batches]
    call read_ticks
    sub eax,[tick_start]
    jnc .elapsed
    add eax,0x1800B0
.elapsed:
    mov [elapsed_ticks],eax
    cmp eax,PERF_TICKS
    jb .batch
    call read_tsc
    sub eax,[tsc_start]
    sbb edx,[tsc_start+4]
    mov [elapsed_tsc],eax
    mov [elapsed_tsc+4],edx
    call print_phase
    mov si,message_end
    call serial_string
    mov eax,[batches]
    call serial_hex32
    mov si,message_calls
    call serial_string
    mov eax,[video_calls]
    call serial_hex32
    mov si,message_ticks
    call serial_string
    mov eax,[elapsed_ticks]
    call serial_hex32
    mov si,message_tsc
    call serial_string
    mov eax,[elapsed_tsc+4]
    call serial_hex32
    mov eax,[elapsed_tsc]
    call serial_hex32
    call serial_newline
    inc byte [phase]
    cmp byte [phase],2
    jb .phase
    mov si,message_done
    call serial_string
    call serial_newline
    mov ax,0x4C00
    int 0x21

failed:
    mov si,message_fail
    call serial_string
    call serial_newline
    mov ax,0x4C01
    int 0x21

prepare_row:
    push ax
    push cx
    push di
    mov eax,[batches]
    add al,dh
    and al,15
    add al,'A'
    mov cx,80
    mov di,row_buffer
    rep stosb
    pop di
    pop cx
    pop ax
    ret

write_row:
    push bp
    push dx
    mov bp,row_buffer
    mov ax,0x1300
    mov bl,0x17
    xor bh,bh
    mov cx,80
    int 0x10
    inc dword [video_calls]
    pop dx
    pop bp
    ret

print_phase:
    mov si,message_phase
    call serial_string
    mov si,name_bulk
    cmp byte [phase],0
    je .print
    mov si,name_scroll
.print:
    call serial_string
    ret

serial_hex32:
    push eax
    shr eax,16
    call serial_hex
    pop eax
    call serial_hex
    ret

read_tsc:
    push ebx
    push ecx
    xor eax,eax
    cpuid
    rdtsc
    pop ecx
    pop ebx
    ret

%include "src/probes/doswindow/common.inc"

message_start db '[DOSPERF] START BIOS text workload; batches are not displayed frames',0
message_phase db '[DOSPERF] PHASE ',0
name_bulk db 'BULK',0
name_scroll db 'SCROLL',0
message_begin db ' BEGIN',0
message_end db ' END batches=',0
message_calls db ' int10=',0
message_ticks db ' ticks=',0
message_tsc db ' tsc=',0
message_done db '[DOSPERF] DONE',0
message_fail db '[DOSPERF] FAIL',0
phase db 0
batches dd 0
video_calls dd 0
tick_start dd 0
elapsed_ticks dd 0
tsc_start dq 0
elapsed_tsc dq 0
row_buffer times 80 db 0
    align 2
    times 2048 db 0
stack_top:
image_end:
