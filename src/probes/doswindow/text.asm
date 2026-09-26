bits 16
%ifndef BIOS_ONLY
%define BIOS_ONLY 0
%endif
%ifndef RUN_TICKS
%define RUN_TICKS 728             ; about 40 seconds at the BIOS 18.2 Hz rate
%endif
%ifndef ENABLE_NEST
%define ENABLE_NEST 1
%endif

%ifdef MZ
section .header start=0 vstart=0
    dw 0x5A4D, (32+image_size) & 511, (32+image_size+511)/512
    dw 0, 2, 0, 0xFFFF, 0, stack_top-start, 0, 0, 0, 0x1C, 0
    times 32-($-$$) db 0
section .text start=32 vstart=0
%else
section .text start=0 vstart=0x100
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
    mov ah, 0x51
    int 0x21
    mov [psp], bx
    mov es, bx
    mov bx, (image_size+0x100+15)/16
    mov ah, 0x4A
    int 0x21
    jc memory_fail
    push cs
    pop es

    ; Do not switch physical video. A DOS-window backend must expose its
    ; logical text mode through the ordinary INT10 query, not a private ABI.
    mov ah, 0x0F
    int 0x10
    and al, 0x7F
    cmp al, 3
    jne video_fail
    cmp ah, 80
    jne video_fail
    test bh, bh
    jnz video_fail

    mov ax, 0x0600
    mov bh, 0x17
    xor cx, cx
    mov dx, 0x184F
    int 0x10
    mov si, title
    mov ah, 0x1F
    mov di, 160+4
    call screen_string
    mov si, help
    mov di, 3*160+4
    call screen_string
    mov si, labels
    mov di, 5*160+4
    call screen_string

    call prefix
    mov si, msg_start
    call serial_string
    mov ax, [psp]
    call serial_hex
    call serial_newline
    call file_roundtrip
    jc file_fail
    call prefix
    mov si, msg_file_ok
    call serial_string
    call serial_newline
    call read_ticks
    mov [start_tick], eax

.loop:
    call elapsed_ticks
    mov [elapsed], ax
    cmp ax, RUN_TICKS
    jae .timed_end
    sub ax, [last_frame]
    cmp ax, 2
    jb .keyboard
    mov ax, [elapsed]
    mov [last_frame], ax
    inc word [frames]
    call draw_frame
    mov ax, [elapsed]
    sub ax, [last_beat]
    cmp ax, 18
    jb .maybe_nested
    mov ax, [elapsed]
    mov [last_beat], ax
    call bios_scroll
    call prefix
    mov si, msg_live
    call serial_string
    call counters
    call serial_newline
.maybe_nested:
    cmp byte [mid_file_done], 0
    jne .nested
    cmp word [elapsed], 364       ; exercise DOS file I/O during the live run
    jb .nested
    mov byte [mid_file_done], 1
    call file_roundtrip
    jc file_fail
    call prefix
    mov si, msg_mid_file
    call serial_string
    call counters
    call serial_newline
.nested:
%if ENABLE_NEST
    cmp byte [nested_done], 0
    jne .keyboard
    cmp word [elapsed], 218       ; nested execution at about 12 seconds
    jb .keyboard
    mov byte [nested_done], 1
    call nested_exec
    jc exec_fail
%endif
.keyboard:
    mov ah, 1
    int 0x16
    jz .wait
    xor ah, ah
    int 0x16
    mov [last_key], ax
    inc word [keys]
    call prefix
    mov si, msg_key
    call serial_string
    mov ax, [last_key]
    call serial_hex
    call serial_newline
    mov di, 6*160+4
    call screen_hex
    cmp byte [last_key], 27
    je .escaped
.wait:
    ; Real hardware timer wake-up; no INT28/DOS idle call or host test hook.
    sti
    hlt
    jmp .loop
.timed_end:
    mov si, msg_timeout
    jmp .end
.escaped:
    mov si, msg_escape
.end:
    push si
    call prefix
    mov si, msg_end
    call serial_string
    call counters
    pop si
    call serial_string
    call serial_newline
    mov ax, 0x4C00
    int 0x21

elapsed_ticks:
    call read_ticks
    cmp eax, [start_tick]
    jae .same_day
    add eax, 0x001800B0
.same_day:
    sub eax, [start_tick]
    ret

draw_frame:
    mov bx, [position]
    mov di, 8*160+4
    shl bx, 1
    add di, bx
    mov ax, 0x1720
    call put_cell
    inc word [position]
    cmp word [position], 75
    jb .position_ready
    mov word [position], 0
.position_ready:
    mov bx, [position]
    mov di, 8*160+4
    shl bx, 1
    add di, bx
    mov ax, 0x1E2A             ; bright moving '*' character
    call put_cell
    mov ax, [elapsed]
    mov di, 5*160+18
    call screen_hex
    mov ax, [frames]
    mov di, 5*160+50
    call screen_hex
    ret

bios_scroll:
    push ax
    push bx
    push cx
    push dx
    push si
    mov ax, 0x0601
    mov bh, 0x17
    mov cx, 0x0C02
    mov dx, 0x174D
    int 0x10
    mov ah, 0x02
    xor bh, bh
    mov dx, 0x1702
    int 0x10
    mov si, scroll_text
.text:
    lodsb
    test al, al
    jz .tick
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .text
.tick:
    mov ax, [elapsed]
    mov di, 23*160+50
    call screen_hex
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

file_roundtrip:
    ; Refuse to overwrite an existing file. Only our newly created scratch
    ; file may be removed on cleanup. Run from a writable test directory.
    mov dx, file_name
    mov ax, 0x3D00
    int 0x21
    jc .absent
    mov bx, ax
    mov ah, 0x3E
    int 0x21
    stc
    ret
.absent:
    cmp ax, 2
    jne .fail
    mov dx, file_name
    xor cx, cx
    mov ah, 0x3C
    int 0x21
    jc .fail
    mov byte [file_owned], 1
    mov [handle], ax
    mov bx, ax
    mov dx, file_payload
    mov cx, file_payload_size
    mov ah, 0x40
    int 0x21
    jc .cleanup_fail
    cmp ax, file_payload_size
    jne .cleanup_fail
    call close_file
    jc .cleanup_fail
    mov dx, file_name
    mov ax, 0x3D00
    int 0x21
    jc .cleanup_fail
    mov [handle], ax
    mov bx, ax
    mov dx, file_readback
    mov cx, file_payload_size+1
    mov ah, 0x3F
    int 0x21
    jc .cleanup_fail
    cmp ax, file_payload_size
    jne .cleanup_fail
    push cs
    pop es
    mov si, file_payload
    mov di, file_readback
    mov cx, file_payload_size
    repe cmpsb
    jne .cleanup_fail
    call close_file
    jc .cleanup_fail
    mov dx, file_name
    mov ah, 0x41
    int 0x21
    ret
.cleanup_fail:
    call close_file
    cmp byte [file_owned], 1
    jne .fail
    mov dx, file_name
    mov ah, 0x41
    int 0x21
.fail:
    stc
    ret
close_file:
    mov bx, [handle]
    cmp bx, 0xFFFF
    je .done
    mov ah, 0x3E
    int 0x21
    mov word [handle], 0xFFFF
    ret
.done:
    clc
    ret

nested_exec:
    call prefix
    mov si, msg_exec_begin
    call serial_string
    call counters
    call serial_newline
    mov ax, cs
    mov es, ax
    mov [params+4], ax
    mov [params+8], ax
    mov [params+12], ax
    mov bx, params
    mov dx, child_path
    mov ax, 0x4B00
    int 0x21
    pushf
    push cs
    pop ds
    push cs
    pop es
    popf
    jc .fail
    mov ah, 0x4D
    int 0x21
    cmp ax, 0x005A
    jne .fail
    mov ah, 0x51
    int 0x21
    cmp bx, [psp]
    jne .fail
    call elapsed_ticks
    mov [elapsed], ax
    call prefix
    mov si, msg_exec_return
    call serial_string
    call counters
    call serial_newline
    clc
    ret
.fail:
    stc
    ret

prefix:
    push si
    mov si, marker_prefix
    call serial_string
    pop si
    ret
counters:
    mov ax, [elapsed]
    call serial_hex
    mov si, msg_frames
    call serial_string
    mov ax, [frames]
    call serial_hex
    ret
memory_fail:
    mov si, msg_memory_fail
    jmp failed
video_fail:
    mov si, msg_video_fail
    jmp failed
file_fail:
    mov si, msg_file_fail
    jmp failed
exec_fail:
    mov si, msg_exec_fail
failed:
    push cs
    pop ds
    call prefix
    call serial_string
    call serial_newline
    mov ax, 0x4C01
    int 0x21

%include "src/probes/doswindow/common.inc"

%if BIOS_ONLY
    %ifdef MZ
        %define KIND 'BIOS-MZ'
    %else
        %define KIND 'BIOS-COM'
    %endif
    child_path db 'DWBIOSCH.COM', 0
%else
    %ifdef MZ
        %define KIND 'DIRECT-MZ'
    %else
        %define KIND 'DIRECT-COM'
    %endif
    child_path db 'DWCHILD.COM', 0
%endif
marker_prefix db '[DOSWIN:', KIND, '] ', 0
title db 'CiukiOS DOS window acceptance: ', KIND, 0
help db 'Type letters to record keys. ESC exits. Automatic exit in about 40 seconds.',0
labels db 'Ticks: 0000     Frame: 0000     BIOS scroll region below',0
scroll_text db 'INT10 scroll is active',0
msg_start db 'START psp=',0
msg_file_ok db 'FILE OK bytes verified and scratch removed',0
msg_mid_file db 'FILE MIDRUN OK ticks=',0
msg_live db 'LIVE ticks=',0
msg_frames db ' frames=',0
msg_key db 'KEY ax=',0
msg_exec_begin db 'EXEC BEGIN ticks=',0
msg_exec_return db 'EXEC RETURN ticks=',0
msg_end db 'END ticks=',0
msg_timeout db ' reason=TIMEOUT',0
msg_escape db ' reason=ESC',0
msg_memory_fail db 'FAIL memory resize',0
msg_video_fail db 'FAIL logical mode must be 80-column color text page zero',0
msg_file_fail db 'FAIL file roundtrip (needs writable CWD and absent DWCHECK.DAT)',0
msg_exec_fail db 'FAIL nested EXEC/status/PSP restore',0
file_name db 'DWCHECK.DAT',0
file_payload db 'DOS window actual file roundtrip',13,10,0x00,0x1A,0xFF,0x55,0xAA
file_payload_size equ $-file_payload
file_readback times file_payload_size+1 db 0
handle dw 0xFFFF
file_owned db 0
psp dw 0
start_tick dd 0
elapsed dw 0
last_frame dw 0
last_beat dw 0
frames dw 0
position dw 0
keys dw 0
last_key dw 0
nested_done db 0
mid_file_done db 0
params dw 0, tail, 0, fcb1, 0, fcb2, 0
tail db 0, 13
fcb1 db 0, '           ', 0, 0, 0, 0
fcb2 db 0, '           ', 0, 0, 0, 0
align 2
    times 1536 db 0
stack_top:
image_end:
image_size equ image_end-start
