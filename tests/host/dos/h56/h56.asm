; H56 guest suite, run as SHELL= of the CiukiDOS fork (f3-56). Results go to
; port E9h as "H56 <test> PASS" or "H56 <test> FAIL <step>"; port EAh ends
; the run. Mode (first command-tail character): U unbound, B bound with the
; harness fixture claims, N bound without claims, F<n> failure injection.
; Every assertion reads genuine FreeDOS structures through DOS calls.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
cpu 386
org 0x100

%macro EXPECT 2                 ; %1 = jcc taken when correct, %2 = step
    mov [cs:dbg_ax], ax             ; MOV keeps the flags being tested
    %1 %%ok
    mov ax, %2
    jmp test_fail
%%ok:
%endmacro
%macro TEST 2                   ; %1 routine, %2 name string
    mov si, %%name
    mov bx, %1
    call run_test
    jmp %%next
%%name db %2, 0
%%next:
%endmacro

start:
    mov sp, stack_top
    mov bx, (image_end - $$ + 0x100 + 15) >> 4
    mov ah, 0x4a
    int 0x21
    mov si, 0x81
.skip:
    lodsb
    cmp al, ' '
    je .skip
    mov [mode], al
    mov al, [si]
    mov [mode2], al
    mov ah, 0x51
    int 0x21
    mov [self_psp], bx
    mov ah, 0x52
    int 0x21
    mov [lol], bx
    mov [lol+2], es
    push cs
    pop es

    TEST t_layout, 'layout'
    cmp byte [mode], 'F'
    je failures
    TEST t_handles, 'handles'
    TEST t_exec, 'exec'
    TEST t_psp, 'psp'
    TEST t_internals, 'internals'
    cmp byte [mode], 'B'
    jne .no_claims
    TEST t_direct, 'direct'
    TEST t_pending, 'pending'
.no_claims:
    cmp byte [mode], 'U'
    je .finish
    TEST t_spoof, 'spoof'
    TEST t_exec, 'exec-after-spoof'
.finish:
    mov si, msg_end
    call puts
    mov ax, [fails]
    call hex
    mov si, crlf
    call puts
    mov al, [fails]
    out 0xea, al
.halt:
    jmp .halt

failures:
    cmp byte [mode2], '3'
    je .begin
    cmp byte [mode2], '4'
    je .commit
    cmp byte [mode2], '1'
    je .create
    jmp start.finish
.begin:
    TEST t_fail_begin, 'fail-exec-begin'
    jmp start.finish
.commit:
    TEST t_fail_commit, 'fail-exec-commit'
    jmp start.finish
.create:
    TEST t_fail_create, 'fail-psp55'
    jmp start.finish

; ---- framework --------------------------------------------------------
run_test:                       ; SI = name, BX = routine
    mov [cur_name], si
    mov [test_sp], sp
    call bx
report:                         ; AX = 0 pass, else failing step
    mov sp, [test_sp]
    push cs
    pop ds
    push cs
    pop es
    push ax
    mov si, msg_h56
    call puts
    mov si, [cur_name]
    call puts
    pop ax
    or ax, ax
    jnz .bad
    mov si, msg_pass
    call puts
    ret
.bad:
    push ax
    mov si, msg_fail
    call puts
    pop ax
    call hex
    mov si, msg_ax
    call puts
    mov ax, [dbg_ax]
    call hex
    mov si, crlf
    call puts
    inc word [fails]
    ret
test_fail:
    jmp report

puts:
    push ax
.next:
    lodsb
    test al, al
    jz .done
    out 0xe9, al
    jmp .next
.done:
    pop ax
    ret
hex:                            ; AX
    push cx
    mov cx, 4
.digit:
    rol ax, 4
    push ax
    and al, 15
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 7
.emit:
    out 0xe9, al
    pop ax
    loop .digit
    pop cx
    ret
puthex_far:                     ; DX:AX = seg:off
    push ax
    mov ax, dx
    call hex
    mov al, ':'
    out 0xe9, al
    pop ax
    jmp hex

; Ledger of the guest's own allocation state: MCB chain (owner, size) and
; every SFT reference count. Out: [mcb_sum], [sft_sum].
ledger:
    push es
    push ax
    push bx
    push cx
    les bx, [lol]
    mov ax, [es:bx-2]           ; first MCB
    xor cx, cx
    mov word [mcb_sum], 0
.mcb:                            ; owned blocks only: free space may split
    mov es, ax
    mov bx, [es:1]
    or bx, bx
    jz .free
    add [mcb_sum], bx
    rol word [mcb_sum], 3
    add [mcb_sum], ax
    rol word [mcb_sum], 3
    mov bx, [es:3]
    add [mcb_sum], bx
    rol word [mcb_sum], 5
.free:
    mov bx, [es:3]
    cmp byte [es:0], 'Z'
    je .sft
    cmp byte [es:0], 'M'
    jne .sft
    add ax, bx
    inc ax
    inc cx
    cmp cx, 1000
    jb .mcb
.sft:
    mov word [sft_sum], 0
    les bx, [lol]
    les bx, [es:bx+4]
.block:
    cmp bx, 0xffff
    je .done
    mov cx, [es:bx+4]
    push bx
    add bx, 6
.entry:
    mov ax, [es:bx]
    add [sft_sum], ax
    rol word [sft_sum], 1
    add bx, 59
    loop .entry
    pop bx
    les bx, [es:bx]
    jmp .block
.done:
    pop cx
    pop bx
    pop ax
    pop es
    ret

; EXEC C:\CHILD.COM with tail character AL. Out: CF/AX of 4B00h, then
; AX = 4Dh return code when it succeeded.
exec_child:
    mov [child_tail+2], al
    mov word [block], 0
    mov word [block+2], child_tail
    mov [block+4], cs
    mov word [block+6], 0x5c
    mov [block+8], cs
    mov word [block+10], 0x6c
    mov [block+12], cs
    mov dx, child_name
exec_dx:
    push cs
    pop es                          ; ES:BX = parameter block
    mov [save_sp], sp
    mov bx, block
    mov ax, 0x4b00
    int 0x21
    cli
    mov bx, cs
    mov ss, bx
    mov sp, [cs:save_sp]
    sti
    push cs
    pop ds
    push cs
    pop es
    jc .out
    mov ah, 0x4d
    int 0x21
    clc
.out:
    ret

; SFT reference count of the entry behind handle BX. Out: AX.
handle_count:
    push es
    push di
    mov ax, 0x1220
    int 0x2f
    jc .bad
    mov bl, [es:di]
    xor bh, bh
    mov ax, 0x1216
    int 0x2f
    jc .bad
    mov ax, [es:di]
    jmp .out
.bad:
    mov ax, 0xffff
.out:
    pop di
    pop es
    ret

; ---- tests -------------------------------------------------------------
; Genuine List-of-Lists, SFT chain, SDA fields and CDS, printed for the
; harness to compare with the native registration.
t_layout:
    les bx, [lol]
    mov si, msg_lol
    call puts
    mov dx, es
    mov ax, bx
    call puthex_far
    les di, [es:bx+4]           ; SFT chain: count entries, check headers
    xor cx, cx
.walk:
    cmp di, 0xffff
    je .walked
    mov ax, [es:di+4]
    or ax, ax
    EXPECT jnz, 1
    add cx, ax
    les di, [es:di]
    jmp .walk
.walked:
    mov [sft_total], cx
    mov si, msg_sft
    call puts
    mov ax, cx
    call hex
    push ds
    mov ax, 0x5d06
    int 0x21
    mov ax, ds
    mov es, ax
    pop ds
    EXPECT jnc, 2
    mov [sda], si
    mov [sda+2], es
    mov si, msg_sda
    call puts
    mov dx, es
    mov ax, [sda]
    call puthex_far
    les bx, [sda]
    mov ax, [es:bx+0x10]        ; selected PSP field
    cmp ax, [self_psp]
    EXPECT je, 3
    mov ah, 0x2f
    int 0x21
    mov dx, es
    les si, [sda]
    cmp bx, [es:si+0x0c]        ; DTA field
    EXPECT je, 4
    cmp dx, [es:si+0x0e]
    EXPECT je, 5
    les bx, [lol]
    mov si, msg_cds
    call puts
    mov ax, [es:bx+0x16]
    mov dx, [es:bx+0x18]
    call puthex_far
    mov al, '/'
    out 0xe9, al
    xor ah, ah
    mov al, [es:bx+0x21]
    call hex
    mov si, crlf
    call puts
    xor ax, ax
    ret

; open/dup/force-dup/close with genuine SFT reference counts, inheritance
; into an EXEC child (first 20, O_NOINHERIT) and a balanced ledger.
t_handles:
    call ledger
    mov ax, [mcb_sum]
    mov [base_mcb], ax
    mov ax, [sft_sum]
    mov [base_sft], ax
    mov dx, file_a
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 1
    mov [h_a], ax
    cmp ax, 5
    EXPECT je, 2
    mov dx, file_b
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 3
    mov bx, ax
    mov ah, 0x3e
    int 0x21
    mov dx, file_b
    mov ax, 0x3d82              ; read/write, no-inherit
    int 0x21
    EXPECT jnc, 4
    mov [h_b], ax
    cmp ax, 6
    EXPECT je, 5
    mov bx, [h_a]
    mov ah, 0x45
    int 0x21
    EXPECT jnc, 6
    mov [h_dup], ax
    mov bx, [h_a]
    call handle_count
    cmp ax, 2
    EXPECT je, 7
    mov bx, [h_a]
    mov cx, 12
    mov ah, 0x46
    int 0x21
    EXPECT jnc, 8
    mov bx, [h_a]
    call handle_count
    cmp ax, 3
    EXPECT je, 9
    mov al, 'H'
    call exec_child
    EXPECT jnc, 10
    cmp ax, 0
    EXPECT je, 11
    mov bx, [h_a]
    call handle_count
    cmp ax, 3
    EXPECT je, 12
    mov bx, 12
    mov ah, 0x3e
    int 0x21
    mov bx, [h_dup]
    mov ah, 0x3e
    int 0x21
    mov bx, [h_a]
    call handle_count
    cmp ax, 1
    EXPECT je, 13
    mov bx, [h_a]
    mov ah, 0x3e
    int 0x21
    mov bx, [h_b]
    mov ah, 0x3e
    int 0x21
    mov dx, file_b
    mov ah, 0x41
    int 0x21
    call ledger
    mov ax, [mcb_sum]
    cmp ax, [base_mcb]
    EXPECT je, 14
    mov ax, [sft_sum]
    cmp ax, [base_sft]
    EXPECT je, 15
    xor ax, ax
    ret

; EXEC success, missing file, nested, child DTA, INT 20h, then TSR via
; 31h and INT 27h. Non-resident runs leave the ledger at its baseline.
t_exec:
    call ledger
    mov ax, [mcb_sum]
    mov [base_mcb], ax
    mov ax, [sft_sum]
    mov [base_sft], ax
    mov al, 'X'
    call exec_child
    EXPECT jnc, 1
    cmp ax, 0x0042
    EXPECT je, 2
    mov dx, missing_name
    call exec_dx
    EXPECT jc, 3
    cmp ax, 2
    EXPECT je, 4
    mov al, 'N'
    call exec_child
    EXPECT jnc, 5
    cmp ax, 0x0042
    EXPECT je, 6
    mov al, 'D'
    call exec_child
    EXPECT jnc, 7
    cmp ax, 0
    EXPECT je, 8
    mov al, 'A'
    call exec_child
    EXPECT jnc, 9
    cmp ax, 0
    EXPECT je, 10
    mov ah, 0x2f                ; parent DTA unchanged by the children
    int 0x21
    cmp bx, 0x80
    EXPECT je, 11
    mov ah, 0x62
    int 0x21
    cmp bx, [self_psp]
    EXPECT je, 12
    call ledger
    mov ax, [mcb_sum]
    cmp ax, [base_mcb]
    EXPECT je, 13
    mov ax, [sft_sum]
    cmp ax, [base_sft]
    EXPECT je, 14
    cmp byte [tsr_done], 0
    jne .no_tsr
    mov byte [tsr_done], 1
    mov al, 'T'
    call exec_child
    EXPECT jnc, 15
    cmp ax, 0x0307
    EXPECT je, 16
    mov al, 'K'
    call exec_child
    EXPECT jnc, 17
    cmp ax, 0x0300
    EXPECT je, 18
.no_tsr:
    xor ax, ax
    ret

; 26h copies without selecting, 55h clones the first 20 eligible handles
; and selects, 50h restores the selection; ancestry stays native.
t_psp:
    mov bx, 0x20
    mov ah, 0x48
    int 0x21
    EXPECT jnc, 1
    mov [psp26], ax
    mov bx, 0x20
    mov ah, 0x48
    int 0x21
    EXPECT jnc, 2
    mov [psp55], ax
    mov dx, [psp26]
    mov ah, 0x26
    int 0x21
    mov ah, 0x62
    int 0x21
    cmp bx, [self_psp]
    EXPECT je, 3
    mov es, [psp26]
    cmp word [es:0], 0x20cd
    EXPECT je, 4
    cmp word [es:0x16], 0           ; RBIL 26h: parent zero
    EXPECT je, 5
    push cs
    pop es
    mov dx, file_a                  ; one open handle to inherit
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 6
    mov [h_a], ax
    mov dx, [psp55]
    mov si, dx
    add si, 0x20
    mov ah, 0x55
    int 0x21
    EXPECT jnc, 7
    mov ah, 0x62
    int 0x21
    cmp bx, [psp55]
    EXPECT je, 8
    mov bx, [h_a]                   ; selected child shares the SFT entry
    call handle_count
    cmp ax, 2
    EXPECT je, 9
    mov es, [psp55]
    mov ax, [self_psp]
    cmp [es:0x16], ax
    EXPECT je, 10
    mov bx, [self_psp]
    mov ah, 0x50
    int 0x21
    mov ah, 0x51
    int 0x21
    cmp bx, [self_psp]
    EXPECT je, 11
    push cs
    pop es
    mov bx, [h_a]
    call handle_count
    cmp ax, 2
    EXPECT je, 12
    mov bx, [h_a]
    mov ah, 0x3e
    int 0x21
    xor ax, ax
    ret

; INT 2Fh/1216h, 1220h and 1226h see the real structures: an opened file
; appears in the JFT and SFT with this PSP as owner and the open count.
t_internals:
    mov dx, file_a
    mov ax, 0x3d02
    int 0x21
    EXPECT jnc, 1
    mov [h_a], ax
    mov bx, ax
    mov ax, 0x1220
    int 0x2f
    EXPECT jnc, 2
    mov bl, [es:di]
    xor bh, bh
    mov [sft_index], bx
    mov ax, 0x1216
    int 0x2f
    EXPECT jnc, 3
    mov ax, [self_psp]
    cmp [es:di+0x31], ax
    EXPECT je, 4
    cmp word [es:di], 1
    EXPECT je, 5
    mov bx, [sft_total]
    mov ax, 0x1216
    int 0x2f
    EXPECT jc, 6
    mov bx, 400
    mov ax, 0x1220
    int 0x2f
    EXPECT jc, 7
    push cs
    pop es
    mov bx, [h_a]
    mov ah, 0x3e
    int 0x21
    mov ax, 0x1226                  ; internal open of the same file
    mov cl, 2
    mov dx, file_a
    int 0x2f
    EXPECT jnc, 8
    mov bx, ax
    mov ax, 0x1227                  ; internal close
    int 0x2f
    EXPECT jnc, 9
    mov dx, file_a
    mov ah, 0x41
    int 0x21
    push ds                         ; 1203h: DS = the DOS data segment (LoL's)
    mov ax, 0x1203
    int 0x2f
    mov ax, ds
    pop ds
    cmp ax, [lol+2]
    EXPECT je, 10
    mov dx, missing_name            ; 122Dh: the last extended error code
    mov ax, 0x3d00
    int 0x21
    EXPECT jc, 11
    mov ax, 0x122d
    int 0x2f
    cmp ax, 2
    EXPECT je, 12
    mov ax, 0x1100                  ; no public redirector is advertised
    int 0x2f
    cmp al, 0
    EXPECT je, 13
    xor ax, ax
    ret

; Claimed AH=61h: DONE with the fixture's registers, once, under an INT 21h
; hook that chains; AH=6Eh: CHAIN, so FreeDOS answers "invalid function".
t_direct:
    mov ax, 0x3521
    int 0x21
    mov [old21], bx
    mov [old21+2], es
    push cs
    pop es
    mov dx, hook21
    mov ax, 0x2521
    int 0x21
    mov word [hook_count], 0
    mov ax, 0x6100
    mov bx, 0x1357
    mov cx, 0x1111
    mov dx, 0x2222
    mov si, 0x3333
    mov di, 0x4444
    stc
    int 0x21
    pushf
    cmp ax, 0x5a5a
    EXPECT je, 1
    cmp bx, 0xeca8
    EXPECT je, 2
    cmp cx, 0x1234
    EXPECT je, 3
    cmp dx, 0xfedc
    EXPECT je, 4
    cmp si, 0x3333
    EXPECT je, 5
    cmp di, 0x4444
    EXPECT je, 6
    popf
    EXPECT jnc, 7
    cmp word [hook_count], 1
    EXPECT je, 8
    mov ax, 0x6e55                  ; FreeDOS default: AL=0, FLAGS as given
    stc
    int 0x21
    EXPECT jc, 9
    cmp ax, 0x6e00
    EXPECT je, 10
    mov ax, 0x6e55
    clc
    int 0x21
    EXPECT jnc, 11
    cmp word [hook_count], 3
    EXPECT je, 12
    push ds
    lds dx, [old21]
    mov ax, 0x2521
    int 0x21
    pop ds
    xor ax, ax
    ret
hook21:
    inc word [cs:hook_count]
    jmp far [cs:old21]

; Claimed AH=6Fh is PENDING in the monitor: our INT 24h handler runs once
; with the fixture's registers, its reply completes the call once.
t_pending:
    mov ax, 0x3524
    int 0x21
    mov [old24], bx
    mov [old24+2], es
    push cs
    pop es
    mov dx, hook24
    mov ax, 0x2524
    int 0x21
    mov word [crit_count], 0
    mov ax, 0x6f00
    int 0x21
    EXPECT jnc, 1
    cmp ax, 0x2401
    EXPECT je, 2
    cmp word [crit_count], 1
    EXPECT je, 3
    cmp word [crit_ax], 0x1a02
    EXPECT je, 4
    cmp word [crit_di], 0x000c
    EXPECT je, 5
    push ds
    lds dx, [old24]
    mov ax, 0x2524
    int 0x21
    pop ds
    xor ax, ax
    ret
hook24:
    inc word [cs:crit_count]
    mov [cs:crit_ax], ax
    mov [cs:crit_di], di
    mov al, 1                       ; Retry
    iret

; Ordinary code calls every gate with its own stack and a forged record:
; the monitor refuses without a side effect and the binding stays usable.
t_spoof:
    mov word [fake+0], 2
    mov word [fake+2], 48
    mov word [fake+0x0c], 3
    mov word [fake+0x0e], 0xffff
    mov si, fake
    mov ax, 0x5555
    call 0xf000:0x1c08
    cmp ax, 0x5555
    EXPECT je, 1
    cmp word [fake+0x0e], 0xffff
    EXPECT je, 2
    call 0xf000:0x1c00
    call 0xf000:0x1c04
    mov ah, 0x62
    int 0x21
    cmp bx, [self_psp]
    EXPECT je, 3
    xor ax, ax
    ret

; F3: the first EXEC_BEGIN is refused natively: EXEC fails with error 8
; before any inheritance, the ledger is unchanged, the retry succeeds.
t_fail_begin:
    call ledger
    mov ax, [mcb_sum]
    mov [base_mcb], ax
    mov ax, [sft_sum]
    mov [base_sft], ax
    mov al, 'X'
    call exec_child
    EXPECT jc, 1
    cmp ax, 8
    EXPECT je, 2
    call ledger
    mov ax, [mcb_sum]
    cmp ax, [base_mcb]
    EXPECT je, 3
    mov ax, [sft_sum]
    cmp ax, [base_sft]
    EXPECT je, 4
    mov al, 'X'
    call exec_child
    EXPECT jnc, 5
    cmp ax, 0x42
    EXPECT je, 6
    xor ax, ax
    ret
; F4: the first EXEC_COMMIT is refused after child_psp() cloned handles:
; the kernel releases the clones and the child's blocks exactly once.
t_fail_commit:
    mov dx, file_a
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 7
    mov [h_a], ax
    call ledger
    mov ax, [mcb_sum]
    mov [base_mcb], ax
    mov ax, [sft_sum]
    mov [base_sft], ax
    mov al, 'X'
    call exec_child
    EXPECT jc, 1
    cmp ax, 8
    EXPECT je, 2
    call ledger
    mov ax, [mcb_sum]
    cmp ax, [base_mcb]
    EXPECT je, 3
    mov ax, [sft_sum]
    cmp ax, [base_sft]
    EXPECT je, 4
    mov bx, [h_a]
    call handle_count
    cmp ax, 1
    EXPECT je, 5
    mov al, 'H'
    call exec_child
    EXPECT jnc, 6
    mov bx, [h_a]
    mov ah, 0x3e
    int 0x21
    xor ax, ax
    ret
; F1: the 55h publication is refused: CF/8, selection unchanged and the
; cloned references released once.
t_fail_create:
    mov dx, file_a
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 1
    mov [h_a], ax
    mov bx, 0x20
    mov ah, 0x48
    int 0x21
    EXPECT jnc, 2
    mov [psp55], ax
    mov dx, ax
    mov si, ax
    add si, 0x20
    mov ah, 0x55
    clc
    int 0x21
    EXPECT jc, 3
    cmp ax, 8
    EXPECT je, 4
    mov ah, 0x62
    int 0x21
    cmp bx, [self_psp]
    EXPECT je, 5
    mov bx, [h_a]
    call handle_count
    cmp ax, 1
    EXPECT je, 6
    mov es, [psp55]
    cmp byte [es:0x18+5], 0xff
    EXPECT je, 7
    push cs
    pop es
    xor ax, ax
    ret

msg_h56 db 'H56 ', 0
msg_pass db ' PASS', 13, 10, 0
msg_fail db ' FAIL ', 0
msg_ax db ' ax=', 0
msg_end db 'H56 END fails=', 0
msg_lol db 'H56 LAYOUT lol=', 0
msg_sft db ' sft=', 0
msg_sda db ' sda=', 0
msg_cds db ' cds=', 0
crlf db 13, 10, 0
child_name db 'C:\CHILD.COM', 0
missing_name db 'C:\NOTHERE.COM', 0
child_tail db 2, ' ?', 13
file_a db 'C:\H56A.DAT', 0
file_b db 'C:\H56B.DAT', 0
mode db 0
mode2 db 0
tsr_done db 0
align 2
fails dw 0
dbg_ax dw 0
cur_name dw 0
test_sp dw 0
save_sp dw 0
self_psp dw 0
lol dd 0
sda dd 0
sft_total dw 0
sft_index dw 0
mcb_sum dw 0
sft_sum dw 0
base_mcb dw 0
base_sft dw 0
h_a dw 0
h_b dw 0
h_dup dw 0
psp26 dw 0
psp55 dw 0
old21 dd 0
old24 dd 0
hook_count dw 0
crit_count dw 0
crit_ax dw 0
crit_di dw 0
block times 14 db 0
fake times 48 db 0
    times 1024 db 0
stack_top:
image_end:
