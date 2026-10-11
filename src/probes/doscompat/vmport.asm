; The documented forbidden port must terminate only this guest.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
mov dx, 0xFFFF
xor al, al
out dx, al
ud2
