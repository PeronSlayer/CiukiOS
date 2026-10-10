; Deliberate VM-local #GP, no protected-mode takeover.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
mov eax, cr0
ud2
