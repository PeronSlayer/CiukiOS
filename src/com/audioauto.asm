bits 16
org 0x0100

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov es, ax
    mov bx, ((image_end - $$ + 0x0100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc .fail
    cld
    push cs
    pop ds

    mov dx, msg_begin
    call print_line
    mov si, path_sb
    call run_child
    jnc .sb_ok
    mov si, path_ac97
    call run_child
    jnc .ac97_ok
    mov si, path_fallback
    call run_child
    jnc .pc_ok
.fail:
    push cs
    pop ds
    mov dx, msg_fail
    call print_line
    mov ax, 0x4C01
    int 0x21
.sb_ok:
    mov dx, msg_sb
    jmp .ok
.ac97_ok:
    mov dx, msg_ac97
    jmp .ok
.pc_ok:
    mov dx, msg_pc
.ok:
    call print_line
    mov ax, 0x4C00
    int 0x21

run_child:
    push ax
    push bx
    push dx
    push ds
    push es
    xor ax, ax
    mov [param_block], ax
    mov ax, ds
    mov [param_block + 4], ax
    mov [param_block + 8], ax
    mov [param_block + 12], ax
    mov dx, si
    mov bx, param_block
    push ds
    pop es
    mov ax, 0x4B00
    int 0x21
    jc .done
    mov ax, 0x4D00
    int 0x21
    or ah, ah
    jnz .child_fail
    or al, al
    jnz .child_fail
    clc
    jmp .done
.child_fail:
    stc
.done:
    pop es
    pop ds
    pop dx
    pop bx
    pop ax
    ret

print_line:
    mov ah, 0x09
    int 0x21
    ret

path_sb db '\SYSTEM\DRIVERS\SB16INIT.COM', 0
path_ac97 db '\SYSTEM\DRIVERS\AC97INIT.COM', 0
path_fallback db '\SYSTEM\DRIVERS\AUDIOTST.COM', 0
empty_tail db 0, 13
param_block dw 0, empty_tail, 0, 0x005C, 0, 0x006C, 0
msg_begin db '[AUDIO] hardware auto-detection', 13, 10, '$'
msg_sb db '[AUDIO] backend=SoundBlaster', 13, 10, '$'
msg_ac97 db '[AUDIO] backend=Intel-ICH-AC97', 13, 10, '$'
msg_pc db '[AUDIO] backend=PC-speaker', 13, 10, '$'
msg_fail db '[AUDIO] no working backend', 13, 10, '$'

align 16
stack_space times 1024 db 0
stack_top:
image_end:
