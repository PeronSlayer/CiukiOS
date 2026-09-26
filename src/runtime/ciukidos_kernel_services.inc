; CIUKIDOS kernel ABI/state services.
;
; Included by the full-profile kernel build of floppy_stage1.asm.  These are
; operations over kernel-owned state, not a forwarding layer to a previous
; Stage1 DOS handler.

; Keep the kernel-owned PSP/DTA stack aligned with the loader's EXEC snapshot
; capacity.  Windows Standard Mode reaches six live frames when it is started
; through the temporary audio wrapper (WIN -> WINCORE -> WSWAP -> DOSX ->
; KRNL386); the old limit of five falsely returned DOS error 8.
%define KERNEL_PROCESS_STACK_MAX DOS_EXEC_STATE_FRAME_MAX
%define KERNEL_PROCESS_FRAME_SIZE 12
%define KFRAME_DTA_SEG 0
%define KFRAME_DTA_OFF 2
%define KFRAME_CURRENT_PSP 4
%define KFRAME_PARENT_PSP 6
%define KFRAME_PREVIOUS_PSP 8
%define KFRAME_CHILD_PSP 10

kernel_runtime_initialize:
    push ax
    push bx
    push dx
    mov word [cs:kernel_runtime_state_flags], 0x0003
    mov word [cs:kernel_runtime_state_handoff_version], 0x0001
    mov al, [cs:boot_drive]
    mov [cs:kernel_runtime_state_boot_drive], al
    mov al, [cs:dos_default_drive]
    mov [cs:kernel_runtime_state_default_drive], al
    int 0x12
    mov [cs:kernel_runtime_state_mem_kb], ax
    mov word [cs:kernel_runtime_state_fat_spt], FAT_SPT
    mov word [cs:kernel_runtime_state_fat_heads], FAT_HEADS
    mov word [cs:kernel_runtime_state_fat_reserved], FAT_RESERVED_SECTORS
    mov byte [cs:kernel_runtime_state_fat_spc], FAT_SECTORS_PER_CLUSTER
    mov byte [cs:kernel_runtime_state_entry_flags], 1
    mov word [cs:kernel_runtime_state_table_off], runtime_service_table
    mov ax, cs
    mov [cs:kernel_runtime_state_table_seg], ax
    mov ax, [cs:current_psp_seg]
    mov [cs:kernel_runtime_state_current_psp], ax
    mov ax, [cs:dta_seg]
    mov [cs:kernel_runtime_state_dta_seg], ax
    mov ax, [cs:dta_off]
    mov [cs:kernel_runtime_state_dta_off], ax
    mov word [cs:kernel_runtime_state_kernel_caps], 0x003F
    mov byte [cs:kernel_runtime_process_depth], 0
    mov word [cs:kernel_runtime_process_last_popped_child], 0
    mov word [cs:kernel_runtime_exec_identity_psp], 0
    mov word [cs:kernel_runtime_last_term_psp], 0
    mov byte [cs:kernel_runtime_last_exit_code], 0
    mov byte [cs:kernel_runtime_last_term_type], 0
    pop dx
    pop bx
    pop ax
    clc
    ret

kernel_runtime_identity_service:
    mov ax, 0x5254
    clc
    retf

kernel_runtime_version_service:
    push cs
    pop ds
    mov si, kernel_runtime_version
    clc
    retf

kernel_runtime_stage2_ready_service:
    push cs
    pop ds
    mov si, kernel_runtime_stage2_ready_marker
    clc
    retf

kernel_runtime_dos_version_service:
    ; DOS API compatibility level (AL=major, AH=minor).
    mov ax, 0x0005
    xor bx, bx
    xor cx, cx
    clc
    retf

kernel_runtime_default_drive_service:
    push cs
    pop ds
    mov si, kernel_runtime_state_default_drive
    clc
    retf

kernel_runtime_get_state_ptr_service:
    push cs
    pop ds
    mov si, kernel_runtime_state_signature
    clc
    retf

; AX:BX parent DTA, CX parent PSP, DX child PSP.
kernel_runtime_prepare_child_dta_service:
    push ax
    push bx
    push cx
    push dx
    push si
    push di

    cmp byte [cs:kernel_runtime_process_depth], KERNEL_PROCESS_STACK_MAX
    jae .overflow
    mov word [cs:kernel_runtime_process_last_popped_child], 0
    mov word [cs:kernel_runtime_last_term_psp], 0
    mov di, ax
    xor ax, ax
    mov al, [cs:kernel_runtime_process_depth]
    mov si, ax
    shl si, 1
    add si, ax
    shl si, 1
    shl si, 1

    mov [cs:kernel_runtime_process_stack + si + KFRAME_DTA_SEG], di
    mov [cs:kernel_runtime_process_stack + si + KFRAME_DTA_OFF], bx
    mov [cs:kernel_runtime_process_stack + si + KFRAME_CURRENT_PSP], cx
    mov ax, [cs:kernel_runtime_state_parent_psp]
    mov [cs:kernel_runtime_process_stack + si + KFRAME_PARENT_PSP], ax
    mov ax, [cs:kernel_runtime_state_previous_psp]
    mov [cs:kernel_runtime_process_stack + si + KFRAME_PREVIOUS_PSP], ax
    mov [cs:kernel_runtime_process_stack + si + KFRAME_CHILD_PSP], dx
    inc byte [cs:kernel_runtime_process_depth]
    mov [cs:kernel_runtime_exec_identity_psp], dx

    mov [cs:kernel_runtime_state_saved_parent_dta_seg], di
    mov [cs:kernel_runtime_state_saved_parent_dta_off], bx
    mov [cs:kernel_runtime_state_parent_psp], cx
    mov [cs:kernel_runtime_state_previous_psp], cx
    mov [cs:kernel_runtime_state_current_psp], dx
    mov [cs:kernel_runtime_state_dta_seg], dx
    mov word [cs:kernel_runtime_state_dta_off], 0x0080

    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    retf
.overflow:
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    retf

; CX is the expected child PSP.  A duplicate pop of the most recently
; restored child succeeds without consuming another frame.
kernel_runtime_restore_parent_dta_service:
    push si
    push di
    mov di, cx
    or di, di
    jz .fail

    xor ax, ax
    mov al, [cs:kernel_runtime_process_depth]
    or ax, ax
    jz .idempotent_check
    dec ax
    mov si, ax
    shl si, 1
    add si, ax
    shl si, 1
    shl si, 1
    cmp [cs:kernel_runtime_process_stack + si + KFRAME_CHILD_PSP], di
    jne .idempotent_check

    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_DTA_SEG]
    mov [cs:kernel_runtime_state_dta_seg], ax
    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_DTA_OFF]
    mov [cs:kernel_runtime_state_dta_off], ax
    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_CURRENT_PSP]
    mov [cs:kernel_runtime_state_current_psp], ax
    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_PARENT_PSP]
    mov [cs:kernel_runtime_state_parent_psp], ax
    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_PREVIOUS_PSP]
    mov [cs:kernel_runtime_state_previous_psp], ax
    dec byte [cs:kernel_runtime_process_depth]
    mov [cs:kernel_runtime_process_last_popped_child], di

    xor ax, ax
    mov al, [cs:kernel_runtime_process_depth]
    or ax, ax
    jz .clear_saved_parent
    dec ax
    mov si, ax
    shl si, 1
    add si, ax
    shl si, 1
    shl si, 1
    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_DTA_SEG]
    mov [cs:kernel_runtime_state_saved_parent_dta_seg], ax
    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_DTA_OFF]
    mov [cs:kernel_runtime_state_saved_parent_dta_off], ax
    mov ax, [cs:kernel_runtime_process_stack + si + KFRAME_CHILD_PSP]
    mov [cs:kernel_runtime_exec_identity_psp], ax
    jmp .success
.clear_saved_parent:
    xor ax, ax
    mov [cs:kernel_runtime_state_saved_parent_dta_seg], ax
    mov [cs:kernel_runtime_state_saved_parent_dta_off], ax
    mov [cs:kernel_runtime_exec_identity_psp], ax
    jmp .success
.idempotent_check:
    cmp [cs:kernel_runtime_process_last_popped_child], di
    jne .fail
.success:
    mov ax, [cs:kernel_runtime_state_dta_seg]
    mov dx, [cs:kernel_runtime_state_dta_off]
    mov bx, [cs:kernel_runtime_state_current_psp]
    pop di
    pop si
    clc
    retf
.fail:
    pop di
    pop si
    stc
    retf

kernel_runtime_kernel_caps_service:
    mov ax, [cs:kernel_runtime_state_kernel_caps]
    ; A zero legacy vector is intentional: the full kernel owns the complete
    ; normal DOS path and has no Stage1 chain target.
    xor bx, bx
    xor dx, dx
    clc
    retf

; AX=DTA segment, BX=DTA offset, CX=current PSP, DX=parent PSP.
kernel_runtime_sync_process_state_service:
    mov [cs:kernel_runtime_state_dta_seg], ax
    mov [cs:kernel_runtime_state_dta_off], bx
    mov [cs:kernel_runtime_state_current_psp], cx
    mov [cs:kernel_runtime_state_parent_psp], dx
    clc
    retf

; AL=exit code, AH=termination type, CX=expected current PSP.
kernel_runtime_record_termination_service:
    cmp cx, [cs:kernel_runtime_exec_identity_psp]
    jne .fail
    or cx, cx
    jz .fail
    cmp cx, [cs:kernel_runtime_last_term_psp]
    je .fail
    mov [cs:kernel_runtime_last_exit_code], al
    mov [cs:kernel_runtime_last_term_type], ah
    mov [cs:kernel_runtime_last_term_psp], cx
    clc
    retf
.fail:
    stc
    retf

kernel_entry_default_drive db 0
kernel_runtime_version db 'CIUKIDOS kernel v0.7.1', 0
kernel_runtime_stage2_ready_marker db '[S2] ready', 13, 10, 0

; Keep this structure byte-for-byte compatible with the legacy CDOSSTAT
; layout retained by ABI2.
; layout consumed by CIUKRTST/PSTACK.  In particular CURRENT_PSP remains at
; offset 35 and the DTA fields at offsets 41/43.
kernel_runtime_state_signature db 'CDOSSTAT'
kernel_runtime_state_version db 0, 6, 7
kernel_runtime_state_flags dw 0
kernel_runtime_state_handoff_version dw 0
kernel_runtime_state_boot_drive db 0
kernel_runtime_state_default_drive db 0
kernel_runtime_state_mem_kb dw 0
kernel_runtime_state_fat_spt dw 0
kernel_runtime_state_fat_heads dw 0
kernel_runtime_state_fat_reserved dw 0
kernel_runtime_state_fat_spc db 0
kernel_runtime_state_entry_flags db 0
kernel_runtime_state_table_off dw 0
kernel_runtime_state_table_seg dw 0
kernel_runtime_state_buffer_seg dw 0
kernel_runtime_state_buffer_size dw 0
kernel_runtime_state_current_psp dw 0
kernel_runtime_state_parent_psp dw 0
kernel_runtime_state_previous_psp dw 0
kernel_runtime_state_dta_seg dw 0
kernel_runtime_state_dta_off dw 0
kernel_runtime_state_saved_parent_dta_seg dw 0
kernel_runtime_state_saved_parent_dta_off dw 0
kernel_runtime_state_jft_seg dw 0
kernel_runtime_state_jft_off dw 0
kernel_runtime_state_jft_count dw 0
kernel_runtime_state_std_handle0 dw 0
kernel_runtime_state_std_handle1 dw 1
kernel_runtime_state_std_handle2 dw 2
kernel_runtime_state_handle_table_seg dw 0
kernel_runtime_state_handle_table_off dw 0
kernel_runtime_state_sft_seg dw 0
kernel_runtime_state_sft_off dw 0
kernel_runtime_state_scratch_seg dw 0
kernel_runtime_state_scratch_off dw 0
kernel_runtime_state_kernel_caps dw 0

kernel_runtime_process_depth db 0
kernel_runtime_process_last_popped_child dw 0
kernel_runtime_exec_identity_psp dw 0
kernel_runtime_last_exit_code db 0
kernel_runtime_last_term_type db 0
kernel_runtime_last_term_psp dw 0
kernel_runtime_process_stack times (KERNEL_PROCESS_STACK_MAX * KERNEL_PROCESS_FRAME_SIZE) db 0
