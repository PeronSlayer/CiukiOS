; CiukiOS DPMI 0.9 exception-0Eh thunk for dpmi_video_fault.c (NASM -f obj,
; OpenWatcom 32-bit flat cdecl). Original implementation.
; Host frame (32-bit handler): return EIP, return CS, error, EIP, CS, EFLAGS,
; ESP, SS. The frame lives on the host's locked exception stack (SS), so it
; is copied into cvpm_frame_data (DS), handled on a private stack, and the
; register/EIP/EFLAGS results are copied back before RETF to the host.

bits 32
cpu 386
segment _TEXT public align=16 use32 class=CODE

global _cvpm_handler, _cvpm_frame_data, _cvpm_stats_data
extern _cvpm_fault

%define FRAME_DWORDS 20                 ; 4 segments + 8 PUSHAD + 8 host dwords
%define STATS_REENTERED 28

_cvpm_handler:
    pushad
    push ds
    push es
    push fs
    push gs
    mov ebp,esp
    mov ax,[cs:_cvpm_data_selector]
    mov ds,ax
    mov es,ax
    cld
    mov al,1
    xchg al,[cvpm_busy]
    test al,al
    jnz .reentered
    xor ecx,ecx
.copy_in:
    mov eax,[ss:ebp+ecx*4]
    mov [_cvpm_frame_data+ecx*4],eax
    inc ecx
    cmp ecx,FRAME_DWORDS
    jb .copy_in
    mov [cvpm_saved_stack],ebp
    mov [cvpm_saved_stack+4],ss
    mov [cvpm_stack_pointer+4],ds
    lss esp,[cvpm_stack_pointer]
    call _cvpm_fault
    lss esp,[cvpm_saved_stack]
    mov ebx,eax
    ; GPRs at +16..+47 (except the pushed ESP), EIP at +60, EFLAGS at +68.
    mov ecx,4
.copy_out:
    cmp ecx,7                            ; PUSHAD's ESP slot is not restored
    je .next
    mov eax,[_cvpm_frame_data+ecx*4]
    mov [ss:ebp+ecx*4],eax
.next:
    inc ecx
    cmp ecx,12
    jb .copy_out
    mov eax,[_cvpm_frame_data+15*4]
    mov [ss:ebp+15*4],eax
    mov eax,[_cvpm_frame_data+17*4]
    mov [ss:ebp+17*4],eax
    mov byte [cvpm_busy],0
    test ebx,ebx
    jnz .chain
    pop gs
    pop fs
    pop es
    pop ds
    popad
    retf
.reentered:
    inc dword [_cvpm_stats_data+STATS_REENTERED]
.chain:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    jmp far [cs:_cvpm_previous_offset]

segment _DATA public align=16 use32 class=DATA
group DGROUP _DATA
global _cvpm_previous_offset, _cvpm_previous_selector, _cvpm_data_selector
align 4
_cvpm_frame_data times FRAME_DWORDS dd 0
_cvpm_stats_data times 8 dd 0
_cvpm_previous_offset dd 0
_cvpm_previous_selector dw 0
_cvpm_data_selector dw 0
cvpm_busy db 0
align 4
cvpm_saved_stack dd 0
    dw 0
cvpm_stack_pointer dd cvpm_stack_end
    dw 0
align 16
cvpm_stack times 16384 db 0
cvpm_stack_end:
