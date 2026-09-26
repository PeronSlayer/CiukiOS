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

    mov dx, msg_prepare
    call print_line

    ; HDPMI is resident only while this launcher owns it. VSBHDA runs Wolf as
    ; a nested client, then synchronously releases AC97 DMA, the physical IRQ
    ; handler and every emulated SB/OPL port trap before we unload HDPMI.
    mov dx, hdpmi_path
    mov bx, hdpmi_install_param_block
    call exec_child
    jc .fail
    ; HDPMI: 0/1/2 installed, 3 reuses another host, >=4 is an error.
    cmp al, 3
    ja .fail
    je .host_ready
    mov byte [host_owned], 1
.host_ready:

    call audio_native_probe
    jnc .native
    mov dx, msg_begin
    call print_line

    call build_vsbhda_tail
    mov dx, vsbhda_path
    mov bx, vsbhda_param_block
    call exec_child
    jc .child_failed
    or al, al
    jz .unload_host
.child_failed:
    mov byte [child_failed], 1

    jmp .unload_host
.native:
    mov dx, msg_native
    call print_line
    call build_native_tail
    mov dx, native_path
    mov bx, native_param_block
    call exec_child
    jc .native_failed
    or al, al
    jz .unload_host
.native_failed:
    mov byte [child_failed], 1

.unload_host:
    cmp byte [host_owned], 0
    je .host_released
    mov dx, hdpmi_path
    mov bx, hdpmi_uninstall_param_block
    call exec_child
    jc .fail
    or al, al
    jnz .fail
.host_released:
    cmp byte [child_failed], 0
    jne .fail

    mov dx, msg_cleanup
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

build_vsbhda_tail:
    push ax
    push bx
    push cx
    push si
    push di
    mov di, vsbhda_tail_text
    mov si, vsbhda_tail_prefix
    mov cx, vsbhda_tail_prefix_len
.copy_base:
    lodsb
    stosb
    loop .copy_base
    mov bl, vsbhda_tail_prefix_len
    mov cl, [0x80]
    cmp cl, 0
    je .done
    mov si, 0x81
.skip_spaces:
    cmp cl, 0
    je .done
    lodsb
    dec cl
    cmp al, ' '
    je .skip_spaces
    cmp al, 9
    je .skip_spaces
    mov ah, al
    mov al, ' '
    stosb
    inc bl
    mov al, ah
.copy_args:
    cmp bl, 126
    jae .done
    stosb
    inc bl
    cmp cl, 0
    je .done
    lodsb
    dec cl
    cmp al, 13
    je .done
    jmp .copy_args
.done:
    cmp bl, 126
    jae .terminate
    mov al, '"'
    stosb
    inc bl
.terminate:
    mov [vsbhda_tail], bl
    mov al, 13
    stosb
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

exec_child:
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
    jnc .loaded
    jmp .done
.loaded:
    mov ax, 0x4D00
    int 0x21
    clc
.done:
    pop es
    pop ds
    pop dx
    pop bx
    ret

msg_prepare db '[WOLF3D] Preparing application',13,10,'$'
msg_begin db '[WOLF3D] LAUNCH AC97 VSBHDA 2.0 TRANSIENT', 13, 10, '$'
msg_cleanup db '[WOLF3D] AUDIO CLEANUP COMPLETE - RETURNING TO SHELL', 13, 10, '$'
msg_fail db '[WOLF3D] EXEC FAIL', 13, 10, '$'
child_failed db 0
host_owned db 0

hdpmi_path db '\SBEMU\HDPMI32I.EXE', 0
hdpmi_install_tail db hdpmi_install_tail_end - hdpmi_install_tail_text
hdpmi_install_tail_text db ' -r -x4'
hdpmi_install_tail_end db 13
hdpmi_install_fcb1 db 0, '           ', 0, 0, 0, 0
hdpmi_install_fcb2 db 0, '           ', 0, 0, 0, 0
hdpmi_install_param_block:
    dw 0
    dw hdpmi_install_tail
    dw 0
    dw hdpmi_install_fcb1
    dw 0
    dw hdpmi_install_fcb2
    dw 0

hdpmi_uninstall_tail db hdpmi_uninstall_tail_end - hdpmi_uninstall_tail_text
hdpmi_uninstall_tail_text db ' -u'
hdpmi_uninstall_tail_end db 13
hdpmi_uninstall_fcb1 db 0, '           ', 0, 0, 0, 0
hdpmi_uninstall_fcb2 db 0, '           ', 0, 0, 0, 0
hdpmi_uninstall_param_block:
    dw 0
    dw hdpmi_uninstall_tail
    dw 0
    dw hdpmi_uninstall_fcb1
    dw 0
    dw hdpmi_uninstall_fcb2
    dw 0

vsbhda_path db '\SBEMU\VSBHDA.EXE', 0
vsbhda_tail_prefix db ' /RM0 /F44100 /B8 /PS512 /VOL9 /RUN:DOS4GW.EXE "/ARG:WOLF4GW.EXE'
vsbhda_tail_prefix_len equ $ - vsbhda_tail_prefix
vsbhda_tail db 0
vsbhda_tail_text times 127 db 0
vsbhda_fcb1 db 0, '           ', 0, 0, 0, 0
vsbhda_fcb2 db 0, '           ', 0, 0, 0, 0
vsbhda_param_block:
    dw 0
    dw vsbhda_tail
    dw 0
    dw vsbhda_fcb1
    dw 0
    dw vsbhda_fcb2
    dw 0

%define NATIVE_PROGRAM 'DOS4GW.EXE'
%define NATIVE_PREFIX ' WOLF4GW.EXE '
%define NATIVE_USER_TAIL 0x80
%include "src/com/audio_native.inc"

align 16
; Nested EXEC/MCB traversal needs the same stack allowance as the Doom launcher.
launcher_stack times 1024 db 0
launcher_stack_top:
launcher_image_end:
