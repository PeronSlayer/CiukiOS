; VMSCHILD.COM - VM manager gate: Sound Blaster DMA from a VM that does not
; run all the time. Runs in VM 1 (VMFORK from VMSTEST); the command tail is
; the TSC rate in kHz (8 hex digits). Begins the DOS window session and the
; device model with SB16 and AC'97 output, then plays a 220 Hz square wave
; (8-bit unsigned C0h/40h, 11025 Hz, auto-init DMA channel 1, IRQ 7) for 12
; blocks while the system VM keeps running. COM1: "[VMS1] ..." lines.
; With a VM digit after the rate (VMATEST: "<kHz> 2") it names itself
; "[VMS2]", plays 220 Hz for VM 1 and 551 Hz (half period 10 samples) for
; the others, also takes the keyboard and plays on until Esc, then reports
; the blocks played ("[VMS2] played nnnn blocks").
; Exit code 0, or the number of the failed step.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_devices_abi.inc"

SB_BASE equ 220h
RATE_TC equ 165                         ; 256 - 1000000/11025
HALF equ 4000                           ; bytes per block
BLOCKS equ 12

start:
    mov sp,stack_top
    mov bx,(image_end-$$+100h+15)/16
    mov ah,4Ah
    int 21h
    ; TSC kHz from the tail, then the optional VM digit.
    mov si,81h
    xor edx,edx
.skip:
    lodsb
    cmp al,' '
    je .skip
.hex:
    cmp al,13
    je .hex_done
    cmp al,' '
    je .vm_digit
    sub al,'0'
    cmp al,9
    jbe .digit
    sub al,7
.digit:
    shl edx,4
    and al,0Fh
    or dl,al
    lodsb
    jmp .hex
.vm_digit:
    lodsb
    cmp al,' '
    je .vm_digit
    cmp al,'1'
    jb .hex_done
    cmp al,'9'
    ja .hex_done
    mov [held],al
    cmp al,'1'
    je .named
    mov word [half_period],10
.named:
    mov bx,named_texts
.name:
    mov di,[bx]
    test di,di
    jz .hex_done
    mov [di+4],al
    add bx,2
    jmp .name
.hex_done:
    mov [tsc_khz],edx
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov [entry],di
    mov [entry+2],es
    push cs
    pop es
    mov ax,VM_OP_BEGIN
    call far [entry]
    push cs
    pop ds
    mov si,msg_begin
    mov bl,1
    jc fail
    mov ebx,CVDEV_CAP_DMA_SB
    cmp byte [held],0
    je .caps
    or ebx,CVDEV_CAP_KEYBOARD
.caps:
    mov ecx,CVDEV_FLAG_AUDIO_OUT
    mov edx,[tsc_khz]
    mov ax,VM_OP_DEV_BEGIN
    call far [entry]
    push cs
    pop ds
    mov si,msg_dev
    mov bl,2
    jc fail_session
    cmp cx,1                            ; AC'97 stream running
    mov si,msg_audio
    mov bl,3
    jne fail_devices
    ; The wave: C0h for half a period (25 samples), 40h for the other half,
    ; in a buffer that does not cross a 64 KB DMA page.
    mov ax,cs
    movzx eax,ax
    shl eax,4
    add eax,wave_area
    mov edx,eax
    add edx,2*HALF-1
    xor edx,eax
    test edx,0FFFF0000h
    jz .buffer_ok
    add eax,2*HALF                      ; the second half of the area
.buffer_ok:
    mov [wave_linear],eax
    mov ecx,eax                         ; its offset in this segment
    mov ax,cs
    movzx eax,ax
    shl eax,4
    sub ecx,eax
    mov di,cx
    mov cx,2*HALF
    xor bx,bx
.fill:
    mov al,0C0h
    cmp bx,[half_period]
    jb .store
    mov al,40h
.store:
    stosb
    inc bx
    mov dx,[half_period]
    add dx,dx
    cmp bx,dx
    jb .next
    xor bx,bx
.next:
    loop .fill
    ; IRQ 7 handler and its line unmasked in the virtual PIC.
    push es
    xor ax,ax
    mov es,ax
    cli
    mov eax,[es:0Fh*4]
    mov [old_irq7],eax
    mov word [es:0Fh*4],irq7
    mov [es:0Fh*4+2],cs
    sti
    pop es
    in al,21h
    mov [old_mask],al
    and al,7Fh
    out 21h,al
    ; DSP reset.
    mov dx,SB_BASE+6
    mov al,1
    out dx,al
    in al,dx
    in al,dx
    in al,dx
    xor al,al
    out dx,al
    mov cx,1000
.reset_wait:
    mov dx,SB_BASE+0Eh
    in al,dx
    test al,80h
    jz .reset_next
    mov dx,SB_BASE+0Ah
    in al,dx
    cmp al,0AAh
    je .reset_ok
.reset_next:
    loop .reset_wait
    mov si,msg_dsp
    mov bl,4
    jmp fail_irq
.reset_ok:
    mov al,0D1h
    call dsp
    mov al,40h
    call dsp
    mov al,RATE_TC
    call dsp
    ; DMA channel 1: auto-init, memory to device, the whole buffer.
    mov al,5
    out 0Ah,al
    xor al,al
    out 0Ch,al
    mov al,59h
    out 0Bh,al
    mov eax,[wave_linear]
    out 02h,al
    mov al,ah
    out 02h,al
    shr eax,16
    out 83h,al
    mov ax,2*HALF-1
    out 03h,al
    mov al,ah
    out 03h,al
    mov al,1
    out 0Ah,al
    ; 8-bit auto-init output, one IRQ per half.
    mov al,48h
    call dsp
    mov ax,HALF-1
    call dsp
    mov al,ah
    call dsp
    mov al,1Ch
    call dsp
    mov si,msg_playing
    call serial_text
    mov cx,2000                         ; ~110 s bound
.wait_blocks:
    cmp byte [held],0
    jne .wait_key
    cmp word [blocks],BLOCKS
    jae .played
    jmp .wait_next
.wait_key:
    mov ah,1
    int 16h
    jz .wait_next
    xor ah,ah
    int 16h
    cmp al,27
    je .played
.wait_next:
    call wait_tick
    loop .wait_blocks
    mov si,msg_noirq
    mov bl,5
    jmp fail_stop
.played:
    mov si,msg_played
    call serial_text
    mov si,msg_crlf
    cmp byte [held],0
    je .played_line
    mov al,' '
    call serial_char
    mov ax,[blocks]
    call serial_hex
    mov si,msg_blocks
.played_line:
    call serial_text
    xor bl,bl
fail_stop:
    push bx
    mov al,0DAh                         ; leave auto-init
    call dsp
    mov al,0D0h
    call dsp
    mov al,0D3h
    call dsp
    mov al,5
    out 0Ah,al
    pop bx
fail_irq:
    push bx
    mov al,[old_mask]
    out 21h,al
    push es
    xor ax,ax
    mov es,ax
    cli
    mov eax,[old_irq7]
    mov [es:0Fh*4],eax
    sti
    pop es
    pop bx
fail_devices:
    push bx
    mov ax,VM_OP_DEV_END
    call far [entry]
    push cs
    pop ds
    pop bx
fail_session:
    push bx
    mov ax,VM_OP_END
    call far [entry]
    push cs
    pop ds
    pop bx
    test bl,bl
    jz exit
fail:
    push bx
    call serial_text
    pop bx
exit:
    mov al,bl
    mov ah,4Ch
    int 21h

irq7:
    push ax
    push dx
    mov dx,SB_BASE+0Eh                  ; 8-bit acknowledge
    in al,dx
    inc word [cs:blocks]
    mov al,20h
    out 20h,al
    pop dx
    pop ax
    iret

; AL -> DSP command port.
dsp:
    push dx
    push ax
    mov dx,SB_BASE+0Ch
.w:
    in al,dx
    test al,80h
    jnz .w
    pop ax
    out dx,al
    pop dx
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

serial_hex:
    push cx
    mov cx,4
.digit:
    rol ax,4
    push ax
    and al,0Fh
    add al,'0'
    cmp al,'9'
    jbe .out
    add al,7
.out:
    call serial_char
    pop ax
    loop .digit
    pop cx
    ret

serial_char:
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

msg_begin db '[VMS1] BEGIN failed',13,10,0
msg_dev db '[VMS1] DEV_BEGIN failed',13,10,0
msg_audio db '[VMS1] no AC97 stream',13,10,0
msg_dsp db '[VMS1] DSP reset failed',13,10,0
msg_noirq db '[VMS1] blocks did not complete',13,10,0
msg_playing db '[VMS1] playing',13,10,0
msg_played db '[VMS1] played',0
msg_blocks db ' blocks'
msg_crlf db 13,10,0
named_texts dw msg_begin,msg_dev,msg_audio,msg_dsp,msg_noirq,msg_playing,msg_played,0
half_period dw 25
held db 0
entry dd 0
tsc_khz dd 0
wave_linear dd 0
old_irq7 dd 0
blocks dw 0
old_mask db 0
    align 16
wave_area times 4*HALF db 80h
    align 2
    times 512 db 0
stack_top:
image_end:
