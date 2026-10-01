; One read-only hardware inventory per boot. Never trust an old machine's CFG.
; Drivers remain bounded native backends, not arbitrary Windows .SYS files.
startup_driver_services:
    pushad
    push ds
    push es
    push cs
    pop ds
    push cs
    pop es
    mov byte [driver_profile_valid],0
    call setup_exec_block
    mov byte [exec_tail],0
    mov byte [exec_tail+1],13
    mov dx,driver_input_path
    mov bx,exec_env_seg
    mov ax,0x4B00
    int 0x21
    jc .inventory
    mov ah,0x4D
    int 0x21
.inventory:
    mov byte [exec_tail],9
    mov dword [exec_tail+1],' /Q '
    mov dword [exec_tail+5],'/SAV'
    mov word [exec_tail+9],0x0D45
    mov dx,driver_detector_path
    mov bx,exec_env_seg
    mov ax,0x4B00
    int 0x21
    jc .done
    mov ah,0x4D
    int 0x21
    test ax,ax
    jnz .done
    mov dx,driver_profile_path
    mov ax,0x3D00
    int 0x21
    jc .done
    mov bx,ax
    mov dx,file_buf
    mov cx,512
    mov ah,0x3F
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    pop cx
    popf
    jc .done
    cmp cx,32
    jb .done
    cmp cx,512
    jae .done
    mov si,file_buf
    cmp dword [si],'SCHE'
    jne .done
    cmp dword [si+4],'MA=1'
    jne .done
    cmp word [si+8],0x0A0D
    jne .done
    mov bx,cx
    cmp dword [si+bx-7],'END='
    jne .done
    cmp word [si+bx-3],0x0D31
    jne .done
    cmp byte [si+bx-1],10
    jne .done
    mov byte [driver_profile_valid],1
    ; Only the Intel native backend and native Sound Blaster player exist in
    ; BOOTSND. Other PCI families remain available to their app launchers.
    mov byte [driver_startup_audio],0
    mov di,driver_audio_ich
    call driver_has_line
    jc .audio
    mov di,driver_audio_sb
    call driver_has_line
    jnc .video
.audio:
    mov byte [driver_startup_audio],1
.video:
    mov di,driver_video_vga
    call driver_has_line
    jnc .done
    mov byte [driver_force_vga],1
.done:
    ; The drivers installed in the Control Panel (\DRIVERS\DRIVERS.CFG);
    ; safe boot starts without them.
    cmp byte [vc_force_safe],1
    je .loaded
    call setup_exec_block
    mov byte [exec_tail],0
    mov byte [exec_tail+1],13
    mov dx,driver_loader_path
    mov bx,exec_env_seg
    mov ax,0x4B00
    int 0x21
.loaded:
    pop es
    pop ds
    popad
    ret

; The desktop's DOS windows run in virtual machines, as in Windows 95: the
; VM manager (Jemm386 + CVSESSION) is resident from the start of every normal
; desktop session. Safe boot keeps the plain real-mode system.
startup_vm_manager:
    cmp byte [vc_force_safe],1
    je .done
    pushad
    call setup_exec_block
    mov byte [exec_tail],0
    mov byte [exec_tail+1],13
    mov dx,driver_vm_path
    mov bx,exec_env_seg
    mov ax,0x4B00
    int 0x21
    push cs
    pop ds
    push cs
    pop es
    popad
.done:
    ret

; Long file names: the kernel's LFN extension (\SYSTEM\LFN.COM, a TSR that
; hooks INT 21h AX=71xxh) is resident before the VM manager, so every DOS VM
; has it too. Safe boot keeps the plain 8.3 system. Preserves every register.
startup_long_names:
    cmp byte [vc_force_safe],1
    je .done
    pushad
    call setup_exec_block
    mov byte [exec_tail],0
    mov byte [exec_tail+1],13
    mov dx,driver_lfn_path
    mov bx,exec_env_seg
    mov ax,0x4B00
    int 0x21
    push cs
    pop ds
    push cs
    pop es
    popad
.done:
    ret

; DI zero-terminated full line, CX report length. Only match line starts.
driver_has_line:
    pushad
    mov bx,file_buf
    add bx,cx
    mov si,file_buf
.line:
    push si
    push di
.character:
    cmp si,bx
    jae .mismatch
    mov al,[di]
    test al,al
    jz .end
    cmp al,[si]
    jne .mismatch
    inc si
    inc di
    jmp .character
.end:
    cmp byte [si],13
    jne .mismatch
    pop di
    pop si
    popad
    stc
    ret
.mismatch:
    pop di
    pop si
.skip:
    cmp si,bx
    jae .absent
    lodsb
    cmp al,10
    jne .skip
    jmp .line
.absent:
    popad
    clc
    ret

driver_profile_valid db 0
driver_startup_audio db 1
driver_force_vga db 0
driver_detector_path db '\DRIVERS\HWDETECT.COM',0
driver_loader_path db '\DRIVERS\LOADDRV.COM',0
driver_input_path db '\DRIVERS\INPUTINI.COM',0
driver_vm_path db '\VM\VMSTART.COM',0
driver_lfn_path db '\SYSTEM\LFN.COM',0
driver_profile_path db '\DRIVERS\ACTIVE.CFG',0
driver_audio_ich db 'AUDIO=ICH_AC97',0
driver_audio_sb db 'AUDIO=SB_NATIVE',0
driver_video_vga db 'VIDEO=VGA',0
