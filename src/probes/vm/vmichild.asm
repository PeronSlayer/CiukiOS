; VMICHILD.COM - VM manager gate, VM 1 side of VMITEST. The tail is the PSP
; and the size in paragraphs (4 hex digits each) of VMITEST's block, which
; VMFORK freed in this VM. The INT 1Ch and INT 09h vectors must not point
; into it any more. Then that block (except this program, if it was loaded
; there) is overwritten with CCh (INT 3), and 40 timer ticks must pass here
; with INT 1Ch running every tick.
; "[VMI1] ..." on COM1; exit code 0, or 1 when a vector still points into
; the freed block.
bits 16
cpu 386
org 100h

start:
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    mov si,81h
    call hex_word
    mov [parent],dx
    call hex_word
    mov [parent_size],dx
    push es
    xor ax,ax
    mov es,ax
    mov ax,[es:1Ch*4+2]
    mov bx,[es:09h*4+2]
    pop es
    cmp ax,[parent]
    je .stale
    cmp bx,[parent]
    je .stale
    mov si,msg_vectors
    call serial_text
    ; Overwrite the freed block, paragraph by paragraph, around this program.
    mov ax,cs
    mov [own_start],ax
    add ax,(image_end-$$+100h+15)/16
    mov [own_end],ax
    mov dx,[parent]
    mov cx,[parent_size]
.fill:
    cmp dx,[own_start]
    jb .overwrite
    cmp dx,[own_end]
    jb .skip
.overwrite:
    push cx
    push es
    mov es,dx
    xor di,di
    mov cx,16
    mov al,0CCh
    rep stosb
    pop es
    pop cx
.skip:
    inc dx
    loop .fill
    mov si,msg_filled
    call serial_text
    mov cx,40
.ticks:
    call wait_tick
    loop .ticks
    mov si,msg_ok
    call serial_text
    mov ax,4C00h
    int 21h
.stale:
    mov si,msg_stale
    call serial_text
    mov ax,4C01h
    int 21h

; DS:SI tail -> DX = next hex word (leading blanks skipped).
hex_word:
    xor dx,dx
.skip:
    lodsb
    cmp al,' '
    je .skip
.digit:
    cmp al,13
    je .end
    cmp al,' '
    je .done
    sub al,'0'
    cmp al,9
    jbe .value
    sub al,7
.value:
    shl dx,4
    and al,0Fh
    or dl,al
    lodsb
    jmp .digit
.end:
    dec si
.done:
    ret

wait_tick:
    push es
    push ecx
    push eax
    push 40h
    pop es
    mov eax,[es:6Ch]
    mov ecx,400000000
.w:
    cmp eax,[es:6Ch]
    jne .d
    dec ecx
    jnz .w
.d:
    pop eax
    pop ecx
    pop es
    ret

serial_text:
    lodsb
    test al,al
    jz .done
    push dx
    push ax
    mov dx,3FDh
.w:
    in al,dx
    test al,20h
    jz .w
    pop ax
    mov dx,3F8h
    out dx,al
    pop dx
    jmp serial_text
.done:
    ret

msg_vectors db '[VMI1] vectors restored',13,10,0
msg_stale db '[VMI1] a vector still points into the freed block',13,10,0
msg_filled db '[VMI1] freed block overwritten',13,10,0
msg_ok db '[VMI1] 40 ticks over overwritten memory',13,10,0
parent dw 0
parent_size dw 0
own_start dw 0
own_end dw 0
    align 2
    times 1024 db 0
stack_top:
image_end:
