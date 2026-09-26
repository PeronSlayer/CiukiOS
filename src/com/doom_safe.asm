bits 16
%include "src/com/audio_launcher.inc"
; Diagnostic fallback for machines where a legacy audio interrupt stalls Doom.
; Run the original engine without installing an audio/DPMI emulation layer.
start:
    audio_launcher_enter
    jc .fail
    push cs
    pop ds
    push cs
    pop es
    mov dx, message
    mov ah, 9
    int 0x21
    mov ax, cs
    mov [parameters+4], ax
    mov [parameters+8], ax
    mov [parameters+12], ax
    mov dx, engine
    mov bx, parameters
    mov ax, 0x4B00
    int 0x21
    jc .fail
    mov ah, 0x4D
    int 0x21
    mov ah, 0x4C
    int 0x21
.fail:
    mov ax, 0x4C01
    int 0x21
engine db 'DOOMCORE.EXE', 0
message db '[DOOM] SAFE: original engine, audio disabled',13,10,'$'
tail db tail_end-tail-1
    db ' -nosound'
tail_end db 13
parameters dw 0, tail, 0, 0, 0, 0, 0
launcher_user_tail times 128 db 0
align 16
launcher_stack times 1024 db 0
launcher_stack_top:
launcher_image_end:
