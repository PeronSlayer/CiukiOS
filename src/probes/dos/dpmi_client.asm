; f3-04 DPMI 0.9 conformance client, assembled twice: DPMI16.COM (BITS32=0)
; and DPMI32.COM (BITS32=1). Every INT 31h row of dos-dpmi-contract.md is
; called with valid and invalid inputs on the real CPU (descriptor loads,
; LSL/LAR/VERW, exceptions, IRQs, mode switches). Results go to the VM
; record port E9h: "<NAME> BEGIN", then "<NAME> PASS" or
; "<NAME> FAIL <stage> <detail>" (two hex bytes each). Exit code 0 on PASS.
; DPMI 0.9 specification sections 5-19; research in f3-acceptance.md f3-04.
; SPDX-License-Identifier: GPL-2.0-only
%ifndef BITS32
%define BITS32 0
%endif
bits 16
org 100h

%if BITS32
%define W 4
%macro IRETX 0
    iretd
%endmacro
%macro RETFX 0
    o32 retf
%endmacro
%else
%define W 2
%macro IRETX 0
    iret
%endmacro
%macro RETFX 0
    retf
%endmacro
%endif

%define RM_EDI 0x00
%define RM_ESI 0x04
%define RM_EBP 0x08
%define RM_EBX 0x10
%define RM_EDX 0x14
%define RM_ECX 0x18
%define RM_EAX 0x1C
%define RM_FL  0x20
%define RM_ES  0x22
%define RM_DS  0x24
%define RM_IP  0x2A
%define RM_CS  0x2C
%define RM_SP  0x2E
%define RM_SS  0x30

%macro DPMI 1
    mov ax, %1
    int 31h
%endmacro
; DPMI call that must succeed / must fail (CF set, AX unchanged).
%macro OK 1
    mov byte [detail], (%1) & 0FFh
    DPMI %1
    jc fail
%endmacro
%macro REFUSE 1
    mov byte [detail], (%1) & 0FFh
    DPMI %1
    jnc fail
    cmp ax, %1
    jne fail
%endmacro
%macro STAGE 1
    mov byte [stage], %1
%endmacro
%macro EXPECT 2                 ; jcc-on-failure, detail
    mov byte [detail], %2
    j%1 fail
%endmacro

start:
    cld
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, stack_top
    mov [rseg], ax
    STAGE 1
    mov bx, (image_end - $$ + 100h + 15) >> 4   ; keep only this image (PSP included)
    mov ah, 4Ah
    int 21h
    jc rm_fail
    mov ax, 1687h
    int 2Fh
    mov byte [detail], 1
    test ax, ax
    jnz rm_fail
    mov byte [detail], 2
    test bl, 1
    jz rm_fail
    mov byte [detail], 3
    cmp dx, 005Ah
    jne rm_fail
    mov byte [detail], 4
    test si, si
    jnz rm_fail
    mov [entry], di
    mov [entry + 2], es
    mov [cpu_class], cl
    push ds
    pop es
    mov byte [detail], 5
    mov ax, BITS32
    call far [entry]
    jc rm_fail

; ---------------------------------------------------------------- PM ----
    mov [pm_cs], cs
    mov [pm_ds], ds
    mov [pm_ss], ss
    mov [psp_sel], es
    mov si, msg_begin
    call emit
    STAGE 2
    mov ax, cs
    and al, 3
    cmp al, 3                     ; ring 3 under Ciuki VMM
    EXPECT ne, 1
    mov bx, [psp_sel]
    lsl ax, bx
    EXPECT nz, 2
    cmp ax, 0FFh
    EXPECT ne, 3
    mov ax, ds
    cmp ax, [pm_ss]               ; DS = SS in a COM image: one selector
    EXPECT ne, 4
    mov ax, fs                    ; FS/GS are zero after entry
    test ax, ax
    EXPECT nz, 7
    mov ax, 1686h
    int 2Fh
    test ax, ax
    EXPECT nz, 5
    mov es, [psp_sel]
    mov bx, [es:2Ch]              ; environment converted to a selector
    lsl ax, bx
    EXPECT nz, 6
    push ds
    pop es

    STAGE 3                       ; 0400h
    OK 0400h
    cmp ax, 005Ah
    EXPECT ne, 1
    test bl, 1
    EXPECT z, 2
    cmp cl, 3
    EXPECT b, 3
    mov [pic_base], dh            ; current virtual PIC bases (DH/DL)
    REFUSE 0401h

    STAGE 4                       ; LDT 0000h-000Dh
    OK 0003h
    cmp ax, 8
    EXPECT ne, 1
    mov [sel_inc], ax
    mov cx, 2
    OK 0000h
    mov [sel_a], ax
    add ax, [sel_inc]
    mov [sel_b], ax
    mov cx, 0
    REFUSE 0000h
    mov bx, ds
    OK 0006h
    mov [ds_base], dx
    mov [ds_base + 2], cx
    mov bx, [sel_a]
    mov dx, [ds_base]
    mov cx, [ds_base + 2]
    add dx, alias_area
    adc cx, 0
    OK 0007h
    mov bx, [sel_a]
    OK 0006h
    mov ax, [ds_base]
    add ax, alias_area
    cmp dx, ax
    EXPECT ne, 2
    mov bx, [sel_a]
    xor cx, cx
    mov dx, 0FFh
    OK 0008h
    mov bx, [sel_a]
%if BITS32
    mov cx, 40F2h
%else
    mov cx, 00F2h
%endif
    OK 0009h
    mov bx, [sel_a]
    lsl ax, bx
    EXPECT nz, 3
    cmp ax, 0FFh
    EXPECT ne, 4
    lar ax, bx
    EXPECT nz, 5
    and ah, 0F0h
    cmp ah, 0F0h
    EXPECT ne, 6
    verw bx
    EXPECT nz, 7
    mov es, bx
    mov byte [es:10h], 5Ah        ; alias of DS:alias_area
    cmp byte [alias_area + 10h], 5Ah
    EXPECT ne, 8
    push ds
    pop es
    mov bx, [sel_a]
    mov cx, 0010h
    xor dx, dx                    ; > 1 MiB, low 12 bits clear
    REFUSE 0008h
    mov bx, [sel_a]
    mov cx, 009Ah                 ; DPL 0
    REFUSE 0009h
    mov bx, [sel_a]
    mov cx, 00FEh                 ; conforming code
    REFUSE 0009h
    mov bx, cs                    ; 000Ah: data alias of the code segment
    OK 000Ah
    mov es, ax
    mov byte [es:code_scratch], 0A5h
    cmp byte [cs:code_scratch], 0A5h
    EXPECT ne, 9
    push ds
    pop es
    mov bx, ax
    OK 0001h
    mov bx, ds
    REFUSE 000Ah                  ; not a code segment
    mov bx, [sel_a]               ; 000Bh/000Ch round trip with a new limit
    mov edi, desc_buf
    OK 000Bh
    mov word [desc_buf], 1FFh
    mov bx, [sel_b]
    mov edi, desc_buf
    OK 000Ch
    mov bx, [sel_b]
    lsl ax, bx
    EXPECT nz, 10
    cmp ax, 1FFh
    EXPECT ne, 11
    mov byte [desc_buf + 5], 92h  ; DPL 0: refused
    mov bx, [sel_b]
    mov edi, desc_buf
    REFUSE 000Ch
    mov bx, 0FFF7h                ; never allocated
    mov edi, desc_buf
    REFUSE 000Bh
    mov bx, 0FFF7h
    xor cx, cx
    xor dx, dx
    REFUSE 0007h
    mov bx, 0B800h                ; 0002h: same segment, same selector
    OK 0002h
    mov [sel_text], ax
    mov bx, 0B800h
    OK 0002h
    cmp ax, [sel_text]
    EXPECT ne, 12
    mov bx, ax
    lsl ax, bx
    EXPECT nz, 13
    cmp ax, 0FFFFh
    EXPECT ne, 14
    mov bx, [sel_text]
    REFUSE 0001h                  ; segment aliases are never freed
    mov cx, 15                    ; 000Dh: first free of the sixteen reserved
.specific:
    mov bx, cx
    shl bx, 3
    or bx, 7
    DPMI 000Dh
    jnc .got_specific
    loop .specific
    mov byte [detail], 15
    jmp fail
.got_specific:
    verr bx
    EXPECT nz, 16
    mov [sel_spec], bx
    REFUSE 000Dh
    mov bx, [sel_spec]
    OK 0001h
    mov bx, [sel_b]
    OK 0001h
    mov bx, [sel_b]
    REFUSE 0001h
    xor bx, bx
    REFUSE 0006h
    xor bp, bp                    ; 0002h fails only when no descriptor is left
.ldt_fill:
    mov cx, 1
    DPMI 0000h
    jc .ldt_full
    mov bx, bp
    add bx, bx
    mov [sel_list + bx], ax
    inc bp
    cmp bp, 1024
    jb .ldt_fill
    mov byte [detail], 17
    jmp fail
.ldt_full:
    cmp bp, 900                   ; most of the 1024-entry LDT was free
    EXPECT b, 18
    mov bx, 0B123h                ; a segment never mapped before
    REFUSE 0002h
.ldt_drain:
    dec bp
    mov bx, bp
    add bx, bx
    mov bx, [sel_list + bx]
    OK 0001h
    test bp, bp
    jnz .ldt_drain
    mov bx, 0B123h                ; descriptors again: the alias succeeds
    OK 0002h
    REFUSE 0004h
    REFUSE 0005h
    REFUSE 000Eh
    REFUSE 000Fh

    STAGE 5                       ; DOS memory 0100h-0102h
    mov bx, 100h
    OK 0100h
    mov [dos_seg], ax
    mov [dos_sel], dx
    mov es, dx
    mov word [es:0], 0C1A0h
    mov bx, dx
    lsl ax, bx
    EXPECT nz, 1
    cmp ax, 0FFFh
    EXPECT ne, 2
    mov bx, [dos_sel]
    OK 0006h
    mov ax, [dos_seg]
    shl ax, 4
    cmp dx, ax
    EXPECT ne, 3
    push ds
    pop es
    mov bx, 80h
    mov dx, [dos_sel]
    OK 0102h
    mov bx, [dos_sel]
    lsl ax, bx
    cmp ax, 7FFh
    EXPECT ne, 4
    mov dx, [dos_sel]
    OK 0101h
    mov dx, [dos_sel]
    mov byte [detail], 5
    DPMI 0101h
    jnc fail
    cmp ax, 9
    EXPECT ne, 6
    mov bx, 0FFFFh
    mov byte [detail], 7
    DPMI 0100h
    jnc fail
    cmp ax, 8
    EXPECT ne, 8
    test bx, bx
    EXPECT z, 9
    mov bx, 80h                   ; 0102h on a selector that is no DOS block
    mov dx, 0FFF7h
    mov byte [detail], 10
    DPMI 0102h
    jnc fail
    cmp ax, 9
    EXPECT ne, 11

    STAGE 6                       ; vectors 0200h-0205h
    mov bl, 60h
    OK 0200h
    mov [old60], dx
    mov [old60 + 2], cx
    mov bl, 60h
    mov cx, [rseg]
    mov dx, rm_int60
    OK 0201h
    mov bl, 60h
    OK 0200h
    cmp dx, rm_int60
    EXPECT ne, 1
    mov bl, 61h
    OK 0204h
    mov [old61], edx
    mov [old61 + 4], cx
    mov bl, 61h
    mov cx, cs
    mov edx, pm_int61
    OK 0205h
    int 61h
    cmp word [int61_hits], 1
    EXPECT ne, 2
    mov bl, 61h
    mov cx, ds                    ; a data selector is no handler
    mov edx, pm_int61
    REFUSE 0205h
    mov bl, 0Dh
    OK 0202h
    mov [old_exc13], edx
    mov [old_exc13 + 4], cx
    mov bl, 0
    OK 0202h
    mov [old_exc0], edx
    mov [old_exc0 + 4], cx
    mov bl, 20h
    REFUSE 0202h
    mov bl, 20h
    mov cx, cs
    mov edx, pm_exc
    REFUSE 0203h
    REFUSE 0210h
    REFUSE 0211h
    REFUSE 0212h
    REFUSE 0213h

    STAGE 7                       ; translation 0300h-0302h
    push word 0BEEFh
    push word 0CAFEh
    call rmcs_clear
    mov dword [rmcs + RM_EAX], 1234h
    mov word [rmcs + RM_FL], 0202h
    mov bl, 60h
    mov bh, 0
    mov cx, 2
    mov edi, rmcs
    OK 0300h
    add sp, 4
    cmp word [rmcs + RM_EAX], 1235h
    EXPECT ne, 1
    cmp word [rmcs + RM_ECX], 0CAFEh
    EXPECT ne, 2
    cmp word [rmcs + RM_EDX], 0BEEFh
    EXPECT ne, 3
    call rmcs_clear
    mov ax, [rseg]
    mov [rmcs + RM_CS], ax
    mov word [rmcs + RM_IP], rm_far
    xor cx, cx
    mov edi, rmcs
    OK 0301h
    cmp word [rmcs + RM_EAX], 7777h
    EXPECT ne, 4
    call rmcs_clear
    mov ax, [rseg]
    mov [rmcs + RM_CS], ax
    mov word [rmcs + RM_IP], rm_iretproc
    mov [rmcs + RM_SS], ax        ; caller-supplied real-mode stack
    mov word [rmcs + RM_SP], rm_stack_top
    xor cx, cx
    mov edi, rmcs
    OK 0302h
    cmp word [rmcs + RM_EAX], 8888h
    EXPECT ne, 5
    mov edi, 0FFF0h               ; structure past the DS limit
    xor cx, cx
    REFUSE 0300h
    mov edi, 0FFF0h
    xor cx, cx
    REFUSE 0301h
    mov edi, 0FFF0h
    xor cx, cx
    REFUSE 0302h
    call rmcs_clear               ; VCPI is never offered (INT 67h/DE00h)
    mov dword [rmcs + RM_EAX], 0DE00h
    mov bx, 67h
    xor cx, cx
    mov edi, rmcs
    OK 0300h
    cmp byte [rmcs + RM_EAX + 1], 0
    EXPECT e, 6
    mov bl, 60h                   ; restore INT 60h before anything else runs
    mov cx, [old60 + 2]
    mov dx, [old60]
    OK 0201h
    mov ah, 30h                   ; default reflection: registers only
    int 21h
    cmp al, 2
    EXPECT b, 7

    STAGE 8                       ; callbacks 0303h/0304h, eight nested levels
    push ds
    mov ax, cs
    mov ds, ax
    mov esi, cb_proc
    mov ax, ss
    mov es, ax
    mov edi, cb_struct
    mov byte [ss:detail], 1
    DPMI 0303h
    pop ds
    push ds
    pop es
    jc fail
    mov [cb_addr], dx
    mov [cb_addr + 2], cx
    mov word [depth], 0
    mov word [max_depth], 0
    mov word [cb_hits], 0
    call rmcs_clear
    mov ax, [rseg]
    mov [rmcs + RM_CS], ax
    mov word [rmcs + RM_IP], rm_call_cb
    mov word [rmcs + RM_FL], 0002h
    xor cx, cx
    mov edi, rmcs
    OK 0301h
    cmp word [rmcs + RM_EAX], 55AAh
    EXPECT ne, 2
    cmp word [cb_hits], 8
    EXPECT ne, 3
    cmp word [max_depth], 8
    EXPECT ne, 4
    mov cx, [cb_addr + 2]
    mov dx, [cb_addr]
    OK 0304h
    mov cx, [cb_addr + 2]
    mov dx, [cb_addr]
    REFUSE 0304h
    mov cx, 1234h
    mov dx, 5678h
    REFUSE 0304h
    xor bp, bp                    ; at least 16 callbacks, then CF
.cb_fill:
    push ds
    mov ax, cs
    mov ds, ax
    mov esi, cb_proc
    mov ax, ss
    mov es, ax
    mov edi, cb_struct
    DPMI 0303h
    pop ds
    push ds
    pop es
    jc .cb_full
    mov bx, bp
    shl bx, 2
    mov [sel_list + bx], dx
    mov [sel_list + bx + 2], cx
    inc bp
    cmp bp, 64
    jb .cb_fill
    mov byte [detail], 5
    jmp fail
.cb_full:
    cmp bp, 16
    EXPECT b, 6
.cb_free:
    dec bp
    mov bx, bp
    shl bx, 2
    mov dx, [sel_list + bx]
    mov cx, [sel_list + bx + 2]
    OK 0304h
    test bp, bp
    jnz .cb_free

    STAGE 9                       ; 0305h state save, 0306h raw switches
    OK 0305h
    mov [save_size], ax
    mov [rm_save], cx
    mov [rm_save + 2], bx
%if BITS32
    mov [pm_save], edi
    mov [pm_save + 4], si
%else
    mov [pm_save], di
    mov [pm_save + 2], si
%endif
    cmp ax, save_buf_end - save_buf
    EXPECT a, 1
    mov edi, save_buf
    xor al, al
    mov bx, 0A5A5h
%if BITS32
    o32 call far [pm_save]
%else
    call far [pm_save]
%endif
    cmp bx, 0A5A5h
    EXPECT ne, 2
    OK 0306h
    mov [rm2pm], cx
    mov [rm2pm + 2], bx
%if BITS32
    mov [pm2rm], edi
    mov [pm2rm + 4], si
%else
    mov [pm2rm], di
    mov [pm2rm + 2], si
%endif
    mov byte [raw_seen], 0
    mov [saved_sp], sp
    mov bp, 1357h
    mov ax, [rseg]
    mov cx, ax
    mov dx, ax
    mov si, ax
    mov ebx, rm_stack_top
    mov edi, rm_raw
%if BITS32
    o32 jmp far [cs:pm2rm]
%else
    jmp far [cs:pm2rm]
%endif
pm_raw_back:
    cmp bp, 1357h
    EXPECT ne, 3
    cmp byte [raw_seen], 1
    EXPECT ne, 4
    mov ax, fs
    test ax, ax
    EXPECT nz, 5
    mov ax, ds
    cmp ax, [pm_ds]
    EXPECT ne, 6

    STAGE 10                      ; memory 0500h-0503h, 0600h-0604h, 0700h-0703h
    mov edi, meminfo
    OK 0500h
    cmp dword [meminfo], 30000h
    EXPECT b, 1
    mov bx, 2
    xor cx, cx                    ; 128 KiB
    OK 0501h
    mov [blk_lin], cx
    mov [blk_lin + 2], bx
    mov [blk_handle], di
    mov [blk_handle + 2], si
    mov cx, 1
    OK 0000h
    mov [sel_m], ax
    call blk_select
    mov es, [sel_m]
    mov ebx, 10010h
    mov byte [es:ebx], 77h        ; above 64 KiB through a large limit
    cmp byte [es:ebx], 77h
    EXPECT ne, 2
    push ds
    pop es
    mov bx, 3
    xor cx, cx
    mov si, [blk_handle + 2]
    mov di, [blk_handle]
    OK 0503h
    mov [blk_lin], cx
    mov [blk_lin + 2], bx
    mov [blk_handle], di
    mov [blk_handle + 2], si
    call blk_select
    mov es, [sel_m]
    mov ebx, 10010h
    cmp byte [es:ebx], 77h        ; contents survive a moving resize
    EXPECT ne, 3
    mov byte [es:0004h], 31h
    mov byte [es:1004h], 32h
    push ds
    pop es
    mov bx, [blk_lin + 2]
    mov cx, [blk_lin]
    xor si, si
    mov di, 1000h
    OK 0600h
    mov bx, [blk_lin + 2]
    mov cx, [blk_lin]
    xor si, si
    mov di, 1000h
    OK 0601h
    mov bx, [blk_lin + 2]
    mov cx, [blk_lin]
    xor si, si
    mov di, 1000h
    REFUSE 0601h                  ; not locked any more
    mov bx, 0C000h
    xor cx, cx
    xor si, si
    mov di, 10h
    REFUSE 0600h                  ; never kernel memory
    mov bx, [ds_base + 2]
    mov cx, [ds_base]
    xor si, si
    mov di, 1000h
    OK 0602h
    mov bx, [ds_base + 2]
    mov cx, [ds_base]
    xor si, si
    mov di, 1000h
    OK 0603h
    OK 0604h
    test bx, bx
    EXPECT nz, 4
    cmp cx, 1000h
    EXPECT ne, 5
    REFUSE 0700h
    REFUSE 0701h
    mov bx, [blk_lin + 2]
    mov cx, [blk_lin]
    xor si, si
    mov di, 2000h
    OK 0702h
    mov bx, [blk_lin + 2]
    mov cx, [blk_lin]
    add cx, 800h
    adc bx, 0
    xor si, si
    mov di, 1800h                 ; partial first page, whole second page
    OK 0703h
    mov es, [sel_m]
    cmp byte [es:0004h], 31h
    EXPECT ne, 6
    cmp byte [es:1004h], 0
    EXPECT ne, 7
    push ds
    pop es
    mov bx, 0C000h
    xor cx, cx
    xor si, si
    mov di, 1000h
    REFUSE 0703h
    mov si, [blk_handle + 2]
    mov di, [blk_handle]
    OK 0502h
    mov si, [blk_handle + 2]
    mov di, [blk_handle]
    REFUSE 0502h
    xor bx, bx
    xor cx, cx
    REFUSE 0501h
    mov si, [blk_handle + 2]
    mov di, [blk_handle]
    xor bx, bx
    mov cx, 1000h
    REFUSE 0503h
    REFUSE 0504h
    REFUSE 0505h
    REFUSE 0506h
    REFUSE 0507h
    REFUSE 0508h
    REFUSE 0509h
    REFUSE 050Ah
    REFUSE 050Bh

    STAGE 11                      ; 0800h, 0900h-0902h, 0A00h, 1.0-only rows
    mov bx, 0FEC0h
    xor cx, cx
    xor si, si
    mov di, 1000h
    REFUSE 0800h
    cmp bx, 0FEC0h                ; no output register changed
    EXPECT ne, 1
    REFUSE 0801h
    mov ax, 0900h
    int 31h
    mov [vif_old], al
    mov ax, 0902h
    int 31h
    cmp al, 0
    EXPECT ne, 2
    mov ax, 0901h
    int 31h
    cmp ax, 0900h                 ; previous state disabled, AH unchanged
    EXPECT ne, 3
    mov ax, 0902h
    int 31h
    cmp ax, 0901h
    EXPECT ne, 4
    cli
    mov ax, 0902h
    int 31h
    cmp al, 0                     ; CLI and 090xh share the virtual IF
    EXPECT ne, 5
    sti
    mov ax, 0902h
    int 31h
    cmp al, 1
    EXPECT ne, 6
    mov esi, vendor
    REFUSE 0A00h
    mov si, refused
.refused:
    mov ax, [si]
    test ax, ax
    jz .refused_done
    mov [ds:refuse_ax], ax
    mov byte [detail], al
    int 31h
    jnc fail
    cmp ax, [ds:refuse_ax]
    jne fail
    add si, 2
    jmp .refused
.refused_done:

    STAGE 12                      ; watchpoints 0B00h-0B03h
    mov bx, [ds_base + 2]
    mov cx, [ds_base]
    add cx, watch_var
    adc bx, 0
    mov dx, 0102h                 ; two bytes, write
    OK 0B00h
    mov [watch], bx
    mov bx, [watch]
    OK 0B02h
    test al, 1
    EXPECT nz, 1
    mov word [watch_var], 1234h   ; hit: recorded, execution continues
    mov bx, [watch]
    OK 0B02h
    test al, 1
    EXPECT z, 2
    mov bx, [watch]
    OK 0B03h
    mov bx, [watch]
    OK 0B02h
    test al, 1
    EXPECT nz, 3
    mov bx, [watch]
    OK 0B01h
    mov bx, [watch]
    REFUSE 0B01h
    mov bx, [watch]               ; a released handle
    REFUSE 0B02h
    mov bx, [watch]
    REFUSE 0B03h
    mov bx, 0C000h
    xor cx, cx
    mov dx, 0104h
    REFUSE 0B00h                  ; kernel linear address
    mov bx, [ds_base + 2]
    mov cx, [ds_base]
    add cx, watch_var + 1
    adc bx, 0
    mov dx, 0102h
    REFUSE 0B00h                  ; misaligned

    STAGE 13                      ; exceptions through 0203h handlers
    mov bl, 0Dh
    mov cx, cs
    mov edx, pm_exc
    OK 0203h
    mov bl, 0
    mov cx, cs
    mov edx, pm_exc
    OK 0203h
    mov word [exc_hits], 0
    mov word [skip], 5
    mov word [exc_want], 0
    mov word [exc_bad], 0
    mov word [exc_at], lgdt_at
lgdt_at:
    lgdt [lgdt_area]              ; privileged at ring 3: #GP(0)
    cmp word [exc_hits], 1
    EXPECT ne, 1
    mov word [skip], 4
    mov word [exc_at], limit_at
    mov es, [sel_a]               ; limit 0FFh
limit_at:
    mov al, [es:200h]             ; beyond the limit: #GP(0)
    cmp word [exc_hits], 2
    EXPECT ne, 2
    push ds
    pop es
    mov cx, 1                     ; stale selector: freed, then loaded
    OK 0000h
    mov bx, ax
    mov [exc_want], ax
    and word [exc_want], 0FFFCh
    OK 0001h
    mov ax, bx
    mov word [skip], 2
    mov word [exc_at], stale_at
stale_at:
    mov es, ax                    ; #GP(selector)
    cmp word [exc_hits], 3
    EXPECT ne, 3
    push ds
    pop es
    mov word [exc_want], 0
    mov word [skip], 2
    mov word [exc_at], div_at
    mov ax, 1
    xor dl, dl
div_at:
    div dl                        ; #DE
    cmp word [exc_hits], 4
    EXPECT ne, 4
    cmp word [exc_bad], 0
    EXPECT ne, 5
    mov bl, 0Dh
    mov cx, [old_exc13 + 4]
    mov edx, [old_exc13]
    OK 0203h
    mov bl, 0
    mov cx, [old_exc0 + 4]
    mov edx, [old_exc0]
    OK 0203h

    STAGE 14                      ; virtual IRQ0 to a protected-mode handler
    mov bl, [pic_base]
    OK 0204h
    mov [old_irq0], edx
    mov [old_irq0 + 4], cx
%if BITS32 == 0
    mov [old_irq0_16], dx
    mov [old_irq0_16 + 2], cx
%endif
    mov dword [ticks], 0
    mov bl, [pic_base]
    mov cx, cs
    mov edx, pm_irq0
    OK 0205h
    in al, 21h
    mov [imr], al
    and al, 0FEh                  ; IRQ0 enabled (8259A: bit clear = enabled)
    out 21h, al
    sti
.wait3:
    cli                           ; test with IF clear, sleep with STI; HLT
    cmp dword [ticks], 3
    jae .waited
    sti
    hlt
    jmp .wait3
.waited:
    sti
    cli                           ; held off: no delivery while IF is clear
    mov ebx, [ticks]
    mov cx, 2
    call pit_wraps
    cmp ebx, [ticks]
    EXPECT ne, 1
    mov ax, 0902h
    int 31h
    cmp al, 0
    EXPECT ne, 2
    sti
.pending:
    cli
    cmp ebx, [ticks]
    jne .delivered
    sti
    hlt
    jmp .pending
.delivered:
    sti
    mov bl, [pic_base]
    mov cx, [old_irq0 + 4]
    mov edx, [old_irq0]
    OK 0205h
    mov al, [imr]
    out 21h, al

    STAGE 15                      ; nested client and opposite-width entry
    call rmcs_clear
    mov ax, [rseg]
    mov [rmcs + RM_CS], ax
    mov word [rmcs + RM_IP], rm_opposite
    xor cx, cx
    mov edi, rmcs
    OK 0301h
    cmp word [rmcs + RM_EAX], 0   ; entry refused with CF
    EXPECT ne, 1
    mov bx, 1                     ; keep a block alive across the child
    xor cx, cx
    OK 0501h
    mov [blk_lin], cx
    mov [blk_lin + 2], bx
    call blk_select
    mov es, [sel_m]
    mov word [es:0], 6B6Bh
    push ds
    pop es
    call rmcs_clear               ; EXEC the same-width child client
    mov ax, [rseg]
    mov [rmcs + RM_DS], ax
    mov [rmcs + RM_ES], ax
    mov word [rmcs + RM_EDX], child_name
    mov word [rmcs + RM_EBX], exec_block
    mov [exec_block + 4], ax
    mov [exec_block + 8], ax
    mov [exec_block + 12], ax
    mov dword [rmcs + RM_EAX], 4B00h
    mov bx, 21h
    xor cx, cx
    mov edi, rmcs
    OK 0300h
    test byte [rmcs + RM_FL], 1
    EXPECT nz, 2
    call rmcs_clear
    mov dword [rmcs + RM_EAX], 4D00h
    mov bx, 21h
    xor cx, cx
    mov edi, rmcs
    OK 0300h
    cmp byte [rmcs + RM_EAX], 2Ah ; the child's exit code
    EXPECT ne, 3
    mov word [int61_hits], 0      ; the parent's vector and block survived
    int 61h
    cmp word [int61_hits], 1
    EXPECT ne, 4
    mov es, [sel_m]
    cmp word [es:0], 6B6Bh
    EXPECT ne, 5
    push ds
    pop es

%if BITS32
    STAGE 16                      ; a 32-bit code segment calling INT 31h
    mov cx, 1
    OK 0000h
    mov [sel_x], ax
    mov bx, cs
    OK 0006h
    mov bx, [sel_x]
    OK 0007h
    mov bx, [sel_x]
    xor cx, cx
    mov dx, 0FFFFh
    OK 0008h
    mov bx, [sel_x]
    mov cx, 40FAh
    OK 0009h
    mov ax, [sel_x]
    mov [far32 + 4], ax
    mov dword [far32], code32
    o32 call far [far32]
    cmp eax, 0C0DE005Ah
    EXPECT ne, 1
%endif

    ; Leave a block, a selector, a callback and a watchpoint allocated:
    ; the host's single cleanup path must release them at INT 21h/4Ch.
    STAGE 17
    mov cx, 1
    OK 0000h
    mov bx, 1
    xor cx, cx
    OK 0501h
    push ds
    mov ax, cs
    mov ds, ax
    mov esi, cb_proc
    mov ax, ss
    mov es, ax
    mov edi, cb_struct
    DPMI 0303h
    pop ds
    push ds
    pop es
    mov byte [detail], 1
    jc fail
    mov bx, [ds_base + 2]
    mov cx, [ds_base]
    add cx, watch_var
    adc bx, 0
    mov dx, 0102h
    OK 0B00h
    mov si, msg_pass
    call emit
    mov ax, 4C00h
    int 21h                       ; protected-mode termination

fail:
    mov al, [stage]
    mov di, msg_fail_hex
    call hex8
    mov al, [detail]
    mov di, msg_fail_hex + 3
    call hex8
    mov si, msg_fail
    call emit
    mov ax, 4C01h
    int 21h

; DS:SI ASCIIZ line to the record port (works in both modes).
emit:
    push ax
    push dx
    push si
    mov dx, 0E9h
.byte:
    lodsb
    test al, al
    jz .done
    out dx, al
    jmp .byte
.done:
    mov al, 10
    out dx, al
    pop si
    pop dx
    pop ax
    ret
hex8:
    push ax
    shr al, 4
    call .digit
    pop ax
    and al, 0Fh
.digit:
    add al, '0'
    cmp al, '9'
    jbe .store
    add al, 7
.store:
    mov [di], al
    inc di
    ret

rmcs_clear:
    push di
    push cx
    push ax
    mov di, rmcs
    mov cx, 32h
    xor al, al
    rep stosb
    pop ax
    pop cx
    pop di
    ret

; Point sel_m at the block (base blk_lin, limit 3FFFFh).
blk_select:
    mov bx, [sel_m]
    mov cx, [blk_lin + 2]
    mov dx, [blk_lin]
    OK 0007h
    mov bx, [sel_m]
    mov cx, 3
    mov dx, 0FFFFh
    OK 0008h
    ret

; Count CX wraps of the virtual PIT channel 0 counter (IF may be clear).
pit_wraps:
    push ax
    push dx
    call pit_read
    mov dx, ax
.next:
    call pit_read
    cmp ax, dx
    mov dx, ax
    jbe .next                     ; counts down; a wrap reads higher
    loop .next
    pop dx
    pop ax
    ret
pit_read:
    mov al, 0
    out 43h, al                   ; latch channel 0
    in al, 40h
    mov ah, al
    in al, 40h
    xchg al, ah
    ret

; ---- protected-mode handlers ----
pm_int61:
    push ds
    push ax
    mov ax, [cs:pm_ds]
    mov ds, ax
    inc word [int61_hits]
    pop ax
    pop ds
    IRETX

pm_irq0:
    push ds
    push ax
    mov ax, [cs:pm_ds]
    mov ds, ax
    inc dword [ticks]
    pop ax
    pop ds
%if BITS32
    o32 jmp far [cs:old_irq0]     ; chain to the default: real-mode reflection
%else
    jmp far [cs:old_irq0_16]
%endif

; Exception handler: checks the faulting IP and error code, skips [skip].
pm_exc:
    push ds
    push ax
    push bp
    mov ax, [cs:pm_ds]
    mov ds, ax
%if BITS32
    mov ebp, esp
    mov ax, [ebp + 6 + 8]         ; error code
    cmp ax, [exc_want]
    je .err_ok
    inc word [exc_bad]
.err_ok:
    mov ax, [ebp + 6 + 12]        ; EIP
    cmp ax, [exc_at]
    je .ip_ok
    inc word [exc_bad]
.ip_ok:
    add ax, [skip]
    mov [ebp + 6 + 12], ax
%else
    mov bp, sp
    mov ax, [bp + 6 + 4]
    cmp ax, [exc_want]
    je .err_ok
    inc word [exc_bad]
.err_ok:
    mov ax, [bp + 6 + 6]
    cmp ax, [exc_at]
    je .ip_ok
    inc word [exc_bad]
.ip_ok:
    add ax, [skip]
    mov [bp + 6 + 6], ax
%endif
    inc word [exc_hits]
    pop bp
    pop ax
    pop ds
    RETFX

; Real-mode callback procedure: DS:(E)SI = real SS:SP, ES:(E)DI = structure,
; locked stack, IF clear. Copies the structure per level (0.9 11.4: reentry),
; simulates the real-mode far return, nests through 0301h up to 8 levels.
cb_proc:
    push ds
    push es
%if BITS32
    pushad
%else
    pusha
%endif
    mov bx, ds                    ; real-mode stack selector
    mov ax, [cs:pm_ds]
    mov ds, ax
    mov ax, [depth]
    mov dx, ax
    inc ax
    cmp ax, [max_depth]
    jbe .depth_ok
    mov [max_depth], ax
.depth_ok:
    inc word [cb_hits]
    imul di, dx, 32h
    add di, cb_copy               ; DS:DI = this level's copy of cb_struct
    push ds
    pop es
    push di
    mov si, cb_struct
    mov cx, 32h
    rep movsb
    pop di
    mov es, bx                    ; ES = real-mode stack
    mov si, [di + RM_SP]
    mov ax, [es:si]
    mov [di + RM_IP], ax
    mov ax, [es:si + 2]
    mov [di + RM_CS], ax
    add word [di + RM_SP], 4      ; far return simulated
    mov word [di + RM_EAX], 55AAh
    cmp dx, 7
    jae .no_nest
    inc word [depth]
    push di
    imul di, dx, 32h
    add di, nest_rmcs
    push ds
    pop es
    push di
    mov cx, 32h
    xor al, al
    rep stosb
    pop di
    mov ax, [rseg]
    mov [di + RM_CS], ax
    mov word [di + RM_IP], rm_call_cb
    mov word [di + RM_FL], 0002h
%if BITS32
    movzx edi, di
%endif
    xor cx, cx
    mov ax, 0301h
    int 31h
    pop di
    dec word [depth]
    jnc .no_nest
    mov word [di + RM_EAX], 0BADh
.no_nest:
    mov [cb_ret_di], di
%if BITS32
    popad
%else
    popa
%endif
    pop es
    pop ds
    mov di, [cs:pm_ds]
    mov es, di
%if BITS32
    movzx edi, word [es:cb_ret_di]
%else
    mov di, [es:cb_ret_di]
%endif
    IRETX                         ; ES:(E)DI = the copy to restore

%if BITS32
bits 32
code32:                           ; 32-bit code segment, same base as CS
    push ds
    push es
    mov ax, [cs:pm_ds]
    mov ds, ax
    mov es, ax
    mov ax, 0400h
    int 31h
    movzx ebx, ax
    mov edi, meminfo
    mov ax, 0500h
    int 31h
    mov eax, 0C0DE0000h
    jc .out
    or eax, ebx
.out:
    pop es
    pop ds
    retf
bits 16
%endif

; ---- real-mode routines (V86) ----
rm_int60:                         ; INT 60h: AX+1, CX/DX = the two copied words
    push bp
    mov bp, sp
    inc ax
    mov cx, [bp + 8]
    mov dx, [bp + 10]
    pop bp
    iret
rm_far:
    mov ax, 7777h
    retf
rm_iretproc:
    mov ax, 8888h
    iret
rm_call_cb:
    call far [cs:cb_addr]
    retf
rm_opposite:
    mov ax, 1 - BITS32
    call far [cs:entry]
    mov ax, 0
    jc .refused
    mov ax, 1
.refused:
    retf
; Raw switch target: real mode with DS=ES=SS=CS=rseg.
rm_raw:
    cmp bp, 1357h
    jne .bad
    mov byte [raw_seen], 1
    mov di, save_buf              ; the real-mode save routine just returns
    xor al, al
    call far [rm_save]
.bad:
    mov ax, [pm_ds]
    mov cx, ax
    mov dx, [pm_ss]
    mov si, [pm_cs]
    movzx ebx, word [saved_sp]
    mov edi, pm_raw_back
    jmp far [rm2pm]

rm_fail:
    mov al, [stage]
    mov di, msg_fail_hex
    call hex8
    mov al, [detail]
    mov di, msg_fail_hex + 3
    call hex8
    mov si, msg_fail
    call emit
    mov ax, 4C01h
    int 21h

%if BITS32
msg_begin db 'DPMI32 BEGIN', 0
msg_pass db 'DPMI32 PASS', 0
child_name db '\DPMICH32.COM', 0
msg_fail db 'DPMI32 FAIL '
%else
msg_begin db 'DPMI16 BEGIN', 0
msg_pass db 'DPMI16 PASS', 0
child_name db '\DPMICH16.COM', 0
msg_fail db 'DPMI16 FAIL '
%endif
msg_fail_hex db '00 00', 0
vendor db 'CIUKI NONE', 0
refused dw 000Eh, 000Fh, 0210h, 0211h, 0212h, 0213h, 0401h, 0504h, 050Bh, 0801h
        dw 0C00h, 0C01h, 0D00h, 0D01h, 0D02h, 0D03h, 0E00h, 0E01h, 0F00h, 0FF00h, 0103h, 0B04h, 0
exec_block dw 0, 80h, 0, 5Ch, 0, 6Ch, 0
code_scratch db 0
align 4
entry dd 0
rseg dw 0
pm_cs dw 0
pm_ds dw 0
pm_ss dw 0
psp_sel dw 0
stage db 0
detail db 0
cpu_class db 0
pic_base db 0
vif_old db 0
imr db 0
raw_seen db 0
align 2
sel_inc dw 0
sel_a dw 0
sel_b dw 0
sel_text dw 0
sel_spec dw 0
sel_m dw 0
sel_x dw 0
ds_base dd 0
dos_seg dw 0
dos_sel dw 0
old60 dd 0
int61_hits dw 0
depth dw 0
max_depth dw 0
cb_hits dw 0
cb_ret_di dw 0
cb_addr dd 0
save_size dw 0
saved_sp dw 0
rm_save dd 0
rm2pm dd 0
watch dw 0
watch_var dw 0
exc_hits dw 0
exc_bad dw 0
exc_want dw 0
exc_at dw 0
skip dw 0
refuse_ax dw 0
old_irq0_16 dd 0
align 4
old61 dd 0, 0
old_exc13 dd 0, 0
old_exc0 dd 0, 0
old_irq0 dd 0, 0
pm_save dd 0, 0
pm2rm dd 0, 0
far32 dd 0, 0
ticks dd 0
blk_lin dd 0
blk_handle dd 0
lgdt_area dd 0, 0
alias_area times 100h db 0
desc_buf dd 0, 0
meminfo times 30h db 0
rmcs times 32h db 0
cb_struct times 32h db 0
cb_copy times 8 * 32h db 0
nest_rmcs times 8 * 32h db 0
save_buf times 64 db 0
save_buf_end:
align 16
rm_stack times 256 db 0
rm_stack_top:
stack times 1024 db 0
stack_top:
sel_list times 1024 dw 0         ; exhaustion tests (0000h/0002h, 0303h)
image_end:
