; Embeds the assembled ring-3 payload (build/f0/payload.bin) in .rodata.
; SPDX-License-Identifier: GPL-2.0-only
section .rodata
align 16
global payload_start, payload_end
payload_start:
    incbin PAYLOAD_BIN
payload_end:
