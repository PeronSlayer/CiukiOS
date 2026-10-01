; LFN.COM - CiukiOS VFAT long file names (the INT 21h AX=71xxh API).
;
; A resident extension of the CiukiDOS kernel, run by the desktop shell at
; startup (docs/design-long-file-names-2026-09-30.md). The kernel image has no
; room left (43,257 of 43,264 bytes), so the long name API lives here:
;   - an INT 21h hook in the IVT (the kernel refuses AH=25h for its own
;     vector) takes AH=71h, plus AH=41h/56h/3Ah to drop the long name entries
;     of what DOS programs delete or rename through the 8.3 API, and AH=00h/4Ch
;     to free the terminating program's find handles; everything else goes
;     straight to the kernel;
;   - lfn.c does the work on the kernel's own sector I/O and 8.3 services.
;     The kernel's routines and variables are found through the listing of
;     the very kernel it was built with (lfn_kernel.inc, scripts/build_lfn.sh)
;     and checked at install: another kernel leaves the extension out.
;
; Memory: a TSR in conventional memory, so every DOS VM (a copy of the first
; 640 KB) has its own copy and its own find handles. Install code (ITEXT) is
; dropped when it goes resident.
;
; Linked first by wlink (format raw bin, offset 100h) as a .COM image.
cpu 386
segment _TEXT class=CODE public align=16 use16
segment CONST class=DATA public align=2 use16
segment CONST2 class=DATA public align=2 use16
segment _DATA class=DATA public align=2 use16
segment _BSS class=BSS public align=2 use16
segment STACK class=STACK public align=16 use16
segment ITEXT class=ICODE public align=16 use16
group DGROUP _TEXT CONST CONST2 _DATA _BSS STACK ITEXT

%include "lfn_kernel.inc"

extern lfn_dispatch_
extern lfn_init_
global start
global _R
global _k_seg
global _kl_fat_valid
global _kl_fat_dirty
global _kl_fat_sector
global _kl_fat_buf
global _kl_indos
global kdos_
global kio_
global mul16_
global com1_
global __U4M
global __I4M
global __U4D
global __I4D

segment _TEXT
start:
    jmp install

; ---------------------------------------------------------------------------
; The INT 21h hook.
int21_entry:
    cmp ah,71h
    je .ours
    cmp ah,41h
    je .ours
    cmp ah,56h
    je .ours
    cmp ah,3Ah
    je .ours
    cmp ah,4Ch
    je .ours
    test ah,ah
    je .ours
.chain:
    jmp far [cs:old21]
.ours:
    cmp byte [cs:busy],0
    jne .busy
    mov byte [cs:busy],1
    mov [cs:_R+0],ax
    mov [cs:_R+2],bx
    mov [cs:_R+4],cx
    mov [cs:_R+6],dx
    mov [cs:_R+8],si
    mov [cs:_R+10],di
    mov [cs:_R+12],ds
    mov [cs:_R+14],es
    mov word [cs:_R+16],0
    mov [cs:_R+18],bp
    ; lfn_dispatch may return "pass through" after parsing an ordinary 8.3
    ; path. Keep the caller's original registers independently of _R: the
    ; pass-through path must not forward scratch values to the DOS kernel.
    mov [cs:saved_ax],ax
    mov [cs:saved_bx],bx
    mov [cs:saved_cx],cx
    mov [cs:saved_dx],dx
    mov [cs:saved_si],si
    mov [cs:saved_di],di
    mov [cs:saved_ds],ds
    mov [cs:saved_es],es
    mov [cs:saved_bp],bp
    mov [cs:caller_sp],sp
    mov [cs:caller_ss],ss
    mov ax,cs
    cli
    mov ss,ax
    mov sp,stack_top
    sti
    mov ds,ax
    mov es,ax
    cld
    call lfn_dispatch_                  ; AX = 0 handled, else chain
    cli
    mov ss,[cs:caller_ss]
    mov sp,[cs:caller_sp]
    sti
    test ax,ax
    jnz .pass
    ; Handled: the caller's registers and carry from _R.
    mov bp,sp
    mov al,[cs:_R+16]
    and al,1
    and byte [bp+4],0FEh
    or [bp+4],al
    mov ax,[cs:_R+0]
    mov bx,[cs:_R+2]
    mov cx,[cs:_R+4]
    mov dx,[cs:_R+6]
    mov si,[cs:_R+8]
    mov di,[cs:_R+10]
    mov es,[cs:_R+14]
    mov bp,[cs:_R+18]
    mov ds,[cs:_R+12]
    mov byte [cs:busy],0
    iret
.pass:
    ; Not ours after all: the caller's registers as they came, to the kernel.
    mov ax,[cs:saved_ax]
    mov bx,[cs:saved_bx]
    mov cx,[cs:saved_cx]
    mov dx,[cs:saved_dx]
    mov si,[cs:saved_si]
    mov di,[cs:saved_di]
    mov es,[cs:saved_es]
    mov bp,[cs:saved_bp]
    mov ds,[cs:saved_ds]
    mov byte [cs:busy],0
    jmp far [cs:old21]
.busy:
    ; Re-entered (an interrupt handler calling DOS while InDOS is set): the
    ; long name API reports itself absent, the 8.3 calls go on unchanged.
    cmp ah,71h
    jne .chain
    mov ax,7100h
    push bp
    mov bp,sp
    or byte [bp+6],1
    pop bp
    iret

; int kdos(struct regs *r): the kernel's INT 21h (the vector before ours) with
; AX BX CX DX SI DI DS ES from *r; results back into *r, FLAGS at +16.
; Returns the carry. Preserves every register but AX.
kdos_:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    mov bp,ax
    push bp
    mov ax,[bp+0]
    mov bx,[bp+2]
    mov cx,[bp+4]
    mov dx,[bp+6]
    mov si,[bp+8]
    mov di,[bp+10]
    mov es,[bp+14]
    mov ds,[bp+12]
    pushf
    call far [cs:old21]
    pushf
    push ds
    push ax
    mov ax,cs
    mov ds,ax
    mov bp,sp
    mov bp,[bp+6]                       ; the regs pointer
    pop word [bp+0]
    pop word [bp+12]
    pop word [bp+16]
    mov [bp+2],bx
    mov [bp+4],cx
    mov [bp+6],dx
    mov [bp+8],si
    mov [bp+10],di
    mov [bp+14],es
    mov ax,[bp+16]
    and ax,1
    add sp,2
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; int kio(struct kio *q): one sector through the kernel's own disk routine
; (read_sector_lba32 / write_sector_lba32: EDD, CHS fallback, retries, its
; disk stack, the volume's LBA offset). q = { u32 lba; u16 off, seg, write }.
; The routines are near procedures in the kernel segment: they return to a
; RETF of the kernel, which returns here. AX = 0 done, 1 failed.
kio_:
    push bx
    push cx
    push dx
    push si
    push di
    push bp
    push ds
    push es
    mov si,ax
    mov ax,[si]
    mov dx,[si+2]
    mov bx,[si+4]
    mov es,[si+6]
    mov di,[si+8]
    push cs
    push word .back
    push word KL_RETF
    push word [cs:_k_seg]
    test di,di
    jnz .write
    push word KL_READ
    retf
.write:
    push word KL_WRITE
    retf
.back:
    mov ax,0
    adc ax,0
    pop es
    pop ds
    pop bp
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    ret

; u32 mul16(u16 a, u16 b): AX * DX -> DX:AX.
mul16_:
    mul dx
    ret

; void com1(const char *s): a line on COM1 for the test gates.
com1_:
    push dx
    push si
    mov si,ax
.char:
    lodsb
    test al,al
    jz .done
    call com_char
    jmp .char
.done:
    mov al,13
    call com_char
    mov al,10
    call com_char
    pop si
    pop dx
    ret
com_char:
    push cx
    push ax
    mov dx,3FDh
    mov cx,30000
.wait:
    in al,dx
    test al,20h
    jnz .ready
    loop .wait
.ready:
    pop ax
    mov dx,3F8h
    out dx,al
    pop cx
    ret

; 32-bit helpers OpenWatcom expects (as in src/apps/app_start.asm).
; DX:AX * CX:BX -> DX:AX
__U4M:
__I4M:
    push si
    push di
    mov si,ax
    mov di,dx
    mul bx
    push dx
    push ax
    mov ax,di
    mul bx
    mov di,ax
    mov ax,si
    mul cx
    add di,ax
    pop ax
    pop dx
    add dx,di
    pop di
    pop si
    ret
; DX:AX / CX:BX -> DX:AX quotient, CX:BX remainder (unsigned)
__U4D:
    push bp
    push si
    push di
    push dx
    push ax
    push cx
    push bx
    mov bp,sp
    mov eax,[bp+4]
    mov ebx,[bp+0]
    xor edx,edx
    test ebx,ebx
    jz .zero
    div ebx
.zero:
    mov ebx,edx
    mov edx,eax
    shr edx,16
    mov ecx,ebx
    shr ecx,16
    add sp,8
    pop di
    pop si
    pop bp
    ret
__I4D:
    push bp
    push si
    push di
    push dx
    push ax
    push cx
    push bx
    mov bp,sp
    mov eax,[bp+4]
    mov ebx,[bp+0]
    cdq
    test ebx,ebx
    jz .zero
    idiv ebx
.zero:
    mov ebx,edx
    mov edx,eax
    shr edx,16
    mov ecx,ebx
    shr ecx,16
    add sp,8
    pop di
    pop si
    pop bp
    ret

segment _DATA
old21 dd 0
_k_seg dw 0
_kl_fat_valid dw KL_FAT_VALID
_kl_fat_dirty dw KL_FAT_DIRTY
_kl_fat_sector dw KL_FAT_SECTOR
_kl_fat_buf dw KL_FAT_BUF_SEG
_kl_indos dw KL_INDOS
busy db 0
align 2
caller_sp dw 0
caller_ss dw 0
; The caller's registers: ax bx cx dx si di ds es flags bp (lfn.c: struct regs)
_R times 10 dw 0
; Their values on entry, for a call passed on to the kernel.
saved_ax equ _R+20
saved_bx equ _R+22
saved_cx equ _R+24
saved_dx equ _R+26
saved_si equ _R+28
saved_di equ _R+30
saved_ds equ _R+32
saved_es equ _R+34
saved_bp equ _R+38
    times 10 dw 0

segment STACK
    times 1536 db 0
stack_top:
resident_end:

; ---------------------------------------------------------------------------
; Install (dropped when resident).
segment ITEXT
install:
    cld
    mov sp,install_stack_top
    ; Already there? (private function 71FFh, BX='LF')
    mov ax,71FFh
    mov bx,4C46h
    stc
    int 21h
    jc .absent
    cmp bx,4F4Bh
    jne .absent
    mov ax,msg_present
    call com1_
    mov ax,4C00h
    int 21h
.absent:
    ; The kernel: its segment from AH=34h (the InDOS byte), which must sit
    ; where this build's listing puts it, and the routines we call must be
    ; the ones listed.
    mov ah,34h
    int 21h
    cmp bx,KL_INDOS
    jne .mismatch
    mov [_k_seg],es
    cmp dword [es:KL_READ],KL_READ_SIG
    jne .mismatch
    cmp dword [es:KL_WRITE],KL_WRITE_SIG
    jne .mismatch
    cmp byte [es:KL_RETF],0CBh
    jne .mismatch
    push cs
    pop es
    ; Save the vector we will call (the kernel's) before any disk access.
    push es
    xor ax,ax
    mov es,ax
    mov ax,[es:21h*4]
    mov [old21],ax
    mov ax,[es:21h*4+2]
    mov [old21+2],ax
    pop es
    call lfn_init_                      ; the volume: AX = 0 usable
    test ax,ax
    jnz .novolume
    ; The environment block is not needed resident.
    mov es,[2Ch]
    mov ah,49h
    int 21h
    mov word [2Ch],0
    push cs
    pop es
    ; The IVT entry itself.
    xor ax,ax
    mov es,ax
    cli
    mov word [es:21h*4],int21_entry
    mov [es:21h*4+2],cs
    sti
    push cs
    pop es
    mov ax,msg_ok
    call com1_
    mov dx,resident_end               ; offset in the .COM segment
    add dx,15
    shr dx,4
    mov ax,3100h
    int 21h
.mismatch:
    push cs
    pop es
    mov ax,msg_kernel
    call com1_
    mov ax,4C01h
    int 21h
.novolume:
    mov ax,msg_volume
    call com1_
    mov ax,4C02h
    int 21h

msg_ok db '[LFN] long file names installed',0
msg_present db '[LFN] already installed',0
msg_kernel db '[LFN] not installed: unknown kernel build',0
msg_volume db '[LFN] not installed: no FAT16 volume',0
    align 2
    times 256 db 0
install_stack_top:
