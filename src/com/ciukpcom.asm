; ciukpcom.asm - nested COM grandchild process-state probe
;
; Invoked only by CIUKPST.COM with the command tail:
;   " PPPP OOOO"
; where PPPP is the parent PSP and OOOO is the mailbox offset in that PSP.

; The probe deliberately changes its DTA, records the observed DOS/PSP state
; in the parent mailbox, and exits with 6Ch so the parent can validate both
; process restoration and AH=4Dh status.

bits 16
org 0x0100

%define CHECK_AH51          0x0001
%define CHECK_AH62          0x0002
%define CHECK_COM_SEGMENT   0x0004
%define CHECK_PSP_INT20     0x0008
%define CHECK_PSP_0050      0x0010
%define CHECK_PARENT_LINK   0x0020
%define CHECK_DEFAULT_DTA   0x0040
%define CHECK_PRIVATE_DTA   0x0080
%define CHECK_ENTRY_GUARD_1 0x0100
%define CHECK_ENTRY_GUARD_2 0x0200
%define CHECKS_ALL          0x03FF
%define MAILBOX_MAGIC       0x4343

start:
    cld

    mov ah, 0x51
    int 0x21
    or bx, bx
    jz fail_psp
    mov [cs:self_psp], bx
    or word [cs:checks], CHECK_AH51

    mov ax, cs
    cmp ax, bx
    jne fail_psp
    or word [cs:checks], CHECK_COM_SEGMENT

    mov ah, 0x62
    int 0x21
    cmp bx, [cs:self_psp]
    jne fail_psp
    or word [cs:checks], CHECK_AH62

    ; DS is the COM PSP on entry.  Validate the PSP signatures before parsing
    ; the command tail, so a malformed PSP cannot be mistaken for test input.
    cmp word [0x0000], 0x20CD
    jne fail_psp
    or word [cs:checks], CHECK_PSP_INT20
    cmp word [0x0050], 0x21CD
    jne fail_psp
    cmp byte [0x0052], 0xCB
    jne fail_psp
    or word [cs:checks], CHECK_PSP_0050

    cmp byte [0x0080], 10
    jne fail_argument
    cmp byte [0x0081], ' '
    jne fail_argument
    cmp byte [0x0086], ' '
    jne fail_argument
    cmp byte [0x008B], 0x0D
    jne fail_argument

    mov si, 0x0082
    call parse_hex_word
    jc fail_argument
    mov [cs:parent_psp], ax
    mov si, 0x0087
    call parse_hex_word
    jc fail_argument
    mov [cs:mailbox_off], ax

    mov bx, [cs:parent_psp]
    or bx, bx
    jz fail_parent
    cmp word [0x0016], bx
    jne fail_parent
    or word [cs:checks], CHECK_PARENT_LINK

    mov ah, 0x2F
    int 0x21
    mov ax, es
    cmp ax, [cs:self_psp]
    jne fail_dta
    cmp bx, 0x0080
    jne fail_dta
    or word [cs:checks], CHECK_DEFAULT_DTA

    cmp word [cs:entry_guard], 0x5043
    jne fail_entry
    cmp word [cs:entry_guard + 2], 0x3147
    jne fail_entry
    or word [cs:checks], CHECK_ENTRY_GUARD_1

    push cs
    pop ds
    mov dx, private_dta
    mov ah, 0x1A
    int 0x21
    mov ah, 0x2F
    int 0x21
    mov ax, es
    mov dx, cs
    cmp ax, dx
    jne fail_dta
    cmp bx, private_dta
    jne fail_dta
    or word [cs:checks], CHECK_PRIVATE_DTA

    cmp word [cs:entry_guard], 0x5043
    jne fail_entry
    cmp word [cs:entry_guard + 2], 0x3147
    jne fail_entry
    or word [cs:checks], CHECK_ENTRY_GUARD_2

    cmp word [cs:checks], CHECKS_ALL
    jne fail_entry

    mov es, [cs:parent_psp]
    mov di, [cs:mailbox_off]
    mov word [es:di + 0], MAILBOX_MAGIC
    mov ax, [cs:self_psp]
    mov [es:di + 2], ax
    mov ax, [cs:parent_psp]
    mov [es:di + 4], ax
    mov ax, [cs:checks]
    mov [es:di + 6], ax

    push cs
    pop ds
    mov dx, msg_pass
    call print_dollar
    mov ax, 0x4C6C
    int 0x21
    jmp halt

parse_hex_word:
    push bx
    push cx
    push dx
    xor bx, bx
    mov cx, 4
.digit:
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
    jmp .merge
.decimal:
    sub al, '0'
.merge:
    mov dl, al
    shl bx, 1
    shl bx, 1
    shl bx, 1
    shl bx, 1
    xor dh, dh
    add bx, dx
    loop .digit
    mov ax, bx
    clc
    jmp .done
.bad:
    stc
.done:
    pop dx
    pop cx
    pop bx
    ret

print_dollar:
    mov ah, 0x09
    int 0x21
    ret

fail_argument:
    mov dx, msg_fail_argument
    jmp fail
fail_psp:
    push cs
    pop ds
    mov dx, msg_fail_psp
    jmp fail
fail_parent:
    push cs
    pop ds
    mov dx, msg_fail_parent
    jmp fail
fail_dta:
    push cs
    pop ds
    mov dx, msg_fail_dta
    jmp fail
fail_entry:
    push cs
    pop ds
    mov dx, msg_fail_entry
fail:
    push cs
    pop ds
    call print_dollar
    mov ax, 0x4CEE
    int 0x21
halt:
    cli
    hlt
    jmp halt

entry_guard dw 0x5043, 0x3147
self_psp dw 0
parent_psp dw 0
mailbox_off dw 0
checks dw 0

private_dta times 128 db 0

msg_pass db '[PSTACK:G] COM2COM CHILD=PASS PSP=PASS DTA=PASS PARENT=PASS ENTRY=PASS EXIT=6C', 13, 10, '$'
msg_fail_argument db '[PSTACK] FAIL G_ARGUMENT', 13, 10, '$'
msg_fail_psp db '[PSTACK] FAIL G_PSP', 13, 10, '$'
msg_fail_parent db '[PSTACK] FAIL G_PARENT', 13, 10, '$'
msg_fail_dta db '[PSTACK] FAIL G_DTA', 13, 10, '$'
msg_fail_entry db '[PSTACK] FAIL G_ENTRY', 13, 10, '$'
