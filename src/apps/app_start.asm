; CiukiOS desktop application module: header, event entry and the few
; helpers OpenWatcom expects from its C library (src/apps/app.h).
; Linked first, as a flat image based at 100h (wlink format raw, offset 100h);
; scripts/build_apps.sh writes the paragraphs needed into the header.
cpu 386
segment _TEXT class=CODE public align=16 use16
segment CONST class=DATA public align=2 use16
segment CONST2 class=DATA public align=2 use16
segment _DATA class=DATA public align=2 use16
segment _BSS class=BSS public align=2 use16
segment STACK class=STACK public align=16 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS STACK

extern app_event_
global _app_title
global app_entry
global intr_
global svc_
global far_call_req_
global far_call_req_far_
global far_regs_
global cpu_vendor_
global __U4M
global __I4M
global __U4D
global __I4D

segment _TEXT
header:
    db 'CAPP'                  ; 100h
    dw 1                       ; 104h version
    dw 0                       ; 106h paragraphs (build_apps.sh)
    dd 0                       ; 108h service entry (the shell)
    dw app_entry               ; 10Ch
    dw 0, 0                    ; 10Eh width, height (the module, at OPEN)
    dw _app_title              ; 112h
    dw 1                       ; 114h flags: resizable
    times 0x40-($-header) db 0 ; 116h host block (the shell)
    times 0x80 db 0            ; 140h argument / saved state
_app_title:
    times 64 db 0

; Far entry: AX = event, DX, BX, CX = arguments (app_event's registers).
; Every register but AX (the answer) is preserved for the shell.
app_entry:
    push bx
    push cx
    push dx
    push ds
    push es
    push bp
    push si
    push di
    mov bp,cs
    mov ds,bp
    mov es,bp
    mov [cs:saved_sp],sp
    mov [cs:saved_ss],ss
    cli
    mov ss,bp
    mov sp,stack_top
    sti
    cld
    call app_event_
    cli
    mov ss,[cs:saved_ss]
    mov sp,[cs:saved_sp]
    sti
    pop di
    pop si
    pop bp
    pop es
    pop ds
    pop dx
    pop cx
    pop bx
    retf

; int svc(int n, struct sargs *a): the shell's far service entry.
svc_:
    push bx
    push cx
    push dx
    push si
    push di
    push es
    mov si,dx
    call far [0x108]
    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; int intr(int n, struct regs *r): AX, BX, CX, DX, SI, DI, DS, ES in,
; the same and FLAGS out. Returns the carry flag.
intr_:
    push bx
    push cx
    push si
    push di
    push bp
    push es
    push ds
    mov [cs:intno],al
    mov bp,dx
    push bp
    push word [bp+12]
    mov es,[bp+14]
    mov ax,[bp+0]
    mov bx,[bp+2]
    mov cx,[bp+4]
    mov dx,[bp+6]
    mov si,[bp+8]
    mov di,[bp+10]
    pop ds
    db 0xCD
intno:
    db 0x21
    pushf
    push ds
    push ax
    mov ax,cs
    mov ds,ax
    mov bp,sp
    mov bp,[bp+6]               ; the regs pointer
    pop word [bp+0]
    mov [bp+2],bx
    mov [bp+4],cx
    mov [bp+6],dx
    mov [bp+8],si
    mov [bp+10],di
    pop word [bp+12]
    mov [bp+14],es
    pop word [bp+16]
    mov ax,[bp+16]
    and ax,1
    add sp,2
    pop ds
    pop es
    pop bp
    pop di
    pop si
    pop cx
    pop bx
    ret

; 32-bit multiply: DX:AX * CX:BX -> DX:AX.
__U4M:
__I4M:
    push esi
    push ecx
    push ebx
    mov si,dx
    shl esi,16
    mov si,ax
    shl ecx,16
    mov cx,bx
    mov eax,esi
    mul ecx
    mov edx,eax
    shr edx,16
    pop ebx
    pop ecx
    pop esi
    ret

; 32-bit divide: DX:AX / CX:BX -> quotient DX:AX, remainder CX:BX.
__U4D:
    push esi
    push edi
    mov si,dx
    shl esi,16
    mov si,ax
    mov di,cx
    shl edi,16
    mov di,bx
    mov eax,esi
    xor edx,edx
    test edi,edi
    jz .zero
    div edi
.zero:
    mov ebx,edx
    mov ecx,edx
    shr ecx,16
    mov edx,eax
    shr edx,16
    pop edi
    pop esi
    ret

__I4D:
    push esi
    push edi
    mov si,dx
    shl esi,16
    mov si,ax
    mov di,cx
    shl edi,16
    mov di,bx
    mov eax,esi
    cdq
    test edi,edi
    jz .zero
    idiv edi
.zero:
    mov ebx,edx
    mov ecx,edx
    shr ecx,16
    mov edx,eax
    shr edx,16
    pop edi
    pop esi
    ret

; void far_call_req(u16 seg, u16 off, void *req): a far call to seg:off
; with ES:DI = the request (MEDIA.DRV's contract). All registers kept.
far_call_req_:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es
    mov [cs:fcr_target],dx
    mov [cs:fcr_target+2],ax
    push ds
    pop es
    mov di,bx
    call far [cs:fcr_target]
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; void far_call_req_far(u16 seg, u16 off, u16 req_seg, u16 req_off):
; far-request variant for packets allocated outside the module's DGROUP.
far_call_req_far_:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push es
    mov [cs:fcr_target],dx
    mov [cs:fcr_target+2],ax
    mov es,bx
    mov di,cx
    call far [cs:fcr_target]
    pop es
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; int far_regs(u16 seg, u16 off, struct regs *r): like intr, a far call.
far_regs_:
    push bx
    push cx
    push si
    push di
    push bp
    push es
    push ds
    mov [cs:fcr_target],dx
    mov [cs:fcr_target+2],ax
    mov bp,bx
    push bp
    push word [bp+12]
    mov es,[bp+14]
    mov ax,[bp+0]
    mov bx,[bp+2]
    mov cx,[bp+4]
    mov dx,[bp+6]
    mov si,[bp+8]
    mov di,[bp+10]
    pop ds
    call far [cs:fcr_target]
    pushf
    push ds
    push eax
    mov ax,cs
    mov ds,ax
    mov bp,sp
    mov bp,[bp+8]
    pop word [bp+0]
    pop word [bp+22]
    mov [bp+2],bx
    mov [bp+4],cx
    mov [bp+6],dx
    mov [bp+8],si
    mov [bp+10],di
    pop word [bp+12]
    mov [bp+14],es
    pop word [bp+16]
    shr ecx,16
    mov [bp+18],cx
    shr edx,16
    mov [bp+20],dx
    mov ax,[bp+16]
    and ax,1
    add sp,2
    pop ds
    pop es
    pop bp
    pop di
    pop si
    pop cx
    pop bx
    ret

; int cpu_vendor(char *out): the CPUID vendor string (13 bytes) and the
; family, or "" and 3 on a CPU without CPUID.
cpu_vendor_:
    push ebx
    push ecx
    push edx
    push di
    mov di,ax
    pushfd
    pop eax
    mov ecx,eax
    xor eax,0x200000
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax,ecx
    test eax,0x200000
    jz .none
    xor eax,eax
    db 0x0F,0xA2                      ; cpuid (the 386 target lacks it)
    mov [di],ebx
    mov [di+4],edx
    mov [di+8],ecx
    mov byte [di+12],0
    mov eax,1
    db 0x0F,0xA2                      ; cpuid (the 386 target lacks it)
    shr eax,8
    and ax,0x0F
    jmp .done
.none:
    mov byte [di],0
    mov ax,3
.done:
    pop di
    pop edx
    pop ecx
    pop ebx
    ret

fcr_target dw 0,0
saved_sp dw 0
saved_ss dw 0

segment STACK
    resb 4096
stack_top:
