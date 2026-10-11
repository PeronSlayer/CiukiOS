; f3-06 starvation-gate payload CLILOOP.COM: "cli; jmp $" forever. Virtual IF
; stays clear, so this VM takes no guest interrupt; the host timer still
; preempts it (guest CLI never masks host IRQs: dos-dpmi-contract.md,
; V86 monitor). Only VM destruction ends it. Announces itself first on the
; record port so the controller knows the loop has started.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
start:
    mov ax, cs
    mov ds, ax
    mov dx, message
    call record_dollar
    cli
.spin:
    jmp .spin
%include "src/probes/doscompat/record.inc"
message db 'CLILOOP PASS', 13, 10, '$'
