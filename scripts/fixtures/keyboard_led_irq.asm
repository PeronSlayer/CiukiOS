; Test-only option ROM: BIOS-style LED updates from inside IRQ1.
; IBM PC AT Technical Reference, Keyboard pp. 5-116 and 5-121:
; SND_LED acknowledges the original PIC interrupt before SND_DATA enables
; interrupts and waits for a nested IRQ1 to acknowledge the keyboard command.
; Send real PS/2 ED/data commands; do not inject an ACK or a DOS result.
bits 16
cpu 386
org 0
%ifndef TEST_EMPTY_REENTRY
%define TEST_EMPTY_REENTRY 0
%endif
    db 0x55, 0xAA, 4
    jmp init

blob:
    db 'KLEDIRQ1'
old_irq dd 0
busy db 0
ack db 0
updates dw 0
acks dw 0
timeouts dw 0
unexpected dw 0
desired db 0
empty_calls dw 0
empty_returns dw 0

irq:
    push ax
    push bx
    push cx
    push dx
    push ds
    mov ax,0x40
    mov ds,ax
    cmp byte [cs:busy-blob],0
    je .ordinary
    in al,0x64
    and al,0x21
    cmp al,1
    jne .unexpected
    in al,0x60
    cmp al,0xFA
    jne .unexpected
    mov byte [cs:ack-blob],1
    inc word [cs:acks-blob]
    or byte [0x97],0x10
    jmp .eoi
.unexpected:
    inc word [cs:unexpected-blob]
.eoi:
    mov al,0x20
    out 0x20,al
    jmp .done
.ordinary:
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    pushf
    call far [cs:old_irq-blob]
    push ax
    push bx
    push cx
    push dx
    push ds
    mov ax,0x40
    mov ds,ax
%if TEST_EMPTY_REENTRY
    ; Separate virtual-IRQ regression control: a duplicate software entry
    ; after the BIOS consumed the key must return without reentering it.
    cmp word [cs:empty_calls-blob],0
    jne .empty_test_done
    in al,0x64
    test al,1
    jnz .empty_test_done
    inc word [cs:empty_calls-blob]
    int 0x09
    inc word [cs:empty_returns-blob]
.empty_test_done:
%endif
    mov al,[0x17]
    shr al,4
    and al,7
    mov bl,[0x97]
    and bl,7
    cmp al,bl
    je .done
    mov [cs:desired-blob],al
    inc word [cs:updates-blob]
    mov byte [cs:busy-blob],1
    or byte [0x97],0x40
    ; The underlying BIOS already sent EOI for the triggering key.
    mov al,0xED
    call send_byte
    jc .failed
    mov al,[cs:desired-blob]
    call send_byte
    jc .failed
    and byte [0x97],0xF8
    mov al,[cs:desired-blob]
    or [0x97],al
.failed:
    and byte [0x97],0xBF
    mov byte [cs:busy-blob],0
.done:
    pop ds
    pop dx
    pop cx
    pop bx
    pop ax
    iret

send_byte:
    cli
    mov dl,al
    mov cx,0xFFFF
.wait_input:
    in al,0x64
    test al,2
    jz .send
    loop .wait_input
    jmp .timeout
.send:
    mov byte [cs:ack-blob],0
    and byte [0x97],0xEF
    mov al,dl
    out 0x60,al
    mov bx,[0x6C]
    sti
.wait_ack:
    cmp byte [cs:ack-blob],1
    je .received
    mov ax,[0x6C]
    sub ax,bx
    cmp ax,18
    jb .wait_ack
.timeout:
    cli
    inc word [cs:timeouts-blob]
    stc
    ret
.received:
    cli
    clc
    ret
blob_end:

init:
    pushf
    pushad
    push ds
    push es
    cli
    xor ax,ax
    mov ds,ax
    dec word [0x413]
    mov ax,[0x413]
    shl ax,6
    mov es,ax
    push cs
    pop ds
    mov si,blob
    xor di,di
    mov cx,blob_end-blob
    cld
    rep movsb
    xor ax,ax
    mov ds,ax
    mov eax,[0x09*4]
    mov [es:old_irq-blob],eax
    mov word [0x09*4],irq-blob
    mov [0x09*4+2],es
    pop es
    pop ds
    popad
    popf
    retf

times 2047-($-$$) db 0
db 0 ; checksum filled by the test runner
