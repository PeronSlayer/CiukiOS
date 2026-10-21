; CIUKIDF.SYS - CiukiDOS DFV2 control driver, loaded by CONFIG.SYS
; (DEVICE=CIUKIDF.SYS). Private wire layout and stack diagrams:
; docs/validation/f3-56-unified-binding.md in the CiukiOS tree.
;
; Copyright (C) 2026 The CiukiOS contributors.
; This file is part of the CiukiDOS fork of the FreeDOS kernel. It is free
; software; you can redistribute it and/or modify it under the terms of the
; GNU General Public License as published by the Free Software Foundation;
; either version 2, or (at your option) any later version.
; SPDX-License-Identifier: GPL-2.0-or-later
;
; INIT (phase 0) discovers the DFV2 host, validates its immutable ROM
; descriptor and gates exactly, and registers only this driver's resident
; memory. The kernel's late binder (ciukinit.c) later calls late_bind with
; the final layout (phase 1). Without a valid host the driver installs
; nothing and FreeDOS stays unbound.

bits 16
cpu 8086
org 0

ROM_SEG         equ 0xf000
GATE_DIRECT     equ 0x1c00
GATE_LOCAL      equ 0x1c04
GATE_EVENT      equ 0x1c08
GATE_CRITICAL   equ 0x1c0c
DESCRIPTOR      equ 0x1c10
ROM_END         equ 0x1c40
FEATURES        equ 0x003f
CALL_BYTES      equ 48
BIND_BYTES      equ 0xd0
STACK_BYTES     equ 512
GENERAL_FAILURE equ 0x1f
FAIL_ON_INT24   equ 0x53            ; INT 24h answered Fail
C_RETRY         equ 0x2a            ; f3-57: 1 = Retry of a CRITICAL result
STATUS_DONE     equ 1
STATUS_CRITICAL equ 2
INPUT_WORDS     equ 13              ; record words 10h-29h

; call record offsets
C_VERSION       equ 0x00
C_BYTES         equ 0x02
C_GENERATION    equ 0x04
C_SERIAL        equ 0x08
C_ROLE          equ 0x0c
C_STATUS        equ 0x0e
C_FLAGS         equ 0x10
C_AX            equ 0x12
C_BX            equ 0x14
C_CX            equ 0x16
C_DX            equ 0x18
C_SI            equ 0x1a
C_DI            equ 0x1c
C_BP            equ 0x1e
C_DS            equ 0x20
C_ES            equ 0x22
C_FRAME_SS      equ 0x24
C_FRAME_SP      equ 0x26
C_ARG           equ 0x28

ROLE_DIRECT     equ 1
ROLE_LOCAL      equ 2
ROLE_EVENT      equ 3
ARITH_FLAGS     equ 0x08d5          ; OF SF ZF AF PF CF

; Switch to the private stack and enter one ROM gate. The far CALL leaves
; exactly CS:return-site at stack_top-4, which the VMM verifies. The gate
; is a synchronous monitor call: interrupts stay off on the private stack
; and the caller's IF is restored with its FLAGS on its own stack.
%macro GATE 2
                pushf
                cli
                mov     [cs:saved_ss], ss
                mov     [cs:saved_sp], sp
                push    cs
                pop     ss
                mov     sp, stack_top
                call    ROM_SEG:%1
%2:
                cli
                mov     ss, [cs:saved_ss]
                mov     sp, [cs:saved_sp]
                popf
%endmacro

header:
                dw      0xffff, 0xffff
                dw      0x8000                  ; character device
                dw      strategy
                dw      interrupt
                db      'CIUKIDF$'
control:                                        ; offset 12h, read by ciukinit.c
                db      'CIUKIDFC'
                dw      3
                dw      control_end - control
                dw      late_bind
                dw      event_entry
                dw      direct_entry
                dw      local_entry
                dw      state
                dw      policy
control_end:

state           db      0                       ; 1 registered, 2 bound, 3 refused
policy          db      0                       ; 1: DEVICE=...CIUKIDF.SYS /LOCAL
busy            db      0
request         dd      0
generation      dd      0
serial          dd      0
saved_ss        dw      0
saved_sp        dw      0
flags_out       dw      0
                align   2
call_record     times CALL_BYTES db 0

; ---- gate subroutines: one call instruction (return site) per role ----
gate_direct:
                GATE    GATE_DIRECT, rs_direct
                ret
gate_local:
                GATE    GATE_LOCAL, rs_local
                ret
gate_event:
                GATE    GATE_EVENT, rs_event
                ret

; AX = role, DS = CS. Never touches the register fields 10h-29h.
prep:
                mov     word [call_record+C_VERSION], 2
                mov     word [call_record+C_BYTES], CALL_BYTES
                mov     [call_record+C_ROLE], ax
                mov     ax, [generation]
                mov     [call_record+C_GENERATION], ax
                mov     ax, [generation+2]
                mov     [call_record+C_GENERATION+2], ax
                add     word [serial], 1
                adc     word [serial+2], 0
                mov     ax, [serial]
                mov     [call_record+C_SERIAL], ax
                mov     ax, [serial+2]
                mov     [call_record+C_SERIAL+2], ax
                mov     word [call_record+C_STATUS], 0xffff
                mov     word [call_record+0x2a], 0
                mov     word [call_record+0x2c], 0
                mov     word [call_record+0x2e], 0
                ret

; DS = CS after an event gate: AX = 0, the returned DOS error, or 1Fh.
event_result:
                mov     ax, GENERAL_FAILURE
                cmp     word [call_record+C_STATUS], 1
                jne     .done
                xor     ax, ax
                test    byte [call_record+C_FLAGS], 1
                jz      .done
                mov     ax, [call_record+C_AX]
                or      ax, ax
                jnz     .done
                mov     ax, GENERAL_FAILURE
.done:          ret

; int far cdecl event_entry(UWORD event, UWORD psp, UWORD parent, UWORD kind)
event_entry:
                push    bp
                mov     bp, sp
                pushf
                push    bx
                push    cx
                push    dx
                push    si
                push    di
                push    ds
                push    es
                push    cs
                pop     ds
                mov     ax, GENERAL_FAILURE
                cmp     byte [state], 2
                jne     .out
                cmp     byte [busy], 0
                jne     .out
                mov     byte [busy], 1
                mov     ax, ROLE_EVENT
                call    prep
                mov     ax, [bp+6]
                mov     [call_record+C_AX], ax
                mov     ax, [bp+8]
                mov     [call_record+C_BX], ax
                mov     ax, [bp+10]
                mov     [call_record+C_CX], ax
                mov     ax, [bp+12]
                mov     [call_record+C_DX], ax
                xor     ax, ax
                mov     [call_record+C_FLAGS], ax
                mov     [call_record+C_SI], ax
                mov     [call_record+C_DI], ax
                mov     [call_record+C_BP], ax
                mov     [call_record+C_DS], ax
                mov     [call_record+C_ES], ax
                mov     [call_record+C_FRAME_SS], ax
                mov     [call_record+C_FRAME_SP], ax
                mov     [call_record+C_ARG], ax
                call    gate_event
                mov     byte [busy], 0
                call    event_result
.out:
                pop     es
                pop     ds
                pop     di
                pop     si
                pop     dx
                pop     cx
                pop     bx
                popf
                pop     bp
                retf

; int far cdecl direct_entry(iregs far *r): 1 = served (r updated), 0 = not
direct_entry:
                push    bp
                mov     bp, sp
                pushf
                push    bx
                push    cx
                push    dx
                push    si
                push    di
                push    ds
                push    es
                push    cs
                pop     ds
                xor     ax, ax
                cmp     byte [state], 2
                jne     .out
                les     si, [bp+6]
                cmp     byte [busy], 0
                jne     .refused                ; no nested redispatch
                mov     byte [busy], 1
                mov     ax, ROLE_DIRECT
                call    prep
                xor     bx, bx
.copy_in:       mov     ax, [es:si+bx]          ; ax bx cx dx si di bp ds es
                mov     [call_record+C_AX+bx], ax
                add     bx, 2
                cmp     bx, 18
                jb      .copy_in
                mov     ax, [es:si+22]          ; original INT 21h FLAGS
                mov     [call_record+C_FLAGS], ax
                mov     [call_record+C_FRAME_SS], es
                mov     [call_record+C_FRAME_SP], si
                mov     word [call_record+C_ARG], 0
                call    gate_direct
                mov     byte [busy], 0
                les     si, [bp+6]
                xor     ax, ax
                cmp     word [call_record+C_STATUS], ax
                je      .out                    ; CHAIN: frame untouched
                cmp     word [call_record+C_STATUS], 1
                jne     .refused
                xor     bx, bx
.copy_out:      mov     ax, [call_record+C_AX+bx]
                mov     [es:si+bx], ax
                add     bx, 2
                cmp     bx, 18
                jb      .copy_out
                mov     ax, [call_record+C_FLAGS]
                and     ax, ARITH_FLAGS
                mov     bx, [es:si+22]
                and     bx, ~ARITH_FLAGS
                or      bx, ax
                mov     [es:si+22], bx
                mov     ax, 1
                jmp     short .out
.refused:       mov     word [es:si], GENERAL_FAILURE   ; no local fallback
                or      byte [es:si+22], 1
                mov     ax, 1
.out:
                pop     es
                pop     ds
                pop     di
                pop     si
                pop     dx
                pop     cx
                pop     bx
                popf
                pop     bp
                retf

;; Entered by "pushf; call far" at a kernel redirector callout:
; SS:SP -> IP, CS, FLAGS, stack argument (the 11xxh convention).
; f3-57: a CRITICAL result is raised through the kernel's own INT 2Fh/1206h
; (FreeDOS CriticalError: genuine INT 24h frame on the user stack, Abort
; restarts the call as 4C00h and never returns here, so busy is clear and
; the Retry copy lives on this kernel stack, not in the driver).
local_entry:
                cmp     byte [cs:state], 2
                jne     .chain
                cmp     byte [cs:busy], 0
                jne     .chain
                mov     byte [cs:busy], 1
                mov     [cs:call_record+C_AX], ax
                mov     [cs:call_record+C_BX], bx
                mov     [cs:call_record+C_CX], cx
                mov     [cs:call_record+C_DX], dx
                mov     [cs:call_record+C_SI], si
                mov     [cs:call_record+C_DI], di
                mov     [cs:call_record+C_BP], bp
                mov     [cs:call_record+C_DS], ds
                mov     [cs:call_record+C_ES], es
                mov     [cs:call_record+C_FRAME_SS], ss
                mov     [cs:call_record+C_FRAME_SP], sp
                mov     bp, sp
                mov     ax, [bp+4]
                mov     [cs:call_record+C_FLAGS], ax
                mov     ax, [bp+6]
                mov     [cs:call_record+C_ARG], ax
                push    cs
                pop     ds
                mov     si, call_record+C_FLAGS
                mov     cx, INPUT_WORDS
.save:          push    word [si]               ; flags first, argument on top
                add     si, 2
                loop    .save
                xor     bx, bx
.issue:         push    bx
                mov     ax, ROLE_LOCAL
                call    prep
                pop     bx
                mov     [call_record+C_RETRY], bx
                call    gate_local
                mov     ax, [call_record+C_STATUS]
                cmp     ax, STATUS_DONE
                je      .done_drop
                cmp     ax, STATUS_CRITICAL
                je      .critical
                add     sp, INPUT_WORDS*2
                ; CHAIN or refused: restore the complete unowned registers.
                mov     bx, [call_record+C_BX]
                mov     cx, [call_record+C_CX]
                mov     dx, [call_record+C_DX]
                mov     si, [call_record+C_SI]
                mov     di, [call_record+C_DI]
                mov     bp, [call_record+C_BP]
                mov     es, [call_record+C_ES]
                mov     ax, [call_record+C_AX]
                mov     ds, [cs:call_record+C_DS]
                mov     byte [cs:busy], 0
.chain:         ; jump to the current INT 2Fh vector with the frame intact
                sub     sp, 4
                push    bp
                mov     bp, sp
                push    ds
                push    ax
                xor     ax, ax
                mov     ds, ax
                mov     ax, [0x2f*4]
                mov     [bp+2], ax
                mov     ax, [0x2f*4+2]
                mov     [bp+4], ax
                push    word [bp+10]            ; caller FLAGS of the frame
                popf
                pop     ax
                pop     ds
                pop     bp
                retf
.critical:      ; AX = INT 24h AH flags/AL drive, DI = cause, BP:SI = device
                mov     byte [busy], 0
                mov     ax, [call_record+C_AX]
                mov     di, [call_record+C_DI]
                mov     si, [call_record+C_SI]
                mov     bp, [call_record+C_BP]
                push    ax                      ; 2F/1206h stack word (INT 24h AX)
                mov     ax, 0x1206
                int     0x2f
                add     sp, 2
                push    cs
                pop     ds
                mov     byte [busy], 1
                mov     bp, sp                  ; restore the original inputs
                mov     si, call_record+C_ARG
                xor     di, di
.restore:       mov     dx, [ss:bp+di]
                mov     [si], dx
                sub     si, 2
                add     di, 2
                cmp     di, INPUT_WORDS*2
                jb      .restore
                cmp     al, 1
                jne     .failed
                mov     bx, 1                   ; Retry: same registers, same frame
                jmp     .issue
.failed:        mov     word [call_record+C_AX], FAIL_ON_INT24
                or      byte [call_record+C_FLAGS], 1
.done_drop:     add     sp, INPUT_WORDS*2
.done:
                pushf
                pop     ax
                and     ax, ~ARITH_FLAGS
                mov     bx, [call_record+C_FLAGS]
                and     bx, ARITH_FLAGS
                or      ax, bx
                mov     [flags_out], ax
                mov     bx, [call_record+C_BX]
                mov     cx, [call_record+C_CX]
                mov     dx, [call_record+C_DX]
                mov     si, [call_record+C_SI]
                mov     di, [call_record+C_DI]
                mov     bp, [call_record+C_BP]
                mov     es, [call_record+C_ES]
                mov     ax, [call_record+C_AX]
                mov     ds, [cs:call_record+C_DS]
                mov     byte [cs:busy], 0
                push    word [cs:flags_out]
                popf
                retf    2

; int far cdecl late_bind(void far *record): phase 1, called once by the
; kernel after init_kernel(). The kernel filled 30h-AFh; this driver adds
; its registered fields and the generation.
late_bind:
                push    bp
                mov     bp, sp
                pushf
                push    bx
                push    cx
                push    dx
                push    si
                push    di
                push    ds
                push    es
                push    cs
                pop     ds
                mov     ax, GENERAL_FAILURE
                cmp     byte [state], 1
                jne     .out
                cmp     byte [busy], 0
                jne     .out
                mov     byte [state], 3          ; refused unless bound below
                les     di, [bp+6]
                cmp     word [es:di], 'DF'
                jne     .out
                cmp     word [es:di+2], 'B2'
                jne     .out
                cmp     word [es:di+4], 2
                jne     .out
                cmp     word [es:di+6], BIND_BYTES
                jne     .out
                cmp     word [es:di+8], 1
                jne     .out
                call    fill_driver_fields
                mov     byte [busy], 1
                mov     ax, ROLE_EVENT
                call    prep
                xor     ax, ax
                mov     [call_record+C_AX], ax  ; event 0: BIND
                mov     [call_record+C_BX], ax
                mov     [call_record+C_CX], ax
                mov     [call_record+C_DX], ax
                mov     [call_record+C_FLAGS], ax
                mov     [call_record+C_DI], ax
                mov     [call_record+C_BP], ax
                mov     [call_record+C_ES], ax
                mov     [call_record+C_FRAME_SS], ax
                mov     [call_record+C_FRAME_SP], ax
                mov     [call_record+C_ARG], ax
                mov     ax, [bp+6]
                mov     [call_record+C_SI], ax
                mov     ax, [bp+8]
                mov     [call_record+C_DS], ax
                call    gate_event
                mov     byte [busy], 0
                call    event_result
                or      ax, ax
                jnz     .out
                mov     byte [state], 2
.out:
                pop     es
                pop     ds
                pop     di
                pop     si
                pop     dx
                pop     cx
                pop     bx
                popf
                pop     bp
                retf

; ES:DI = bind record, DS = CS: generation and the registered driver fields.
fill_driver_fields:
                mov     ax, [generation]
                mov     [es:di+0x0c], ax
                mov     ax, [generation+2]
                mov     [es:di+0x0e], ax
                mov     [es:di+0x10], cs
                mov     word [es:di+0x12], resident_end
                mov     word [es:di+0x14], call_record
                mov     word [es:di+0x16], stack_bottom
                mov     word [es:di+0x18], STACK_BYTES
                mov     word [es:di+0x1a], rs_direct
                mov     word [es:di+0x1c], rs_local
                mov     word [es:di+0x1e], rs_event
                push    si
                push    cx
                push    di
                add     di, 0x20
                mov     si, driver_identity
                mov     cx, 8
                cld
                rep     movsw
                pop     di
                pop     cx
                pop     si
                ret

strategy:
                mov     [cs:request], bx
                mov     [cs:request+2], es
                retf

interrupt:
                push    ax
                push    bx
                push    cx
                push    dx
                push    si
                push    di
                push    bp
                push    ds
                push    es
                push    cs
                pop     ds
                les     bx, [request]
                cmp     byte [es:bx+2], 0
                jne     .unknown
                cmp     byte [state], 0
                jne     .unknown                ; INIT runs once
                call    init
                jmp     short .out
.unknown:       mov     word [es:bx+3], 0x8103  ; error, done, unknown command
.out:
                pop     es
                pop     ds
                pop     bp
                pop     di
                pop     si
                pop     dx
                pop     cx
                pop     bx
                pop     ax
                retf

driver_identity db      'CIUKIDF2 F3-57 1'

                align   16
stack_bottom    times STACK_BYTES db 0
stack_top:
resident_end:

; ---------------- discarded after INIT ----------------

; ES:BX = request header, DS = CS.
init:
                push    es
                push    bx
                call    parse_policy
                mov     ax, 0xc148
                mov     bx, 0x4644
                mov     cx, 2
                mov     dx, 0x564d
                int     0x2f
                cmp     ax, 0
                jne     .absent
                cmp     cx, 2
                jne     .absent
                mov     ax, es
                cmp     ax, ROM_SEG
                jne     .invalid
                cmp     di, DESCRIPTOR
                jne     .invalid
                call    validate_descriptor
                jc      .invalid
                mov     ax, [es:di+0x1c]
                mov     [generation], ax
                mov     ax, [es:di+0x1e]
                mov     [generation+2], ax
                ; phase 0 record: registration of resident memory only
                push    cs
                pop     es
                mov     di, bind0
                mov     cx, BIND_BYTES / 2
                xor     ax, ax
                cld
                rep     stosw
                mov     di, bind0
                mov     word [di], 'DF'
                mov     word [di+2], 'B2'
                mov     word [di+4], 2
                mov     word [di+6], BIND_BYTES
                mov     word [di+8], 0
                call    fill_driver_fields
                mov     byte [busy], 1
                mov     ax, ROLE_EVENT
                call    prep
                xor     ax, ax
                mov     [call_record+C_AX], ax
                mov     [call_record+C_BX], ax
                mov     [call_record+C_CX], ax
                mov     [call_record+C_DX], ax
                mov     [call_record+C_FLAGS], ax
                mov     [call_record+C_DI], ax
                mov     [call_record+C_BP], ax
                mov     [call_record+C_ES], ax
                mov     [call_record+C_FRAME_SS], ax
                mov     [call_record+C_FRAME_SP], ax
                mov     [call_record+C_ARG], ax
                mov     word [call_record+C_SI], bind0
                mov     [call_record+C_DS], cs
                mov     si, bind0               ; phase 0: live DS:SI = record
                call    gate_event
                mov     byte [busy], 0
                call    event_result
                or      ax, ax
                jnz     .refused
                mov     byte [state], 1
                pop     bx
                pop     es
                mov     word [es:bx+3], 0x0100  ; done
                mov     word [es:bx+14], resident_end
                mov     [es:bx+16], cs
                ret
.absent:        mov     dx, msg_absent
                jmp     short .fail
.invalid:       mov     dx, msg_invalid
                jmp     short .fail
.refused:       mov     dx, msg_refused
.fail:          mov     ah, 9
                int     0x21
                pop     bx
                pop     es
                ; contained failure: nothing stays resident or linked
                mov     word [es:bx+3], 0x810c
                mov     word [es:bx+14], 0
                mov     [es:bx+16], cs
                ret

; ES:BX = request header. "/LOCAL" on the DEVICE= line selects the explicit
; isolated-fixture policy: a refused activation keeps FreeDOS's local FAT.
; Without it, kernel=unified stops before INSTALL/shell (ciukinit.c).
parse_policy:
                push    es
                push    bx
                les     si, [es:bx+0x12]
                mov     cx, 128
.scan:          mov     al, [es:si]
                cmp     al, 13
                je      .none
                cmp     al, 10
                je      .none
                or      al, al
                jz      .none
                cmp     al, '/'
                jne     .next
                push    si
                push    cx
                mov     di, local_switch
                mov     cx, 5
.cmp:           inc     si
                mov     al, [es:si]
                cmp     al, 'a'
                jb      .upper
                cmp     al, 'z'
                ja      .upper
                sub     al, 0x20
.upper:         cmp     al, [di]
                jne     .mismatch
                inc     di
                loop    .cmp
                pop     cx
                pop     si
                mov     byte [policy], 1
                jmp     short .none
.mismatch:      pop     cx
                pop     si
.next:          inc     si
                loop    .scan
.none:          pop     bx
                pop     es
                ret
local_switch    db      'LOCAL'

; ES:DI = F000:1C10. CF set unless every byte matches the reserved slot.
validate_descriptor:
                push    si
                push    cx
                push    di
                mov     si, expected_descriptor
                mov     cx, expected_end - expected_descriptor
                cld
.cmp:           lodsb
                cmp     al, [es:di]
                jne     .bad
                inc     di
                loop    .cmp
                ; the generation (1Ch) is variable; compare the rest
                pop     di
                push    di
                cmp     word [es:di+0x20], CALL_BYTES
                jne     .bad
                cmp     word [es:di+0x22], BIND_BYTES
                jne     .bad
                cmp     word [es:di+0x24], GATE_DIRECT
                jne     .bad
                cmp     word [es:di+0x26], ROM_END
                jne     .bad
                push    bx
                xor     bx, bx
.reserved:      cmp     word [es:di+bx+0x28], 0
                jne     .bad_bx
                add     bx, 2
                cmp     bx, 8
                jb      .reserved
                pop     bx
                ; immutable gate bytes and role markers
                mov     si, expected_gates
                mov     di, GATE_DIRECT
                mov     cx, 16
.gate:          lodsb
                cmp     al, [es:di]
                jne     .bad
                inc     di
                loop    .gate
                pop     di
                pop     cx
                pop     si
                clc
                ret
.bad_bx:        pop     bx
.bad:           pop     di
                pop     cx
                pop     si
                stc
                ret

expected_descriptor:
                db      'DFV2'
                dw      2, 48
                dd      FEATURES
                dw      GATE_DIRECT, ROM_SEG
                dw      GATE_LOCAL, ROM_SEG
                dw      GATE_EVENT, ROM_SEG
                dw      GATE_CRITICAL, ROM_SEG
expected_end:
expected_gates:
                db      0xcd, 0xfe, 0xcb, 0xd2  ; direct
                db      0xcd, 0xfe, 0xcb, 0xa2  ; local 11xxh
                db      0xcd, 0xfe, 0xcb, 0xe2  ; lifecycle
                db      0xcd, 0xfe, 0xf4, 0xc2  ; critical continuation

msg_absent      db      'CIUKIDF: no DFV2 host; driver not installed', 13, 10, '$'
msg_invalid     db      'CIUKIDF: DFV2 descriptor rejected; driver not installed', 13, 10, '$'
msg_refused     db      'CIUKIDF: DFV2 registration refused; driver not installed', 13, 10, '$'

                align   2
bind0           times BIND_BYTES db 0
