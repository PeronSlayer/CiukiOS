; CiukiOS HDPMI 3.24 protected-mode VGA I/O adapter, original implementation.
; NASM -f obj. OpenWatcom 32-bit flat cdecl. No runtime or hardware passthrough.
;
; Authoritative ABI (the packaged host, not the older fork):
; https://github.com/Baron-von-Riedesel/HX/blob/f2276db9accfc57facf2588bc016a27130597bb1/Src/HDPMI/HDPMIAPI.TXT
; I2FHDPMI.ASM:is0006/is0007 and HDPMI.ASM:ioinstr1/2/3 at the same revision.
; https://www.delorie.com/djgpp/doc/dpmi/ch4.5.html (32-bit exception frame).
; VSBHDA HAPI.ASM/STACKIO.ASM were inspected to confirm the calling convention;
; no handler code is copied. In particular, this adapter never uses AX=8 to
; bypass virtual video, never changes another driver's context mode or traps.

bits 32
cpu 386
segment _TEXT public align=16 use32 class=CODE

global _cvio_init, _cvio_install, _cvio_remove, _cvio_set_status1
extern _cvga_read_port, _cvga_write_port

%define S_HANDLE      0
%define S_TRAPS       4
%define S_READS       8
%define S_WRITES     12
%define S_UNSUPPORTED 16
%define S_FATAL      20
%define S_REENTRIES  24
%define S_FLAGS      28
%define S_PORT       32
%define S_EIP        36

; Four saved segment registers, PUSHAD, direction word, DPMI 0.9 frame.
%define F_EDX       36
%define F_ECX       40
%define F_EAX       44
%define F_OUT       48
%define F_ERROR     60
%define F_EIP       64

_cvio_init:
    cmp dword [_cvio_stats_data + S_HANDLE], 0
    jne .installed
    cmp byte [io_busy], 0
    jne .installed
    mov eax, [esp+4]
    test eax, eax
    jz .nostate
    mov [_cvio_state], eax
    push edi
    xor eax, eax
    mov edi, _cvio_stats_data
    mov ecx, 10
.clear:
    mov [edi], eax
    add edi, 4
    loop .clear
    pop edi
    mov [io_status1], eax
    ret
.installed:
    mov eax, 10
    ret
.nostate:
    mov eax, 1
    ret

_cvio_set_status1:
    mov eax, [esp+4]
    and eax, 9
    mov [io_status1], eax
    ret

_cvio_install:
    push ebp
    push ebx
    push esi
    push edi
    push ds
    push es
    cmp dword [_cvio_stats_data+S_HANDLE], 0
    jne .already
    cmp dword [_cvio_state], 0
    je .nostate
    ; CS-relative access to our saved DS requires equal CS and DS bases.
    xor ebx, ebx
    mov bx, ds
    mov eax, 6
    int 31h
    jc .notflat
    movzx ebp, cx
    shl ebp, 16
    mov bp, dx
    xor ebx, ebx
    mov bx, cs
    mov eax, 6
    int 31h
    jc .notflat
    movzx eax, cx
    shl eax, 16
    mov ax, dx
    cmp eax, ebp
    jne .notflat
    mov [io_data_selector], ds
    mov [io_stack_pointer+4], ds
    mov [io_trap_procs+4], cs
    mov [io_trap_procs+10], cs
    xor edi, edi
    mov es, di
    mov esi, io_vendor
    mov eax, 168ah
    int 2fh
    test al, al
    jnz .nohost
    mov ax, es
    test ax, ax
    jz .nohost
    mov [io_api], edi
    mov [io_api+4], ax
    mov esi, io_trap_procs
    mov edx, 3b0h
    mov ecx, 30h
    mov eax, 6
    call far [io_api]
    jc .busy
    test eax, eax
    jz .busy                    ; never treat handle zero as releasable ownership
    mov [_cvio_stats_data+S_HANDLE], eax
    xor eax, eax
    jmp .done
.already: mov eax, 10
    jmp .done
.nostate: mov eax, 1
    jmp .done
.notflat: mov eax, 9
    jmp .done
.nohost: mov eax, 2
    jmp .done
.busy: mov eax, 3
.done:
    pop es
    pop ds
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

_cvio_remove:
    push ebx
    push esi
    push edi
    mov edx, [_cvio_stats_data+S_HANDLE]
    test edx, edx
    jz .empty
    mov eax, 7
    call far [io_api]
    jc .failed
    mov dword [_cvio_stats_data+S_HANDLE], 0
.empty:
    xor eax, eax
    jmp .done
.failed:
    mov eax, 4                  ; retain ownership/state on failure
.done:
    pop edi
    pop esi
    pop ebx
    ret

io_input:
    push dword 0
    jmp io_exception
io_output:
    push dword 1
io_exception:
    pushad
    push ds
    push es
    push fs
    push gs
    mov ebp, esp
    mov ax, cs:[io_data_selector]
    mov ds, ax
    mov es, ax
    mov ax, ss
    mov fs, ax                 ; old exception stack stays addressable across calls
    cld
    mov ecx, [ss:ebp+F_ERROR]
    mov edx, [ss:ebp+F_EIP]
    mov [_cvio_stats_data+S_FLAGS], ecx
    mov [_cvio_stats_data+S_EIP], edx
    inc dword [_cvio_stats_data+S_TRAPS]
    movzx esi, word [ss:ebp+F_EDX]
    test ecx, 8
    jnz .port_ready
    test ecx, 40h
    jz .port_ready
    mov esi, ecx
    shr esi, 8
    and esi, 255
.port_ready:
    mov [_cvio_stats_data+S_PORT], esi
    and edx, 0                 ; consume exactly the host-decoded instruction
    mov edx, ecx
    and edx, 7
    jz .bad_length
    add [ss:ebp+F_EIP], edx
    test ecx, 8
    jnz .string_unsupported
    mov ebx, ecx
    and ebx, 30h
    cmp ebx, 20h
    je .bad_width
    shr ebx, 4
    inc ebx                    ; 00->1, 01->2, 11->4
    cmp esi, 3b0h
    jb .range_crossing
    lea edx, [esi+ebx]
    cmp edx, 3e0h
    ja .range_crossing
    mov al, 1
    xchg al, [io_busy]
    test al, al
    jnz .reentry
    mov [io_saved_stack], ebp
    mov [io_saved_stack+4], ss
    lss esp, [io_stack_pointer]
    xor edi, edi
    mov dword [io_result], 0
    cmp dword [fs:ebp+F_OUT], 0
    jne .out_begin
    inc dword [_cvio_stats_data+S_READS]
.read_byte:
    push dword [io_status1]
    lea eax, [esi+edi]
    push eax
    push dword [_cvio_state]
    call _cvga_read_port
    add esp, 12
    movzx eax, al
    lea ecx, [edi*8]
    shl eax, cl
    or [io_result], eax
    inc edi
    cmp edi, ebx
    jb .read_byte
    mov eax, [io_result]
    cmp ebx, 1
    je .set_byte
    cmp ebx, 2
    je .set_word
    mov [fs:ebp+F_EAX], eax
    jmp .finished
.set_byte:
    mov [fs:ebp+F_EAX], al
    jmp .finished
.set_word:
    mov [fs:ebp+F_EAX], ax
    jmp .finished
.out_begin:
    inc dword [_cvio_stats_data+S_WRITES]
.write_byte:
    mov eax, [fs:ebp+F_EAX]
    lea ecx, [edi*8]
    shr eax, cl
    and eax, 255
    push eax
    lea eax, [esi+edi]
    push eax
    push dword [_cvio_state]
    call _cvga_write_port
    add esp, 12
    inc edi
    cmp edi, ebx
    jb .write_byte
.finished:
    mov byte [io_busy], 0
    lss esp, [io_saved_stack]
    jmp .restore
.bad_length:
    ; Corrupt/unknown ABI must not spin forever at one EIP. Mark fatal and
    ; consume one byte; caller must terminate the session on this diagnostic.
    inc dword [ss:ebp+F_EIP]
    mov eax, 8
    jmp .fatal
.string_unsupported:
    mov eax, 5
    jmp .fatal
.bad_width:
    mov eax, 6
    jmp .fatal
.range_crossing:
    mov eax, 11
    jmp .fatal
.reentry:
    inc dword [_cvio_stats_data+S_REENTRIES]
    mov eax, 7
.fatal:
    inc dword [_cvio_stats_data+S_UNSUPPORTED]
    cmp dword [_cvio_stats_data+S_FATAL], 0
    jne .restore
    mov [_cvio_stats_data+S_FATAL], eax
.restore:
    pop gs
    pop fs
    pop es
    pop ds
    popad
    add esp, 4                 ; direction
    retf                       ; exact DPMI exception-frame return

segment _DATA public align=16 use32 class=DATA
group DGROUP _DATA
global _cvio_state, _cvio_stats_data
_cvio_state dd 0
_cvio_stats_data times 10 dd 0
io_status1 dd 0
io_result dd 0
io_busy db 0
align 4
io_data_selector dw 0
io_api dd 0
    dw 0
io_trap_procs dd io_input
    dw 0
    dd io_output
    dw 0
io_stack_pointer dd io_stack_end
    dw 0
io_saved_stack dd 0
    dw 0
io_vendor db 'HDPMI',0
align 16
io_stack times 8192 db 0
io_stack_end:
