bits 16
section .start
global _start
extern media_service, media_request, _bss_start, _bss_end, _stack_top
extern module_paragraphs
; Raw module header: entry offset0, magic at4, ABI at12, loaded image size14,
; allocation paragraphs16, request size18. ES:DI is the request, copied both
; ways; all caller registers/segments/flags are preserved.
_start:
    jmp near entry
    nop
    db 'CMEDIA01'
    dw 1
    dw module_image_end
    dw module_paragraphs
    dw 1192
    dw media_request
    times 32-($-$$) db 0
entry:
    pushfd
    pushad
    push ds
    push es
    push fs
    push gs
    cli
    mov [cs:caller_ss],ss
    mov [cs:caller_sp],sp
    mov ax,cs
    mov ss,ax
    mov sp,_stack_top
    mov [cs:packet_off],di
    mov ax,es
    mov [cs:packet_seg],ax
    mov ax,cs
    mov ds,ax
    mov es,ax
    cld
    cmp byte [initialized],1
    je .initialized
    mov di,_bss_start
    mov cx,_bss_end
    sub cx,di
    xor ax,ax
    rep stosb
    mov byte [initialized],1
.initialized:
    mov ax,[packet_seg]
    mov ds,ax
    mov si,[cs:packet_off]
    mov di,media_request
    mov cx,1192
    rep movsb
    push cs
    pop ds
    sti
    call media_service
    mov ax,[packet_seg]
    mov es,ax
    mov di,[packet_off]
    mov si,media_request
    mov cx,1192
    cld
    rep movsb
    cli
    mov ax,[caller_ss]
    mov ss,ax
    mov sp,[cs:caller_sp]
    pop gs
    pop fs
    pop es
    pop ds
    popad
    popfd
    retf
section .data
initialized db 0
caller_ss dw 0
caller_sp dw 0
packet_off dw 0
packet_seg dw 0
extern module_image_end
