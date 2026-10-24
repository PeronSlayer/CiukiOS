; H57 guest suite (f3-57), run as SHELL= of the CiukiDOS fork with the live
; IFS backend. Results go to port E9h as "H57 <test> PASS" or
; "H57 <test> FAIL <step> ax=<ax>"; port EAh ends the run. Port EBh is the
; harness control (00 disarm, 1n n provider read faults, 1F persistent,
; 20 sample the native ledger). Mode (first tail character): F full suite,
; U unowned (activation refused, /LOCAL), A abort child.
; Every result is a genuine DOS call through the fork's own dispatcher.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
cpu 386
org 0x100

%macro EXPECT 2                 ; %1 = jcc taken when correct, %2 = step
    mov [cs:dbg_ax], ax
    %1 %%ok
    mov ax, %2
    jmp test_fail
%%ok:
%endmacro
%macro TEST 2
    mov si, %%name
    mov bx, %1
    call run_test
    jmp %%next
%%name db %2, 0
%%next:
%endmacro
%macro DOSCALL 1                ; AX preset by caller is replaced by %1
    mov ax, %1
    int 0x21
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
    cmp al, 'A'
    je child_abort
    cmp al, 'U'
    jne .full
    TEST t_local, 'local'
    jmp finish
.full:
    TEST t_classify, 'classify'
    TEST t_files, 'files'
    TEST t_dup, 'dup'
    TEST t_find, 'find'
    TEST t_attr, 'attr'
    TEST t_rename, 'rename'
    TEST t_dirs, 'dirs'
    TEST t_space, 'space'
    TEST t_lock, 'lock'
    TEST t_extopen, 'extopen'
    TEST t_time, 'time'
    TEST t_fcb, 'fcb-close-metadata'
    TEST t_readonly, 'readonly-d'
    TEST t_retry, 'critical-retry'
    TEST t_fail, 'critical-fail'
    TEST t_nocrit, 'nocrit'
    TEST t_abort, 'critical-abort'
    TEST t_after, 'after-abort'
finish:
    mov al, 0x20
    out 0xeb, al
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

; ---- framework --------------------------------------------------------
run_test:                       ; the native ledger is sampled before every test
    call sample
    mov [cur_name], si
    mov [test_sp], sp
    call bx
report:
    mov sp, [test_sp]
    push cs
    pop ds
    push cs
    pop es
    push ax
    mov si, msg_h57
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
hex:
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
sample:                         ; native ledger sample (harness prints it)
    push ax
    mov al, 0x20
    out 0xeb, al
    pop ax
    ret
fault:                          ; AL = control value
    out 0xeb, al
    ret

; open DX (AL mode) -> BX, CF on error
open:
    mov ah, 0x3d
    int 0x21
    mov bx, ax
    ret
close:
    mov ah, 0x3e
    int 0x21
    ret
; ---- tests ------------------------------------------------------------
t_classify:
    mov ax, 0x4409
    mov bl, 3
    int 0x21
    EXPECT jnc, 1
    cmp ax, 0x0300
    EXPECT je, 2
    test dx, 0x1000
    EXPECT jnz, 3
    mov ax, 0x4408
    mov bl, 3
    int 0x21
    EXPECT jc, 4
    cmp ax, 0x000f
    EXPECT je, 5
    push ds
    mov ah, 0x32
    mov dl, 3
    int 0x21
    pop ds
    cmp al, 0xff
    EXPECT je, 6
    mov ax, 0x1100
    int 0x2f
    cmp al, 0xff
    EXPECT jne, 7
    mov ah, 0x19
    int 0x21
    cmp al, 2
    EXPECT je, 8
    mov ah, 0x47
    xor dl, dl
    mov si, buf2
    int 0x21
    EXPECT jnc, 9
    cmp byte [buf2], 0
    EXPECT je, 10
    xor ax, ax
    ret

t_files:
    mov dx, p_dir
    mov ah, 0x39
    int 0x21
    EXPECT jnc, 1
    mov di, buf
    mov cx, 1000
    xor al, al
.fill:
    stosb
    inc al
    loop .fill
    mov dx, p_a
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 2
    mov bx, ax
    mov dx, buf
    mov cx, 1000
    mov ah, 0x40
    int 0x21
    EXPECT jnc, 3
    cmp ax, 1000
    EXPECT je, 4
    call close
    EXPECT jnc, 5
    mov dx, p_a
    mov al, 0
    call open
    EXPECT jnc, 6
    mov dx, buf2
    mov cx, 1000
    mov ah, 0x3f
    int 0x21
    EXPECT jnc, 7
    cmp ax, 1000
    EXPECT je, 8
    mov si, buf
    mov di, buf2
    mov cx, 1000
    repe cmpsb
    EXPECT je, 9
    mov ax, 0x4202
    xor cx, cx
    xor dx, dx
    int 0x21
    cmp ax, 1000
    EXPECT je, 10
    mov dx, buf2
    mov cx, 10
    mov ah, 0x3f
    int 0x21
    EXPECT jnc, 11
    or ax, ax
    EXPECT jz, 12
    call close
    mov dx, p_a
    mov al, 2
    call open
    EXPECT jnc, 13
    mov ax, 0x4200
    xor cx, cx
    mov dx, 500
    int 0x21
    xor cx, cx
    mov ah, 0x40                ; CX=0: truncate at the position
    int 0x21
    EXPECT jnc, 14
    mov ax, 0x4202
    xor cx, cx
    xor dx, dx
    int 0x21
    cmp ax, 500
    EXPECT je, 15
    call close
    mov dx, p_none
    mov al, 0
    call open
    EXPECT jc, 16
    cmp ax, 2
    EXPECT je, 17
    mov dx, p_nodir
    mov al, 0
    call open
    EXPECT jc, 18
    cmp ax, 3
    EXPECT je, 19
    xor ax, ax
    ret

t_dup:
    call sample
    mov dx, p_a
    mov al, 0
    call open
    EXPECT jnc, 1
    mov [h1], bx
    mov ah, 0x45
    int 0x21
    EXPECT jnc, 2
    mov [h2], ax
    mov bx, [h1]
    call close
    mov bx, [h2]
    mov dx, buf2
    mov cx, 10
    mov ah, 0x3f
    int 0x21
    EXPECT jnc, 3
    cmp ax, 10
    EXPECT je, 4
    mov dx, p_a
    mov al, 0
    call open
    EXPECT jnc, 5
    mov [h3], bx
    mov bx, [h2]
    mov cx, [h3]
    mov ah, 0x46                ; forced dup: h3 -> h2's SFT (old h3 closed)
    int 0x21
    EXPECT jnc, 6
    mov bx, [h3]
    mov dx, buf2+10
    mov cx, 10
    mov ah, 0x3f
    int 0x21
    EXPECT jnc, 7
    cmp ax, 10
    EXPECT je, 8
    mov si, buf
    mov di, buf2
    mov cx, 20
    repe cmpsb                  ; shared position: bytes 0-19 in order
    EXPECT je, 9
    mov bx, [h2]
    call close
    mov bx, [h3]
    call close
    EXPECT jnc, 10
    mov bx, [h3]
    call close
    EXPECT jc, 11               ; closed twice: invalid handle
    call sample
    xor ax, ax
    ret

t_find:
    mov dx, p_b
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 1
    mov bx, ax
    call close
    mov dx, dta1
    mov ah, 0x1a
    int 0x21
    mov dx, p_wild
    xor cx, cx
    mov ah, 0x4e
    int 0x21
    EXPECT jnc, 2
    test byte [dta1], 0x80
    EXPECT jnz, 3
    mov si, dta1
    mov di, dta2
    mov cx, 43
    rep movsb
    mov ah, 0x4f
    int 0x21
    EXPECT jnc, 4
    mov si, dta1+0x1e
    mov di, name2
    mov cx, 13
    rep movsb
    mov ah, 0x4f
    int 0x21
    EXPECT jc, 5
    cmp ax, 0x12
    EXPECT je, 6
    mov dx, dta2
    mov ah, 0x1a
    int 0x21
    mov ah, 0x4f
    int 0x21
    EXPECT jnc, 7
    mov si, dta2+0x1e
    mov di, name2
    mov cx, 13
    repe cmpsb
    EXPECT je, 8
    mov dx, 0x80
    mov ah, 0x1a
    int 0x21
    xor ax, ax
    ret

t_attr:
    mov dx, p_b
    mov cx, 1
    mov ax, 0x4301
    int 0x21
    EXPECT jnc, 1
    mov ax, 0x4300
    int 0x21
    EXPECT jnc, 2
    test cl, 1
    EXPECT jnz, 3
    mov al, 1
    call open
    EXPECT jc, 4
    cmp ax, 5
    EXPECT je, 5
    mov dx, p_b
    xor cx, cx
    mov ax, 0x4301
    int 0x21
    EXPECT jnc, 6
    xor ax, ax
    ret

t_rename:
    mov dx, p_b
    mov di, p_c
    mov ah, 0x56
    int 0x21
    EXPECT jnc, 1
    mov dx, p_b
    mov al, 0
    call open
    EXPECT jc, 2
    cmp ax, 2
    EXPECT je, 3
    mov dx, p_c
    mov ah, 0x41
    int 0x21
    EXPECT jnc, 4
    mov dx, p_c
    mov ah, 0x41
    int 0x21
    EXPECT jc, 5
    cmp ax, 2
    EXPECT je, 6
    xor ax, ax
    ret

t_dirs:
    mov dx, p_sub
    mov ah, 0x39
    int 0x21
    EXPECT jnc, 1
    mov ah, 0x39
    int 0x21
    EXPECT jc, 2
    cmp ax, 5
    EXPECT je, 3
    mov ah, 0x3b
    int 0x21
    EXPECT jnc, 4
    mov ah, 0x47
    xor dl, dl
    mov si, buf2
    int 0x21
    EXPECT jnc, 5
    mov si, buf2
    mov di, s_sub
    mov cx, 8
    repe cmpsb
    EXPECT je, 6
    mov dx, p_sub
    mov ah, 0x3a
    int 0x21
    EXPECT jc, 7
    cmp ax, 0x10
    EXPECT je, 8
    mov dx, p_root
    mov ah, 0x3b
    int 0x21
    EXPECT jnc, 9
    mov dx, p_sub
    mov ah, 0x3a
    int 0x21
    EXPECT jnc, 10
    mov ah, 0x3b
    int 0x21
    EXPECT jc, 11
    cmp ax, 3
    EXPECT je, 12
    xor ax, ax
    ret

t_space:
    mov ah, 0x36
    mov dl, 3
    int 0x21
    cmp ax, 0xffff
    EXPECT jne, 1
    cmp cx, 512
    EXPECT je, 2
    cmp bx, dx
    EXPECT jbe, 3
    or dx, dx
    EXPECT jnz, 4
    ; 36h: Win98 caps total/free at 2 GB - 32 KB (spc*bps*clusters).
    mov ax, 0x7303                  ; FreeCOM DIR's call; network drive path
    mov dx, sp_root
    push ds
    pop es
    mov di, xfs_buf
    mov cx, 44
    stc
    int 0x21
    EXPECT jnc, 5
    mov ax, [xfs_buf+4]             ; sectors per cluster (low word)
    cmp ax, 128                     ; media byte leaking into AH gave 0xF8xx
    EXPECT jbe, 6
    cmp word [xfs_buf+6], 0
    EXPECT je, 7
    mov ax, [xfs_buf+8]
    cmp ax, 512
    EXPECT je, 8
    mov ax, [xfs_buf+12]            ; free clusters (low word)
    or ax, [xfs_buf+14]
    EXPECT jnz, 9
    mov ax, [xfs_buf+16]            ; total clusters
    cmp ax, [xfs_buf+12]
    EXPECT jae, 10                  ; total >= free (volume <= 2 GB here)
    ; free bytes = free_clusters * spc * 512 must stay below 2 GB.
    mov ax, [xfs_buf+12]
    mul word [xfs_buf+4]            ; dx:ax = clusters*spc (sectors)
    cmp dx, 0x4000                  ; sectors < 2^22 -> bytes < 2 GB
    EXPECT jb, 11
    xor ax, ax
    ret

t_lock:
    mov dx, p_a
    mov al, 0x42
    call open
    EXPECT jnc, 1
    mov [h1], bx
    mov dx, p_a
    mov al, 0x42
    call open
    EXPECT jnc, 2
    mov [h2], bx
    mov bx, [h1]
    xor cx, cx
    xor dx, dx
    xor si, si
    mov di, 100
    mov ax, 0x5c00
    int 0x21
    EXPECT jnc, 3
    mov bx, [h2]
    mov ax, 0x5c00
    int 0x21
    EXPECT jc, 4
    cmp ax, 0x21
    EXPECT je, 5
    mov bx, [h1]
    mov ax, 0x5c01
    int 0x21
    EXPECT jnc, 6
    mov bx, [h2]
    mov ax, 0x5c00
    int 0x21
    EXPECT jnc, 7
    mov ax, 0x5c01
    int 0x21
    EXPECT jnc, 8
    mov bx, [h1]
    call close
    mov bx, [h2]
    call close
    xor ax, ax
    ret

t_extopen:
    mov si, p_e
    mov bx, 2
    xor cx, cx
    mov dx, 0x12
    mov ax, 0x6c00
    int 0x21
    EXPECT jnc, 1
    cmp cx, 2
    EXPECT je, 2
    mov bx, ax
    call close
    mov si, p_e
    mov bx, 2
    xor cx, cx
    mov dx, 0x12
    mov ax, 0x6c00
    int 0x21
    EXPECT jnc, 3
    cmp cx, 3
    EXPECT je, 4
    mov bx, ax
    call close
    mov si, p_e
    xor bx, bx
    xor cx, cx
    mov dx, 0x01
    mov ax, 0x6c00
    int 0x21
    EXPECT jnc, 5
    cmp cx, 1
    EXPECT je, 6
    mov bx, ax
    call close
    mov si, p_e
    xor bx, bx
    xor cx, cx
    mov dx, 0x10
    mov ax, 0x6c00
    int 0x21
    EXPECT jc, 7
    cmp ax, 0x50
    EXPECT je, 8
    mov si, p_none
    xor bx, bx
    xor cx, cx
    mov dx, 0x01
    mov ax, 0x6c00
    int 0x21
    EXPECT jc, 9
    cmp ax, 2
    EXPECT je, 10
    mov dx, p_e
    mov ah, 0x41
    int 0x21
    EXPECT jnc, 11
    xor ax, ax
    ret

t_time:
    mov dx, p_a
    mov al, 2
    call open
    EXPECT jnc, 1
    mov cx, 0x6000
    mov dx, 0x5a21
    mov ax, 0x5701
    int 0x21
    EXPECT jnc, 2
    call close
    mov dx, p_a
    mov al, 0
    call open
    EXPECT jnc, 3
    mov ax, 0x5700
    int 0x21
    EXPECT jnc, 4
    cmp cx, 0x6000
    EXPECT je, 5
    cmp dx, 0x5a21
    EXPECT je, 6
    call close
    xor ax, ax
    ret

t_fcb:
    mov dx, buf
    mov ah, 0x1a
    int 0x21
    mov di, fcb
    call fcb_init
    mov dx, fcb
    mov ah, 0x16
    int 0x21
    cmp al, 0
    EXPECT je, 1
    mov word [fcb+0x0e], 1      ; record size 1
    mov dword [fcb+0x21], 0
    mov dx, fcb
    mov cx, 10
    mov ah, 0x28
    int 0x21
    cmp al, 0
    EXPECT je, 2
    mov dx, fcb
    mov ah, 0x10
    int 0x21
    cmp al, 0
    EXPECT je, 3
    mov di, fcb
    call fcb_init
    mov dx, fcb
    mov ah, 0x0f
    int 0x21
    cmp al, 0
    EXPECT je, 4
    cmp dword [fcb+0x10], 10
    EXPECT je, 5
    mov dword [fcb+0x10], 4     ; checked FCB-close size metadata
    mov dx, fcb
    mov ah, 0x10
    int 0x21
    cmp al, 0
    EXPECT je, 6
    mov dx, 0x80
    mov ah, 0x1a
    int 0x21
    mov dx, p_f
    mov al, 0
    call open
    EXPECT jnc, 7
    mov ax, 0x4202
    xor cx, cx
    xor dx, dx
    int 0x21
    cmp ax, 4
    EXPECT je, 8
    call close
    mov dx, p_f
    mov ah, 0x41
    int 0x21
    EXPECT jnc, 9
    xor ax, ax
    ret
fcb_init:                       ; DI = FCB for C:F.DAT (current directory: root)
    push di
    mov cx, 37
    xor al, al
    rep stosb
    pop di
    mov byte [di], 3
    mov si, fcb_name
    inc di
    mov cx, 11
    rep movsb
    ret

t_readonly:
    mov dx, p_ro
    mov al, 0
    call open
    EXPECT jnc, 1
    mov dx, buf2
    mov cx, 5
    mov ah, 0x3f
    int 0x21
    EXPECT jnc, 2
    cmp ax, 5
    EXPECT je, 3
    cmp dword [buf2], 'HELL'
    EXPECT je, 4
    call close
    mov dx, p_ronew
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jc, 5
    cmp ax, 5
    EXPECT je, 6
    mov ax, 0x4409
    mov bl, 4
    int 0x21
    EXPECT jnc, 7
    test dx, 0x1000
    EXPECT jnz, 8
    xor ax, ax
    ret

install24:                      ; AL = reply
    mov [crit_reply], al
    mov word [crit_count], 0
    mov dx, crit
    mov ax, 0x2524
    int 0x21
    ret
crit:                           ; genuine INT 24h: IRET, then AX..ES, then the INT 21h frame
    push bp
    mov bp, sp
    push ax
    mov ax, [bp+8]              ; caller AX at the INT 21h
    mov [cs:crit_user_ax], ax
    pop ax
    mov [cs:crit_ax], ax
    mov [cs:crit_di], di
    inc word [cs:crit_count]
    mov al, [cs:crit_reply]
    pop bp
    iret

read100:                        ; BX handle -> CF/AX
    mov dx, buf2
    mov cx, 100
    mov ah, 0x3f
    int 0x21
    ret

t_retry:
    mov al, 1
    call install24
    mov dx, p_a
    mov al, 0
    call open
    EXPECT jnc, 1
    mov [h1], bx
    mov al, 0x11
    call fault
    mov bx, [h1]
    call read100
    EXPECT jnc, 2
    cmp ax, 100
    EXPECT je, 3
    mov ax, [crit_count]
    cmp ax, 1
    EXPECT je, 4
    mov ax, [crit_ax]
    and ah, 0x18
    cmp ax, 0x1802
    EXPECT je, 5
    mov ax, [crit_di]
    cmp al, 0x0b
    EXPECT je, 6
    mov ax, [crit_user_ax]
    cmp ah, 0x3f
    EXPECT je, 7
    mov si, buf
    mov di, buf2
    mov cx, 100
    repe cmpsb
    EXPECT je, 8
    mov bx, [h1]
    call close
    xor ax, ax
    ret

t_fail:
    mov al, 3
    call install24
    mov dx, p_a
    mov al, 0
    call open
    EXPECT jnc, 1
    mov [h1], bx
    mov al, 0x1f
    call fault
    mov bx, [h1]
    call read100
    pushf
    push ax
    mov al, 0
    call fault
    pop ax
    popf
    EXPECT jc, 2
    cmp ax, 0x53
    EXPECT je, 3
    mov ax, [crit_count]
    cmp ax, 1
    EXPECT je, 4
    mov bx, [h1]
    call read100                ; the same handle works after Fail
    EXPECT jnc, 5
    mov bx, [h1]
    call close
    xor ax, ax
    ret

t_nocrit:
    mov al, 3
    call install24
    mov si, p_a
    mov bx, 0x2000
    xor cx, cx
    mov dx, 1
    mov ax, 0x6c00
    int 0x21
    EXPECT jnc, 1
    mov [h1], ax
    mov al, 0x1f
    call fault
    mov bx, [h1]
    call read100
    pushf
    push ax
    mov al, 0
    call fault
    pop ax
    popf
    EXPECT jc, 2
    cmp ax, 0x1e
    EXPECT je, 3
    mov ax, [crit_count]
    or ax, ax
    EXPECT jz, 4
    mov bx, [h1]
    call close
    xor ax, ax
    ret

t_abort:
    call sample
    mov word [exec_blk], 0
    mov word [exec_blk+2], tail_abort
    mov [exec_blk+4], cs
    mov word [exec_blk+6], 0x5c
    mov [exec_blk+8], cs
    mov word [exec_blk+10], 0x6c
    mov [exec_blk+12], cs
    mov dx, p_self
    mov bx, exec_blk
    mov ax, 0x4b00
    int 0x21
    cli                         ; EXEC may change SS:SP and segment registers
    mov ax, cs
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov sp, [test_sp]
    sub sp, 2                   ; run_test's return address
    sti
    mov al, 0
    call fault
    mov ah, 0x4d
    int 0x21
    mov [child_rc], ax
    ; K2043 clears ErrorMode at the 4Ch dispatch (inthndlr.c:470) before
    ; case 4Ch reads it (:1110), so 4Dh reports type 0 after an INT 24h
    ; Abort; MS-DOS reports 2. The pristine oracle is K2043: expect 0000h;
    ; a child that came back from the read exits with 4C07h instead.
    cmp ax, 0x0000
    EXPECT je, 1
    xor ax, ax
    ret

t_after:                        ; the binding and backend are still sane
    mov dx, p_a
    mov al, 0
    call open
    EXPECT jnc, 1
    call read100
    EXPECT jnc, 2
    cmp ax, 100
    EXPECT je, 3
    call close
    EXPECT jnc, 4
    mov dx, p_a
    mov ah, 0x41
    int 0x21
    EXPECT jnc, 5
    mov dx, p_dir
    mov ah, 0x3a
    int 0x21
    EXPECT jnc, 6
    xor ax, ax
    ret

child_abort:                    ; INT 24h answers Abort; never returns from read
    push cs
    pop ds
    mov al, 2
    call install24
    mov dx, p_a
    mov al, 0
    call open
    jc .bad
    mov al, 0x1f
    call fault
    call read100
.bad:
    mov si, msg_child
    call puts
    mov ax, 0x4c07
    int 0x21

t_local:                        ; activation refused with /LOCAL: C: is local FAT
    mov ax, 0x4409
    mov bl, 3
    int 0x21
    EXPECT jnc, 1
    test dx, 0x1000
    EXPECT jz, 2
    mov dx, p_l
    xor cx, cx
    mov ah, 0x3c
    int 0x21
    EXPECT jnc, 3
    mov bx, ax
    mov dx, msg_h57
    mov cx, 4
    mov ah, 0x40
    int 0x21
    EXPECT jnc, 4
    call close
    mov dx, p_l
    mov ah, 0x41
    int 0x21
    EXPECT jnc, 5
    xor ax, ax
    ret

msg_h57 db 'H57 ', 0
msg_pass db ' PASS', 13, 10, 0
msg_fail db ' FAIL ', 0
msg_ax db ' ax=', 0
msg_end db 'H57 END fails=', 0
msg_child db 'H57 CHILD returned from an aborted read', 13, 10, 0
crlf db 13, 10, 0
p_dir db 'C:\H57', 0
p_sub db 'C:\H57\SUB', 0
s_sub db 'H57\SUB', 0
p_root db 'C:\', 0
sp_root db 'C:\', 0
xfs_buf times 44 db 0
p_a db 'C:\H57\A.TXT', 0
p_b db 'C:\H57\B.TXT', 0
p_c db 'C:\H57\C.TXT', 0
p_e db 'C:\H57\E.TXT', 0
p_f db 'C:\F.DAT', 0
p_l db 'C:\LOCAL.TXT', 0
p_none db 'C:\H57\NONE.TXT', 0
p_nodir db 'C:\NODIR\X.TXT', 0
p_wild db 'C:\H57\*.TXT', 0
p_ro db 'D:\RO.TXT', 0
p_ronew db 'D:\NEW.TXT', 0
p_self db 'C:\H57.COM', 0
fcb_name db 'F       DAT'
tail_abort db 2, ' A', 13
align 2
mode db 0
fails dw 0
dbg_ax dw 0
cur_name dw 0
test_sp dw 0
h1 dw 0
h2 dw 0
h3 dw 0
crit_reply db 0
align 2
crit_count dw 0
crit_ax dw 0
crit_di dw 0
crit_user_ax dw 0
child_rc dw 0
exec_blk times 14 db 0
dta1 times 64 db 0
dta2 times 64 db 0
name2 times 16 db 0
fcb times 40 db 0
buf times 1024 db 0
buf2 times 1024 db 0
align 16
times 1024 db 0
stack_top:
image_end:
