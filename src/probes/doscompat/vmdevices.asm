; f3-01 DOS-side device probe. Private mailbox at 0000:0600; byte record E9.
; This runs through COMMAND.COM on the canonical private DOS disk.
; SPDX-License-Identifier: GPL-2.0-only
bits 16
org 0x100
%define magic 0x600
%define stage 0x602
%define command 0x604
%define ticks 0x606
%define sti_mark 0x608
%define sti_seen 0x60A
%define ss_seen 0x60C
%define last_ip 0x61A
%define keys 0x60E
%define last_key 0x610
%define isr0 0x612
%define isr1 0x614
%define mouse_release 0x616
%define callback_count 0x618
start:
    cli
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov di,magic
    mov cx,14                   ; 0600h..061Bh: the whole mailbox, last_ip included
    rep stosw
    mov word [8*4],timer
    mov [8*4+2],cs
    mov word [9*4],keyboard
    mov [9*4+2],cs
    mov al,0x11
    out 0x20,al
    out 0xA0,al
    mov al,8
    out 0x21,al
    mov al,0x70
    out 0xA1,al
    mov al,4
    out 0x21,al
    mov al,2
    out 0xA1,al
    mov al,1
    out 0x21,al
    out 0xA1,al
    mov al,0xFC                 ; IRQ0/1 enabled (mask bits clear), slave absent
    out 0x21,al
    mov al,0xFF
    out 0xA1,al
    mov al,0x34
    out 0x43,al
    mov ax,1193
    out 0x40,al
    mov al,ah
    out 0x40,al
    mov word [magic],0xD301
    mov word [stage],1
.wait_cli:
    cmp word [command],1
    jne .wait_cli
    sti
    inc word [sti_mark]         ; must precede first timer IRQ
    mov word [stage],2
.wait_ticks:
    hlt
    cmp word [ticks],64
    jb .wait_ticks
    cli
    mov word [stage],3
.wait_ss:
    cmp word [command],3
    jne .wait_ss
    mov bx,ss
    mov al,0xFF                 ; loaded before STI: the MOV SS shadow must end at the OUT
    sti
    mov ss,bx                  ; STI shadow covers MOV SS; MOV SS shadow covers the trapped OUT
    out 0x21,al                 ; mask pending IRQ before it can run
    cli
    mov ax,[ticks]
    mov [ss_seen],ax
    mov word [stage],4
.wait_input:
    cmp word [command],4
    jne .wait_input
    mov al,0xFD                 ; OCW1 bit=1 masks (8259A p.13): only IRQ1 enabled,
                                ; the pending IRQ0 stays masked
    out 0x21,al
    sti
    mov word [stage],5
.wait_keys:
    hlt
    cmp word [keys],2
    jb .wait_keys
    xor ax,ax
    int 0x33
    push cs
    pop es
    mov ax,0x0C
    mov cx,0x7F
    mov dx,mouse_callback
    int 0x33
    mov word [stage],6
.wait_focus:
    hlt
    mov ax,3
    int 0x33
    test bx,bx
    jnz .wait_focus
    cmp byte [last_key],0x9E
    jne .wait_focus
    cmp word [callback_count],0
    je .wait_focus
    mov word [mouse_release],1
    mov word [stage],7
    push cs
    pop ds
    mov si,done
.print:
    lodsb
    test al,al
    jz .fault
    out 0xE9,al
    jmp .print
.fault:
    mov dx,0x40
    out dx,ax                  ; malformed wide PIT I/O must fault this VM
    cli
    hlt
    jmp $
timer:
    push ax
    push ds
    xor ax,ax
    mov ds,ax
    inc word [ticks]
    push bp
    mov bp,sp
    mov ax,[ss:bp+6]            ; interrupted IP: where the IRQ was delivered
    mov [last_ip],ax
    pop bp
    cmp word [sti_seen],0
    jne .seen
    mov ax,[sti_mark]
    mov [sti_seen],ax
.seen:
    mov al,0x0B
    out 0x20,al
    in al,0x20
    xor ah,ah
    mov [isr0],ax
    mov al,0x20
    out 0x20,al
    pop ds
    pop ax
    iret
keyboard:
    push ax
    push ds
    xor ax,ax
    mov ds,ax
    in al,0x60
    xor ah,ah
    mov [last_key],ax
    inc word [keys]
    mov al,0x0B
    out 0x20,al
    in al,0x20
    xor ah,ah
    mov [isr1],ax
    mov al,0x20
    out 0x20,al
    pop ds
    pop ax
    iret
mouse_callback:
    push ds
    xor ax,ax
    mov ds,ax
    inc word [callback_count]
    pop ds
    retf
done db 'VMDEV devices complete',10,0
