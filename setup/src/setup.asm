; CiukiOS Setup - Main Installer Entry Point
; Target: x86 real-mode, COM-style single-segment binary
; Assembled with: nasm -f bin setup.asm -o SETUP.COM
;
; TODO: implement full installer flow
;   1. Print welcome screen (WELCOME.TXT or inline)
;   2. License acceptance prompt (Y/N)
;   3. Target drive/partition detection and selection
;   4. Component selection menu (Minimal / Standard / Full+GEM)
;   5. Call copy engine (setup_copy.asm) for each disk/file
;   6. Write CIUKIOS.CFG via setup_cfg.asm
;   7. Print completion message and reboot prompt

bits 16
org 0x100           ; COM binary base

start:
    mov  ah, 09h
    mov  dx, msg_stub
    int  21h
    mov  ax, 4C00h
    int  21h

msg_stub db "CiukiOS Setup - not yet implemented", 13, 10, '$'
