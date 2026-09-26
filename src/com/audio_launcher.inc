; One launcher body, emitted as COM or as a genuine MZ executable. Explicit
; "run doom.exe" must initialize audio just like extensionless "doom".
%ifdef LAUNCHER_MZ
section .header start=0 vstart=0
    dw 0x5A4D
    dw (32 + launcher_image_end - start) % 512
    dw (32 + launcher_image_end - start + 511) / 512
    dw 0, 2, 0, 0xFFFF
    dw 0, launcher_stack_top, 0, start, 0, 0x1C, 0
    dw 0, 0
section .text follows=.header vstart=0 align=1
%else
org 0x0100
%endif

%macro audio_launcher_enter 0
    cli
    mov ax, cs
    mov ss, ax
    mov sp, launcher_stack_top
    sti
    ; DOS supplies DS=PSP for both formats. MZ CS is the load module, not PSP.
    cld
    mov es, ax
    mov si, 0x80
    mov di, launcher_user_tail
    mov cx, 128
    rep movsb
    push ds
    pop es
    push cs
    pop ds
    mov bx, ((launcher_image_end - $$ + 0x0100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    push cs
    pop es
%endmacro
