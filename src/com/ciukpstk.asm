; ciukpstk.asm - nested CIUKIDOS process-state ownership probe
;
; Topology exercised by this COM program:
;   SHELL.COM (parent) -> CIUKPST.COM (this child)
;                                      -> CIUKTRM.EXE (MZ grandchild)
;                                      -> CIUKPCOM.COM (COM grandchild)
;
; The grandchild is run through INT 21h/AH=4Bh four times and terminates via
; AH=4Ch, AH=00h, INT 20h and AH=31h.  After every return this program checks
; the live PSP/DTA view, AH=4Dh status, the preserved outer CIUKIDOS frame,
; service-8 expected-child/idempotent-pop semantics, and true TSR residency
; across a subsequent EXEC followed by unload and deterministic double-free.

bits 16
org 0x0100

%ifndef CIUKIDOS_RUNTIME_SEG
%define CIUKIDOS_RUNTIME_SEG 0x0900
%endif
%ifndef CIUKIDOS_ABI_VERSION
%define CIUKIDOS_ABI_VERSION 2
%endif

%define RUNTIME_SCAN_BYTES 3584
%define RTSV_HEADER_SIZE 10
%define RTSV_DESCRIPTOR_SIZE 8
%define RTSV_SERVICE_COUNT 11
%define RUNTIME_SCAN_CANDIDATES (RUNTIME_SCAN_BYTES - (RTSV_HEADER_SIZE + RTSV_SERVICE_COUNT * RTSV_DESCRIPTOR_SIZE) + 1)

; Offsets relative to the CDOSSTAT signature returned by service 6.
%define CDOSSTATE_CURRENT_PSP 35
%define CDOSSTATE_PARENT_PSP 37
%define CDOSSTATE_PREVIOUS_PSP 39
%define CDOSSTATE_DTA_SEG 41
%define CDOSSTATE_DTA_OFF 43
%define CDOSSTATE_SAVED_PARENT_DTA_SEG 45
%define CDOSSTATE_SAVED_PARENT_DTA_OFF 47

%define MODE_4C 0x4334
%define MODE_00 0x3030
%define MODE_20 0x3032
%define MODE_31 0x3133

%define MAILBOX_MAGIC 0x5350
%define MAILBOX_CHECKS_ALL 0x01FF
%define COM_MAILBOX_MAGIC 0x4343
%define COM_MAILBOX_CHECKS_ALL 0x03FF
%define TSR_PSP_SENTINEL_0 0x5354
%define TSR_PSP_SENTINEL_1 0x3152
%define TSR_IMAGE_SENTINEL_0 0x4954
%define TSR_IMAGE_SENTINEL_1 0x314D

start:
    cld
    push cs
    pop ds

    ; DOS gives a COM process the complete available arena.  A COM program
    ; must move its stack into the retained block and shrink with AH=4Ah
    ; before it can legally EXEC another child.  Keep this probe honest now
    ; that CIUKIDOS exposes a real MCB chain instead of fixed child slots.
    cli
    mov ax, cs
    mov ss, ax
    mov sp, probe_stack_top
    sti
    mov es, ax
    mov bx, (probe_stack_top - $$ + 0x100 + 15) / 16
    mov ah, 0x4A
    int 0x21
    jc fail_resize

    call locate_runtime_services
    jc fail_services
    call capture_entry_state
    jc fail_entry_state
    call command_mode
    cmp al, 1
    je root_check
    or al, al
    jnz fail_argument

    mov dx, msg_begin
    call print_dollar

    push cs
    pop ds
    mov dx, parent_dta
    mov ah, 0x1A
    int 0x21
    jc fail_set_dta
    call check_parent_view
    jc fail_set_dta
    call install_parent_vectors
    jc fail_vectors

    mov word [current_mode], MODE_4C
    mov byte [expected_exit_code], 0x41
    mov byte [expected_term_type], 0x00
    call run_one_mode
    mov dx, msg_mode_4c_pass
    call print_dollar

    mov word [current_mode], MODE_00
    mov byte [expected_exit_code], 0x00
    mov byte [expected_term_type], 0x00
    call run_one_mode
    mov dx, msg_mode_00_pass
    call print_dollar

    mov word [current_mode], MODE_20
    mov byte [expected_exit_code], 0x00
    mov byte [expected_term_type], 0x00
    call run_one_mode
    mov dx, msg_mode_20_pass
    call print_dollar

    ; Exercise a true COM -> COM nested load before the resident AH=31h lane.
    ; The grandchild changes its DTA and exits with 6Ch; this parent
    ; must remain executable at the same PSP with all outer state restored.
    call run_com_grandchild
    mov dx, msg_com_pass
    call print_dollar

    ; AH=31h is deliberately last.  In addition to termination type and parent
    ; restoration, run_one_mode proves that the retained PSP/image survives a
    ; later EXEC and can be unloaded exactly once with AH=49h.
    mov word [current_mode], MODE_31
    mov byte [expected_exit_code], 0x73
    mov byte [expected_term_type], 0x03
    call run_one_mode
    mov dx, msg_mode_31_pass
    call print_dollar

    mov dx, msg_all_pass
    call print_dollar
    mov ax, 0x4C5A
    int 0x21

root_check:
    ; The immediately preceding main probe exits with 5Ah.  Query before any
    ; nested EXEC so this proves both the outer pop and AH=4Dh persistence.
    mov ah, 0x4D
    int 0x21
    cmp al, 0x5A
    jne fail_root_status
    cmp ah, 0x00
    jne fail_root_status

    ; capture_entry_state already proved that the DTA saved for our parent is
    ; in the SHELL.COM PSP segment.  A lost outer frame leaves the previous
    ; CIUKPST.COM DTA segment here instead.
    mov ax, [outer_saved_dta_seg]
    cmp ax, [parent_psp]
    jne fail_root_frame
    mov dx, msg_root_pass
    call print_dollar
    mov dx, msg_root_all_pass
    call print_dollar
    mov ax, 0x4C00
    int 0x21

; Return AL=0 for the main mode, AL=1 for /ROOT, AL=FFh otherwise.
command_mode:
    xor ax, ax
    mov cl, [cs:0x0080]
    xor ch, ch
    jcxz .main
    mov si, 0x0081
.skip_spaces:
    jcxz .main
    cmp byte [cs:si], ' '
    jne .token
    inc si
    dec cx
    jmp .skip_spaces
.token:
    cmp cx, 5
    jne .invalid
    cmp byte [cs:si], '/'
    jne .invalid
    inc si
    mov al, [cs:si]
    and al, 0xDF
    cmp al, 'R'
    jne .invalid
    inc si
    mov al, [cs:si]
    and al, 0xDF
    cmp al, 'O'
    jne .invalid
    inc si
    mov al, [cs:si]
    and al, 0xDF
    cmp al, 'O'
    jne .invalid
    inc si
    mov al, [cs:si]
    and al, 0xDF
    cmp al, 'T'
    jne .invalid
    mov al, 1
    ret
.main:
    xor al, al
    ret
.invalid:
    mov al, 0xFF
    ret

locate_runtime_services:
    mov ax, 0x0006
    call find_runtime_service
    jc .fail
    mov ax, [found_service_ptr]
    mov [service6_ptr], ax
    mov ax, [found_service_ptr + 2]
    mov [service6_ptr + 2], ax

    mov ax, 0x0008
    call find_runtime_service
    jc .fail
    mov ax, [found_service_ptr]
    mov [service8_ptr], ax
    mov ax, [found_service_ptr + 2]
    mov [service8_ptr + 2], ax
    clc
    ret
.fail:
    stc
    ret

; Input AX=service id.  Output is stored in found_service_ptr.
find_runtime_service:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es
    mov dx, ax
    mov ax, CIUKIDOS_RUNTIME_SEG
    mov es, ax
    xor di, di
    mov cx, RUNTIME_SCAN_CANDIDATES
.scan:
    cmp word [es:di], 0x5452       ; "RT"
    jne .next
    cmp word [es:di + 2], 0x5653   ; "SV"
    jne .next
    cmp word [es:di + 4], CIUKIDOS_ABI_VERSION
    jne .next
    mov bx, [es:di + 6]
    cmp bx, RTSV_SERVICE_COUNT
    jne .not_found
    mov si, [es:di + 8]
    cmp si, RTSV_DESCRIPTOR_SIZE
    jne .not_found
    add di, RTSV_HEADER_SIZE
.descriptor:
    cmp word [es:di], dx
    je .candidate
    add di, si
    dec bx
    jnz .descriptor
    jmp .not_found
.candidate:
    test word [es:di + 2], 1
    jz .not_found
    mov ax, [es:di + 4]
    or ax, ax
    jz .not_found
    mov [cs:found_service_ptr], ax
    mov ax, CIUKIDOS_RUNTIME_SEG
    add ax, [es:di + 6]
    mov [cs:found_service_ptr + 2], ax
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret
.next:
    inc di
    loop .scan
.not_found:
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    stc
    ret

capture_entry_state:
    call far [cs:service6_ptr]
    jc .fail
    mov ax, ds
    cmp ax, CIUKIDOS_RUNTIME_SEG
    jne .restore_fail
    or si, si
    jz .restore_fail
    mov [cs:state_seg], ax
    mov [cs:state_off], si
    push cs
    pop ds

    mov ah, 0x51
    int 0x21
    mov ax, cs
    cmp bx, ax
    jne .fail
    mov [self_psp], bx
    mov ah, 0x62
    int 0x21
    cmp bx, [self_psp]
    jne .fail

    mov ax, [cs:0x0016]
    or ax, ax
    jz .fail
    mov [parent_psp], ax

    mov ah, 0x2F
    int 0x21
    mov ax, es
    mov dx, cs
    cmp ax, dx
    jne .fail
    cmp bx, 0x0080
    jne .fail

    mov ax, [state_seg]
    mov es, ax
    mov di, [state_off]
    cmp word [es:di], 0x4443       ; "CD"
    jne .fail
    cmp word [es:di + 2], 0x534F   ; "OS"
    jne .fail
    cmp word [es:di + 4], 0x5453   ; "ST"
    jne .fail
    cmp word [es:di + 6], 0x5441   ; "AT"
    jne .fail
    mov ax, cs
    cmp [es:di + CDOSSTATE_CURRENT_PSP], ax
    jne .fail
    mov ax, [parent_psp]
    cmp [es:di + CDOSSTATE_PARENT_PSP], ax
    jne .fail
    mov ax, cs
    cmp [es:di + CDOSSTATE_DTA_SEG], ax
    jne .fail
    cmp word [es:di + CDOSSTATE_DTA_OFF], 0x0080
    jne .fail
    mov ax, [parent_psp]
    cmp [es:di + CDOSSTATE_SAVED_PARENT_DTA_SEG], ax
    jne .fail

    mov ax, [es:di + CDOSSTATE_PARENT_PSP]
    mov [outer_parent_psp], ax
    mov ax, [es:di + CDOSSTATE_PREVIOUS_PSP]
    mov [outer_previous_psp], ax
    mov ax, [es:di + CDOSSTATE_SAVED_PARENT_DTA_SEG]
    mov [outer_saved_dta_seg], ax
    mov ax, [es:di + CDOSSTATE_SAVED_PARENT_DTA_OFF]
    mov [outer_saved_dta_off], ax
    push cs
    pop es
    clc
    ret

.restore_fail:
    push cs
    pop ds
.fail:
    push cs
    pop ds
    push cs
    pop es
    stc
    ret

run_one_mode:
    call prepare_exec_request

    push cs
    pop ds
    push cs
    pop es
    mov dx, grandchild_path
    mov bx, exec_block
    mov ax, 0x4B00
    int 0x21
    jc fail_exec
    push cs
    pop ds

    cmp word [mailbox_magic], MAILBOX_MAGIC
    jne fail_mailbox
    mov ax, [mailbox_mode]
    cmp ax, [current_mode]
    jne fail_mailbox
    cmp word [mailbox_checks], MAILBOX_CHECKS_ALL
    jne fail_mailbox
    mov ax, [mailbox_parent_psp]
    cmp ax, [self_psp]
    jne fail_mailbox
    mov ax, [mailbox_grand_psp]
    or ax, ax
    jz fail_mailbox
    cmp ax, [self_psp]
    je fail_mailbox

    call check_parent_view
    jc fail_parent_restore
    call check_parent_vectors
    jc fail_vectors

    mov ah, 0x4D
    int 0x21
    cmp al, [expected_exit_code]
    jne fail_return_status
    cmp ah, [expected_term_type]
    jne fail_return_status

    ; The loader already performed the real pop and its after-call duplicate.
    ; A third pop for the same child must be a successful state-preserving no-op.
    mov cx, [mailbox_grand_psp]
    call far [cs:service8_ptr]
    jc fail_idempotent_pop
    mov si, cs
    cmp ax, si
    jne fail_idempotent_pop
    cmp dx, parent_dta
    jne fail_idempotent_pop
    cmp bx, [self_psp]
    jne fail_idempotent_pop
    call check_parent_view
    jc fail_idempotent_pop

    ; FFFFh can never be a valid conventional-memory PSP in this lane.
    mov cx, 0xFFFF
    call far [cs:service8_ptr]
    jnc fail_mismatch_pop
    call check_parent_view
    jc fail_mismatch_pop

    cmp word [current_mode], MODE_31
    jne .done

    ; The block must be visible immediately after AH=31h, remain intact across
    ; another real EXEC/return, and then support an exact resident unload.
    call check_tsr_resident
    jc fail_tsr_residency
    call run_com_grandchild
    call check_tsr_resident
    jc fail_tsr_exec_survival

    mov ax, [mailbox_grand_psp]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jc fail_tsr_unload
    call check_tsr_unloaded
    jc fail_tsr_unload

    ; A second free of the same resident PSP must be rejected as DOS error 9.
    mov ax, [mailbox_grand_psp]
    mov es, ax
    mov ah, 0x49
    int 0x21
    jnc fail_tsr_double_free
    cmp ax, 0x0009
    jne fail_tsr_double_free
    push cs
    pop es
.done:
    ret

; Verify the retained PSP MCB, PSP sentinel and an image-resident sentinel.
; Output CF=0 only when the complete resident identity is still intact.
check_tsr_resident:
    push ax
    push bx
    push cx
    push dx
    push si
    push es

    mov dx, [mailbox_grand_psp]
    or dx, dx
    jz .fail
    mov bx, [mailbox_tsr_keep]
    cmp bx, 0x0010
    jbe .fail

    mov es, dx
    cmp word [es:0x0080], TSR_PSP_SENTINEL_0
    jne .fail
    cmp word [es:0x0082], TSR_PSP_SENTINEL_1
    jne .fail
    mov ax, dx
    add ax, bx
    cmp [es:0x0002], ax
    jne .fail
    mov ax, [self_psp]
    cmp [es:0x0016], ax
    jne .fail

    mov si, [mailbox_tsr_sentinel_off]
    mov ax, bx
    sub ax, 0x0010
    mov cl, 4
    shl ax, cl
    mov cx, si
    add cx, 4
    jc .fail
    cmp cx, ax
    ja .fail
    mov ax, dx
    add ax, 0x0010
    mov es, ax
    cmp word [es:si], TSR_IMAGE_SENTINEL_0
    jne .fail
    cmp word [es:si + 2], TSR_IMAGE_SENTINEL_1
    jne .fail

    mov ax, dx
    dec ax
    mov es, ax
    cmp byte [es:0x0000], 'M'
    je .mcb_type_ok
    cmp byte [es:0x0000], 'Z'
    jne .fail
.mcb_type_ok:
    cmp [es:0x0001], dx
    jne .fail
    cmp [es:0x0003], bx
    jne .fail
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret

; Walk the public DOS MCB chain after AH=49h.  The former child PSP must no
; longer own any live chain entry; stale bytes inside a coalesced gap do not
; count because only linked MCBs are inspected.
check_tsr_unloaded:
    push ax
    push bx
    push cx
    push dx
    push es

    mov ah, 0x52
    int 0x21
    mov dx, [es:bx - 2]
    mov cx, 256
.scan:
    or dx, dx
    jz .fail
    mov es, dx
    mov al, [es:0x0000]
    cmp al, 'M'
    je .type_ok
    cmp al, 'Z'
    jne .fail
.type_ok:
    mov bl, al
    mov ax, [mailbox_grand_psp]
    cmp [es:0x0001], ax
    je .fail
    cmp bl, 'Z'
    je .ok
    mov ax, [es:0x0003]
    inc ax
    jz .fail
    add ax, dx
    jc .fail
    cmp ax, dx
    jbe .fail
    mov dx, ax
    loop .scan
.fail:
    stc
    jmp .done
.ok:
    clc
.done:
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    ret

run_com_grandchild:
    cmp word [com_parent_entry_guard], 0x5043
    jne fail_com_entry
    cmp word [com_parent_entry_guard + 2], 0x3150
    jne fail_com_entry
    call prepare_com_exec_request

    push cs
    pop ds
    push cs
    pop es
    mov dx, com_grandchild_path
    mov bx, exec_block
    mov ax, 0x4B00
    int 0x21
    jc fail_com_exec
    push cs
    pop ds

    ; A fixed-slot COM collision overwrites this parent's image.  Reaching
    ; this continuation is not enough: require an explicit intact sentinel.
    cmp word [com_parent_entry_guard], 0x5043
    jne fail_com_entry
    cmp word [com_parent_entry_guard + 2], 0x3150
    jne fail_com_entry

    cmp word [com_mailbox_magic], COM_MAILBOX_MAGIC
    jne fail_com_mailbox
    cmp word [com_mailbox_checks], COM_MAILBOX_CHECKS_ALL
    jne fail_com_mailbox
    mov ax, [com_mailbox_parent_psp]
    cmp ax, [self_psp]
    jne fail_com_mailbox
    mov ax, [com_mailbox_grand_psp]
    or ax, ax
    jz fail_com_mailbox
    cmp ax, [self_psp]
    je fail_com_mailbox

    call check_parent_view
    jc fail_com_restore
    call check_parent_vectors
    jc fail_vectors

    mov ah, 0x4D
    int 0x21
    cmp al, 0x6C
    jne fail_com_status
    cmp ah, 0x00
    jne fail_com_status

    ; As for the MZ lane, the loader's duplicate service-8 pop must be an
    ; idempotent success and leave this COM parent's live state unchanged.
    mov cx, [com_mailbox_grand_psp]
    call far [cs:service8_ptr]
    jc fail_com_pop
    mov si, cs
    cmp ax, si
    jne fail_com_pop
    cmp dx, parent_dta
    jne fail_com_pop
    cmp bx, [self_psp]
    jne fail_com_pop
    call check_parent_view
    jc fail_com_pop
    ret

prepare_com_exec_request:
    push ax
    push cx
    push di
    push es
    push cs
    pop es
    mov di, com_mailbox_magic
    xor ax, ax
    mov cx, (com_mailbox_end - com_mailbox_magic) / 2
    rep stosw

    mov ax, cs
    mov di, com_exec_tail + 2
    call write_hex_word
    mov ax, com_mailbox_magic
    mov di, com_exec_tail + 7
    call write_hex_word

    mov word [exec_env_seg], 0
    mov word [exec_tail_off], com_exec_tail
    mov word [exec_tail_seg], cs
    mov word [exec_fcb1_off], 0x005C
    mov ax, [self_psp]
    mov [exec_fcb1_seg], ax
    mov word [exec_fcb2_off], 0x006C
    mov [exec_fcb2_seg], ax
    pop es
    pop di
    pop cx
    pop ax
    ret

prepare_exec_request:
    push ax
    push bx
    push cx
    push di
    push es
    push cs
    pop es
    mov di, mailbox_magic
    xor ax, ax
    mov cx, (mailbox_end - mailbox_magic) / 2
    rep stosw

    mov ax, [current_mode]
    mov [exec_tail + 3], ax
    mov ax, cs
    mov di, exec_tail + 6
    call write_hex_word
    mov ax, mailbox_magic
    mov di, exec_tail + 11
    call write_hex_word

    mov word [exec_env_seg], 0
    mov word [exec_tail_off], exec_tail
    mov word [exec_tail_seg], cs
    mov word [exec_fcb1_off], 0x005C
    mov ax, [self_psp]
    mov [exec_fcb1_seg], ax
    mov word [exec_fcb2_off], 0x006C
    mov [exec_fcb2_seg], ax
    pop es
    pop di
    pop cx
    pop bx
    pop ax
    ret

install_parent_vectors:
    push ax
    push dx
    push ds
    push cs
    pop ds
    mov dx, parent_int23
    mov ax, 0x2523
    int 0x21
    jc .fail
    mov dx, parent_int24
    mov ax, 0x2524
    int 0x21
    jc .fail
    call check_parent_vectors
    jmp .done
.fail:
    stc
.done:
    pop ds
    pop dx
    pop ax
    ret

check_parent_vectors:
    push ax
    push bx
    push dx
    push es
    mov ax, 0x3523
    int 0x21
    mov dx, es
    mov ax, cs
    cmp dx, ax
    jne .fail
    cmp bx, parent_int23
    jne .fail
    mov ax, 0x3524
    int 0x21
    mov dx, es
    mov ax, cs
    cmp dx, ax
    jne .fail
    cmp bx, parent_int24
    jne .fail
    clc
    jmp .done
.fail:
    stc
.done:
    pop es
    pop dx
    pop bx
    pop ax
    ret

parent_int23:
    iret

parent_int24:
    mov al, 0x03
    iret

write_hex_word:
    push ax
    push bx
    push cx
    mov bx, ax
    mov cx, 4
.digit:
    rol bx, 1
    rol bx, 1
    rol bx, 1
    rol bx, 1
    mov al, bl
    and al, 0x0F
    cmp al, 9
    jbe .decimal
    add al, 'A' - 10
    jmp .store
.decimal:
    add al, '0'
.store:
    mov [cs:di], al
    inc di
    loop .digit
    pop cx
    pop bx
    pop ax
    ret

check_parent_view:
    mov ah, 0x51
    int 0x21
    cmp bx, [cs:self_psp]
    jne .fail
    mov ah, 0x62
    int 0x21
    cmp bx, [cs:self_psp]
    jne .fail
    mov ah, 0x2F
    int 0x21
    mov ax, es
    mov dx, cs
    cmp ax, dx
    jne .fail
    cmp bx, parent_dta
    jne .fail

    mov ax, [cs:state_seg]
    mov es, ax
    mov di, [cs:state_off]
    mov ax, [cs:self_psp]
    cmp [es:di + CDOSSTATE_CURRENT_PSP], ax
    jne .fail
    mov ax, [cs:outer_parent_psp]
    cmp [es:di + CDOSSTATE_PARENT_PSP], ax
    jne .fail
    mov ax, [cs:outer_previous_psp]
    cmp [es:di + CDOSSTATE_PREVIOUS_PSP], ax
    jne .fail
    mov ax, cs
    cmp [es:di + CDOSSTATE_DTA_SEG], ax
    jne .fail
    cmp word [es:di + CDOSSTATE_DTA_OFF], parent_dta
    jne .fail
    mov ax, [cs:outer_saved_dta_seg]
    cmp [es:di + CDOSSTATE_SAVED_PARENT_DTA_SEG], ax
    jne .fail
    mov ax, [cs:outer_saved_dta_off]
    cmp [es:di + CDOSSTATE_SAVED_PARENT_DTA_OFF], ax
    jne .fail
    push cs
    pop ds
    push cs
    pop es
    clc
    ret
.fail:
    push cs
    pop ds
    push cs
    pop es
    stc
    ret

print_dollar:
    push ax
    push ds
    push cs
    pop ds
    mov ah, 0x09
    int 0x21
    pop ds
    pop ax
    ret

fail_services:
    mov dx, msg_fail_services
    jmp fail
fail_entry_state:
    mov dx, msg_fail_entry
    jmp fail
fail_argument:
    mov dx, msg_fail_argument
    jmp fail
fail_resize:
    push cs
    pop ds
    mov dx, msg_fail_resize
    jmp fail
fail_set_dta:
    mov dx, msg_fail_set_dta
    jmp fail
fail_exec:
    mov dx, msg_fail_exec
    jmp fail
fail_com_exec:
    mov dx, msg_fail_com_exec
    jmp fail
fail_com_mailbox:
    mov dx, msg_fail_com_mailbox
    jmp fail
fail_com_restore:
    mov dx, msg_fail_com_restore
    jmp fail
fail_com_status:
    mov dx, msg_fail_com_status
    jmp fail
fail_com_pop:
    mov dx, msg_fail_com_pop
    jmp fail
fail_com_entry:
    mov dx, msg_fail_com_entry
    jmp fail
fail_mailbox:
    mov dx, msg_fail_mailbox
    jmp fail
fail_parent_restore:
    mov dx, msg_fail_restore
    jmp fail
fail_return_status:
    mov dx, msg_fail_status
    jmp fail
fail_idempotent_pop:
    mov dx, msg_fail_idempotent
    jmp fail
fail_mismatch_pop:
    mov dx, msg_fail_mismatch
    jmp fail
fail_vectors:
    mov dx, msg_fail_vectors
    jmp fail
fail_tsr_residency:
    mov dx, msg_fail_tsr_residency
    jmp fail
fail_tsr_exec_survival:
    mov dx, msg_fail_tsr_exec_survival
    jmp fail
fail_tsr_unload:
    mov dx, msg_fail_tsr_unload
    jmp fail
fail_tsr_double_free:
    mov dx, msg_fail_tsr_double_free
    jmp fail
fail_root_status:
    mov dx, msg_fail_root_status
    jmp fail
fail_root_frame:
    mov dx, msg_fail_root_frame

fail:
    call print_dollar
    mov ax, 0x4CFF
    int 0x21

grandchild_path db '\APPS\CIUKTRM.EXE', 0
com_grandchild_path db '\APPS\CIUKPCOM.COM', 0

; DOS EXEC parameter block.
exec_block:
exec_env_seg  dw 0
exec_tail_off dw exec_tail
exec_tail_seg dw 0
exec_fcb1_off dw 0x005C
exec_fcb1_seg dw 0
exec_fcb2_off dw 0x006C
exec_fcb2_seg dw 0

; Length 14: " /MM PPPP OOOO" followed by CR.
exec_tail db 14, ' ', '/', '4', 'C', ' ', '0', '0', '0', '0'
          db ' ', '0', '0', '0', '0', 0x0D

; Length 10: " PPPP OOOO" followed by CR.
com_exec_tail db 10, ' ', '0', '0', '0', '0'
              db ' ', '0', '0', '0', '0', 0x0D

found_service_ptr dw 0, 0
service6_ptr dw 0, 0
service8_ptr dw 0, 0
state_seg dw 0
state_off dw 0
self_psp dw 0
parent_psp dw 0
outer_parent_psp dw 0
outer_previous_psp dw 0
outer_saved_dta_seg dw 0
outer_saved_dta_off dw 0
current_mode dw 0
expected_exit_code db 0
expected_term_type db 0

mailbox_magic dw 0
mailbox_grand_psp dw 0
mailbox_parent_psp dw 0
mailbox_mode dw 0
mailbox_checks dw 0
mailbox_tsr_keep dw 0
mailbox_tsr_sentinel_off dw 0
mailbox_end:

com_mailbox_magic dw 0
com_mailbox_grand_psp dw 0
com_mailbox_parent_psp dw 0
com_mailbox_checks dw 0
com_mailbox_end:

; Sentinel in the resident CIUKPST image, checked across nested COM EXEC.
com_parent_entry_guard dw 0x5043, 0x3150

parent_dta times 128 db 0

msg_begin db '[PSTACK:C] BEGIN', 13, 10, '$'
msg_mode_4c_pass db '[PSTACK:C] MODE=4C RESTORE=PASS VECTORS=PASS AH4D=41/00 POP=IDEMPOTENT MISMATCH=REJECTED', 13, 10, '$'
msg_mode_00_pass db '[PSTACK:C] MODE=00 RESTORE=PASS VECTORS=PASS AH4D=00/00 POP=IDEMPOTENT MISMATCH=REJECTED', 13, 10, '$'
msg_mode_20_pass db '[PSTACK:C] MODE=20 RESTORE=PASS VECTORS=PASS AH4D=00/00 POP=IDEMPOTENT MISMATCH=REJECTED', 13, 10, '$'
; Keep gate markers below the 80-column console wrap boundary.  The code path
; above still verifies vectors and the idempotent process-stack pop before it
; can emit this marker.
msg_com_pass db '[PSTACK:C] COM2COM=PASS ENTRY=PASS RESTORE=PASS AH4D=6C/00', 13, 10, '$'
msg_mode_31_pass db '[PSTACK:C] MODE=31 RESIDENCY=PASS EXEC-SURVIVE=PASS UNLOAD=PASS', 13, 10, '$'
msg_all_pass db '[PSTACK:C] ALL=PASS TSR-EXEC-UNLOAD=PASS EXIT=5A', 13, 10, '$'
msg_root_pass db '[PSTACK:R] SHELL_RESTORE=PASS AH4D=5A/00', 13, 10, '$'
msg_root_all_pass db '[PSTACK:R] ALL=PASS', 13, 10, '$'

msg_fail_services db '[PSTACK] FAIL C_SERVICES', 13, 10, '$'
msg_fail_entry db '[PSTACK] FAIL C_ENTRY_STATE', 13, 10, '$'
msg_fail_argument db '[PSTACK] FAIL C_ARGUMENT', 13, 10, '$'
msg_fail_resize db '[PSTACK] FAIL C_RESIZE', 13, 10, '$'
msg_fail_set_dta db '[PSTACK] FAIL C_SET_DTA', 13, 10, '$'
msg_fail_exec db '[PSTACK] FAIL C_EXEC', 13, 10, '$'
msg_fail_com_exec db '[PSTACK] FAIL C_COM_EXEC', 13, 10, '$'
msg_fail_com_mailbox db '[PSTACK] FAIL C_COM_MAILBOX', 13, 10, '$'
msg_fail_com_restore db '[PSTACK] FAIL C_COM_PARENT_RESTORE', 13, 10, '$'
msg_fail_com_status db '[PSTACK] FAIL C_COM_AH4D', 13, 10, '$'
msg_fail_com_pop db '[PSTACK] FAIL C_COM_POP_IDEMPOTENT', 13, 10, '$'
msg_fail_com_entry db '[PSTACK] FAIL C_COM_PARENT_ENTRY', 13, 10, '$'
msg_fail_mailbox db '[PSTACK] FAIL C_MAILBOX', 13, 10, '$'
msg_fail_restore db '[PSTACK] FAIL C_PARENT_RESTORE', 13, 10, '$'
msg_fail_status db '[PSTACK] FAIL C_AH4D', 13, 10, '$'
msg_fail_idempotent db '[PSTACK] FAIL C_POP_IDEMPOTENT', 13, 10, '$'
msg_fail_mismatch db '[PSTACK] FAIL C_POP_MISMATCH', 13, 10, '$'
msg_fail_vectors db '[PSTACK] FAIL C_VECTOR_RESTORE', 13, 10, '$'
msg_fail_tsr_residency db '[PSTACK] FAIL C_TSR_RESIDENCY', 13, 10, '$'
msg_fail_tsr_exec_survival db '[PSTACK] FAIL C_TSR_EXEC_SURVIVAL', 13, 10, '$'
msg_fail_tsr_unload db '[PSTACK] FAIL C_TSR_UNLOAD', 13, 10, '$'
msg_fail_tsr_double_free db '[PSTACK] FAIL C_TSR_DOUBLE_FREE', 13, 10, '$'
msg_fail_root_status db '[PSTACK] FAIL R_AH4D', 13, 10, '$'
msg_fail_root_frame db '[PSTACK] FAIL R_SHELL_FRAME', 13, 10, '$'

align 16, db 0
probe_stack times 1024 db 0
probe_stack_top:
