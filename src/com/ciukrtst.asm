; ciukrtst.asm - black-box CIUKIDOS ownership probe
;
; Proves that the live INT 21h vector belongs to CIUKIDOS and that its first
; stateful service tranche (version/default drive/PSP/DTA) is coherent from a
; normal external COM child.  The marker is consumed by full/full-CD gates.

bits 16
org 0x0100

%ifndef CIUKIDOS_RUNTIME_SEG
%define CIUKIDOS_RUNTIME_SEG 0x0900
%endif
%ifndef CIUKIDOS_ABI_VERSION
%define CIUKIDOS_ABI_VERSION 2
%endif

start:
    cld
    push cs
    pop ds

    xor ax, ax
    mov es, ax
    cmp word [es:(0x21 * 4) + 2], CIUKIDOS_RUNTIME_SEG
    jne fail_owner

    ; Locate the exported ABI table inside the bounded runtime window instead
    ; of trusting a build-time service count.  The header contains the eight-
    ; byte descriptor size, followed by id, version, offset and segment delta.
    mov ax, CIUKIDOS_RUNTIME_SEG
    mov es, ax
    xor di, di
    mov cx, 3491
.find_service_table:
    cmp word [es:di], 0x5452       ; "RT"
    jne .next_table_byte
    cmp word [es:di + 2], 0x5653   ; "SV"
    jne .next_table_byte
    cmp word [es:di + 4], CIUKIDOS_ABI_VERSION
    jne fail_services_table
    cmp word [es:di + 6], 11
    jb fail_services_table
    cmp word [es:di + 8], 8
    jne fail_services_table
    cmp word [es:di + 74], 9
    jne fail_services_table
    cmp word [es:di + 76], 1
    jne fail_services_table
    cmp word [es:di + 82], 10
    jne fail_services_table
    cmp word [es:di + 84], 1
    jne fail_services_table
    cmp word [es:di + 90], 11
    jne fail_services_table
    cmp word [es:di + 92], 1
    jne fail_services_table

    mov ax, [es:di + 78]
    mov [cs:service9_ptr], ax
    mov word [cs:service9_ptr + 2], CIUKIDOS_RUNTIME_SEG
    call far [cs:service9_ptr]
    jc fail_services_call
    cmp ax, 0x003F
    jne fail_services_caps
    ; ABI 2 closes the Stage1 chain boundary.  Service 9 therefore returns a
    ; deliberately null legacy target; any nonzero pointer would mean normal
    ; DOS execution can still escape the CIUKIDOS kernel.
    or bx, dx
    jz .services_vector_ok
    jmp fail_services_vector
.services_vector_ok:
    jmp .services_ok

.next_table_byte:
    inc di
    loop .continue_table_scan
    jmp fail_services_table
.continue_table_scan:
    jmp .find_service_table

.services_ok:

    mov ah, 0x30
    int 0x21
    cmp ax, 0x0005
    jne fail_version

    mov ah, 0x19
    int 0x21
    cmp al, 2
    jb fail_drive
    cmp al, 3
    ja fail_drive

    mov ah, 0x51
    int 0x21
    or bx, bx
    jz fail_psp

    mov ah, 0x2F
    int 0x21
    mov [cs:saved_dta_off], bx
    mov ax, es
    mov [cs:saved_dta_seg], ax

    push cs
    pop ds
    mov dx, test_dta
    mov ah, 0x1A
    int 0x21
    jc fail_dta

    mov ah, 0x2F
    int 0x21
    mov ax, es
    mov dx, cs
    cmp ax, dx
    jne restore_dta_fail
    cmp bx, test_dta
    jne restore_dta_fail

    call restore_dta
    mov dx, msg_pass
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C00
    int 0x21

restore_dta_fail:
    call restore_dta
fail_dta:
    mov dx, msg_fail_dta
    jmp fail

fail_owner:
    mov dx, msg_fail_owner
    jmp fail
fail_services_table:
    mov dx, msg_fail_services_table
    jmp fail
fail_services_call:
    mov dx, msg_fail_services_call
    jmp fail
fail_services_caps:
    mov dx, msg_fail_services_caps
    jmp fail
fail_services_vector:
    mov dx, msg_fail_services_vector
    jmp fail
fail_version:
    mov dx, msg_fail_version
    jmp fail
fail_drive:
    mov dx, msg_fail_drive
    jmp fail
fail_psp:
    mov dx, msg_fail_psp

fail:
    push cs
    pop ds
    mov ah, 0x09
    int 0x21
    mov ax, 0x4C01
    int 0x21

restore_dta:
    push ax
    push dx
    push ds
    mov ax, [cs:saved_dta_seg]
    mov ds, ax
    mov dx, [cs:saved_dta_off]
    mov ah, 0x1A
    int 0x21
    pop ds
    pop dx
    pop ax
    ret

msg_pass         db '[CIUKRTST] OWNER=CIUKIDOS ABI=2 SERVICES=11 CHAIN=0 STATE=PASS', 13, 10, '$'
msg_fail_owner   db '[CIUKRTST] FAIL OWNER', 13, 10, '$'
msg_fail_services_table db '[CIUKRTST] FAIL SERVICES TABLE', 13, 10, '$'
msg_fail_services_call db '[CIUKRTST] FAIL SERVICES CALL', 13, 10, '$'
msg_fail_services_caps db '[CIUKRTST] FAIL SERVICES CAPS', 13, 10, '$'
msg_fail_services_vector db '[CIUKRTST] FAIL SERVICES VECTOR', 13, 10, '$'
msg_fail_version db '[CIUKRTST] FAIL VERSION', 13, 10, '$'
msg_fail_drive   db '[CIUKRTST] FAIL DRIVE', 13, 10, '$'
msg_fail_psp     db '[CIUKRTST] FAIL PSP', 13, 10, '$'
msg_fail_dta     db '[CIUKRTST] FAIL DTA', 13, 10, '$'

saved_dta_off dw 0
saved_dta_seg dw 0
service9_ptr dw 0, 0
test_dta times 128 db 0
