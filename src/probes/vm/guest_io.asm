; GUESTIO.COM: ordinary DOS program for the native DOS window (VGA session
; mode). It never talks to CVSESSION: it hooks INT 9 and reads port 60h, uses
; INT 33h (polling and an event handler) and, on 'P', plays Sound Blaster DMA
; blocks through IRQ7 and an OPL note, exactly like a DOS game. The window's
; guest devices supply all of it. Findings are written to COM1; raw Esc exits.
bits 16
cpu 586
org 100h

SB_BASE     equ 220h
BLOCK_BYTES equ 4096
SB_BLOCKS   equ 6

start:
    cli
    mov ax,cs
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    cld
    mov bx,(program_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    jc quit
    mov bx,(BLOCK_BYTES*2+15)/16+1
    mov ah,48h
    int 21h
    jc quit
    mov [dma_segment],ax
    mov ax,13h                             ; graphics mode: mouse range 640x200
    int 10h
    xor ax,ax
    int 33h
    mov [mouse_reset],ax
    mov [mouse_buttons_count],bx
    mov ax,0Ch
    mov cx,7Fh
    push cs
    pop es
    mov dx,mouse_event
    int 33h
    cli
    xor ax,ax
    mov es,ax
    mov eax,[es:9*4]
    mov [old_int9],eax
    mov word [es:9*4],kbd_isr
    mov [es:9*4+2],cs
    sti
    push cs
    pop es
    mov si,ready_text
    call serial_puts
    mov ax,[mouse_reset]
    call hex16
    mov al,' '
    call serial_char
    mov ax,[mouse_buttons_count]
    call hex16
    mov si,crlf
    call serial_puts

main_loop:
    ; Report raw keyboard bytes.
    mov bx,[key_read]
    cmp bx,[key_write]
    je .mouse
    mov al,[key_ring+bx]
    inc bx
    and bx,63
    mov [key_read],bx
    push ax
    mov si,key_text
    call serial_puts
    pop ax
    push ax
    call hex8
    mov si,crlf
    call serial_puts
    pop ax
    cmp al,01h                             ; Esc make: leave
    je finish
    cmp al,19h                             ; P make: play
    jne main_loop
    call sb_phase
    call opl_phase
    jmp main_loop
.mouse:
    mov ax,3
    int 33h
    cmp cx,[last_x]
    jne .changed
    cmp dx,[last_y]
    jne .changed
    cmp bx,[last_buttons]
    je .events
.changed:
    mov [last_x],cx
    mov [last_y],dx
    mov [last_buttons],bx
    mov si,mouse_text
    call serial_puts
    mov ax,cx
    call hex16
    mov al,' '
    call serial_char
    mov ax,dx
    call hex16
    mov al,' '
    call serial_char
    mov ax,bx
    call hex16
    mov si,crlf
    call serial_puts
.events:
    mov ax,[event_count]
    cmp ax,[reported_events]
    je .idle
    mov [reported_events],ax
    mov si,event_text
    call serial_puts
    mov ax,[event_count]
    call hex16
    mov al,' '
    call serial_char
    mov ax,[event_mask]
    call hex16
    mov al,' '
    call serial_char
    mov ax,[event_x]
    call hex16
    mov al,' '
    call serial_char
    mov ax,[event_y]
    call hex16
    mov al,' '
    call serial_char
    mov ax,[event_buttons]
    call hex16
    mov si,crlf
    call serial_puts
.idle:
    hlt
    jmp main_loop

finish:
    cli
    xor ax,ax
    mov es,ax
    mov eax,[old_int9]
    mov [es:9*4],eax
    sti
    push cs
    pop es
    xor ax,ax                              ; drop the event handler
    int 33h
    mov ax,3
    int 10h
    mov si,exit_text
    call serial_puts
quit:
    mov ax,4C00h
    int 21h

kbd_isr:
    push ax
    push bx
    push ds
    push cs
    pop ds
    in al,64h
    in al,60h
    mov bx,[key_write]
    mov [key_ring+bx],al
    inc bx
    and bx,63
    mov [key_write],bx
    mov al,20h
    out 20h,al
    pop ds
    pop bx
    pop ax
    iret

mouse_event:                               ; INT 33h user handler (FAR)
    push ds
    push cs
    pop ds
    inc word [event_count]
    or [event_mask],ax
    mov [event_x],cx
    mov [event_y],dx
    mov [event_buttons],bx
    pop ds
    retf

; ---- Sound Blaster 16: six single-cycle 8-bit DMA blocks through IRQ7 ----
sb_phase:
    mov dx,SB_BASE+6
    mov al,1
    out dx,al
    mov cx,100
.delay:
    in al,dx
    loop .delay
    xor al,al
    out dx,al
    mov cx,1000
.wait_ready:
    mov dx,SB_BASE+0Eh
    in al,dx
    test al,80h
    jnz .read_aa
    loop .wait_ready
    jmp .failed
.read_aa:
    mov dx,SB_BASE+0Ah
    in al,dx
    cmp al,0AAh
    jne .failed
    mov ax,[dma_segment]
    movzx eax,ax
    shl eax,4
    add eax,15
    and eax,0FFFFFFF0h
    mov ebx,eax
    and ebx,0FFFFh
    add ebx,BLOCK_BYTES
    cmp ebx,10000h
    jbe .no_cross
    add eax,BLOCK_BYTES
.no_cross:
    mov [dma_physical],eax
    mov edi,eax
    shr eax,4
    mov es,ax
    and di,15
    mov cx,BLOCK_BYTES
    xor bx,bx
.fill:
    mov al,0C0h
    cmp bx,12
    jb .store
    mov al,40h
.store:
    stosb
    inc bx
    cmp bx,25
    jb .next
    xor bx,bx
.next:
    loop .fill
    push cs
    pop es
    cli
    xor ax,ax
    mov es,ax
    mov eax,[es:0Fh*4]
    mov [old_irq7],eax
    mov word [es:0Fh*4],sb_isr
    mov [es:0Fh*4+2],cs
    push cs
    pop es
    in al,21h
    mov [old_mask],al
    and al,7Fh
    out 21h,al
    sti
    mov al,0D1h
    call dsp_write
    mov al,40h
    call dsp_write
    mov al,165
    call dsp_write
    mov word [sb_irqs],0
    mov cx,SB_BLOCKS
.block:
    push cx
    call dma_program
    mov al,14h
    call dsp_write
    mov al,(BLOCK_BYTES-1) & 0FFh
    call dsp_write
    mov al,(BLOCK_BYTES-1) >> 8
    call dsp_write
    mov bx,[sb_irqs]
    inc bx
    mov cx,36
.irq_wait:
    cmp [sb_irqs],bx
    jae .irq_done
    call wait_tick
    loop .irq_wait
.irq_done:
    pop cx
    loop .block
    mov al,0D3h
    call dsp_write
    cli
    mov al,[old_mask]
    out 21h,al
    xor ax,ax
    mov es,ax
    mov eax,[old_irq7]
    mov [es:0Fh*4],eax
    sti
    push cs
    pop es
.failed:
    mov si,sb_text
    call serial_puts
    mov ax,[sb_irqs]
    call hex16
    mov si,crlf
    call serial_puts
    ret

dma_program:
    mov al,5
    out 0Ah,al
    xor al,al
    out 0Ch,al
    mov al,49h
    out 0Bh,al
    mov eax,[dma_physical]
    out 02h,al
    mov al,ah
    out 02h,al
    shr eax,16
    out 83h,al
    xor al,al
    out 0Ch,al
    mov al,(BLOCK_BYTES-1) & 0FFh
    out 03h,al
    mov al,(BLOCK_BYTES-1) >> 8
    out 03h,al
    mov al,1
    out 0Ah,al
    ret

sb_isr:
    push ax
    push dx
    push ds
    push cs
    pop ds
    inc word [sb_irqs]
    mov dx,SB_BASE+0Eh
    in al,dx
    mov al,20h
    out 20h,al
    pop ds
    pop dx
    pop ax
    iret

dsp_write:
    push cx
    push dx
    mov ah,al
    mov dx,SB_BASE+0Ch
    mov cx,10000
.busy:
    in al,dx
    test al,80h
    jz .ready
    loop .busy
.ready:
    mov al,ah
    out dx,al
    pop dx
    pop cx
    ret

opl_phase:
    mov si,opl_table
.write:
    lodsw
    cmp ax,0FFFFh
    je .notes
    call opl_write
    jmp .write
.notes:
    mov ax,0B032h
    call opl_write
    mov cx,20
.hold:
    call wait_tick
    loop .hold
    mov ax,0B012h
    call opl_write
    mov si,opl_text
    call serial_puts
    ret

opl_write:
    push cx
    push dx
    push ax
    mov dx,388h
    mov al,ah
    out dx,al
    mov cx,6
.index_delay:
    in al,dx
    loop .index_delay
    pop ax
    push ax
    inc dx
    out dx,al
    dec dx
    mov cx,35
.data_delay:
    in al,dx
    loop .data_delay
    pop ax
    pop dx
    pop cx
    ret

wait_tick:
    push es
    push eax
    mov ax,40h
    mov es,ax
    mov eax,[es:6Ch]
.wait:
    cmp eax,[es:6Ch]
    je .wait
    pop eax
    pop es
    ret

hex16:
    push ax
    mov al,ah
    call hex8
    pop ax
hex8:
    push ax
    shr al,4
    call .nibble
    pop ax
.nibble:
    and al,15
    add al,'0'
    cmp al,'9'
    jbe serial_char
    add al,7
serial_char:
    push ax
    push cx
    push dx
    mov ah,al
    mov cx,65535
    mov dx,3FDh
.wait:
    in al,dx
    test al,20h
    jnz .send
    loop .wait
.send:
    mov dx,3F8h
    mov al,ah
    out dx,al
    pop dx
    pop cx
    pop ax
    ret
serial_puts:
    lodsb
    test al,al
    jz .done
    call serial_char
    jmp serial_puts
.done:
    ret

opl_table:
    dw 0120h, 0800h
    dw 2001h, 4010h, 60F0h, 8077h
    dw 2301h, 4300h, 63F0h, 8377h
    dw 0C000h, 0E000h, 0E300h
    dw 0A044h
    dw 0FFFFh

old_int9 dd 0
old_irq7 dd 0
dma_physical dd 0
dma_segment dw 0
mouse_reset dw 0
mouse_buttons_count dw 0
last_x dw 0FFFFh
last_y dw 0FFFFh
last_buttons dw 0FFFFh
event_count dw 0
reported_events dw 0
event_mask dw 0
event_x dw 0
event_y dw 0
event_buttons dw 0
sb_irqs dw 0
key_read dw 0
key_write dw 0
old_mask db 0
key_ring times 64 db 0
ready_text db '[GUESTIO] READY MOUSE ',0
key_text db '[GUESTIO] KEY ',0
mouse_text db '[GUESTIO] MOUSE ',0
event_text db '[GUESTIO] EVENTS ',0
sb_text db '[GUESTIO] SB DONE IRQS ',0
opl_text db '[GUESTIO] OPL DONE',13,10,0
exit_text db '[GUESTIO] EXIT',13,10,0
crlf db 13,10,0
times 1024 db 0
stack_top:
program_end:
