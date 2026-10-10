; COMMAND /C status probe's named child.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
mov ax, 0x4C25
int 0x21
