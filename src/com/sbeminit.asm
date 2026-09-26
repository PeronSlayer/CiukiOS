bits 16
org 0x0100

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, launcher_stack_top
    sti
    mov es, ax
    mov bx, ((launcher_image_end - $$ + 0x0100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc .fail

    cld
    push cs
    pop ds
    push cs
    pop es

    mov dx, msg_hdpmi
    call print_line
    mov dx, hdpmi_path
    mov bx, hdpmi_param_block
    call exec_child
    jc .fail

    mov dx, msg_sbemu
    call print_line
    mov dx, sbemu_path
    mov bx, sbemu_param_block
    call exec_child
    jc .fail

    mov dx, msg_ready
    call print_line
    mov ax, 0x4C00
    int 0x21

.fail:
    mov dx, msg_fail
    call print_line
    mov ax, 0x4C01
    int 0x21

print_line:
    mov ah, 0x09
    int 0x21
    ret

exec_child:
    ; IN: DS:DX path ASCIIZ, DS:BX DOS EXEC parameter block.
    push ax
    push bx
    push dx
    push ds
    push es
    xor ax, ax
    mov [bx + 0], ax
    mov ax, ds
    mov [bx + 4], ax
    mov [bx + 8], ax
    mov [bx + 12], ax
    push ds
    pop es
    mov ax, 0x4B00
    int 0x21
    jc .done
    mov ax, 0x4D00
    int 0x21
    ; AH=3 is a TSR completion.  Some HDPMI versions report ordinary success
    ; (AH=0/AL=0) when an instance is already present.  Every other status is
    ; a real loader/initialization failure and must never print a false READY.
    cmp ah, 3
    je .success
    or ax, ax
    jz .success
    stc
    jmp .done
.success:
    clc
    jmp .done
.done:
    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

msg_hdpmi db '[AUDIOPCI] Loading protected-mode host', 13, 10, '$'
msg_sbemu db '[AUDIOPCI] Detecting PCI audio and enabling SB16/OPL fallback', 13, 10, '$'
msg_ready db '[AUDIOPCI] READY A220 I7 D1 H5 T6', 13, 10, '$'
msg_fail db '[AUDIOPCI] UNAVAILABLE - application fallback remains active', 13, 10, '$'

hdpmi_path db '\SBEMU\HDPMI32I.EXE', 0
hdpmi_tail db hdpmi_tail_end - hdpmi_tail_text
hdpmi_tail_text db ' -r -x'
hdpmi_tail_end db 13
hdpmi_fcb1 dw 0, 0
hdpmi_fcb2 dw 0, 0
hdpmi_param_block:
    dw 0
    dw hdpmi_tail
    dw 0
    dw hdpmi_fcb1
    dw 0
    dw hdpmi_fcb2
    dw 0

sbemu_path db '\SBEMU\SBEMU.EXE', 0
sbemu_tail db sbemu_tail_end - sbemu_tail_text
sbemu_tail_text db ' /T6 /I7 /D1 /H5 /K22050 /VOL100 /RM0'
sbemu_tail_end db 13
sbemu_fcb1 dw 0, 0
sbemu_fcb2 dw 0, 0
sbemu_param_block:
    dw 0
    dw sbemu_tail
    dw 0
    dw sbemu_fcb1
    dw 0
    dw sbemu_fcb2
    dw 0

align 16
launcher_stack times 1024 db 0
launcher_stack_top:
launcher_image_end:
