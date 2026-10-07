bits 16
%include "src/com/audio_launcher.inc"
%include "src/vm/session_abi.inc"

; Run the original bound DOOMCORE.EXE as a child of VSBHDA, then synchronously
; release the physical PCI controller, IRQ handler and all I/O traps.  Nothing
; remains resident after Doom exits, so a second launch and the shell share no
; stale DPMI/audio state.

start:
    audio_launcher_enter
    jc .fail
    call doom_session_active
    jnc .session_native
    call doom_pvi_enter

    cld
    push cs
    pop ds
    push cs
    pop es
    mov dx, doom_dir
    mov ah, 0x3B
    int 0x21
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
    call doom_pvi_leave
    mov ax, 0x4C00
    int 0x21

.fail:
    mov dx, msg_fail
    call print_line
    call doom_pvi_leave
    mov ax, 0x4C01
    int 0x21

.session_native:
    ; DPMIRUN /V already owns the HDPMI host and virtual SB16/OPL devices.
    ; Run the engine directly; do not install another host, probe port 22Ch,
    ; or modify CR4.PVI in the session's guest.
    mov dx, doom_dir
    mov ah, 0x3B
    int 0x21
    mov dx, msg_session
    call print_line
    call build_native_tail
    mov dx, native_path
    mov bx, native_param_block
    call exec_child
    jc .fail
    or al, al
    jnz .fail
    mov dx, msg_session_done
    call print_line
    mov ax, 0x4C00
    int 0x21

print_line:
    mov ah, 0x09
    int 0x21
    ret

; CF=0 only when this DOS process belongs to an active CiukiOS DOS session
; with an installed 32-bit DPMI host. CVSESSION is the session discriminator;
; DPMI presence alone also occurs in plain DOS with a resident host.
doom_session_active:
    push ax
    push bx
    push cx
    push dx
    push di
    push es
    xor ax, ax
    mov es, ax
    xor di, di
    mov ax, 0x1684
    mov bx, VM_DEVICE_ID
    int 0x2F
    mov ax, es
    or ax, di
    jz .inactive
    mov [cs:doom_session_entry], di
    mov [cs:doom_session_entry + 2], es
    push cs
    pop es
    xor cx, cx
    mov ax, VM_OP_QUERY
    call far [cs:doom_session_entry]
    jc .inactive
    or dx, dx
    jz .inactive
    ; The session launcher installs its protected-mode host before COMMAND.COM.
    mov ax, 0x1687
    int 0x2F
    or ax, ax
    jnz .inactive
    test bx, 1                    ; the engine requires a 32-bit client host
    jz .inactive
    clc
    jmp .done
.inactive:
    stc
.done:
    pop es
    pop di
    pop dx
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
    jcxz .done
    mov si, launcher_user_tail + 1
.skip_spaces:
    jcxz .done
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
    jcxz .done
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
    jc .done
    mov ax, 0x4D00
    int 0x21
    clc
.done:
    pop es
    pop ds
    pop dx
    pop bx
    ret

msg_prepare db '[DOOM] Preparing application',13,10,'$'
msg_session db '[DOOM] Using the active DOS session audio devices',13,10,'$'
msg_session_done db '[DOOM] DOS session game returned',13,10,'$'
msg_begin db '[DOOM] LAUNCH AC97 VSBHDA 2.0 TRANSIENT', 13, 10, '$'
msg_cleanup db '[DOOM] AUDIO CLEANUP COMPLETE - RETURNING TO SHELL', 13, 10, '$'
msg_fail db '[DOOM] EXEC FAIL', 13, 10, '$'
doom_dir db '\APPS\DOOM', 0
child_failed db 0
host_owned db 0

hdpmi_path db '\SBEMU\HDPMI32I.EXE', 0
hdpmi_install_tail db hdpmi_install_tail_end - hdpmi_install_tail_text
hdpmi_install_tail_text db ' -r -x4'
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
doom_session_entry dd 0

vsbhda_path db '\SBEMU\VSBHDA.EXE', 0
; The original DOS/4GW Professional engine intermittently stalls with IRQ0
; left in service while sound IRQ callbacks are active. VSBHDA's documented
; CF4 option masks the PIT only inside its sound ISR and restores the mask
; before returning. Keep this compatibility setting local to original Doom.
vsbhda_tail_prefix db ' /RM0 /CF4 /F44100 /B8 /PS512 /VOL9 /RUN:DOOMCORE.EXE "/ARG:'
vsbhda_tail_prefix_len equ $ - vsbhda_tail_prefix
vsbhda_tail db 0
vsbhda_tail_text times 127 db 0
launcher_user_tail times 128 db 0
vsbhda_fcb1 db 0, '           ', 0, 0, 0, 0
vsbhda_fcb2 db 0, '           ', 0, 0, 0, 0
vsbhda_param_block:
    dw 0, vsbhda_tail, 0, vsbhda_fcb1, 0, vsbhda_fcb2, 0

%define NATIVE_PROGRAM 'DOOMCORE.EXE'
%define NATIVE_PREFIX ' '
%define NATIVE_USER_TAIL launcher_user_tail
%include "src/com/audio_native.inc"

align 16
launcher_stack times 1024 db 0
launcher_stack_top:
launcher_image_end:
