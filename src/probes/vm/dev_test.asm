; DEVTEST.COM: actual V86 guest-device probe for CVSESSION.
; An ordinary DOS program: it programs the keyboard controller, ISA DMA,
; Sound Blaster DSP and OPL ports and hooks INT 9 / IRQ7 exactly as a DOS
; game would. The session supplies those devices from the peripheral model;
; the physical host has no Sound Blaster at all. Results go to COM1.
bits 16
cpu 586
org 100h
%include "src/vm/session_abi.inc"
%include "src/vm/session_scheduler_abi.inc"
%include "src/vm/session_devices_abi.inc"

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
    jc fail
    mov bx,(BLOCK_BYTES*2+15)/16+1          ; DMA buffer (no 64 KiB crossing)
    mov ah,48h
    int 21h
    jc fail
    mov [dma_segment],ax
    xor di,di
    mov es,di
    mov ax,1684h
    mov bx,VM_DEVICE_ID
    int 2Fh
    mov ax,es
    or ax,di
    jz fail
    mov [entry],di
    mov [entry+2],es
    push cs
    pop es
    xor cx,cx
    mov ax,VM_OP_QUERY
    call far [cs:entry]
    jc fail
    test bx,VM_CAP_GUEST_INPUT
    jz no_devices
    test bx,VM_CAP_GUEST_AUDIO
    jz no_devices
    mov di,descriptor
    mov cx,CVSCHED_BYTES
    mov ax,VM_OP_BIND_SCHED
    call far [cs:entry]
    jc fail
    mov byte [bound_flag],1
    mov ax,VM_OP_BEGIN
    call far [cs:entry]
    jc fail
    mov byte [session_flag],1
    ; TSC kHz from two BIOS ticks (virtual IRQ0 through the profile).
    call measure_tsc
    mov ebx,CVDEV_CAP_KEYBOARD|CVDEV_CAP_MOUSE|CVDEV_CAP_DMA_SB|CVDEV_CAP_OPL3
    mov ecx,CVDEV_FLAG_AUDIO_OUT
    mov edx,[tsc_khz]
    mov ax,VM_OP_DEV_BEGIN
    call far [cs:entry]
    jc fail
    mov byte [device_flag],1
    mov [granted],ebx
    mov [audio_status],ecx
    mov si,begin_text
    call serial_puts
    mov eax,[granted]
    call hex32
    mov al,' '
    call serial_char
    mov eax,[audio_status]
    call hex32
    mov si,crlf
    call serial_puts

    call keyboard_phase
    call sb_phase
    call opl_phase

    push cs
    pop es
    mov di,report
    mov ax,VM_OP_DEV_STATE
    call far [cs:entry]
    jc fail
    mov si,state_text
    call serial_puts
    mov si,report
    mov cx,32
.state:
    lodsd
    call hex32
    mov al,' '
    call serial_char
    loop .state
    mov si,crlf
    call serial_puts
    call shutdown
    jc fail
    mov si,pass_text
    call serial_puts
    mov ax,4C00h
    int 21h

no_devices:
    mov si,absent_text
    call serial_puts
fail:
    sti
    call restore_vectors
    push cs
    pop ds
    push cs
    pop es
    cmp byte [device_flag],0
    je .no_state
    mov di,report
    mov ax,VM_OP_DEV_STATE
    call far [cs:entry]
    jc .no_state
    mov si,state_text
    call serial_puts
    mov si,report
    mov cx,32
.dump:
    lodsd
    call hex32
    mov al,' '
    call serial_char
    loop .dump
    mov si,crlf
    call serial_puts
.no_state:
    call shutdown
    push cs
    pop ds
    mov si,fail_text
    call serial_puts
    mov al,[stage]
    call hex8
    mov si,crlf
    call serial_puts
    mov ax,4C01h
    int 21h

; ---- Lifecycle ----
shutdown:
    cmp byte [device_flag],0
    je .session
    mov ax,VM_OP_DEV_END
    call far [cs:entry]
    jc .error
    mov byte [device_flag],0
.session:
    cmp byte [session_flag],0
    je .unbind
    mov ax,VM_OP_END
    call far [cs:entry]
    jc .error
    mov byte [session_flag],0
.unbind:
    cmp byte [bound_flag],0
    je .done
    mov ax,VM_OP_UNBIND_SCHED
    call far [cs:entry]
    jc .error
    mov byte [bound_flag],0
.done:
    clc
    ret
.error:
    stc
    ret

measure_tsc:
    push es
    mov ax,40h
    mov es,ax
    mov ebx,[es:6Ch]
.edge:
    cmp ebx,[es:6Ch]
    je .edge
    rdtsc
    mov esi,eax
    mov edi,edx
    mov ebx,[es:6Ch]
    add ebx,2
.wait:
    mov eax,[es:6Ch]
    sub eax,ebx
    js .wait
    rdtsc
    sub eax,esi
    sbb edx,edi
    ; two ticks = 109.85 ms: kHz = cycles / 109.85
    mov ecx,110
    div ecx
    mov [tsc_khz],eax
    pop es
    ret

; Wait AX BIOS ticks (virtual IRQ0 delivered by the profile).
wait_ticks:
    push es
    push eax
    push ebx
    push ax
    mov bx,40h
    mov es,bx
    pop bx
    movzx ebx,bx
    add ebx,[es:6Ch]
.loop:
    mov eax,[es:6Ch]                        ; ticks may arrive back to back:
    sub eax,ebx                             ; wait until now - target >= 0
    js .loop
    pop ebx
    pop eax
    pop es
    ret

set_focus:                                  ; BX focus
    mov ax,VM_OP_DEV_FOCUS
    call far [cs:entry]
    ret

; ---- Keyboard: a real INT 9 handler reading port 60h ----
keyboard_phase:
    mov byte [stage],1
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
    mov bx,1
    call set_focus
    jc fail
    mov word [key_count],0
    mov si,focused_text
    call serial_puts
    ; The harness types 'a' 'b' now: expect 1E 9E 30 B0.
    mov cx,90
.focused_wait:
    cmp word [key_count],4
    jae .focused_done
    mov ax,1
    call wait_ticks
    loop .focused_wait
.focused_done:
    mov si,keys_text
    call print_keys
    mov word [key_count],0
    xor bx,bx
    call set_focus
    jc fail
    mov si,unfocused_text
    call serial_puts
    mov ax,54                               ; ~3 s: harness types 'c'
    call wait_ticks
    mov si,withheld_text
    call print_keys
    mov word [key_count],0
    mov bx,1
    call set_focus
    jc fail
    mov si,hold_text
    call serial_puts
    ; Harness holds 'd' for 3 s: wait for its make code, then take focus
    ; away while it is still held. The model must release it (A0).
    mov cx,90
.hold_wait:
    cmp word [key_count],1
    jae .held
    mov ax,1
    call wait_ticks
    loop .hold_wait
.held:
    xor bx,bx
    call set_focus
    jc fail
    mov ax,9
    call wait_ticks
    mov si,release_text
    call print_keys
    cli
    xor ax,ax
    mov es,ax
    mov eax,[old_int9]
    mov [es:9*4],eax
    mov dword [old_int9],0
    sti
    push cs
    pop es
    mov ax,54                               ; physical release arrives unfocused
    call wait_ticks
    mov bx,1
    call set_focus
    ret

kbd_isr:
    push ax
    push bx
    push ds
    push cs
    pop ds
    in al,64h
    in al,60h
    mov bx,[key_count]
    cmp bx,32
    jae .full
    mov [key_log+bx],al
    inc word [key_count]
.full:
    mov al,20h
    out 20h,al
    pop ds
    pop bx
    pop ax
    iret

print_keys:
    call serial_puts
    mov cx,[key_count]
    mov si,key_log
    jcxz .done
.byte:
    lodsb
    call hex8
    mov al,' '
    call serial_char
    loop .byte
.done:
    mov si,crlf
    call serial_puts
    ret

; ---- Sound Blaster: single-cycle 8-bit DMA blocks, one IRQ7 per block ----
sb_phase:
    mov byte [stage],2
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
    jmp fail
.read_aa:
    mov dx,SB_BASE+0Ah
    in al,dx
    cmp al,0AAh
    jne fail
    mov al,0E1h
    call dsp_write
    call dsp_read
    mov [dsp_version],al
    call dsp_read
    mov [dsp_version+1],al
    ; Square wave, 11025 Hz: 25-sample period = 441 Hz.
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
    mov al,165                              ; 256 - 1000000/11025
    call dsp_write
    mov si,sb_text
    call serial_puts
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
    mov cx,36                               ; 2 s per block at most
.irq_wait:
    cmp [sb_irqs],bx
    jae .irq_done
    mov ax,1
    call wait_ticks
    loop .irq_wait
    pop cx
    jmp fail
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
    mov dword [old_irq7],0
    sti
    push cs
    pop es
    mov si,sb_done_text
    call serial_puts
    mov ax,[sb_irqs]
    call hex16
    mov al,' '
    call serial_char
    mov ax,[dsp_version]
    xchg al,ah
    call hex16
    mov si,crlf
    call serial_puts
    ret

dma_program:
    mov al,5
    out 0Ah,al                              ; mask channel 1
    xor al,al
    out 0Ch,al
    mov al,49h                              ; single, memory->device, ch 1
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

dsp_read:
    push cx
    push dx
    mov dx,SB_BASE+0Eh
    mov cx,10000
.empty:
    in al,dx
    test al,80h
    jnz .full
    loop .empty
.full:
    mov dx,SB_BASE+0Ah
    in al,dx
    pop dx
    pop cx
    ret

; ---- OPL2: one 440 Hz FM note on channel 0 for about one second ----
opl_phase:
    mov byte [stage],3
    mov si,opl_table
.write:
    lodsw
    cmp ax,0FFFFh
    je .notes
    call opl_write
    jmp .write
.notes:
    mov si,opl_text
    call serial_puts
    mov ax,0B032h                            ; key on, block 4, F 244h
    call opl_write
    mov ax,20
    call wait_ticks
    mov ax,0B012h                            ; key off
    call opl_write
    mov ax,4
    call wait_ticks
    mov si,opl_done_text
    call serial_puts
    ret

opl_write:                                  ; AH register, AL value
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

restore_vectors:
    cli
    xor ax,ax
    mov es,ax
    mov eax,[old_int9]
    test eax,eax
    jz .irq7
    mov [es:9*4],eax
.irq7:
    mov eax,[old_irq7]
    test eax,eax
    jz .done
    mov [es:0Fh*4],eax
    mov al,[old_mask]
    out 21h,al
.done:
    sti
    push cs
    pop es
    ret

; ---- Serial ----
hex32:
    push eax
    shr eax,16
    call hex16
    pop eax
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

; Register, value pairs (AH, AL) ending with FFFFh.
opl_table:
    dw 0120h, 0800h
    dw 2001h, 4010h, 60F0h, 8077h
    dw 2301h, 4300h, 63F0h, 8377h
    dw 0C000h, 0E000h, 0E300h
    dw 0A044h
    dw 0FFFFh

entry dd 0
tsc_khz dd 0
granted dd 0
audio_status dd 0
dma_physical dd 0
old_int9 dd 0
old_irq7 dd 0
dma_segment dw 0
key_count dw 0
sb_irqs dw 0
dsp_version dw 0
old_mask db 0
bound_flag db 0
session_flag db 0
device_flag db 0
stage db 0
key_log times 32 db 0
begin_text db '[DEVTEST] DEVICES GRANTED/AUDIO ',0
focused_text db '[DEVTEST] KEYS FOCUSED',13,10,0
keys_text db '[DEVTEST] FOCUSED BYTES ',0
unfocused_text db '[DEVTEST] KEYS UNFOCUSED',13,10,0
withheld_text db '[DEVTEST] UNFOCUSED BYTES ',0
hold_text db '[DEVTEST] HOLD KEY',13,10,0
release_text db '[DEVTEST] FOCUS-LOSS BYTES ',0
sb_text db '[DEVTEST] SB PLAY',13,10,0
sb_done_text db '[DEVTEST] SB DONE IRQS/VERSION ',0
opl_text db '[DEVTEST] OPL PLAY',13,10,0
opl_done_text db '[DEVTEST] OPL DONE',13,10,0
state_text db '[DEVTEST] STATE ',0
absent_text db '[DEVTEST] GUEST DEVICES NOT OFFERED',13,10,0
pass_text db '[DEVTEST] PASS actual V86 keyboard, SB16 DMA/IRQ and OPL through the session',13,10,0
fail_text db '[DEVTEST] FAIL stage=',0
crlf db 13,10,0
align 4
report times CVDEV_REPORT_BYTES db 0
descriptor:
    dd CVSCHED_MAGIC
    dw CVSCHED_VERSION,CVSCHED_BYTES
    times CVSCHED_BYTES-8 db 0
times 1024 db 0
stack_top:
program_end:
