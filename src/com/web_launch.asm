; Start the optional MicroWeb browser with its asset directory as CWD.
bits 16
org 0x100

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    mov ds, ax
    mov es, ax
    mov bx, ((image_end - $$ + 0x100) + 15) >> 4
    mov ah, 0x4A
    int 0x21
    jc .error

    mov si, 0x80
    mov cl, [si]
    xor ch, ch
    cmp cx, 117
    jbe .tail_ok
    mov cx, 117
.tail_ok:
    mov bx, cx
    add bl, 9
    mov [child_tail], bl
    mov di, child_tail + 1
    mov si, video_arg
    mov cx, 9
    rep movsb
    mov cx, bx
    sub cx, 9
    mov si, 0x81
    rep movsb
    mov byte [di], 13

    mov dx, browser_dir
    mov ah, 0x3B
    int 0x21
    jc .error

    mov ax, cs
    mov [child_block + 4], ax
    mov [child_block + 8], ax
    mov [child_block + 12], ax
    mov bx, child_block
    mov dx, browser_exe
    mov ax, 0x4B00
    int 0x21
    jc .error
    mov ah, 0x4D
    int 0x21
    mov ah, 0x4C
    int 0x21
.error:
    mov dx, error_text
    mov ah, 9
    int 0x21
    mov ax, 0x4C01
    int 0x21

browser_dir db '\NET\BROWSER',0
browser_exe db '\NET\BROWSER\MICROWEB.EXE',0
video_arg db ' -video=i'
error_text db 'WEB: browser could not start.',13,10,'$'
child_tail times 128 db 0
child_fcb1 times 16 db 0
child_fcb2 times 16 db 0
child_block dw 0, child_tail, 0, child_fcb1, 0, child_fcb2, 0
stack_space times 512 db 0
stack_top:
image_end:
