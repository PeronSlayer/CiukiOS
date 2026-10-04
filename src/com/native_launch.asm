; NATIVE.COM - transport a bounded CN32 image to CVSESSION's trusted loader.
; DOS handles the file while V86 is active. The protected host independently
; validates and copies the image before switching page tables or entering CPL3.
bits 16
cpu 386
org 100h
%include "src/vm/session_abi.inc"

CN32_HEADER_BYTES equ 36
CN32_MAX_IMAGE_BYTES equ 61440

start:
    cli
    mov ax, cs
    mov ss, ax
    mov sp, stack_top
    sti
    push cs
    pop ds
    push cs
    pop es
    mov bx, (image_end - $$ + 100h + 15) / 16
    mov ah, 4Ah
    int 21h
    jc fail_memory
    call parse_path
    jc fail_path

    mov dx, path
    mov ax, 3D00h
    int 21h
    jc fail_open
    mov [file_handle], ax
    mov bx, ax
    mov dx, header
    mov cx, CN32_HEADER_BYTES
    mov ah, 3Fh
    int 21h
    jc fail_read
    cmp ax, CN32_HEADER_BYTES
    jne fail_format
    call check_header
    jc fail_format

    ; The PSP was shrunk first, so this allocation does not alias the COM
    ; image. Offset zero through total-1 fits in a single real-mode segment.
    movzx ebx, word [total_bytes]
    add ebx, 15
    shr ebx, 4
    mov ah, 48h
    int 21h
    jc fail_memory
    mov [buffer_seg], ax
    mov es, ax
    mov si, header
    xor di, di
    mov cx, CN32_HEADER_BYTES
    cld
    rep movsb
    mov word [read_offset], CN32_HEADER_BYTES
    mov ax, [payload_bytes]
    mov [remaining], ax
.read_payload:
    cmp word [remaining], 0
    je .check_eof
    mov bx, [file_handle]
    mov cx, [remaining]
    mov dx, [read_offset]
    mov ax, [buffer_seg]
    mov ds, ax
    mov ah, 3Fh
    int 21h
    push cs
    pop ds
    jc fail_read
    test ax, ax
    jz fail_format
    sub [remaining], ax
    add [read_offset], ax
    jmp .read_payload
.check_eof:
    mov bx, [file_handle]
    mov dx, eof_byte
    mov cx, 1
    mov ah, 3Fh
    int 21h
    jc fail_read
    test ax, ax
    jnz fail_format
    call close_file

    xor di, di
    mov es, di
    mov ax, 1684h
    mov bx, VM_DEVICE_ID
    int 2Fh
    mov [vm_entry], di
    mov [vm_entry+2], es
    mov ax, di
    mov dx, es
    or ax, dx
    jz fail_host

    mov ax, [buffer_seg]
    mov ds, ax
    mov es, ax
    xor si, si
    movzx ecx, word [cs:total_bytes]
    mov ax, VM_OP_NATIVE_RUN
    call far [cs:vm_entry]
    pushf
    mov [cs:run_result], eax
    mov [cs:last_tag], ebx
    mov [cs:last_value], ecx
    mov [cs:report_count], edx
    mov [cs:fault_vector], esi
    mov [cs:steps], edi
    mov [cs:fault_error], ebp
    push cs
    pop ds
    popf
    jc fail_host
    cmp dword [run_result], 0
    jne fail_native
    cmp dword [fault_error], 0
    jne .nonzero_exit
    cmp dword [report_count], 2
    jne .generic_exit
    cmp dword [last_tag], 2
    jne .generic_exit
    cmp dword [last_value], 4349554Bh
    jne .generic_exit
    mov si, msg_sample
    call serial_text
.generic_exit:
    mov si, msg_exit
    call serial_text
    mov ax, 4C00h
    int 21h
.nonzero_exit:
    mov si, msg_exit_status
    call serial_text
    mov si, fault_error
    call serial_hex32
    mov si, msg_newline
    call serial_text
    mov al, [fault_error]
    mov ah, 4Ch
    int 21h

; A caller may supply one image path; without an argument use the installed
; sample. A bounded DOS 8.3 path is sufficient for the first native fixture.
parse_path:
    mov si, 81h
    mov cl, [80h]
    xor ch, ch
.skip:
    test cx, cx
    jz .default
    mov al, [si]
    cmp al, ' '
    ja .copy_start
    inc si
    dec cx
    jmp .skip
.copy_start:
    mov di, path
    mov bx, 127
.copy:
    test cx, cx
    jz .done
    mov al, [si]
    cmp al, ' '
    jbe .done
    test bx, bx
    jz .bad
    stosb
    inc si
    dec cx
    dec bx
    jmp .copy
.done:
    mov byte [di], 0
    clc
    ret
.default:
    mov si, default_path
    mov di, path
.default_copy:
    lodsb
    stosb
    test al, al
    jnz .default_copy
    clc
    ret
.bad:
    stc
    ret

check_header:
    cmp dword [header], 32334E43h   ; "CN32"
    jne .bad
    cmp word [header+4], 1
    jne .bad
    cmp word [header+6], CN32_HEADER_BYTES
    jne .bad
    cmp dword [header+8], 1
    jne .bad
    mov eax, [header+16]             ; code bytes
    test eax, eax
    jz .bad
    cmp dword [header+12], eax       ; entry must be within code
    jae .bad
    add eax, [header+20]             ; data bytes
    jc .bad
    cmp eax, [header+28]
    jne .bad
    cmp eax, CN32_MAX_IMAGE_BYTES-CN32_HEADER_BYTES
    ja .bad
    cmp dword [header+24], 4096      ; at least one stack page
    jb .bad
    mov [payload_bytes], ax
    add ax, CN32_HEADER_BYTES
    mov [total_bytes], ax
    clc
    ret
.bad:
    stc
    ret

close_file:
    mov bx, [file_handle]
    mov ah, 3Eh
    int 21h
    mov word [file_handle], 0FFFFh
    ret

fail_read:
    mov si, msg_read
    jmp fail_with_file
fail_format:
    mov si, msg_format
fail_with_file:
    push si
    call close_file
    pop si
    jmp fail
fail_path:
    mov si, msg_path
    jmp fail
fail_open:
    mov si, msg_open
    jmp fail
fail_memory:
    mov si, msg_memory
    jmp fail
fail_host:
    mov si, msg_host
    jmp fail
fail_native:
    mov si, msg_native
    call serial_text
    mov si, run_result
    call serial_hex32
    mov si, msg_vector
    call serial_text
    mov si, fault_vector
    call serial_hex32
    mov si, msg_error
    call serial_text
    mov si, fault_error
    call serial_hex32
    mov si, msg_steps
    call serial_text
    mov si, steps
    call serial_hex32
    mov si, msg_newline
    jmp fail
fail:
    call serial_text
    mov ax, 4C01h
    int 21h

serial_text:
    lodsb
    test al, al
    jz .done
    push si
    mov dl, al
    mov ah, 02h
    int 21h
    pop si
    jmp serial_text
.done:
    ret

serial_hex32:
    push ax
    push bx
    push cx
    push si
    add si, 3
    mov cx, 4
.byte:
    mov bl, [si]
    mov al, bl
    shr al, 4
    call .nibble
    mov al, bl
    and al, 0Fh
    call .nibble
    dec si
    loop .byte
    pop si
    pop cx
    pop bx
    pop ax
    ret
.nibble:
    cmp al, 10
    jb .digit
    add al, 'A'-10
    jmp .emit
.digit:
    add al, '0'
.emit:
    push bx
    push cx
    push si
    mov dl, al
    mov ah, 02h
    int 21h
    pop si
    pop cx
    pop bx
    ret

default_path db '\VM\NATIVE32.N32',0
msg_exit db '[NATIVE32] EXIT 0',13,10,0
msg_exit_status db '[NATIVE32] EXIT status=',0
msg_sample db '[NATIVE32] SAMPLE PASS reports=2 tag=2 value=CIUK',13,10,0
msg_path db '[NATIVE32] FAIL path',13,10,0
msg_open db '[NATIVE32] FAIL open',13,10,0
msg_read db '[NATIVE32] FAIL read',13,10,0
msg_format db '[NATIVE32] FAIL image',13,10,0
msg_memory db '[NATIVE32] FAIL DOS memory',13,10,0
msg_host db '[NATIVE32] FAIL native host',13,10,0
msg_native db '[NATIVE32] FAIL native execution result=',0
msg_vector db ' vector=',0
msg_error db ' error=',0
msg_steps db ' steps=',0
msg_newline db 13,10,0
file_handle dw 0FFFFh
buffer_seg dw 0
payload_bytes dw 0
total_bytes dw 0
remaining dw 0
read_offset dw 0
eof_byte db 0
vm_entry dd 0
run_result dd 0
last_tag dd 0
last_value dd 0
report_count dd 0
fault_vector dd 0
fault_error dd 0
steps dd 0
header times CN32_HEADER_BYTES db 0
path times 128 db 0
stack_space times 1024 db 0
stack_top:
image_end:
