; Minimal flat 32-bit native process probe.
;
; This payload has no DOS or BIOS dependencies and makes no port I/O. Its only
; operating-system boundary is INT 80h, whose ring-3 ABI is described in
; src/native/README.md. The future loader maps code/data into a private address
; space and supplies a separate zeroed stack; it must provide an INT 80h gate
; callable from CPL3 before entering this image.

bits 32
cpu 386
org 0

%ifndef NATIVE_DATA_ONLY
section .text
entry:
    ; report(tag=1, value=0xC1A0) -- a deterministic smoke marker
    mov eax, 1
    mov ebx, 1
    mov ecx, 0xC1A0
    int 0x80

    ; ESI points to the process's private writable data region.
    mov eax, 1
    mov ebx, 2
    mov ecx, [esi]
    int 0x80

    ; exit(status=0)
    xor eax, eax
    xor ebx, ebx
    int 0x80
.unexpected_return:
    jmp .unexpected_return
%endif

%ifndef NATIVE_CODE_ONLY
section .data
sample_cookie: dd 0x4349554B ; "CIUK" little-endian test data
%endif
