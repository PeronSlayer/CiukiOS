; Standalone QEMU CPU-transition probe. The boot sector loads this file at
; 1000:0100; the framebuffer guard is the unmodified production include.
bits 16
org 0x100
jmp start
db 'LFBG'
%macro export 1
    dw %1
    db %str(%1),0
%endmacro
export ready
export entered
export exited
export vc_fb_begin
export vc_fb_end
export vc_fb_limits
export vc_fb_limits.protected
export vc_fb_rm_jump
export vc_fb_real
export vc_fb_nmi_pm
export vc_fb_nmi_rm
export expected_nmi
export actual_nmi
export actual_trap
export saved_flags
dw 0

start:
    cli
    mov ax,cs
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,0xFF00
    cld
    ; This isolated test owns its A20 gate and real-mode interrupt vectors.
    in al,0x92
    and al,0xFE
    or al,2
    out 0x92,al
    xor ax,ax
    mov es,ax
    mov word [es:4],trap_handler
    mov [es:6],cs
    mov word [es:8],nmi_handler
    mov [es:10],cs
    mov word [es:0x2F*4],multiplex_handler
    mov [es:0x2F*4+2],cs
    ; A relocated IVT proves that the guard chains the saved IDTR's vector.
    xor si,si
    xor di,di
    mov ax,0x8000
    mov es,ax
    xor ax,ax
    mov ds,ax
    mov cx,512
    rep movsw
    push cs
    pop ds
    o32 lidt [original_idtr]
    o32 lgdt [original_gdtr]
    call vc_lfb_open
    jc fail
    mov byte [vc_lfb],1
    mov ax,0x3456
    mov es,ax
    mov ax,0x2345
    mov fs,ax
    mov ax,0x4567
    mov gs,ax
    mov eax,0x12345678
    mov ebx,0x87654321
    mov ecx,0x11223344
    mov edx,0x55667788
    mov esi,0x10203040
    mov edi,0x50607080
    mov ebp,0x98765432
ready:
    push word 0x0F57         ; IF, TF, DF, OF, ZF, AF, PF and CF all set
    popf
    call vc_fb_begin
entered:
    call vc_fb_begin         ; nested bursts must not overwrite saved state
    call vc_fb_end
    ; A burst really has flat ES, while IRQ and tracing are held off.
    pushf
    pop word [cs:inside_flags]
    mov byte [es:dword 0x110000],0xA5
    call vc_fb_end
exited:
    pushf
    pop word [cs:saved_flags]
    push word 2             ; stop the single-step test before verification
    popf
    cmp eax,0x12345678
    jne fail
    cmp ebx,0x87654321
    jne fail
    cmp ecx,0x11223344
    jne fail
    cmp edx,0x55667788
    jne fail
    cmp esi,0x10203040
    jne fail
    cmp edi,0x50607080
    jne fail
    cmp ebp,0x98765432
    jne fail
    mov ax,es
    cmp ax,0x3456
    jne fail
    mov ax,fs
    cmp ax,0x2345
    jne fail
    mov ax,gs
    cmp ax,0x4567
    jne fail
    cmp word [saved_flags],0x0F57
    jne fail
    test word [inside_flags],0x300
    jnz fail
    cmp byte [vc_fb_depth],0
    jne fail
    mov ax,[expected_nmi]
    cmp [actual_nmi],ax
    jne fail
    cmp word [actual_trap],0
    je fail
    call check_tables
    jc fail
    mov si,pass_text
    jmp finish
fail:
    mov si,fail_text
finish:
    cld
.byte:
    lodsb
    test al,al
    jz .exit
    out 0xE9,al
    jmp .byte
.exit:
    mov dx,0xF4
    mov al,0x10
    out dx,al
    cli
    hlt

trap_handler:
    push ax
    smsw ax
    test al,1
    jnz interrupt_fail
    inc word [cs:actual_trap]
    pop ax
    iret

nmi_handler:
    push ax
    smsw ax
    test al,1
    jnz interrupt_fail
    mov ax,es
    cmp ax,0x3456
    jne interrupt_fail
    call check_tables
    jc interrupt_fail
    inc word [cs:actual_nmi]
    pop ax
    iret

interrupt_fail:
    cli
    mov al,'!'
    out 0xE9,al
    mov dx,0xF4
    mov al,0x11
    out dx,al
    hlt

check_tables:
    push eax
    o32 sidt [cs:observed_idtr]
    o32 sgdt [cs:observed_gdtr]
    mov ax,[cs:observed_idtr]
    cmp ax,[cs:original_idtr]
    jne .bad
    mov eax,[cs:observed_idtr+2]
    cmp eax,[cs:original_idtr+2]
    jne .bad
    mov ax,[cs:observed_gdtr]
    cmp ax,[cs:original_gdtr]
    jne .bad
    mov eax,[cs:observed_gdtr+2]
    cmp eax,[cs:original_gdtr+2]
    jne .bad
    clc
    pop eax
    ret
.bad:
    stc
    pop eax
    ret

multiplex_handler:
    iret

original_idtr dw 0x3FF
    dd 0x80000
original_gdtr dw 0x2F
    dd 0x70000
observed_idtr times 6 db 0
observed_gdtr times 6 db 0
expected_nmi dw 0
actual_nmi dw 0
actual_trap dw 0
saved_flags dw 0
inside_flags dw 0
pass_text db 'LFB GUARD PASS',10,0
fail_text db 'LFB GUARD FAIL',10,0

; Unused renderer exports supplied for the complete production include.
vc_map:
    stc
    ret
vc_access_bytes dd 0x100000
vc_pitch dw 800
vc_bytes db 1
vc_colors times 256 dd 0
%include "src/com/vbe_fb.inc"
