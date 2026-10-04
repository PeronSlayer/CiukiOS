; Event sounds are serviced in the foreground, never during compositor or
; DOS-window interrupt callbacks. The external driver returns after DMA start.
ui_sfx_begin:
    pushad
    push es
    cmp word [ui_sfx_entry+2],0
    jne .done
    mov dx,ui_sfx_path
    mov ax,0x3D00
    int 0x21
    jc .done
    mov [ui_sfx_handle],ax
    mov bx,0x400
    mov ah,0x48
    int 0x21
    jc .close
    mov [ui_sfx_entry+2],ax
    mov ds,ax
    xor dx,dx
    mov cx,0x4000
    mov bx,[cs:ui_sfx_handle]
    mov ah,0x3F
    int 0x21
    push cs
    pop ds
    jc .free
    mov es,[ui_sfx_entry+2]
    cmp ax,32
    jb .free
    cmp ax,[es:12]
    jne .free
    cmp dword [es:3],'CSFX'
    jne .free
    cmp dword [es:7],'0001'
    jne .free
    cmp word [es:14],1
    jne .free
    add ax,15
    shr ax,4
    mov bx,ax
    mov ah,0x4A
    int 0x21
    jc .free
    cmp dword [boot_session_mode],'SAFE'
    je .close
    cmp dword [boot_session_mode],'MUTE'
    je .close
    cmp dword [boot_session_mode],'HARD'
    je .close
    cmp byte [driver_startup_audio],0
    je .close
    xor ax,ax
    call ui_sfx_call
    jmp .close
.free:
    mov es,[ui_sfx_entry+2]
    mov ah,0x49
    int 0x21
    mov word [ui_sfx_entry+2],0
.close:
    mov bx,[ui_sfx_handle]
    mov ah,0x3E
    int 0x21
.done:
    pop es
    popad
    ret

ui_sfx_service:
    push ax
    xor ax,ax
    xchg al,[ui_sfx_pending]
    test ax,ax
    jnz .event
    mov ax,1
.event:
    call ui_sfx_call
    pop ax
    ret
ui_sfx_call:
    cmp word [ui_sfx_entry+2],0
    je .done
    push bx
    mov bx,ax
    call far [ui_sfx_entry]
    cmp bx,1
    jne .status
    cmp ax,3
    jb .polled
.status:
    mov [ui_sfx_status],ax
.polled:
    pop bx
.done:
    ret

ui_sfx_end:
    pushad
    push es
    mov ax,2
    call ui_sfx_call
    mov ax,[ui_sfx_entry+2]
    test ax,ax
    jz .done
    mov es,ax
    mov ah,0x49
    int 0x21
    mov word [ui_sfx_entry+2],0
.done:
    mov byte [ui_sfx_pending],0
    pop es
    popad
    ret

ui_sfx_notice:
    cmp byte [ui_dialog],4
    jne .done
    mov byte [ui_sfx_pending],3
.done:
    ret

ui_sound_set:
    ; AL='0' or '1'. Persist in the same preference used at boot.
    xchg al,[ui_sound_preference_value]
    push ax
    mov dx,ui_sound_preference_path
    xor cx,cx
    mov ah,0x3C
    int 0x21
    jc .error
    mov bx,ax
    mov dx,ui_sound_preference_value
    mov cx,1
    mov ah,0x40
    int 0x21
    pushf
    push ax
    mov ah,0x3E
    int 0x21
    jc .close_error
    pop ax
    popf
    jc .error
    cmp ax,1
    jne .error
    pop ax
    mov byte [ui_sfx_pending],6
    jmp ui_redraw
.close_error:
    pop ax
    pop ax
.error:
    pop ax
    mov [ui_sound_preference_value],al
    mov byte [ui_dialog],4
    call ui_windows_open
    jmp ui_redraw

ui_sfx_path db '\SYSTEM\DRIVERS\SFX.DRV',0
ui_sfx_entry dw 0,0
ui_sfx_handle dw 0
ui_sfx_status dw 3
ui_sfx_pending db 0
ui_sfx_messages dw us_ready,us_started,us_muted,us_device,us_file,us_busy,us_memory,us_failed
us_ready db 'Ready.',0
us_started db 'PCM playback started.',0
us_muted db 'Sounds are disabled.',0
us_device db 'PCM audio is unavailable.',0
us_file db 'Sound file missing or invalid.',0
us_busy db 'Audio device is busy.',0
us_memory db 'Not enough memory for audio.',0
us_failed db 'Audio playback failed.',0
