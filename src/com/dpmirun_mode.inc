; Optional CiukiOS COM launch declaration, version 1. Only a plain absolute
; direct command can opt out of DPMI-host installation. The 16-byte header is
; EB0E, "CIUKILCH", version=1, flags=1 (real mode), size=16, reserved=0.
; File errors, short reads, malformed declarations and COMMAND semantics all
; retain the ordinary host path. No external executable is patched or guessed.
vm_launch_mode_probe:
    pushad
    push ds
    push es
    mov byte [vm_real_mode],0
    and byte [vm_launch_record+254],0FBh
    call command_program
    jc .done
    call vm_direct_command
    cmp byte [vm_direct],1
    jne .done
    ; The opt-in format belongs to COM launchers, never MZ/PE/NE/BW images.
    mov si,program
.end_path:
    lodsb
    test al,al
    jnz .end_path
    sub si,5
    mov eax,[si]
    or eax,20202000h
    cmp eax,'.com'
    jne .done
    mov dx,program
    mov ax,3D00h
    int 21h
    jc .done
    mov bx,ax
    mov dx,vm_mode_header
    mov cx,16
    mov ah,3Fh
    int 21h
    pushf
    push ax
    mov ah,3Eh
    int 21h
    pop dx
    pop ax                           ; saved read FLAGS
    jc .done                         ; failed close cannot opt out
    test ax,1
    jnz .done
    cmp dx,16
    jne .done
    cmp word [vm_mode_header],0EEBh
    jne .done
    cmp dword [vm_mode_header+2],'CIUK'
    jne .done
    cmp dword [vm_mode_header+6],'ILCH'
    jne .done
    cmp word [vm_mode_header+10],0101h
    jne .done
    cmp dword [vm_mode_header+12],16
    jne .done
    mov byte [vm_real_mode],1
    or byte [vm_launch_record+254],4  ; declared real-mode target, host skipped
.done:
    pop es
    pop ds
    popad
    ret

vm_real_mode db 0
vm_mode_header times 16 db 0
