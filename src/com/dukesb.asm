; fx-duke3d: select the SB16 IRQ for Duke's A220 I5 D1 H5 preset.
; The VM starts at IRQ 7. The launcher restores it after DUKE3D.EXE exits.
; CT1745 mixer 80h: bit 1 = IRQ 5, bit 2 = IRQ 7; 81h: DMA 1 + DMA 5.
; SPDX-License-Identifier: MIT
bits 16
org 100h
    mov bl, 2
    cmp byte [82h], '7'
    jne .select
    mov bl, 4
.select:
    mov dx, 224h
    mov al, 80h
    out dx, al
    inc dx
    mov al, bl
    out dx, al
    dec dx
    mov al, 81h
    out dx, al
    inc dx
    mov al, 22h
    out dx, al
    mov ax, 4c00h
    int 21h
