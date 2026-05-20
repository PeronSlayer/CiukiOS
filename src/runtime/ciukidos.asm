bits 16
org 0x0000

runtime_start:
    jmp short runtime_entry

runtime_signature db 'CIUKIDOS'
runtime_version db 'CIUKIDOS runtime v0.6.7', 0
runtime_stage2_ready_marker db '[S2] ready', 13, 10, 0

runtime_state_signature db 'CDOSSTAT'
runtime_state_version db 0, 6, 7
runtime_state_flags dw 0
runtime_state_handoff_version dw 0
runtime_state_boot_drive db 0
runtime_state_default_drive db 0
runtime_state_mem_kb dw 0
runtime_state_fat_spt dw 0
runtime_state_fat_heads dw 0
runtime_state_fat_reserved dw 0
runtime_state_fat_spc db 0
runtime_state_entry_flags db 0
runtime_state_table_off dw 0
runtime_state_table_seg dw 0
runtime_state_buffer_seg dw 0
runtime_state_buffer_size dw 0
runtime_state_current_psp dw 0
runtime_state_parent_psp dw 0
runtime_state_previous_psp dw 0
runtime_state_dta_seg dw 0
runtime_state_dta_off dw 0
runtime_state_saved_parent_dta_seg dw 0
runtime_state_saved_parent_dta_off dw 0
runtime_state_jft_seg dw 0
runtime_state_jft_off dw 0
runtime_state_jft_count dw 0
runtime_state_std_handle0 dw 0
runtime_state_std_handle1 dw 1
runtime_state_std_handle2 dw 2
runtime_state_handle_table_seg dw 0
runtime_state_handle_table_off dw 0
runtime_state_sft_seg dw 0
runtime_state_sft_off dw 0
runtime_state_scratch_seg dw 0
runtime_state_scratch_off dw 0

runtime_entry:
    push ax
    push bx
    push ds
    push es
    push di
    cld
    mov bx, di
    push cs
    pop ds

    mov ax, [es:bx + 6]
    mov [runtime_state_handoff_version], ax
    mov al, [es:bx + 8]
    mov [runtime_state_boot_drive], al
    mov al, [es:bx + 9]
    mov [runtime_state_default_drive], al
    mov ax, [es:bx + 10]
    mov [runtime_state_mem_kb], ax
    mov ax, [es:bx + 12]
    mov [runtime_state_fat_spt], ax
    mov ax, [es:bx + 14]
    mov [runtime_state_fat_heads], ax
    mov ax, [es:bx + 16]
    mov [runtime_state_fat_reserved], ax
    mov al, [es:bx + 18]
    mov [runtime_state_fat_spc], al
    mov al, [es:bx + 19]
    mov [runtime_state_entry_flags], al

    mov ax, runtime_service_table
    mov [runtime_state_table_off], ax
    mov ax, cs
    mov [runtime_state_table_seg], ax
    mov word [runtime_state_flags], 0x0001

    mov ax, runtime_service_table
    mov [es:bx + 0], ax
    mov ax, cs
    mov [es:bx + 2], ax
    mov word [es:bx + 4], 0x0001

    pop di
    pop es
    pop ds
    pop bx
    pop ax
    retf

runtime_service_table:
    db 'R', 'T', 'S', 'V'
    dw 0x0001
    dw 0x0008
    dw 0x0008
    dw 0x0001
    dw 0x0001
    dw runtime_identity_service
    dw 0x0000
    dw 0x0002
    dw 0x0001
    dw runtime_version_service
    dw 0x0000
    dw 0x0003
    dw 0x0001
    dw runtime_stage2_ready_service
    dw 0x0000
    dw 0x0004
    dw 0x0001
    dw runtime_dos_version_service
    dw 0x0000
    dw 0x0005
    dw 0x0001
    dw runtime_default_drive_service
    dw 0x0000
    dw 0x0006
    dw 0x0001
    dw runtime_get_state_ptr_service
    dw 0x0000
    dw 0x0007
    dw 0x0001
    dw runtime_prepare_child_dta_service
    dw 0x0000
    dw 0x0008
    dw 0x0001
    dw runtime_restore_parent_dta_service
    dw 0x0000

runtime_identity_service:
    mov ax, 0x5254
    clc
    retf

runtime_version_service:
    push cs
    pop ds
    mov si, runtime_version
    clc
    retf

runtime_stage2_ready_service:
    push cs
    pop ds
    mov si, runtime_stage2_ready_marker
    clc
    retf

runtime_dos_version_service:
    mov ax, 0x0005
    xor bx, bx
    xor cx, cx
    clc
    retf

runtime_default_drive_service:
    push cs
    pop ds
    mov si, runtime_state_default_drive
    clc
    retf

runtime_get_state_ptr_service:
    push cs
    pop ds
    mov si, runtime_state_signature
    clc
    retf

runtime_prepare_child_dta_service:
    mov [runtime_state_saved_parent_dta_seg], ax
    mov [runtime_state_saved_parent_dta_off], bx
    mov [runtime_state_parent_psp], cx
    mov [runtime_state_previous_psp], cx
    mov [runtime_state_current_psp], dx
    mov [runtime_state_dta_seg], dx
    mov word [runtime_state_dta_off], 0x0080
    clc
    retf

runtime_restore_parent_dta_service:
    mov ax, [runtime_state_saved_parent_dta_seg]
    or ax, ax
    jz .fail
    mov dx, [runtime_state_saved_parent_dta_off]
    mov [runtime_state_dta_seg], ax
    mov [runtime_state_dta_off], dx
    mov bx, [runtime_state_parent_psp]
    mov [runtime_state_current_psp], bx
    xor bx, bx
    mov [runtime_state_saved_parent_dta_seg], bx
    mov [runtime_state_saved_parent_dta_off], bx
    clc
    retf

.fail:
    stc
    retf
