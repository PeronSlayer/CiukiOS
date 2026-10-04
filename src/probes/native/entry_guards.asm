; Deterministic CPL3 fault/guard probes for the bounded CN32 entry gate.
; The test harness wraps code/data in a CRC-checked CN32 image. These execute
; actual guest instructions; no host memory patch or fault injection is used.
bits 32
cpu 386
org 0

%ifndef NATIVE_CASE
 %error NATIVE_CASE is required
%endif

%ifdef NATIVE_DATA_ONLY
 dd 0x4349554B
 dd 0
%else
%if NATIVE_CASE = 1                  ; inherited DOS page is supervisor-only
 mov dword [0x10000],0xDEADC0DE
%elif NATIVE_CASE = 2                ; private user code is read-only
 mov byte [0x40001000],0x90
%elif NATIVE_CASE = 3                ; all native I/O denied by owned TSS
 mov dx,0x21
 out dx,al
%elif NATIVE_CASE = 4                ; DOS INT gate is not native ABI
 int 0x21
%elif NATIVE_CASE = 5                ; stack has an absent lower guard page
 mov esp,0x40040000
 push byte 0
%elif NATIVE_CASE = 6                ; POPFD cannot remove the step budget
 push byte 2
 popfd
.loop:
 jmp .loop
%elif NATIVE_CASE = 7                ; CPL3 + writable zeroed heap + syscall ABI
 xor eax,eax
 mov ax,cs
 and eax,3
 cmp eax,3
 jne .bad
 cmp dword [esi+4],0
 jne .bad
 mov dword [esi+4],0x78563412
 cmp dword [esi+4],0x78563412
 jne .bad
 mov eax,0xFFFF
 int 0x80
 cmp eax,-38
 jne .bad
 mov eax,1
 mov ebx,1
 mov ecx,3
 int 0x80
 mov eax,1
 mov ebx,2
 mov ecx,[esi]
 int 0x80
 xor eax,eax
 xor ebx,ebx
 int 0x80
.bad:
 ud2
%elif NATIVE_CASE = 8                ; valid nonzero process exit status
 xor eax,eax
 mov ebx,7
 int 0x80
%elif NATIVE_CASE = 9                ; absent native FPU state cannot corrupt DOS
 fld1
%else
 %error Unknown NATIVE_CASE
%endif
 ; A forbidden operation returning would be an actual gate defect.
 ud2
%endif
