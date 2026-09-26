bits 16
org 0x0100

; Windows 3.1 audio wrapper.  VSBHDA16 remains the owning protected-mode
; client while WINCORE.COM runs as its synchronous child.  When Windows exits,
; the same client releases PCI DMA, IRQ and every SB/OPL port trap before
; HDPMI16 is unloaded.  This avoids cross-client TSR teardown and permits a
; clean second WIN launch.

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

    mov dx, vsbhda_path
    mov bx, vsbhda_run_param_block
    call exec_child
    jc .mark_failed
    or al, al
    jz .unload_host

.mark_failed:
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
    jc .done
    mov ah, 0x4D
    int 0x21
    clc
.done:
    pop es
    pop ds
    pop dx
    pop bx
    ret

msg_prepare db '[WIN31] Preparing application',13,10,'$'
msg_begin db '[WIN31] LAUNCH STANDARD AC97 VSBHDA16 SYNCHRONOUS', 13, 10, '$'
msg_cleanup db '[WIN31] AUDIO CLEANUP COMPLETE - RETURNING TO SHELL', 13, 10, '$'
msg_fail db '[WIN31] EXEC FAIL - APPLICATION OR AUDIO HOST FAILED', 13, 10, '$'
child_failed db 0
host_owned db 0

hdpmi_path db '\SBEMU\HDPMI16I.EXE', 0
hdpmi_install_tail db hdpmi_install_tail_end - hdpmi_install_tail_text
hdpmi_install_tail_text db ' -r -x2'
hdpmi_install_tail_end db 13
hdpmi_install_fcb1 db 0, '           ', 0, 0, 0, 0
hdpmi_install_fcb2 db 0, '           ', 0, 0, 0, 0
hdpmi_install_param_block:
    dw 0, hdpmi_install_tail, 0, hdpmi_install_fcb1, 0, hdpmi_install_fcb2, 0

hdpmi_uninstall_tail db hdpmi_uninstall_tail_end - hdpmi_uninstall_tail_text
hdpmi_uninstall_tail_text db ' -u'
hdpmi_uninstall_tail_end db 13
hdpmi_uninstall_fcb1 db 0, '           ', 0, 0, 0, 0
hdpmi_uninstall_fcb2 db 0, '           ', 0, 0, 0, 0
hdpmi_uninstall_param_block:
    dw 0, hdpmi_uninstall_tail, 0, hdpmi_uninstall_fcb1, 0, hdpmi_uninstall_fcb2, 0

vsbhda_path db '\SBEMU\VSBHDA16.EXE', 0
vsbhda_run_tail db vsbhda_run_tail_end - vsbhda_run_tail_text
vsbhda_run_tail_text db ' /RM0 /F44100 /B8 /PS512 /VOL9 /RUN:WINCORE.COM /ARG:/S'
vsbhda_run_tail_end db 13
vsbhda_run_fcb1 db 0, '           ', 0, 0, 0, 0
vsbhda_run_fcb2 db 0, '           ', 0, 0, 0, 0
vsbhda_run_param_block:
    dw 0, vsbhda_run_tail, 0, vsbhda_run_fcb1, 0, vsbhda_run_fcb2, 0

%define NATIVE_PROGRAM 'WINCORE.COM'
%define NATIVE_PREFIX ' /S'
%define NATIVE_USER_TAIL native_empty_tail
%include "src/com/audio_native.inc"
native_empty_tail db 0,13

align 16
launcher_stack times 1024 db 0
launcher_stack_top:
launcher_image_end:
