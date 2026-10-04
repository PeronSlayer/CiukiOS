; MOUSEBTN.COM - read-only CiukiDOS physical mouse-button diagnostic.
; Press and release tap, left switch and right switch separately, then press
; any keyboard key to print a snapshot. Q exits. No PS/2 commands are sent.
bits 16
cpu 386
org 100h

start:
    push cs
    pop ds
    mov dx,intro
    call print_string
    call snapshot
.wait:
    mov ah,08h
    int 21h
    cmp al,'q'
    je .exit
    cmp al,'Q'
    je .exit
    call snapshot
    jmp .wait
.exit:
    mov ax,4C00h
    int 21h

snapshot:
    mov ax,7F00h
    int 33h
    cmp ax,0C155h
    je .supported
    mov dx,unsupported
    call print_string
    ret
.supported:
    mov [packets],bx
    mov [left_presses],cx
    mov [right_presses],dx
    mov [last_raw],si
    mov ax,0003h
    int 33h
    xor bh,bh
    mov [current_buttons],bx
    mov dx,packets_label
    call print_string
    mov ax,[packets]
    call print_hex_word
    mov dx,left_label
    call print_string
    mov ax,[left_presses]
    call print_hex_word
    mov dx,right_label
    call print_string
    mov ax,[right_presses]
    call print_hex_word
    mov dx,raw_label
    call print_string
    mov ax,[last_raw]
    call print_hex_word
    mov dx,current_label
    call print_string
    mov ax,[current_buttons]
    call print_hex_word
    mov dx,newline
    call print_string
    ret

print_string:
    mov ah,09h
    int 21h
    ret

print_hex_word:
    push bx
    push cx
    push dx
    mov bx,ax
    mov cx,4
.digit:
    rol bx,4
    mov dl,bl
    and dl,0Fh
    cmp dl,9
    jbe .decimal
    add dl,'A'-10
    jmp .emit
.decimal:
    add dl,'0'
.emit:
    mov ah,02h
    int 21h
    loop .digit
    pop dx
    pop cx
    pop bx
    ret

intro db 'CiukiOS mouse-button diagnostic. Tap or press and release a physical',13,10
      db 'button; press any key for a snapshot, Q to quit.',13,10,'$'
unsupported db 'CiukiDOS mouse diagnostic is unavailable.',13,10,'$'
packets_label db 'Packets=', '$'
left_label db ' raw-left=', '$'
right_label db ' raw-right=', '$'
raw_label db ' last-byte=', '$'
current_label db ' current=', '$'
newline db 13,10,'$'
packets dw 0
left_presses dw 0
right_presses dw 0
last_raw dw 0
current_buttons dw 0
