; CN32 process isolation probe. Assemble with -DWRITER=1 or -DREADER=1.
; The initialized data occupies four bytes; byte +100h is within the same
; mapped page but outside the copied image, and must start zero on every run.
bits 32
cpu 386
org 0

%ifdef WRITER
    mov dword [esi+100h], 0A55A5AA5h
    mov ecx, [esi+100h]
    mov eax, 1
    mov ebx, 1
    int 80h
%endif

%ifdef READER
    mov ecx, [esi+100h]
    test ecx, ecx
    jnz bad_reuse
    mov eax, 1
    mov ebx, 1
    int 80h
%endif

    mov eax, 1
    mov ebx, 2
    mov ecx, 4349554Bh                 ; CIUK sample marker for NATIVE.COM
    int 80h
    xor eax, eax
%ifdef WRITER
    mov ebx, 3                        ; distinctive successful writer result
%else
    mov ebx, 4                        ; distinctive successful reader result
%endif
    int 80h

%ifdef READER
bad_reuse:
    xor eax, eax
    mov ebx, 77                       ; nonzero native exit on stale data
    int 80h
%endif

unreachable:
    jmp unreachable
