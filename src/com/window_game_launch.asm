; Launch a cooperative source port from its installed data directory.
; Does not install drivers or alter the working full-screen game launchers.
bits 16
org 0x100
    cld
    mov ax,cs
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,stack_top
    mov bx,(stack_top-$$+0x100+15)/16
    mov ah,0x4A
    int 0x21
    jc failed
    mov ah,0x19
    int 0x21
    add al,'A'
    mov [previous_directory],al
    mov si,previous_directory+3
    xor dx,dx
    mov ah,0x47
    int 0x21
    jc failed
    mov dx,game_directory
    mov ah,0x3B
    int 0x21
    jc failed
    push cs
    pop ds
    push cs
    pop es
    mov si,default_arguments
    mov di,child_tail+1
    mov cx,default_arguments_end-default_arguments
    rep movsb
    movzx cx,byte [0x80]
    cmp cx,126-(default_arguments_end-default_arguments)
    ja restore_failed
    mov si,0x81
    rep movsb
    mov ax,di
    sub ax,child_tail+1
    mov [child_tail],al
    mov byte [di],13
    mov [parameters+4],cs
    mov [parameters+8],cs
    mov [parameters+12],cs
    mov bx,parameters
    mov dx,executable
    mov ax,0x4B00
    int 0x21
    jc restore_failed
    mov ah,0x4D
    int 0x21
    mov [cs:exit_code],al
restore:
    push cs
    pop ds
    mov dx,previous_directory
    mov ah,0x3B
    int 0x21
    mov al,[exit_code]
    mov ah,0x4C
    int 0x21
restore_failed:
    mov byte [cs:exit_code],1
    jmp restore
failed:
    push cs
    pop ds
    mov dx,error_text
    mov ah,9
    int 0x21
    mov ax,0x4C01
    int 0x21
%ifdef WINDOW_WOLF
game_directory db '\APPS\WOLF3D',0
executable db '\APPS\WOLFWIN.EXE',0
default_arguments db ' --nowait '
%else
game_directory db '\APPS',0
executable db '\APPS\DOOMWIN.EXE',0
; DOS has no Unix zenity helper. Error/recording exit must return directly
; to the native host instead of trying to spawn an unavailable dialog.
default_arguments db ' -iwad \APPS\DOOM\DOOM.WAD -nosound -nogui '
%endif
default_arguments_end:
parameters dw 0,child_tail,0,0x5C,0,0x6C,0
previous_directory db 'C:\'
    times 65 db 0
exit_code db 0
child_tail times 128 db 0
error_text db 'Windowed game could not start. Check its installed files.',13,10,'$'
    times 512 db 0
stack_top:
