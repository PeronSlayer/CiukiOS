bits 16
%include "src/com/audio_launcher.inc"

start:
    audio_launcher_enter
    jnc .memory_ready
    mov word [cs:exec_error], 0x0008
    jmp .fail
.memory_ready:

    cld
    push cs
    pop ds
    push cs
    pop es

    mov dx, msg_prepare
    call print_line

    mov dx, hdpmi_path
    mov bx, hdpmi_install_param_block
    call exec_child
    jc .fail
    ; HDPMI: 0/1/2 installed, 3 reuses another host, >=4 is an error.
    cmp al, 3
    ja .host_failed
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
    mov [exec_error], ax
    mov byte [child_failed], 1

    jmp .unload_host
.native:
    ; Native ISA IRQ callbacks also need DOS/4GW CLI/STI virtualization.
    call doom_pvi_enter
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
    mov [exec_error], ax
    mov byte [child_failed], 1

.unload_host:
    cmp byte [host_owned], 0
    je .host_released
    mov dx, hdpmi_path
    mov bx, hdpmi_uninstall_param_block
    call exec_child
    jc .fail
    or al, al
    jnz .host_failed
.host_released:
    cmp byte [child_failed], 0
    jne .fail

    mov dx, msg_cleanup
    call print_line

    call doom_pvi_leave
    mov ax, 0x4C00
    int 0x21

.host_failed:
    mov [exec_error], ax
.fail:
    mov dx, msg_fail
    call print_line
    mov ax, [exec_error]
    call print_hex16
    mov dx, crlf
    call print_line
    call doom_pvi_leave
    mov ax, 0x4C01
    int 0x21

print_line:
    mov ah, 0x09
    int 0x21
    ret

print_hex16:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.digit:
    rol bx, 4
    mov al, bl
    and al, 0x0F
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    mov dl, al
    mov ah, 0x02
    int 0x21
    loop .digit
    pop cx
    pop bx
    pop ax
    ret

%include "src/com/dos4gw_pvi.inc"

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
    mov cl, [launcher_user_tail]
    cmp cl, 0
    je .done
    mov si, launcher_user_tail + 1
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
    cmp bl, 125
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
    mov [exec_error], ax
    jc .done
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

msg_prepare db '[DOOMVAN] Preparing application',13,10,'$'
msg_begin db '[DOOMVAN] LAUNCH AC97 VSBHDA 2.0 TRANSIENT', 13, 10, '$'
msg_cleanup db '[DOOMVAN] AUDIO CLEANUP COMPLETE - RETURNING TO SHELL', 13, 10, '$'
msg_fail db '[DOOMVAN] EXEC FAIL', 13, 10, '$'
crlf db 13, 10, '$'
exec_error dw 0
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
vsbhda_tail_prefix db ' /RM0 /F44100 /B8 /PS512 /VOL9 /RUN:DOS4GW.EXE "/ARG:PCDMCORE.EXE'
vsbhda_tail_prefix_len equ $ - vsbhda_tail_prefix
vsbhda_tail db 0
vsbhda_tail_text times 127 db 0
launcher_user_tail times 128 db 0
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
%define NATIVE_PREFIX ' PCDMCORE.EXE '
%define NATIVE_USER_TAIL launcher_user_tail
%include "src/com/audio_native.inc"

align 16
launcher_stack times 1024 db 0
launcher_stack_top:
launcher_image_end:
