; ciukptrm.asm - MZ grandchild for the CIUKIDOS process-stack probe
;
; Command tail supplied by CIUKPST.COM:
;   " /MM PPPP OOOO"
; where MM is 4C, 00, 20 or 31, PPPP is the parent PSP segment and
; OOOO is the mailbox offset in that segment.

bits 16
org 0x0000

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

%define MODE_4C 0x4334
%define MODE_00 0x3030
%define MODE_20 0x3032
%define MODE_31 0x3133

%define MAILBOX_MAGIC 0x5350
%define CHECK_PSP_51 0x0001
%define CHECK_PSP_62 0x0002
%define CHECK_PARENT 0x0004
%define CHECK_DTA_DEFAULT 0x0008
%define CHECK_DTA_CUSTOM 0x0010
%define CHECK_BAD_POP_CF 0x0020
%define CHECK_BAD_POP_STABLE 0x0040
%define CHECK_VECTOR_23 0x0080
%define CHECK_VECTOR_24 0x0100
%define CHECKS_ALL 0x01FF
%define TSR_PSP_SENTINEL_0 0x5354
%define TSR_PSP_SENTINEL_1 0x3152
%define TSR_IMAGE_SENTINEL_0 0x4954
%define TSR_IMAGE_SENTINEL_1 0x314D

%define IMAGE_OFF(x) (x - image_start)

mz_header:
    dw 0x5A4D
    dw file_size_mod_512
    dw file_size_pages
    dw 0x0000                ; relocation count
    dw 0x0002                ; 32-byte header
    dw 0x0000                ; minimum allocation beyond image
    dw 0xFFFF                ; request the available arena
    dw 0x0000                ; SS relative to image
    dw 0xFFFE                ; SP
    dw 0x0000                ; checksum
    dw 0x0000                ; IP
    dw 0x0000                ; CS relative to image
    dw 0x001C                ; relocation-table offset (empty)
    dw 0x0000                ; overlay

times 0x20 - ($ - mz_header) db 0

image_start:
start:
    cld

    mov ah, 0x51
    int 0x21
    or bx, bx
    jz fail_psp
    mov [cs:IMAGE_OFF(psp_seg)], bx
    or word [cs:IMAGE_OFF(checks)], CHECK_PSP_51

    mov ah, 0x62
    int 0x21
    cmp bx, [cs:IMAGE_OFF(psp_seg)]
    jne fail_psp
    or word [cs:IMAGE_OFF(checks)], CHECK_PSP_62

    ; Parse the fixed command tail while DS still addresses the PSP.
    mov ds, bx
    cmp byte [0x0080], 14
    jne fail_tail
    cmp byte [0x0081], ' '
    jne fail_tail
    cmp byte [0x0082], '/'
    jne fail_tail
    cmp byte [0x0085], ' '
    jne fail_tail
    cmp byte [0x008A], ' '
    jne fail_tail
    cmp byte [0x008F], 0x0D
    jne fail_tail
    mov ax, [0x0083]
    cmp ax, MODE_4C
    je .mode_ok
    cmp ax, MODE_00
    je .mode_ok
    cmp ax, MODE_20
    je .mode_ok
    cmp ax, MODE_31
    jne fail_tail
.mode_ok:
    mov [cs:IMAGE_OFF(mode_word)], ax

    mov si, 0x0086
    call parse_hex_word
    jc fail_tail
    or ax, ax
    jz fail_tail
    mov [cs:IMAGE_OFF(parent_seg)], ax

    mov si, 0x008B
    call parse_hex_word
    jc fail_tail
    or ax, ax
    jz fail_tail
    cmp ax, 0xFFF2
    ja fail_tail
    mov [cs:IMAGE_OFF(mailbox_off)], ax

    ; The PSP relationship must agree with the explicit parent segment.
    mov ax, [cs:IMAGE_OFF(psp_seg)]
    mov es, ax
    mov ax, [es:0x0016]
    cmp ax, [cs:IMAGE_OFF(parent_seg)]
    jne fail_parent
    or word [cs:IMAGE_OFF(checks)], CHECK_PARENT

    ; The default child DTA is always PSP:0080.
    mov ah, 0x2F
    int 0x21
    mov ax, es
    cmp ax, [cs:IMAGE_OFF(psp_seg)]
    jne fail_default_dta
    cmp bx, 0x0080
    jne fail_default_dta
    or word [cs:IMAGE_OFF(checks)], CHECK_DTA_DEFAULT

    push cs
    pop ds
    mov dx, IMAGE_OFF(leaf_dta)
    mov ah, 0x1A
    int 0x21
    jc fail_custom_dta
    mov ah, 0x2F
    int 0x21
    mov ax, es
    mov dx, cs
    cmp ax, dx
    jne fail_custom_dta
    cmp bx, IMAGE_OFF(leaf_dta)
    jne fail_custom_dta
    or word [cs:IMAGE_OFF(checks)], CHECK_DTA_CUSTOM

    mov ax, 0x0008
    call find_runtime_service
    jc fail_services
    mov ax, [cs:IMAGE_OFF(found_service_ptr)]
    mov [cs:IMAGE_OFF(service8_ptr)], ax
    mov ax, [cs:IMAGE_OFF(found_service_ptr + 2)]
    mov [cs:IMAGE_OFF(service8_ptr + 2)], ax

    ; A definitely-wrong expected child must fail without consuming the frame.
    mov cx, 0xFFFF
    call far [cs:IMAGE_OFF(service8_ptr)]
    jnc fail_bad_pop
    or word [cs:IMAGE_OFF(checks)], CHECK_BAD_POP_CF

    mov ah, 0x51
    int 0x21
    cmp bx, [cs:IMAGE_OFF(psp_seg)]
    jne fail_bad_pop_state
    mov ah, 0x62
    int 0x21
    cmp bx, [cs:IMAGE_OFF(psp_seg)]
    jne fail_bad_pop_state
    mov ah, 0x2F
    int 0x21
    mov ax, es
    mov dx, cs
    cmp ax, dx
    jne fail_bad_pop_state
    cmp bx, IMAGE_OFF(leaf_dta)
    jne fail_bad_pop_state
    or word [cs:IMAGE_OFF(checks)], CHECK_BAD_POP_STABLE

    ; Replace the parent's handlers.  Stage1 must later restore the vectors
    ; saved in this child's PSP, for every termination path under test.
    push cs
    pop ds
    mov dx, IMAGE_OFF(leaf_int23)
    mov ax, 0x2523
    int 0x21
    jc fail_vectors
    mov ax, 0x3523
    int 0x21
    mov dx, es
    mov ax, cs
    cmp dx, ax
    jne fail_vectors
    cmp bx, IMAGE_OFF(leaf_int23)
    jne fail_vectors
    or word [cs:IMAGE_OFF(checks)], CHECK_VECTOR_23

    mov dx, IMAGE_OFF(leaf_int24)
    mov ax, 0x2524
    int 0x21
    jc fail_vectors
    mov ax, 0x3524
    int 0x21
    mov dx, es
    mov ax, cs
    cmp dx, ax
    jne fail_vectors
    cmp bx, IMAGE_OFF(leaf_int24)
    jne fail_vectors
    or word [cs:IMAGE_OFF(checks)], CHECK_VECTOR_24

    cmp word [cs:IMAGE_OFF(checks)], CHECKS_ALL
    jne fail_bad_pop_state
    call write_mailbox
    call print_mode_marker
    jmp terminate_selected

; Parse four hexadecimal characters at DS:SI.  Return AX and CF status.
parse_hex_word:
    push bx
    push cx
    push dx
    xor bx, bx
    mov cx, 4
.next:
    lodsb
    cmp al, '0'
    jb .bad
    cmp al, '9'
    jbe .decimal
    and al, 0xDF
    cmp al, 'A'
    jb .bad
    cmp al, 'F'
    ja .bad
    sub al, 'A' - 10
    jmp .nibble
.decimal:
    sub al, '0'
.nibble:
    xor ah, ah
    mov dx, ax
    shl bx, 1
    shl bx, 1
    shl bx, 1
    shl bx, 1
    or bx, dx
    loop .next
    mov ax, bx
    pop dx
    pop cx
    pop bx
    clc
    ret
.bad:
    pop dx
    pop cx
    pop bx
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
    cmp word [es:di], 0x5452
    jne .next_byte
    cmp word [es:di + 2], 0x5653
    jne .next_byte
    cmp word [es:di + 4], CIUKIDOS_ABI_VERSION
    jne .next_byte
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
    mov [cs:IMAGE_OFF(found_service_ptr)], ax
    mov ax, CIUKIDOS_RUNTIME_SEG
    add ax, [es:di + 6]
    mov [cs:IMAGE_OFF(found_service_ptr + 2)], ax
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    clc
    ret
.next_byte:
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

write_mailbox:
    push ax
    push di
    push es
    mov ax, [cs:IMAGE_OFF(parent_seg)]
    mov es, ax
    mov di, [cs:IMAGE_OFF(mailbox_off)]
    mov word [es:di + 0], MAILBOX_MAGIC
    mov ax, [cs:IMAGE_OFF(psp_seg)]
    mov [es:di + 2], ax
    mov ax, [cs:IMAGE_OFF(parent_seg)]
    mov [es:di + 4], ax
    mov ax, [cs:IMAGE_OFF(mode_word)]
    mov [es:di + 6], ax
    mov ax, [cs:IMAGE_OFF(checks)]
    mov [es:di + 8], ax
    mov word [es:di + 10], TSR_KEEP_PARAS
    mov word [es:di + 12], IMAGE_OFF(resident_sentinel)
    pop es
    pop di
    pop ax
    ret

print_mode_marker:
    mov ax, [cs:IMAGE_OFF(mode_word)]
    cmp ax, MODE_4C
    je .mode_4c
    cmp ax, MODE_00
    je .mode_00
    cmp ax, MODE_20
    je .mode_20
    mov dx, IMAGE_OFF(msg_mode_31)
    jmp print_dollar
.mode_4c:
    mov dx, IMAGE_OFF(msg_mode_4c)
    jmp print_dollar
.mode_00:
    mov dx, IMAGE_OFF(msg_mode_00)
    jmp print_dollar
.mode_20:
    mov dx, IMAGE_OFF(msg_mode_20)
    jmp print_dollar

terminate_selected:
    mov ax, [cs:IMAGE_OFF(mode_word)]
    cmp ax, MODE_4C
    je .term_4c
    cmp ax, MODE_00
    je .term_00
    cmp ax, MODE_20
    je .term_20
    cmp ax, MODE_31
    jne fail_tail

    ; Leave two independent signatures inside the retained PSP/image so the
    ; parent can prove persistence before and after another EXEC.
    mov ax, [cs:IMAGE_OFF(psp_seg)]
    mov es, ax
    mov word [es:0x0080], TSR_PSP_SENTINEL_0
    mov word [es:0x0082], TSR_PSP_SENTINEL_1
    mov dx, TSR_KEEP_PARAS
    mov ax, 0x3173
    int 0x21
    jmp fail_returned
.term_4c:
    mov ax, 0x4C41
    int 0x21
    jmp fail_returned
.term_00:
    mov ax, [cs:IMAGE_OFF(psp_seg)]
    mov [cs:IMAGE_OFF(psp_int21_ptr + 2)], ax
    xor ax, ax
    call far [cs:IMAGE_OFF(psp_int21_ptr)]
    jmp fail_returned
.term_20:
    ; MZ CS is the image segment, not the PSP.  Execute PSP:0000 so INT 20h
    ; observes the DOS-required CS=PSP relationship.
    mov ax, [cs:IMAGE_OFF(psp_seg)]
    push ax
    push word 0x0000
    retf

leaf_int23:
    iret

leaf_int24:
    mov al, 0x03
    iret

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

fail_tail:
    mov dx, IMAGE_OFF(msg_fail_tail)
    jmp fail
fail_services:
    mov dx, IMAGE_OFF(msg_fail_services)
    jmp fail
fail_psp:
    mov dx, IMAGE_OFF(msg_fail_psp)
    jmp fail
fail_parent:
    mov dx, IMAGE_OFF(msg_fail_parent)
    jmp fail
fail_default_dta:
    mov dx, IMAGE_OFF(msg_fail_default_dta)
    jmp fail
fail_custom_dta:
    mov dx, IMAGE_OFF(msg_fail_custom_dta)
    jmp fail
fail_bad_pop:
    mov dx, IMAGE_OFF(msg_fail_bad_pop)
    jmp fail
fail_bad_pop_state:
    mov dx, IMAGE_OFF(msg_fail_bad_pop_state)
    jmp fail
fail_vectors:
    mov dx, IMAGE_OFF(msg_fail_vectors)
    jmp fail
fail_returned:
    mov dx, IMAGE_OFF(msg_fail_returned)

fail:
    call print_dollar
    mov ax, 0x4CEE
    int 0x21
.hang:
    hlt
    jmp .hang

found_service_ptr dw 0, 0
service8_ptr dw 0, 0
psp_int21_ptr dw 0x0050, 0
psp_seg dw 0
parent_seg dw 0
mailbox_off dw 0
mode_word dw 0
checks dw 0

leaf_dta times 128 db 0

msg_mode_4c db '[PSTACK:G] MODE=4C PRETERM=PASS', 13, 10, '$'
msg_mode_00 db '[PSTACK:G] MODE=00 PRETERM=PASS', 13, 10, '$'
msg_mode_20 db '[PSTACK:G] MODE=20 PRETERM=PASS', 13, 10, '$'
msg_mode_31 db '[PSTACK:G] MODE=31 PRETERM=PASS TSR=REQUESTED', 13, 10, '$'

msg_fail_tail db '[PSTACK] FAIL G_TAIL', 13, 10, '$'
msg_fail_services db '[PSTACK] FAIL G_SERVICES', 13, 10, '$'
msg_fail_psp db '[PSTACK] FAIL G_PSP', 13, 10, '$'
msg_fail_parent db '[PSTACK] FAIL G_PARENT', 13, 10, '$'
msg_fail_default_dta db '[PSTACK] FAIL G_DTA_DEFAULT', 13, 10, '$'
msg_fail_custom_dta db '[PSTACK] FAIL G_DTA_CUSTOM', 13, 10, '$'
msg_fail_bad_pop db '[PSTACK] FAIL G_POP_MISMATCH', 13, 10, '$'
msg_fail_bad_pop_state db '[PSTACK] FAIL G_POP_MUTATED', 13, 10, '$'
msg_fail_vectors db '[PSTACK] FAIL G_VECTOR_SET', 13, 10, '$'
msg_fail_returned db '[PSTACK] FAIL G_TERMINATE_RETURNED', 13, 10, '$'

resident_sentinel dw TSR_IMAGE_SENTINEL_0, TSR_IMAGE_SENTINEL_1
resident_end:
TSR_KEEP_PARAS equ 0x0010 + ((resident_end - image_start + 15) / 16)

file_end:
file_size equ file_end - mz_header
file_size_mod_512 equ file_size & 0x01FF
file_size_pages equ (file_size + 511) / 512
