; Desktop TestGames launcher. Each copy changes to the game's data directory
; before EXEC. DOOM uses its installed COM wrapper so plain DOS and a native
; CiukiOS DOS session both take the wrapper's supported audio/host path.
bits 16
org 0x100

%ifndef GAME_KIND
    %error "GAME_KIND must select a TestGames launcher"
%endif

%if GAME_KIND = 2
    ; Explicit real-mode launch declaration. DPMIRUN recognizes only this
    ; complete versioned header; DOS itself simply jumps over it. Original
    ; Wolf uses Turbo C conventional heaps and never enters a DPMI client.
    jmp short start
    db 'CIUKILCH',1,1                 ; signature, version, real-mode-only
    dw 16,0                          ; complete header size, reserved
%endif

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    mov bx, ((image_end - $$ + 0x100) + 15) >> 4
    mov ah, 0x4A                 ; release unused DOS memory before nested EXEC
    int 0x21
    jc .error

    mov si, 0x80                ; copy the caller's PSP command tail
    mov di, child_tail
    mov cl, [si]
    xor ch, ch
    cmp cx, 126
    jbe .tail_ok
    mov cx, 126
.tail_ok:
    mov [di], cl
    inc si
    inc di
    rep movsb
    mov byte [di], 13

    mov dx, game_dir
    mov ah, 0x3B
    int 0x21
    jc .error

    mov ax, cs
    mov [child_block + 4], ax
    mov [child_block + 8], ax
    mov [child_block + 12], ax
    mov bx, child_block
    mov dx, game_exe
    mov ax, 0x4B00
    int 0x21
    jc .error
    mov ah, 0x4D
    int 0x21
    mov ah, 0x4C                 ; AL is the child's exit status
    int 0x21
.error:
    mov dx, error_text
    mov ah, 9
    int 0x21
    mov ax, 0x4C01
    int 0x21

%if GAME_KIND = 0
game_dir db '\APPS\DOOM',0
game_exe db '\APPS\DOOM\DOOM.COM',0
%elif GAME_KIND = 1
game_dir db '\APPS\DOOMVAN',0
game_exe db '\APPS\DOOMVAN\PCDMCORE.EXE',0
%elif GAME_KIND = 2
game_dir db '\APPS\WOLF3D',0
game_exe db '\APPS\WOLF3D\WOLF3D.EXE',0
%else
    %error "unknown GAME_KIND"
%endif

error_text db 'TestGames: game could not start.',13,10,'$'
child_tail times 128 db 0
child_fcb1 times 16 db 0
child_fcb2 times 16 db 0
child_block dw 0, child_tail, 0, child_fcb1, 0, child_fcb2, 0
stack_space times 512 db 0
stack_top:
image_end:
